#include "BusWatch.h"
#include "DataModel.h"
#include <math.h>
#include <initializer_list>

namespace BusWatch {

static volatile uint8_t s_state = (uint8_t)State::Starting;
static bool     s_started     = false;
static volatile uint32_t s_startMs     = 0;   // first tick: the grace runs from here
static bool     s_hadData     = false;
static volatile uint32_t s_lastAliveMs = 0;   // when the newest data value arrived, as of the last tick that saw the bus alive (read by quietMs() from any task)
static uint32_t s_aisSweepMs  = 0;

State state()  { return (State)s_state; }
bool  silent() { const State s = state(); return s == State::Silent || s == State::Never; }

uint32_t quietMs(uint32_t nowMs) {
    switch (state()) {
        case State::Silent: return (uint32_t)(nowMs - s_lastAliveMs);
        case State::Never:  return (uint32_t)(nowMs - s_startMs);
        default:            return 0;
    }
}

const char *stateKey(State s) {
    switch (s) {
        case State::Ok:     return "ok";
        case State::Silent: return "silent";
        case State::Never:  return "never";
        default:            return "starting";
    }
}

// One value (or group) that came in with its own stamp: blank it once the
// stamp is older than maxAge, or right away when `all` is set (the bus went
// silent). The stamp goes to 0, so the next tick leaves it alone, and 0
// is also what "never received" looks like - nothing to blank there.
template <typename F>
static inline void expire(uint32_t &stamp, uint32_t now, uint32_t maxAge, bool all, F blank) {
    if (all || (stamp != 0 && !fresh(stamp, now, maxAge))) {
        blank();
        stamp = 0;
    }
}

// Own position or motion gone: a CPA computed from them is gone too (it
// is recomputed with the next target report once they are back).
static void dropCpa(DataModel &d) {
    if (!d.aisTargets) return;
    for (int i = 0; i < d.aisCount; i++) d.aisTargets[i].cpa = d.aisTargets[i].tcpa = NAN;
}

// The 5-minute AIS rule, now on every screen (it used to run only while the
// AIS radar was showing, so the web's target count and the table kept
// targets from long ago). A target heard only by its static data (name,
// no position yet) has lastSeen 0 and stays until a position arrives or
// the slot is needed; findOrCreateAis() takes such a one first.
// A row that goes leaves its name, callsign and type in the model's static
// cache (aisKeepStatic()): a vessel heard again - after a bus dropout, or
// back in range - has them at once, not with its next static report.
static_assert(AIS_STALE_MS == AIS_RANK_MAX_AGE_MS,
              "a cached AIS rank must age out when a row would (DataModel.h)");
static void sweepAis(DataModel &d, uint32_t now, bool all) {
    if (!d.aisTargets) { d.aisCount = 0; return; }
    if (all) {
        for (int i = 0; i < d.aisCount; i++) d.aisKeepStatic(d.aisTargets[i]);
        d.resetAisSlots(); d.aisCount = 0;
        return;
    }
    int j = 0;
    for (int i = 0; i < d.aisCount; i++) {
        const AisTarget &t = d.aisTargets[i];
        if (t.lastSeen == 0 || fresh(t.lastSeen, now, AIS_STALE_MS)) {
            if (i != j) d.aisTargets[j] = t;
            j++;
        } else {
            d.aisKeepStatic(t);
        }
    }
    for (int i = j; i < d.aisCount; i++) d.aisTargets[i] = AisTarget{};
    d.aisCount = j;
}

static void expireAll(DataModel &d, uint32_t now, bool all) {
    const uint32_t V = VALUE_STALE_MS, S = SLOW_STALE_MS;

    // ---- Navigation ----
    expire(d.lastGpsUpdate, now, V, all, [&] { d.lat = d.lon = NAN; dropCpa(d); });
    expire(d.lastSogRx, now, V, all, [&] { d.sog = NAN; dropCpa(d); });
    expire(d.lastCogRx, now, V, all, [&] { d.cog = NAN; d.cogMagnetic = false; dropCpa(d); });
    expire(d.lastHdgRx, now, V, all, [&] { d.hdg = NAN; d.hdgDeviation = NAN; d.hdgMagnetic = false; });
    expire(d.lastStwRx, now, V, all, [&] { d.stw = NAN; });
    expire(d.lastVariationUpdate, now, VARIATION_STALE_MS, all, [&] { d.variation = NAN; });
    if (all) {
        // The wind pipeline's own input stamps (it ages them after 5 s
        // anyway), and the wind and pilot pipelines themselves: their
        // published values are re-copied from the pipeline on every tick, so
        // only a reset blanks them (WindPipeline::tick, ApPipeline).
        d.lastHdgUpdate = d.lastStwUpdate = d.lastSogUpdate = d.lastCogUpdate = 0;
        d.awa = d.aws = d.twa = d.tws = d.twd = NAN;
        d.lastWindUpdate = 0;
        d.windPipe.reset();
        d.apMode       = ApDecode::MODE_UNKNOWN;
        d.apEngaged    = false;
        d.apModeSource = ApDecode::SRC_NONE;
        d.apStatus     = ApDecode::STATUS_UNKNOWN;
        d.apSetpoint   = NAN;
        d.apSetpointKind = ApDecode::SP_NONE;
        d.lastApModeMs = d.lastApSetpointMs = 0;
        d.apTrack.reset();
        // The bus tide too; its screen already lets it go after 10 min.
        d.tideBusLevel = NAN;
        d.lastTideBusUpdate = 0;
    }

    // ---- Depth, rudder, motion ----
    expire(d.lastDepthUpdate, now, V, all, [&] { d.depth = NAN; });
    expire(d.lastRudderUpdate, now, V, all, [&] { d.rudderAngle = NAN; });
    expire(d.lastAttitudeUpdate, now, V, all, [&] { d.roll = d.pitch = NAN; });
    expire(d.lastYawUpdate, now, V, all, [&] { d.yaw = NAN; });
    expire(d.lastRotUpdate, now, V, all, [&] { d.rateOfTurn = NAN; });
    // The wave estimate is recomputed only when a heave sample arrives, so
    // it has to go with the heave.
    expire(d.lastHeaveUpdate, now, V, all, [&] { d.heave = d.waveHeight = d.wavePeriod = NAN; });

    // ---- Engine, electrical ----
    expire(d.lastEngineUpdate, now, V, all, [&] { d.rpm = NAN; });
    expire(d.lastOilUpdate, now, V, all, [&] { d.oilPressure = NAN; });
    expire(d.lastCoolantUpdate, now, V, all, [&] { d.coolantTemp = NAN; });
    expire(d.lastFuelFlowUpdate, now, V, all, [&] { d.fuelFlow = NAN; });
    expire(d.lastEngineHoursUpdate, now, V, all, [&] { d.engineHours = NAN; });
    expire(d.lastBattUpdate, now, S, all, [&] { d.batteryVoltage = d.batteryCurrent = NAN; });
    // Tanks and banks keep their slot - its instance and type, a tank's
    // capacity - and lose their readings: the screens show such an entry as
    // missing, the web keeps it in its editors.
    for (int i = 0; i < d.tankCount; i++) {
        TankInfo &t = d.tanks[i];
        expire(t.lastUpdate, now, S, all, [&] { t.level = NAN; });
    }
    for (int i = 0; i < d.battCount; i++) {
        BatteryBank &b = d.batteries[i];
        expire(b.lastUpdate, now, S, all, [&] { b.voltage = b.current = b.temperature = NAN; });
        expire(b.lastStatusUpdate, now, S, all, [&] { b.soc = b.timeRemMin = NAN; });
    }

    // ---- Environment, log ----
    expire(d.lastAirTempUpdate, now, S, all, [&] { d.airTemp = NAN; });
    expire(d.lastWaterTempUpdate, now, S, all, [&] { d.waterTemp = NAN; });
    expire(d.lastHumidityUpdate, now, S, all, [&] { d.humidity = NAN; });
    expire(d.lastPressureUpdate, now, S, all, [&] { d.pressure = NAN; });
    expire(d.lastLogUpdate, now, S, all, [&] { d.logDistance = d.tripDistance = NAN; });

    // ---- Route, pilot commands, stereo ----
    expire(d.lastNavUpdate, now, V, all, [&] {
        d.navActive = false;
        d.navDtw = d.navBtw = d.navVmc = NAN;
        d.navWpNum = 0;
    });
    expire(d.lastXteUpdate, now, V, all, [&] { d.navXte = NAN; });
    // The wind's values are blanked by its own pipeline (10 s), but its
    // stamp is counted below as bus data and nothing else ever clears it:
    // left alone it would read as fresh again after ~24.8 days (signed age)
    // and keep a silent bus from being reported.
    if (d.lastWindUpdate && !fresh(d.lastWindUpdate, now, V)) d.lastWindUpdate = 0;
    expire(d.lastApUpdate, now, V, all, [&] { d.apTargetHeading = d.apRudder = NAN; });
    // Only a stereo that was heard: the demo's stereo never stamps lastMediaRx
    // and must not be wiped by this.
    expire(d.lastMediaRx, now, S, all, [&] { if (d.mediaConnected) d.clearMedia(); });

    // ---- AIS ----
    expire(d.lastAisRx, now, V, all, [] {});
    if (all || !fresh(s_aisSweepMs, now, 1000)) {
        s_aisSweepMs = now ? now : 1;
        sweepAis(d, now, all);
    }
}

// The newest stamp of any DATA value, or 0, and which value it was (for the
// log line). Not counted: time (a plotter sends it with every instrument
// off), the stereo (its stamp also follows our own commands), the magnetic
// variation and a tide station (near-constant or rare, and shown nowhere
// prominent - a bus that delivers only those leaves every instrument on
// "--", which is exactly what the warning is for), and the update protocol,
// heartbeats, address claims and anchor sync, which never touch the model.
uint32_t lastDataMs(const DataModel &d, const char **which) {
    uint32_t newest = 0;
    const char *from = nullptr;
    auto consider = [&](uint32_t s, const char *name) {
        if (s != 0 && (newest == 0 || (int32_t)(s - newest) > 0)) { newest = s; from = name; }
    };
    consider(d.lastGpsUpdate,       "position");
    consider(d.lastSogRx,           "SOG");
    consider(d.lastCogRx,           "COG");
    consider(d.lastHdgRx,           "heading");
    consider(d.lastStwRx,           "STW");
    consider(d.lastWindUpdate,      "wind");
    consider(d.lastDepthUpdate,     "depth");
    consider(d.lastRudderUpdate,    "rudder");
    consider(d.lastAttitudeUpdate,  "attitude");
    consider(d.lastYawUpdate,       "yaw");
    consider(d.lastRotUpdate,       "rate of turn");
    consider(d.lastHeaveUpdate,     "heave");
    consider(d.lastEngineUpdate,    "engine rpm");
    consider(d.lastOilUpdate,       "oil pressure");
    consider(d.lastCoolantUpdate,   "coolant");
    consider(d.lastFuelFlowUpdate,  "fuel flow");
    consider(d.lastEngineHoursUpdate, "engine hours");
    consider(d.lastBattUpdate,      "battery");
    consider(d.lastAirTempUpdate,   "air temperature");
    consider(d.lastWaterTempUpdate, "water temperature");
    consider(d.lastHumidityUpdate,  "humidity");
    consider(d.lastPressureUpdate,  "pressure");
    consider(d.lastLogUpdate,       "distance log");
    consider(d.lastNavUpdate,       "route");
    consider(d.lastXteUpdate,       "cross-track error");
    consider(d.lastApUpdate,        "autopilot command");
    consider(d.lastApModeMs,        "autopilot mode");
    consider(d.lastApSetpointMs,    "autopilot setpoint");
    for (int i = 0; i < d.tankCount; i++) consider(d.tanks[i].lastUpdate, "tank");
    for (int i = 0; i < d.battCount; i++) {
        consider(d.batteries[i].lastUpdate, "battery bank");
        consider(d.batteries[i].lastStatusUpdate, "battery state");
    }
    // A report that found no room in a full table stamps no row, and the bus is
    // anything but silent while hundreds of those arrive.
    consider(d.lastAisRx, "AIS");
    if (d.aisTargets)
        for (int i = 0; i < d.aisCount; i++) consider(d.aisTargets[i].lastSeen, "AIS");
    if (which) *which = from;
    return newest;
}

bool positionFresh(const DataModel &d, uint32_t nowMs) {
    return !isnan(d.lat) && !isnan(d.lon) && fresh(d.lastGpsUpdate, nowMs, VALUE_STALE_MS);
}

Change tick(DataModel &d, uint32_t now) {
    if (!s_started) { s_started = true; s_startMs = now; }

    expireAll(d, now, false);

    const State    from   = state();
    const char    *what   = nullptr;
    const uint32_t newest = lastDataMs(d, &what);
    const bool     alive  = fresh(newest, now, BUS_SILENT_MS);
    const uint32_t prevAliveMs = s_lastAliveMs;
    State to;
    if (alive) {
        to = State::Ok;
        s_hadData = true;
        s_lastAliveMs = newest;   // when the data came, not when we looked
    } else if (s_hadData) {
        // Measured from the newest data seen while there was any, kept here:
        // after the demo is switched off every stamp is 0 at once, and a live
        // bus needs a moment to refill them - that must not count as silence.
        to = fresh(s_lastAliveMs, now, BUS_SILENT_MS) ? from : State::Silent;
    } else {
        // Once Never, Never until data comes: a signed compare of the grace
        // alone would read "just started" again after ~24.8 days.
        to = (from == State::Never || (uint32_t)(now - s_startMs) >= BOOT_GRACE_MS)
             ? State::Never : State::Starting;
    }

    Change c;
    if (to != from) {
        c.changed = true;
        c.from = from;
        c.to = to;
        c.quietMs = prevAliveMs ? (uint32_t)(now - prevAliveMs) : (uint32_t)(now - s_startMs);
        c.what = what;
        // Everything at once - the slow values (30 s) would otherwise stay
        // up for another 20 s under the warning that says there is no data.
        if (to == State::Silent || to == State::Never) expireAll(d, now, true);
        s_state = (uint8_t)to;
    }
    return c;
}

} // namespace BusWatch
