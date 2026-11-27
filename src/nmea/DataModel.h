#pragma once
#include <Arduino.h>
#include <cmath>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../PsramArena.h"
#include "WindCalc.h"
#include "ApDecode.h"

// ============================================================
// DataModel – central store for all live NMEA 2000 data.
// Access always through the RAII lock guard.
//
// The bulk buffers (AIS targets, the AIS static-data cache and the history
// rings) are NOT members any more, only pointers into the PSRAM arena – see
// initBuffers(). As plain arrays the table and the rings were ~10.5 KB of
// .bss in a global object, on boards where the free internal RAM is measured
// in tens of kilobytes and WiFi/HTTP is the first thing to suffer. None of
// that data is touched from an ISR or used as a DMA target, so PSRAM is a
// safe home for it.
//
// Consequence to keep in mind when editing this file: sizeof() on those
// members is now the size of a POINTER. Anything that clears or copies them
// must spell out count * element size.
// ============================================================

// to - from in degrees of longitude, the short way round: across the date line a
// plain difference is 359.9 for two vessels 3 nm apart.
static inline float lonDelta(float to, float from) {
    float d = to - from;
    if (d > 180.f) d -= 360.f;
    else if (d < -180.f) d += 360.f;
    return d;
}

struct AisTarget {
    uint32_t mmsi       = 0;
    float    lat        = NAN;
    float    lon        = NAN;
    float    sog        = NAN;   // knots
    float    cog        = NAN;   // degrees true
    float    hdg        = NAN;   // degrees
    float    rateOfTurn = NAN;   // deg/min
    char     name[21]   = {};
    char     callsign[8]= {};
    uint8_t  shipType   = 0;
    uint8_t  navStatus  = 15;    // 15 = undefined
    float    cpa        = NAN;   // nm
    float    tcpa       = NAN;   // min
    bool     classB     = false;
    uint32_t lastSeen   = 0;     // millis()
};

// The static data of a vessel that has no row in the AIS table - not yet, or
// not any more. Name and callsign come only every few minutes (class A
// message 5, class B 24A/B), so with a full table a vessel that was given a
// row showed without them until its next static report, and one pushed out of
// the table or swept off it (BusWatch) came back nameless. The cache keeps
// them for findOrCreateAis(), which hands them to the vessel's next row.
// Ranked like the table (aisRank2(), lower = matters more): when it is full,
// the worst rank goes, an unknown one counting as the worst, of equals the
// oldest - except an entry that is WAITING (aisStaticWaits()), see below.
static constexpr float AIS_RANK_UNKNOWN = 3.0e38f;
// A rank is where the vessel stood when it was last heard. Once that is as old
// as a row may go unheard (BusWatch::AIS_STALE_MS, asserted equal in
// BusWatch.cpp) the vessel has gone quiet - switched off, out of range - and
// the rank says nothing any more: the entry then counts as unknown. Without
// this, vessels that went silent near own ship (a marina's boats switching
// off) kept their near ranks for good, and once 64 of them had gathered the
// cache stopped naming newcomers.
static constexpr uint32_t AIS_RANK_MAX_AGE_MS = 300000;
// A vessel heard for the first time by its static report has no rank until
// its first position report - and in a crowded feed the next static report,
// a second later, used to take its place long before that came (with 640
// vessels within 60 nm only 44 % of the vessels that got a row had their name
// at once). So an entry without a rank that was heard within AIS_WAIT_MS
// WAITS: it is not pushed out while fewer than half the entries wait, and
// once half or more do, it counts as unknown (the worst) like the rest, so a
// flood of static reports cannot push out every ranked vessel. The window is
// the rank's age limit: it covers a vessel that reports its position only
// every 3 minutes (class A at anchor or moored, class B below 2 kn) with room
// to spare, and an entry whose rank has aged out - a vessel gone quiet, the
// ghosts above - had its last POSITION at least that long ago: aging forgets
// its stamp (aisStaticAge()), so it does not wait - a longer window would
// shield the ghosts again. That holds even if its vessel sent a static report
// a moment before: saved while the rank was still valid, the report keeps the
// position's stamp (aisStaticSave()). Only a static report saved after the
// rank aged out makes the entry wait again. The shield holds only while fewer
// than half wait: with every vessel sending its static data every 6 minutes,
// roughly while vessels x position interval stays below about 20,000 s (2,000
// vessels at 10 s, 640 at 30 s). Beyond that, more and more newcomers lose
// their entry before their first position report (simulated: 75 to 79 % named
// at once with 1,000 vessels at 30 s, 34 to 48 % with 640 to 2,000 at 3 min).
static constexpr uint32_t AIS_WAIT_MS = AIS_RANK_MAX_AGE_MS;
struct AisStatic {
    uint32_t mmsi     = 0;                  // 0 = free
    uint32_t seq      = 0;                  // last write; of equal ranks the lowest goes
    float    rank     = AIS_RANK_UNKNOWN;   // where it stood when last ranked
    uint32_t rankAt   = 0;                  // millis() of the position that rank is from;
                                            // without a rank, when the vessel was last
                                            // heard (0: not known, or long ago)
    char     name[21] = {};
    char     callsign[8] = {};
    uint8_t  shipType = 0;
};   // 48 bytes
static_assert(sizeof(AisStatic::name) == sizeof(AisTarget::name) &&
              sizeof(AisStatic::callsign) == sizeof(AisTarget::callsign),
              "AisStatic copies these fields whole");
static_assert(AIS_WAIT_MS <= AIS_RANK_MAX_AGE_MS, "an aged rank must never wait (see AIS_WAIT_MS)");

struct WindSample {
    float    twd;
    float    tws;
    uint32_t t;
};

struct TankInfo {
    uint8_t  instance   = 0;
    uint8_t  fluidType  = 0xFF;   // tN2kFluidType (0=Fuel,1=Water,2=Gray,5=Black,…); 0xFF = empty
    float    level      = NAN;    // fill level [%]
    float    capacity   = NAN;    // tank capacity [litres]
    uint32_t lastUpdate = 0;
};

struct BatteryBank {
    uint8_t  instance    = 0xFF;  // 0xFF = empty slot
    float    voltage     = NAN;   // V
    float    current     = NAN;   // A (+ = charging, - = discharging)
    float    soc         = NAN;   // state of charge [%]
    float    timeRemMin  = NAN;   // minutes to empty (discharging) or full (charging)
    float    temperature = NAN;   // °C
    uint32_t lastUpdate  = 0;     // voltage, current, temperature (127508)
    uint32_t lastStatusUpdate = 0;   // soc, time remaining (127506)
};

class DataModel {
public:
    // ---- Navigation --------------------------------------------------------
    float lat = NAN, lon = NAN;
    float sog = NAN;        // speed over ground [kn]
    float cog = NAN;        // course over ground [deg true]
    bool  cogMagnetic = false; // ... unless 129026 said MAGNETIC (read by the true wind only)
    float hdg = NAN;        // heading as received (PGN 127250 + compass offset) [deg]
    bool  hdgMagnetic = false; // ... and 127250 said its reference is MAGNETIC
    float hdgDeviation = NAN;  // ... and the compass deviation it sent [deg, E positive]
    float variation = NAN;  // magnetic variation [deg, E positive]
    float stw = NAN;        // speed through water [kn]
    uint32_t lastGpsUpdate = 0;
    // When each input of the true-wind calculation last arrived (millis,
    // 0 = never). Older than WindCalc::FRESH_MS and it no longer counts -
    // WindPipeline::tick() then resets it to 0, so read them as "fresh or 0".
    uint32_t lastHdgUpdate = 0;
    uint32_t lastStwUpdate = 0;
    uint32_t lastSogUpdate = 0;
    // COG: only for TWD without a compass (cal.twd_cog). A 129026 that marks
    // COG "not available" sets it back to 0 at once; cog itself keeps the
    // last course for the other screens, as it always has.
    uint32_t lastCogUpdate = 0;
    // When the VALUES above last arrived, for BusWatch: the four stamps just
    // above belong to the wind pipeline, which zeroes them after 5 s, so they
    // cannot also carry a 10 s rule. COG counts every 129026 that still
    // carries a SOG: a GPS at anchor marks COG "not available" while its
    // speed reads about 0, and the last course is kept while it does (see
    // lastCogUpdate); one without a fix marks both, and the course goes too.
    uint32_t lastSogRx = 0, lastCogRx = 0, lastHdgRx = 0, lastStwRx = 0;
    uint32_t lastVariationUpdate = 0;
    // The last position that was ever known - NOT blanked when the GPS goes
    // stale. Only for things that need "where roughly" rather than "where
    // now": the tide station of the internet forecast and the solar theme.
    float lastKnownLat = NAN, lastKnownLon = NAN;

    // ---- Wind --------------------------------------------------------------
    // The DAMPED values - what every screen, the sidebar and /api/data show.
    // Written in exactly one place, WindPipeline.cpp, from windPipe below:
    // the boat measures apparent wind, the display computes true wind itself
    // (WindCalc.h has the rules). NaN = not known, or gone stale.
    float awa = NAN;        // apparent wind angle [deg, -180..+180, port negative]
    float aws = NAN;        // apparent wind speed [kn]
    float twa = NAN;        // true wind angle [deg, -180..+180, port negative]
    float tws = NAN;        // true wind speed [kn]
    float twd = NAN;        // true wind direction [deg true, 0..360]
    uint32_t lastWindUpdate = 0;   // last wind sample that was USED
    WindCalc::Pipeline windPipe;   // filters + freshness state behind the five

    // ---- Depth -------------------------------------------------------------
    float depth = NAN;      // depth below transducer [m]
    float depthOffset = 0;  // + = to keel, - = to surface
    uint32_t lastDepthUpdate = 0;

    // ---- Engine ------------------------------------------------------------
    float rpm            = NAN;
    float oilPressure    = NAN;   // hPa
    float coolantTemp    = NAN;   // °C
    float engineHours    = NAN;   // h
    float fuelFlow       = NAN;   // L/h
    uint8_t engineInstance = 0;
    uint32_t lastEngineUpdate = 0;      // rpm (127488)
    // 127489, one each: a failed oil sender must not live on through a
    // working coolant sender in the same frame.
    uint32_t lastOilUpdate = 0, lastCoolantUpdate = 0;
    uint32_t lastFuelFlowUpdate = 0, lastEngineHoursUpdate = 0;

    // ---- Electrical --------------------------------------------------------
    float batteryVoltage = NAN;   // V (house bank)
    float batteryCurrent = NAN;   // A
    uint32_t lastBattUpdate = 0;  // the two above (bank 0 of 127508)

    // ---- Rudder ------------------------------------------------------------
    float rudderAngle = NAN;      // deg, positive = starboard
    uint32_t lastRudderUpdate = 0;

    // ---- Attitude / Motion (attitude sensor (AHRS): PGN 127257 / 127251 / 127252)
    float roll       = NAN;       // deg, + = starboard side down (heel)
    float pitch      = NAN;       // deg, + = bow up
    float yaw        = NAN;       // deg true (heading from the attitude sensor)
    float rateOfTurn = NAN;       // deg/min, + = turning to starboard
    float heave      = NAN;       // m, + = up (vertical wave-induced motion)
    uint32_t lastAttitudeUpdate = 0;   // a valid pitch or roll
    uint32_t lastYawUpdate      = 0;
    uint32_t lastRotUpdate      = 0;
    uint32_t lastHeaveUpdate    = 0;
    // Derived wave estimate from the heave signal (no direction):
    float waveHeight = NAN;       // m, peak-to-trough over a ~15 s window
    float wavePeriod = NAN;       // s, mean heave oscillation period

    // ---- Environment (PGN 130310 / 130311 / 130314) ------------------------
    float airTemp   = NAN;        // outside air temperature [°C]
    float waterTemp = NAN;        // sea water temperature [°C]
    float humidity  = NAN;        // relative humidity [%]
    float pressure  = NAN;        // barometric pressure [hPa]
    // One stamp each: four PGNs carry these in different mixes, and with a
    // shared stamp a living air sensor kept a dead water temperature "fresh".
    uint32_t lastAirTempUpdate = 0, lastWaterTempUpdate = 0;
    uint32_t lastHumidityUpdate = 0, lastPressureUpdate = 0;

    // ---- Distance log (PGN 128275) -----------------------------------------
    float logDistance  = NAN;     // total distance through the water [nm]
    float tripDistance = NAN;     // trip distance through the water [nm]
    uint32_t lastLogUpdate = 0;

    // ---- Time / date (PGN 126992 system time + 129033 local offset) --------
    uint16_t sysDays        = 0;   // days since 1970-01-01 (UTC)
    double   sysSecOfDay    = 0;   // seconds since midnight UTC
    int16_t  localOffsetMin = 0;   // local time = UTC + this [minutes]
    bool     localOffsetIsRule = false; // offset is the SNTP time zone rule's and
                                        // follows DST; a bus offset clears it,
                                        // clearValues() sets it again on an
                                        // SNTP clock
    uint32_t lastTimeUpdate = 0;   // millis() of the last time fix
    bool     timeValid      = false;
    bool     timeIsReal     = false; // set by the SNTP sync only (bus time never
                                     // sets it) and never cleared: the demo
                                     // leaves the clock alone while it is set,
                                     // and clearValues() keeps the clock and
                                     // hands the offset back to the zone rule,
                                     // which BshTide's SNTP start has installed by then

    // ---- Tide forecast (BSH Wasserstandsvorhersage, when online) -----------
    // Official German HW/NW predictions fetched from the BSH OGC API. Times are
    // absolute UTC; heights are cm above chart datum (Seekartennull). When no
    // fresh BSH data is present the ClockScreen falls back to its own estimate.
    static constexpr int MAX_TIDE_FC = 6;
    struct TideExtreme {
        uint32_t unixUtc = 0;        // event time, seconds since 1970 UTC
        int16_t  cmCD    = 0;        // height above chart datum [cm]
        bool     isHigh  = false;    // true = HW, false = NW
    };
    TideExtreme tideFc[MAX_TIDE_FC];
    int      tideFcCount     = 0;
    char     tideStation[40] = {0};  // BSH gauge label
    bool     tideIsBsh       = false;// true once BSH data has been stored
    uint32_t lastTideFcMs    = 0;    // millis() of the last successful BSH fetch

    // ---- Tide from the bus (PGN 130320 Tide Station Data) ------------------
    // Live water level + tendency from a tide station on the NMEA 2000 bus, if
    // one transmits it (rare). Takes priority over BSH/estimate when fresh.
    float    tideBusLevel   = NAN;   // current water level [m]
    bool     tideBusRising  = false; // tendency (true = rising)
    char     tideBusStation[24] = {0};
    uint32_t lastTideBusUpdate = 0;  // millis() of the last PGN 130320

    // ---- Waypoint navigation (PGN 129283 XTE + 129284 nav data) ------------
    bool     navActive = false;    // a route/waypoint is being navigated
    float    navDtw    = NAN;      // distance to waypoint [nm]
    float    navBtw    = NAN;      // bearing to waypoint [deg true]
    float    navXte    = NAN;      // cross-track error [m] (+ = steer right)
    float    navVmc    = NAN;      // velocity made good toward the waypoint [kn]
    uint32_t navWpNum  = 0;        // destination waypoint number
    uint32_t lastNavUpdate = 0;    // 129284 with at least one value
    uint32_t lastXteUpdate = 0;    // 129283

    // ---- Autopilot ---------------------------------------------------------
    // From PGN 127237 (heading/track control), as before. It does NOT decide
    // the mode: pilots have been seen reporting steering mode 7 there even
    // while engaged.
    float   apHeading       = NAN;  // current heading [deg]
    float   apTargetHeading = NAN;  // heading-to-steer [deg]
    float   apRudder        = NAN;  // commanded rudder [deg]
    uint32_t lastApUpdate   = 0;    // last heading-to-steer
    // The pilot computer's own state, from the proprietary PGNs of
    // manufacturer code 1857 (65305 / 65340 / 65341). Written in one place
    // only, ApPipeline.cpp, from apTrack; read-only everywhere else. The mode
    // starts - and falls back after 10 s without a status - as UNKNOWN, which
    // the screen shows as "--": a STANDBY nobody reported is not shown.
    uint8_t apMode          = ApDecode::MODE_UNKNOWN;   // ApDecode::Mode
    bool    apEngaged       = false;  // apMode is neither UNKNOWN nor STANDBY
    uint8_t apModeSource    = ApDecode::SRC_NONE;       // which PGN decided it
    uint8_t apPilotSa       = 0xFF;   // its source address (0xFF = none)
    uint8_t apStatus        = ApDecode::STATUS_UNKNOWN; // 65305 report 2
    // Setpoint of 65341 that belongs to the mode (ApDecode::setpointKindFor):
    // the held heading [deg, 0..360] in AUTO/NAV/NO DRIFT, the held wind
    // angle [deg, -180..+180, port negative] in WIND, the newer of the two
    // while the mode is not known (ACTIVE or unknown). NaN when there is no
    // fresh one, and always in STANDBY.
    float   apSetpoint      = NAN;
    uint8_t apSetpointKind  = ApDecode::SP_NONE;        // ApDecode::SetpointKind
    uint32_t lastApModeMs     = 0;
    uint32_t lastApSetpointMs = 0;
    ApDecode::Tracker apTrack;      // priority + debounce + staleness behind them

    // ---- Media / audio (NMEA 2000 entertainment, manufacturer-419 protocol) --
    static constexpr int MEDIA_MAX_SOURCES = 8;
    static constexpr int MEDIA_NUM_ZONES   = 3;     // Zone 1..3 (+ derived Master)
    char     mediaSourceName[MEDIA_MAX_SOURCES][16] = {};  // source/input labels
    uint8_t  mediaSourceCount = 0;
    int8_t   mediaSource      = -1;       // index of current source (-1 = unknown)
    char     mediaTitle[48]   = {};       // track title / station name
    char     mediaArtist[40]  = {};
    char     mediaAlbum[40]   = {};
    uint32_t mediaElapsedMs   = 0;        // track elapsed time
    uint32_t mediaTotalMs     = 0;        // track length (0 = unknown / live)
    uint8_t  mediaPlayState   = 0;        // 0=stopped 1=playing 2=paused
    uint8_t  mediaZoneVol[MEDIA_NUM_ZONES]  = {0,0,0};        // 0..100 per zone
    bool     mediaZoneMute[MEDIA_NUM_ZONES] = {false,false,false};  // per-zone mute
    bool     mediaConnected   = false;    // have we heard from a radio on the bus?
    uint32_t lastMediaUpdate  = 0;        // also set by our own commands' local echo
    uint32_t lastMediaRx      = 0;        // the stereo's own status (130820) only

    // Master volume = loudest zone (the proportional reference level).
    uint8_t mediaMasterVol() const {
        uint8_t m = 0;
        for (int i = 0; i < MEDIA_NUM_ZONES; i++) if (mediaZoneVol[i] > m) m = mediaZoneVol[i];
        return m;
    }
    // ---- AIS ---------------------------------------------------------------
    static constexpr int MAX_AIS = 50;
    AisTarget *aisTargets = nullptr;   // MAX_AIS entries, PSRAM (initBuffers)
    int       aisCount = 0;
    uint32_t  lastAisRx = 0;   // newest position report, kept in the table or not

    // ---- Tanks (PGN 127505 Fluid Level) ------------------------------------
    static constexpr int MAX_TANKS = 6;
    TankInfo  tanks[MAX_TANKS];
    int       tankCount = 0;
    TankInfo* findOrCreateTank(uint8_t inst, uint8_t type) {
        for (int i = 0; i < tankCount; i++)
            if (tanks[i].instance == inst && tanks[i].fluidType == type) return &tanks[i];
        if (tankCount < MAX_TANKS) {
            tanks[tankCount] = TankInfo{};
            tanks[tankCount].instance = inst;
            tanks[tankCount].fluidType = type;
            return &tanks[tankCount++];
        }
        return nullptr;
    }

    // ---- Battery banks (PGN 127508 status + 127506 DC detailed) ------------
    static constexpr int MAX_BATT = 4;
    BatteryBank batteries[MAX_BATT];
    int         battCount = 0;
    BatteryBank* findOrCreateBattery(uint8_t inst) {
        for (int i = 0; i < battCount; i++)
            if (batteries[i].instance == inst) return &batteries[i];
        if (battCount < MAX_BATT) {
            batteries[battCount] = BatteryBank{};
            batteries[battCount].instance = inst;
            return &batteries[battCount++];
        }
        return nullptr;
    }

    // ---- History (ring buffers) --------------------------------------------
    // Wind history (TWD/TWS, the wind-history page): one sample per
    // WIND_HIST_MS on a clock, so the 360 entries are six minutes of valid
    // true wind on every boat. Seconds without a TWD or TWS (no heading, no
    // boat speed) are skipped, not stored, so after such a gap the ring
    // reaches further back than six minutes. It used to take one per wind
    // PGN, which made the span depend on the sensor - about 13 s with a
    // simulator sending three 130306 frames every 100 ms.
    static constexpr int      WIND_HIST    = 360;
    static constexpr uint32_t WIND_HIST_MS = 1000;
    static constexpr int DEPTH_HIST  = 300;
    static constexpr int SPEED_HIST  = 120;

    // Short boat-relative wind trace, for the two history curves drawn inside
    // the wind instrument. A SEPARATE ring from windHistory on purpose:
    //
    //  * Different quantity. windHistory keeps TWD, the COMPASS direction, for
    //    the wind-history page. The instrument needs the BOAT-relative angle -
    //    and it cannot be derived after the fact, because computing TWA from a
    //    stored TWD and the CURRENT heading swings the whole historical curve
    //    round every time the boat turns. A tack would look like a 90 degree
    //    wind shift.
    //  * Different length. This one is about a minute; the history page spans
    //    six.
    //  * Different stack cost. WindPlotScreen copies the whole windHistory onto
    //    the stack (4.3 KB of the loop task's 8 KB), so growing WindSample
    //    would grow that copy. This ring is small enough to copy safely.
    //
    // No timestamp per sample: the push runs on a fixed timer, so the index IS
    // the time axis. A gap in the data is stored as NaN, which breaks the drawn
    // line rather than inventing a straight segment across it.
    static constexpr int      WIND_TRACE    = 120;   // at 2 Hz => 60 s
    static constexpr uint32_t WIND_TRACE_MS = 500;   // fixed push interval
    // Silence longer than this is a hole in the curve, not a steady wind.
    static constexpr uint32_t WIND_TRACE_STALE_MS = 5000;
    struct WindTracePoint { float twa, tws, awa, aws; };
    WindTracePoint *windTrace   = nullptr;           // WIND_TRACE entries
    int   windTraceIdx  = 0;
    bool  windTraceFull = false;
    uint32_t lastWindTraceMs = 0;

    // The rings live in the PSRAM arena, only the indices stay here.
    WindSample *windHistory = nullptr;   // WIND_HIST entries
    int        windHistIdx  = 0;
    bool       windHistFull = false;
    uint32_t   windHistNextMs = 0;       // next history slot (0 = clock not started)

    float *depthHistory = nullptr;       // DEPTH_HIST entries
    int   depthHistIdx  = 0;
    bool  depthHistFull = false;
    // Bumped by every write to the ring - pushDepthSample() and the reset
    // below - and never otherwise, so an unchanged value means an unchanged
    // ring. The Depth screen skips a repaint on it (DepthScreen::update()):
    // depthHistIdx alone cannot tell, it comes back to the same value after
    // DEPTH_HIST pushes. Whatever else writes the ring must bump it as well.
    uint32_t depthHistSeq = 0;

    float *speedHistory = nullptr;       // SPEED_HIST entries
    int   speedHistIdx  = 0;

    // Heave ring for the wave estimator (~16 s @ 5 Hz). Stores value + timestamp.
    static constexpr int HEAVE_HIST = 80;
    float    *heaveHistVal = nullptr;    // HEAVE_HIST entries
    uint32_t *heaveHistMs  = nullptr;    // HEAVE_HIST entries
    int      heaveHistIdx  = 0;
    bool     heaveHistFull = false;

    // Barometric pressure trend ring (~1.5 h @ one sample/min on real data).
    static constexpr int PRESS_HIST = 90;
    float    *pressHistVal = nullptr;    // PRESS_HIST entries
    int      pressHistIdx  = 0;
    bool     pressHistFull = false;
    uint32_t lastPressPushMs = 0;

    // ---- Mutex (acquire before any read/write) -----------------------------
    SemaphoreHandle_t mutex;

    DataModel() {
        mutex = xSemaphoreCreateMutex();
        // The bulk buffers are deliberately NOT allocated here. This object is
        // a global (main.cpp / sim_main.cpp), so its constructor runs before
        // setup() and therefore before the PSRAM arena exists. initBuffers()
        // does the allocation, right after PsramArena::init().
    }

    // Allocate the AIS table and the history rings from the PSRAM arena.
    // Call ONCE at startup, after PsramArena::init() and before anything can
    // read or write the model. Both entry points must do it themselves:
    // main.cpp for the device, sim_main.cpp for the PC simulator — the
    // simulator never runs the device's startup path. Miss it there and the
    // first render walks straight into a null pointer, because screens like
    // WindPlot/Depth memcpy the whole ring unconditionally.
    void initBuffers() {
        // The "already done" flag is deliberately NOT `if (aisTargets)`: if the
        // AIS table were the one allocation that failed, that test would stay
        // false and a second call would allocate all the OTHER buffers a second
        // time — and the arena never frees, so the first set would be lost for
        // good.
        if (buffersInited) return;
        buffersInited = true;

        aisTargets   = (AisTarget  *)allocBuf(sizeof(AisTarget)  * MAX_AIS,    "aisTargets");
        windHistory  = (WindSample *)allocBuf(sizeof(WindSample) * WIND_HIST,  "windHistory");
        windTrace    = (WindTracePoint *)allocBuf(sizeof(WindTracePoint) * WIND_TRACE, "windTrace");
        depthHistory = (float      *)allocBuf(sizeof(float)      * DEPTH_HIST, "depthHistory");
        speedHistory = (float      *)allocBuf(sizeof(float)      * SPEED_HIST, "speedHistory");
        heaveHistVal = (float      *)allocBuf(sizeof(float)      * HEAVE_HIST, "heaveHistVal");
        heaveHistMs  = (uint32_t   *)allocBuf(sizeof(uint32_t)   * HEAVE_HIST, "heaveHistMs");
        pressHistVal = (float      *)allocBuf(sizeof(float)      * PRESS_HIST, "pressHistVal");
        // The AIS static cache has no DRAM fallback (PsramArena::alloc, not
        // allocBuf): it is a convenience, and without it - null - the table
        // works exactly as it did before there was one.
        aisStatic    = (AisStatic  *)PsramArena::alloc(sizeof(AisStatic)  * AIS_STATIC_N);
        // Arena memory arrives zero-filled, which is the right starting state
        // for every numeric ring (that is what the old constructor memset did).
        // It is NOT right for an AIS slot: its declared defaults are NaN
        // position and navStatus 15 (undefined), while all-zero would read as a
        // real ship sitting at 0°N 0°E. Nor for a cache entry, whose rank
        // starts unknown, not at the best there is.
        resetAisSlots();
        aisStaticClear();
    }

    // Reset every value to "no data" (NaN / zero) while KEEPING the mutex and
    // the arena buffers - and a clock set by SNTP (see "Time / date" below).
    // Call with the lock held.
    //
    // This clears the fields IN PLACE. The old implementation built a
    // `DataModel blank` on the stack and assigned it over *this; that is now
    // impossible for two independent reasons, and the second one was already a
    // live defect before any of this moved to PSRAM:
    //   1. windHistory & co. are pointers into the arena now. The temporary's
    //      copies are null, so the assignment would replace every live buffer
    //      with a null pointer and all later samples would be written through
    //      it.
    //   2. That temporary was ~11.7 KB of automatic storage. The only caller is
    //      the DEMO task in main.cpp, whose stack is 4,096 bytes — so booting
    //      in demo mode and then switching demo off overflowed that stack.
    //      In place, clearValues() needs no meaningful stack at all.
    // The mutex needs no special handling any more either: it is simply never
    // written, so a Lock waiting on it keeps waiting on the same semaphore.
    void clearValues() {
        // ---- Navigation ----
        lat = lon = sog = cog = hdg = variation = stw = NAN;
        hdgMagnetic = false; hdgDeviation = NAN; cogMagnetic = false;
        lastGpsUpdate = 0;
        lastHdgUpdate = lastStwUpdate = lastSogUpdate = lastCogUpdate = 0;
        lastSogRx = lastCogRx = lastHdgRx = lastStwRx = 0;
        lastVariationUpdate = 0;
        // The demo's invented position must not pick the tide station.
        lastKnownLat = lastKnownLon = NAN;

        // ---- Wind ----
        // The pipeline too: its filters would otherwise start the next
        // source's first sample from the demo's last value.
        awa = aws = twa = tws = twd = NAN;
        lastWindUpdate = 0;
        windPipe.reset();

        // ---- Depth ----
        depth = NAN;
        depthOffset = 0;
        lastDepthUpdate = 0;

        // ---- Engine ----
        rpm = oilPressure = coolantTemp = engineHours = fuelFlow = NAN;
        engineInstance = 0;
        lastEngineUpdate = 0;
        lastOilUpdate = lastCoolantUpdate = lastFuelFlowUpdate = lastEngineHoursUpdate = 0;

        // ---- Electrical ----
        batteryVoltage = batteryCurrent = NAN;
        lastBattUpdate = 0;

        // ---- Rudder ----
        rudderAngle = NAN;
        lastRudderUpdate = 0;

        // ---- Attitude / motion ----
        roll = pitch = yaw = rateOfTurn = heave = NAN;
        lastAttitudeUpdate = 0;
        lastYawUpdate      = 0;
        lastRotUpdate      = 0;
        lastHeaveUpdate    = 0;
        waveHeight = wavePeriod = NAN;

        // ---- Environment ----
        airTemp = waterTemp = humidity = pressure = NAN;
        lastAirTempUpdate = lastWaterTempUpdate = 0;
        lastHumidityUpdate = lastPressureUpdate = 0;

        // ---- Distance log ----
        logDistance = tripDistance = NAN;
        lastLogUpdate = 0;

        // ---- Time / date ----
        // A clock set by SNTP stays: it is real time, not demo data (the demo
        // leaves the clock alone once timeIsReal is set), and the system clock
        // and the zone rule behind it still run. Dropping it blanked the clock
        // until a GPS time came, which then showed as UTC until the next sync,
        // up to a day later. The offset goes back to the
        // rule, as right after a sync: a bus offset (PGN 129033) is bus data
        // and comes back with the next one, and bshLocalOffsetTick() puts in
        // the rule's value within a second.
        if (timeIsReal) {
            localOffsetIsRule = true;
        } else {
            sysDays        = 0;
            sysSecOfDay    = 0;
            localOffsetMin = 0;
            localOffsetIsRule = false;
            lastTimeUpdate = 0;
            timeValid      = false;
        }

        // ---- Tide forecast (BSH) ----
        for (int i = 0; i < MAX_TIDE_FC; i++) tideFc[i] = TideExtreme{};
        tideFcCount = 0;
        memset(tideStation, 0, sizeof(tideStation));
        tideIsBsh    = false;
        lastTideFcMs = 0;

        // ---- Tide from the bus ----
        tideBusLevel  = NAN;
        tideBusRising = false;
        memset(tideBusStation, 0, sizeof(tideBusStation));
        lastTideBusUpdate = 0;

        // ---- Waypoint navigation ----
        navActive = false;
        navDtw = navBtw = navXte = navVmc = NAN;
        navWpNum = 0;
        lastNavUpdate = lastXteUpdate = 0;

        // ---- Autopilot ----
        // The tracker too, so the demo's AUTO is not carried over onto a bus
        // whose pilot computer has not said anything yet.
        apHeading = apTargetHeading = apRudder = NAN;
        lastApUpdate = 0;
        apMode       = ApDecode::MODE_UNKNOWN;
        apEngaged    = false;
        apModeSource = ApDecode::SRC_NONE;
        apPilotSa    = 0xFF;
        apStatus     = ApDecode::STATUS_UNKNOWN;
        apSetpoint   = NAN;
        apSetpointKind   = ApDecode::SP_NONE;
        lastApModeMs     = 0;
        lastApSetpointMs = 0;
        apTrack.reset();

        // ---- Media / audio ----
        clearMedia();

        // ---- AIS ----
        // The static cache too: nothing the demo invented may outlive it, and a
        // pending static report belongs to the source that is gone.
        resetAisSlots();
        aisCount = 0;
        lastAisRx = 0;
        aisStaticClear();

        // ---- Tanks / battery banks ----
        for (int i = 0; i < MAX_TANKS; i++) tanks[i] = TankInfo{};
        tankCount = 0;
        for (int i = 0; i < MAX_BATT; i++) batteries[i] = BatteryBank{};
        battCount = 0;

        clearHistories();
    }

    // The stereo's state, as before any 130820 was heard. Lock held.
    void clearMedia() {
        memset(mediaSourceName, 0, sizeof(mediaSourceName));
        mediaSourceCount = 0;
        mediaSource      = -1;
        memset(mediaTitle,  0, sizeof(mediaTitle));
        memset(mediaArtist, 0, sizeof(mediaArtist));
        memset(mediaAlbum,  0, sizeof(mediaAlbum));
        mediaElapsedMs = 0;
        mediaTotalMs   = 0;
        mediaPlayState = 0;
        memset(mediaZoneVol,  0, sizeof(mediaZoneVol));
        memset(mediaZoneMute, 0, sizeof(mediaZoneMute));
        mediaConnected   = false;
        lastMediaUpdate  = 0;
        lastMediaRx      = 0;
    }

    // Every history ring back to empty. Lock held.
    void clearHistories() {
        // sizeof(ring) is DELIBERATELY not used below: these members are
        // pointers, so sizeof() would be 4 and each memset would clear one
        // sample instead of the whole buffer — while still looking correct.
        if (windHistory)  memset(windHistory,  0, sizeof(WindSample) * WIND_HIST);
        // NaN, not zero: an all-zero trace would draw a curve pinned at 0 kn
        // instead of drawing nothing at all.
        if (windTrace) for (int i = 0; i < WIND_TRACE; i++)
            windTrace[i] = { NAN, NAN, NAN, NAN };
        if (depthHistory) memset(depthHistory, 0, sizeof(float)      * DEPTH_HIST);
        if (speedHistory) memset(speedHistory, 0, sizeof(float)      * SPEED_HIST);
        if (heaveHistVal) memset(heaveHistVal, 0, sizeof(float)      * HEAVE_HIST);
        if (heaveHistMs)  memset(heaveHistMs,  0, sizeof(uint32_t)   * HEAVE_HIST);
        if (pressHistVal) memset(pressHistVal, 0, sizeof(float)      * PRESS_HIST);
        windHistIdx  = 0; windHistFull  = false; windHistNextMs = 0;
        windTraceIdx = 0; windTraceFull = false; lastWindTraceMs = 0;
        depthHistIdx = 0; depthHistFull = false; depthHistSeq++;
        speedHistIdx = 0;
        heaveHistIdx = 0; heaveHistFull = false;
        pressHistIdx = 0; pressHistFull = false;
        lastPressPushMs = 0;
    }

    // Put every AIS slot back to its declared defaults. Does NOT touch
    // aisCount — the callers decide what that means.
    void resetAisSlots() {
        if (!aisTargets) return;
        for (int i = 0; i < MAX_AIS; i++) aisTargets[i] = AisTarget{};
    }

    // RAII guard – use: { auto lock = data.lock(); ... }
    struct Lock {
        SemaphoreHandle_t m;
        Lock(SemaphoreHandle_t m) : m(m) { xSemaphoreTake(m, portMAX_DELAY); }
        ~Lock() { xSemaphoreGive(m); }
    };
    Lock lock() { return Lock(mutex); }

    // ---- Helpers -----------------------------------------------------------
    // The null checks in the push helpers cover the window before
    // initBuffers() has run (and the theoretical case of an exhausted arena
    // AND an exhausted DRAM heap). Dropping a sample there is far better than
    // a null-pointer store: the ring simply stays empty.
    // Called by WindPipeline::tick() once per WIND_HIST_MS, and only with a
    // valid TWD and TWS - see the note at WIND_HIST.
    void pushWindSample(float twd_deg, float tws_kn, uint32_t nowMs) {
        if (!windHistory) return;
        windHistory[windHistIdx] = { twd_deg, tws_kn, nowMs };
        windHistIdx = (windHistIdx + 1) % WIND_HIST;
        if (windHistIdx == 0) windHistFull = true;
    }

    // Boat-relative wind trace, sampled on a CLOCK rather than on arrival.
    //
    // A ring filled on every wind PGN spans whatever time the sensor's rate
    // makes of it: six minutes at 1 Hz, ninety seconds at 4 Hz. Useless for a
    // curve that is meant to say "the last minute" on every boat. This one is
    // driven by the display tick and stores a point every WIND_TRACE_MS
    // regardless of how fast the bus talks (the wind history next to it now
    // works the same way, at 1 Hz).
    //
    // Stale data becomes NaN rather than a repeat of the last value: a wind
    // sensor that stopped should leave a GAP in the curve, not a straight line
    // pretending the wind held steady.
    // The ONLY gate is the age of the last wind sample. Do not add a test on
    // twa here: whether each quantity is known is already carried by the
    // value itself - twa is NaN while there is no boat speed to compute it
    // from, and gating the whole point on it blanked the apparent curve too,
    // on exactly the installation that needs it most.
    void pushWindTraceDue(uint32_t nowMs) {
        if (!windTrace) return;
        if (lastWindTraceMs && (nowMs - lastWindTraceMs) < WIND_TRACE_MS) return;
        lastWindTraceMs = nowMs;
        const bool fresh = lastWindUpdate &&
                           (nowMs - lastWindUpdate) < WIND_TRACE_STALE_MS;
        windTrace[windTraceIdx] = fresh
            ? WindTracePoint{ twa, tws, awa, aws }
            : WindTracePoint{ NAN, NAN, NAN, NAN };
        windTraceIdx = (windTraceIdx + 1) % WIND_TRACE;
        if (windTraceIdx == 0) windTraceFull = true;
    }

    void pushDepthSample(float d) {
        if (!depthHistory) return;
        depthHistory[depthHistIdx] = d;
        depthHistIdx = (depthHistIdx + 1) % DEPTH_HIST;
        if (depthHistIdx == 0) depthHistFull = true;
        depthHistSeq++;
    }

    // Feed one heave sample; updates waveHeight (peak-to-trough) + wavePeriod
    // (mean upward mean-crossing interval) over a ~15 s sliding window.
    // LOCK-FREE: callers (DemoData::tick, N2kHandler::onHeave) already hold the
    // data lock; this mutex is NON-recursive, so re-locking here would deadlock.
    void pushHeaveSample(float h, uint32_t nowMs) {
        if (isnan(h)) return;
        if (!heaveHistVal || !heaveHistMs) return;
        heaveHistVal[heaveHistIdx] = h;
        heaveHistMs [heaveHistIdx] = nowMs;
        heaveHistIdx = (heaveHistIdx + 1) % HEAVE_HIST;
        if (heaveHistIdx == 0) heaveHistFull = true;

        const uint32_t WIN = 15000;                       // ms sliding window
        int total = heaveHistFull ? HEAVE_HIST : heaveHistIdx;
        int start = heaveHistFull ? heaveHistIdx : 0;     // oldest sample first

        // Pass 1: min / max / mean over the in-window samples.
        float mn = 1e30f, mx = -1e30f, sum = 0; int cnt = 0;
        for (int k = 0; k < total; k++) {
            int i = (start + k) % HEAVE_HIST;
            if (nowMs - heaveHistMs[i] > WIN) continue;
            float v = heaveHistVal[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v; cnt++;
        }
        if (cnt < 4) return;                              // not enough data yet
        float mean = sum / (float)cnt;
        float p2t  = mx - mn;
        waveHeight = (p2t < 0.05f) ? 0.0f : p2t;

        // Pass 2: mean interval between upward mean-crossings -> period [s].
        bool  havePrev = false; float prevV = 0;
        bool  haveCross = false; uint32_t lastCross = 0;
        float sumIvl = 0; int ivlCnt = 0;
        for (int k = 0; k < total; k++) {
            int i = (start + k) % HEAVE_HIST;
            if (nowMs - heaveHistMs[i] > WIN) { havePrev = false; continue; }
            float v = heaveHistVal[i] - mean;
            if (havePrev && prevV < 0 && v >= 0) {        // rising mean-crossing
                uint32_t c = heaveHistMs[i];
                if (haveCross) { sumIvl += (float)(c - lastCross); ivlCnt++; }
                lastCross = c; haveCross = true;
            }
            prevV = v; havePrev = true;
        }
        wavePeriod = (ivlCnt >= 1) ? (sumIvl / (float)ivlCnt) / 1000.0f : NAN;
    }

    // Append a barometric sample for the trend ring, throttled to ~1/min so the
    // ring spans ~1.5 h. LOCK-FREE: callers already hold the data lock.
    void pushPressureSample(float hPa, uint32_t nowMs) {
        if (isnan(hPa)) return;
        if (!pressHistVal) return;
        bool first = (pressHistIdx == 0 && !pressHistFull);
        if (!first && (nowMs - lastPressPushMs) < 60000) return;
        lastPressPushMs = nowMs;
        pressHistVal[pressHistIdx] = hPa;
        pressHistIdx = (pressHistIdx + 1) % PRESS_HIST;
        if (pressHistIdx == 0) pressHistFull = true;
    }

    // Calculate CPA/TCPA for an AIS target against own ship.
    // Call after updating own nav data and a target's data.
    // Without own position or motion the CPA is not known: it used to keep the
    // last result, which kept colouring a target red long after the GPS died.
    void calcCpa(AisTarget &t) const {
        if (isnan(lat) || isnan(lon) || isnan(sog) || isnan(cog) ||
            isnan(t.lat) || isnan(t.lon) || isnan(t.sog) || isnan(t.cog)) {
            t.cpa = t.tcpa = NAN;
            return;
        }
        float avgLat = (lat + t.lat) / 2.0f * DEG_TO_RAD;
        float dx = lonDelta(t.lon, lon) * 60.0f * cosf(avgLat);  // nm
        float dy = (t.lat - lat) * 60.0f;                  // nm
        float c0 = cog * DEG_TO_RAD,  v0 = sog;
        float c1 = t.cog * DEG_TO_RAD, v1 = t.sog;
        float dvx = v1 * sinf(c1) - v0 * sinf(c0);
        float dvy = v1 * cosf(c1) - v0 * cosf(c0);
        float dv2 = dvx*dvx + dvy*dvy;
        if (dv2 < 1e-6f) { t.cpa = sqrtf(dx*dx + dy*dy); t.tcpa = 0; return; }
        float tcpa_h = -(dx*dvx + dy*dvy) / dv2;
        t.tcpa = tcpa_h * 60.0f;  // convert to minutes
        t.cpa  = sqrtf(powf(dx + dvx*tcpa_h, 2) + powf(dy + dvy*tcpa_h, 2));
    }

    // The targets that count as known: one heard only by its static data has no
    // position yet, is drawn nowhere and may be long gone (BusWatch keeps it).
    int aisWithPosition() const {
        if (!aisTargets) return 0;
        int n = 0;
        for (int i = 0; i < aisCount; i++)
            if (!isnan(aisTargets[i].lat) && !isnan(aisTargets[i].lon)) n++;
        return n;
    }

    // Remove stale AIS targets (not heard for > timeoutMs)
    void purgeAisTargets(uint32_t timeoutMs = 300000) {
        if (!aisTargets) { aisCount = 0; return; }
        uint32_t now = millis();
        int j = 0;
        for (int i = 0; i < aisCount; i++) {
            if ((now - aisTargets[i].lastSeen) < timeoutMs) {
                if (i != j) aisTargets[j] = aisTargets[i];
                j++;
            }
        }
        aisCount = j;
    }

    // Find or allocate an AIS target slot by MMSI.
    //
    // NEVER returns null, and must not start to. The AIS handlers in
    // N2kHandler.cpp (onAisClassA/B, onAisStaticA/B) write through the result
    // immediately — `t->lat = ...`, `strncpy(t->name, ...)` — without checking
    // it, which was always safe because aisTargets used to be a plain member
    // array. Now that the table is an arena allocation it can in theory be
    // missing (arena exhausted AND the DRAM fallback in allocBuf() failed), so
    // that case is served from a single scratch slot: the message is parsed
    // into a bit bucket and dropped. Dropping AIS traffic on a board that has
    // no memory left for an AIS table is the correct behaviour; returning null
    // here would only move the fault into four unguarded stores on the live-bus
    // path.
    //
    // With a full table a report may only push out a target that matters less than
    // itself: what can come close is what matters (aisRank2()). This used to replace
    // the one heard longest ago, whoever that was. With 640 vessels on the bus, up to
    // 480 nm away (live AIS, measured 05.11.2026), the table turned over about once
    // a second and the few vessels within the radar's range came and went.
    // The r arguments are this report's position and motion, horizonMin how far ahead
    // to look. Without a position (static data) a report pushes nothing out: it is
    // kept in the static cache (AisStatic) until the vessel gets a row.
    //
    // Every new row starts with what the cache knows of its vessel, and a row that
    // is pushed out leaves its static data there. A static report that finds no row
    // is written into the scratch slot, prefilled from the cache, and saved at the
    // top of the NEXT call - the caller fills it in only after this one returns.
    // All of it here rather than in the handlers, so the host test covers it.
    AisTarget* findOrCreateAis(uint32_t mmsi, float rLat = NAN, float rLon = NAN,
                               float rSog = NAN, float rCog = NAN, float horizonMin = 0.f) {
        // First, before anything below can reset the scratch slot or look for
        // that vessel's entry.
        if (aisScratchPending) { aisScratchPending = false; aisStaticSave(aisScratch, AIS_RANK_UNKNOWN); }
        if (!aisTargets) return aisDropped(mmsi);
        for (int i = 0; i < aisCount; i++)
            if (aisTargets[i].mmsi == mmsi) return &aisTargets[i];
        if (aisCount < MAX_AIS) return aisNewRow(aisCount++, mmsi, false, AIS_RANK_UNKNOWN);
        if (isnan(rLat) || isnan(rLon)) return aisStaticScratch(mmsi);

        int victim = -1;
        float victimRank = AIS_RANK_UNKNOWN;
        // No position of ours, not even an old one: nothing to rank by.
        const float oLat = !isnan(lat) ? lat : lastKnownLat;
        const float oLon = !isnan(lat) ? lon : lastKnownLon;
        if (isnan(oLat) || isnan(oLon)) {
            victim = 0;
            for (int i = 1; i < aisCount; i++)
                if (aisTargets[i].lastSeen < aisTargets[victim].lastSeen) victim = i;
        } else {
            AisTarget r;
            r.lat = rLat; r.lon = rLon; r.sog = rSog; r.cog = rCog;
            calcCpa(r);
            float worst = aisRank2(r, oLat, oLon, horizonMin);
            for (int i = 0; i < aisCount; i++) {
                const AisTarget &t = aisTargets[i];
                // One without a position is drawn nowhere and warns of nothing.
                if (isnan(t.lat) || isnan(t.lon)) { victim = i; break; }
                const float d = aisRank2(t, oLat, oLon, horizonMin);
                if (d > worst) { worst = d; victim = i; }
            }
            if (victim < 0) {
                // Nothing ranked below this report, so `worst` is still its own
                // rank: the vessel's cache entry, if it has one, learns it.
                aisStaticRank(mmsi, worst);
                return aisDropped(mmsi);
            }
            victimRank = aisRankOrUnknown(aisTargets[victim], oLat, oLon, horizonMin);
        }
        return aisNewRow(victim, mmsi, true, victimRank);
    }

    // A row about to be wiped (BusWatch's sweeps): its static data goes into the
    // cache, so the vessel has its name back the moment it is heard again. Ranked
    // by where it was, its CPA left out: a row swept as stale has not been heard
    // for minutes, and on a silent bus own position has gone already. A stale
    // row's rank is as old as the row (AIS_RANK_MAX_AGE_MS), so it counts as
    // unknown from the next save on - heard that long ago, it does not wait
    // (AIS_WAIT_MS); a row wiped by a bus dropout keeps its rank for the rest
    // of those 5 minutes. Lock held.
    void aisKeepStatic(const AisTarget &row) {
        const float oLat = !isnan(lat) ? lat : lastKnownLat;
        const float oLon = !isnan(lat) ? lon : lastKnownLon;
        aisStaticSave(row, aisRankOrUnknown(row, oLat, oLon, 0.f));
    }

private:
    // Squared distance in degrees of latitude: enough to rank by, and cheap.
    static float aisDist2(float aLat, float aLon, float oLat, float oLon) {
        const float dy = aLat - oLat;
        const float dx = lonDelta(aLon, oLon) * cosf(oLat * 0.017453292f);
        return dx * dx + dy * dy;
    }
    // How close a vessel gets, same unit: on a course that closes within horizonMin
    // its closest point of approach, else where it is now. A ship 3 nm off at 20 kn
    // and heading for us ranks before a moored one at 1 nm, a ship that will pass
    // 4 nm clear does not. The row's CPA is from its last report.
    static float aisRank2(const AisTarget &t, float oLat, float oLon, float horizonMin) {
        if (!isnan(t.cpa) && !isnan(t.tcpa) && t.tcpa > 0.f && t.tcpa < horizonMin) {
            const float c = t.cpa / 60.f;
            return c * c;
        }
        return aisDist2(t.lat, t.lon, oLat, oLon);
    }
    // aisRank2(), or AIS_RANK_UNKNOWN when there is nothing to rank by.
    static float aisRankOrUnknown(const AisTarget &t, float oLat, float oLon, float horizonMin) {
        if (isnan(t.lat) || isnan(t.lon) || isnan(oLat) || isnan(oLon)) return AIS_RANK_UNKNOWN;
        const float r = aisRank2(t, oLat, oLon, horizonMin);
        return r < AIS_RANK_UNKNOWN ? r : AIS_RANK_UNKNOWN;   // a NaN as well
    }
    // For a report with nowhere to go. aisCount does not cover it; only a static
    // report is read back out of it (aisStaticScratch()), everything else is lost.
    AisTarget* aisDropped(uint32_t mmsi) {
        aisScratch = AisTarget{};
        aisScratch.mmsi = mmsi;
        return &aisScratch;
    }

    // ---- AIS static cache (AisStatic). Lock held, like everything above. ----
    AisStatic* aisStaticFind(uint32_t mmsi) {
        if (!aisStatic || !mmsi) return nullptr;
        for (int i = 0; i < AIS_STATIC_N; i++) if (aisStatic[i].mmsi == mmsi) return &aisStatic[i];
        return nullptr;
    }
    // Keep t's static data, ranked `rank` (AIS_RANK_UNKNOWN keeps the rank its
    // entry has; a known one is from t's position, heard at t.lastSeen): in its
    // own entry, else a free one, else in place of the one that matters least
    // (aisStaticVictim()). An entry left without a rank - new, or its rank
    // aged out - notes t.lastSeen as when the vessel was heard: from a row
    // its last position, from a static report the report (aisStaticScratch()).
    // A vessel with nothing to show takes no entry.
    void aisStaticSave(const AisTarget &t, float rank) {
        if (!aisStatic || !t.mmsi || (!t.name[0] && !t.callsign[0] && !t.shipType)) return;
        aisStaticAge();   // first: an aged rank is no rank for the rule below either
        AisStatic *e = aisStaticFind(t.mmsi);
        if (!e) {
            e = aisStaticVictim();
            *e = AisStatic{};
            e->mmsi = t.mmsi;
        }
        if (rank < AIS_RANK_UNKNOWN) { e->rank = rank; e->rankAt = t.lastSeen; }
        else if (!(e->rank < AIS_RANK_UNKNOWN)) e->rankAt = t.lastSeen;
        e->seq = ++aisStaticSeq;
        memcpy(e->name, t.name, sizeof(e->name));
        memcpy(e->callsign, t.callsign, sizeof(e->callsign));
        e->shipType = t.shipType;
    }
    // A position report of a vessel without a row was dropped: its entry, if it
    // has one, learns where the vessel stands - now.
    void aisStaticRank(uint32_t mmsi, float rank) {
        AisStatic *e = aisStaticFind(mmsi);
        if (!e) return;
        e->rank = rank < AIS_RANK_UNKNOWN ? rank : AIS_RANK_UNKNOWN;
        e->rankAt = millis();
        e->seq = ++aisStaticSeq;
    }
    // Every rank as old as AIS_RANK_MAX_AGE_MS becomes unknown (see there).
    // Signed age, as in BusWatch::fresh(): a stamp a moment ahead of the clock
    // is new, not 49 days old. Vessels still on the air renew theirs with every
    // dropped position report (aisStaticRank()), so only silent ones age.
    // An entry without a rank that was heard AIS_WAIT_MS ago or longer - an
    // aged one among them, in the same pass - forgets when: rankAt 0, which
    // never waits. Read signed, the old stamp would turn "new" again once the
    // clock has run on for 24.8 days.
    void aisStaticAge() {
        const uint32_t now = millis();
        for (int i = 0; i < AIS_STATIC_N; i++) {
            AisStatic &c = aisStatic[i];
            const int32_t age = (int32_t)(now - c.rankAt);
            if (c.rank < AIS_RANK_UNKNOWN && age >= (int32_t)AIS_RANK_MAX_AGE_MS) c.rank = AIS_RANK_UNKNOWN;
            if (!(c.rank < AIS_RANK_UNKNOWN) && age >= (int32_t)AIS_WAIT_MS) c.rankAt = 0;
        }
    }
    // An entry without a rank, heard within AIS_WAIT_MS: it waits for its
    // vessel's next position report (see AIS_WAIT_MS). Signed, as above. A
    // stamp of 0 is no time - a row heard by its static data only (lastSeen 0,
    // as in BusWatch), or one heard too long ago (aisStaticAge()) - and never
    // waits, however far the clock has run. Run aisStaticAge() first, or an
    // aged rank still reads as a rank.
    static bool aisStaticWaits(const AisStatic &c, uint32_t now) {
        return c.mmsi && c.rankAt && !(c.rank < AIS_RANK_UNKNOWN) &&
               (int32_t)(now - c.rankAt) < (int32_t)AIS_WAIT_MS;
    }
    // The entry a new vessel takes: a free one, else the one that matters
    // least - the worst rank, of equals the oldest. An unknown rank counts as
    // the worst, but while fewer than half the entries wait, a waiting one is
    // never taken (it ranks below everything); once half or more wait, they
    // count as unknown like the rest, the oldest first. With the shield up, a
    // full cache always holds one that does not wait. Lock held, ages current
    // (aisStaticSave()).
    AisStatic* aisStaticVictim() {
        const uint32_t now = millis();
        int waiting = 0;
        for (int i = 0; i < AIS_STATIC_N; i++) if (aisStaticWaits(aisStatic[i], now)) waiting++;
        const bool shield = waiting < AIS_STATIC_N / 2;
        auto key = [shield, now](const AisStatic &c) {
            return (shield && aisStaticWaits(c, now)) ? -1.f : c.rank;   // every rank is >= 0
        };
        AisStatic *e = &aisStatic[0];
        float ek = key(*e);
        for (int i = 1; i < AIS_STATIC_N && e->mmsi; i++) {
            const AisStatic &c = aisStatic[i];
            const float ck = key(c);
            if (!c.mmsi || ck > ek || (ck == ek && c.seq < e->seq)) { e = &aisStatic[i]; ek = ck; }
        }
        return e;
    }
    static void aisStaticToRow(const AisStatic &e, AisTarget &r) {
        memcpy(r.name, e.name, sizeof(r.name));
        memcpy(r.callsign, e.callsign, sizeof(r.callsign));
        r.shipType = e.shipType;
    }
    // Row i, blank, for a vessel, with what the cache knows of it. With `evict`
    // the row's old occupant leaves its static data in the cache, ranked
    // victimRank. The newcomer's entry is taken - and freed - FIRST: the old
    // occupant may then go into exactly that slot, and saving it can never push
    // out the entry that is about to be used.
    AisTarget* aisNewRow(int i, uint32_t mmsi, bool evict, float victimRank) {
        AisStatic known;
        AisStatic *e = aisStaticFind(mmsi);
        const bool had = e != nullptr;
        if (had) { known = *e; *e = AisStatic{}; }
        if (evict) aisStaticSave(aisTargets[i], victimRank);
        aisTargets[i] = AisTarget{};
        aisTargets[i].mmsi = mmsi;
        if (had) aisStaticToRow(known, aisTargets[i]);
        return &aisTargets[i];
    }
    // A static report with no row to go to: the scratch slot, prefilled from the
    // cache - one class B half (129809 name, 129810 callsign and type) must not
    // blank the other - and saved by the next findOrCreateAis(). Its lastSeen
    // is when the report came (the handlers set none for static data): what an
    // entry without a rank notes as heard (aisStaticSave()).
    AisTarget* aisStaticScratch(uint32_t mmsi) {
        AisTarget *s = aisDropped(mmsi);
        s->lastSeen = millis();
        const AisStatic *e = aisStaticFind(mmsi);
        if (e) aisStaticToRow(*e, *s);
        aisScratchPending = aisStatic != nullptr;
        return s;
    }
    // Every entry free, nothing pending. Value-initialised, not memset: the
    // cache is a pointer (see the note at the top) and AisStatic has declared
    // defaults.
    void aisStaticClear() {
        if (aisStatic) for (int i = 0; i < AIS_STATIC_N; i++) aisStatic[i] = AisStatic{};
        aisStaticSeq = 0;
        aisScratchPending = false;
    }

    // AIS_STATIC_N entries in the PSRAM arena (initBuffers()); null when it had
    // no room, and then names come with the next static report, as they used to.
    static constexpr int AIS_STATIC_N = 64;   // 3,072 bytes
    AisStatic *aisStatic = nullptr;
    uint32_t   aisStaticSeq = 0;
    bool       aisScratchPending = false;     // aisScratch holds a static report to keep

    bool buffersInited = false;   // initBuffers() has run (see there)

    // Bit bucket for findOrCreateAis() when a report has no slot to go to.
    // ~76 bytes of internal RAM against the 3,800 the table itself no longer
    // costs — the price of keeping that function's "never null" contract.
    AisTarget aisScratch;

    // One arena allocation for initBuffers(), with a DRAM fallback. A null
    // here would turn every later write into a null-pointer store, so an
    // exhausted arena must not be accepted silently: fall back to the internal
    // heap, which is exactly where these buffers used to live. The arena hands
    // back zeroed memory, malloc() does not — so zero it ourselves.
    //
    // If both fail, say so loudly. Not every consumer of these buffers checks
    // for null — DepthScreen and WindPlotScreen memcpy their whole ring
    // unconditionally — so a silent null resurfaces much later as an
    // unexplained crash in a render task, where the backtrace no longer says
    // WHICH buffer was missing. `what` is the one piece of information that
    // cannot be recovered afterwards.
    //
    // It deliberately does not halt or restart: on a healthy board this path is
    // unreachable (initBuffers() runs immediately after PsramArena::init(),
    // with the whole arena still free), and the 4-inch board is a released
    // product that must not gain a new way to boot-loop.
    static void *allocBuf(size_t bytes, const char *what) {
        void *p = PsramArena::alloc(bytes);
        if (!p) {
            p = malloc(bytes);
            if (p) memset(p, 0, bytes);
        }
        if (!p) {
            Serial.printf("[DataModel] FATAL: %s (%u bytes) not allocated - "
                          "PSRAM arena exhausted AND internal heap full\n",
                          what, (unsigned)bytes);
            Serial.flush();
        }
        return p;
    }
};

extern DataModel data;

// Resolve a config field key ("sog","depth","oil",…) to its live value.
// Takes the data lock internally. Shared by GridScreen + EngineScreen.
float dmFieldByKey(const char *key);
