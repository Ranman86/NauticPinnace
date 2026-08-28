#pragma once
#include <Arduino.h>
#include <cmath>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../PsramArena.h"

// ============================================================
// DataModel – central store for all live NMEA 2000 data.
// Access always through the RAII lock guard.
//
// The bulk buffers (AIS targets and the history rings) are NOT members any
// more, only pointers into the PSRAM arena – see initBuffers(). As plain
// arrays they were ~10.5 KB of .bss in a global object, on boards where the
// free internal RAM is measured in tens of kilobytes and WiFi/HTTP is the
// first thing to suffer. None of that data is touched from an ISR or used as
// a DMA target, so PSRAM is a safe home for it.
//
// Consequence to keep in mind when editing this file: sizeof() on those
// members is now the size of a POINTER. Anything that clears or copies them
// must spell out count * element size.
// ============================================================

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
    uint32_t lastUpdate  = 0;
};

class DataModel {
public:
    // ---- Navigation --------------------------------------------------------
    float lat = NAN, lon = NAN;
    float sog = NAN;        // speed over ground [kn]
    float cog = NAN;        // course over ground [deg true]
    float hdg = NAN;        // magnetic heading [deg]
    float variation = NAN;  // magnetic variation [deg, E positive]
    float stw = NAN;        // speed through water [kn]
    uint32_t lastGpsUpdate = 0;

    // ---- Wind --------------------------------------------------------------
    float awa = NAN;        // apparent wind angle [deg, -180..+180, port negative]
    float aws = NAN;        // apparent wind speed [kn]
    float twa = NAN;        // true wind angle [deg, -180..+180]
    float tws = NAN;        // true wind speed [kn]
    float twd = NAN;        // true wind direction [deg true]
    uint32_t lastWindUpdate = 0;

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
    uint32_t lastEngineUpdate = 0;

    // ---- Electrical --------------------------------------------------------
    float batteryVoltage = NAN;   // V (house bank)
    float batteryCurrent = NAN;   // A

    // ---- Rudder ------------------------------------------------------------
    float rudderAngle = NAN;      // deg, positive = starboard
    uint32_t lastRudderUpdate = 0;

    // ---- Attitude / Motion (Precision-9: PGN 127257 / 127251 / 127252) -----
    float roll       = NAN;       // deg, + = starboard side down (heel)
    float pitch      = NAN;       // deg, + = bow up
    float yaw        = NAN;       // deg true (heading from the attitude sensor)
    float rateOfTurn = NAN;       // deg/min, + = turning to starboard
    float heave      = NAN;       // m, + = up (vertical wave-induced motion)
    uint32_t lastAttitudeUpdate = 0;
    uint32_t lastHeaveUpdate    = 0;
    // Derived wave estimate from the heave signal (no direction):
    float waveHeight = NAN;       // m, peak-to-trough over a ~15 s window
    float wavePeriod = NAN;       // s, mean heave oscillation period

    // ---- Environment (PGN 130310 / 130311 / 130314) ------------------------
    float airTemp   = NAN;        // outside air temperature [°C]
    float waterTemp = NAN;        // sea water temperature [°C]
    float humidity  = NAN;        // relative humidity [%]
    float pressure  = NAN;        // barometric pressure [hPa]
    uint32_t lastEnvUpdate = 0;

    // ---- Distance log (PGN 128275) -----------------------------------------
    float logDistance  = NAN;     // total distance through the water [nm]
    float tripDistance = NAN;     // trip distance through the water [nm]
    uint32_t lastLogUpdate = 0;

    // ---- Time / date (PGN 126992 system time + 129033 local offset) --------
    uint16_t sysDays        = 0;   // days since 1970-01-01 (UTC)
    double   sysSecOfDay    = 0;   // seconds since midnight UTC
    int16_t  localOffsetMin = 0;   // local time = UTC + this [minutes]
    uint32_t lastTimeUpdate = 0;   // millis() of the last time fix
    bool     timeValid      = false;
    bool     timeIsReal     = false; // set once a real clock (GPS/SNTP) is known;
                                     // demo time stops overriding when true

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
    uint32_t lastNavUpdate = 0;

    // ---- Autopilot ---------------------------------------------------------
    float   apHeading       = NAN;  // current heading [deg]
    float   apTargetHeading = NAN;  // commanded heading [deg]
    float   apRudder        = NAN;  // commanded rudder [deg]
    uint8_t apMode          = 0;    // 0=standby, 1=heading, 2=wind, 3=track
    bool    apEngaged       = false;
    uint32_t lastApUpdate   = 0;

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
    uint32_t lastMediaUpdate  = 0;

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
    static constexpr int WIND_HIST   = 360;
    static constexpr int DEPTH_HIST  = 300;
    static constexpr int SPEED_HIST  = 120;

    // The rings live in the PSRAM arena, only the indices stay here.
    WindSample *windHistory = nullptr;   // WIND_HIST entries
    int        windHistIdx  = 0;
    bool       windHistFull = false;

    float *depthHistory = nullptr;       // DEPTH_HIST entries
    int   depthHistIdx  = 0;
    bool  depthHistFull = false;

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
        depthHistory = (float      *)allocBuf(sizeof(float)      * DEPTH_HIST, "depthHistory");
        speedHistory = (float      *)allocBuf(sizeof(float)      * SPEED_HIST, "speedHistory");
        heaveHistVal = (float      *)allocBuf(sizeof(float)      * HEAVE_HIST, "heaveHistVal");
        heaveHistMs  = (uint32_t   *)allocBuf(sizeof(uint32_t)   * HEAVE_HIST, "heaveHistMs");
        pressHistVal = (float      *)allocBuf(sizeof(float)      * PRESS_HIST, "pressHistVal");
        // Arena memory arrives zero-filled, which is the right starting state
        // for every numeric ring (that is what the old constructor memset did).
        // It is NOT right for an AIS slot: its declared defaults are NaN
        // position and navStatus 15 (undefined), while all-zero would read as a
        // real ship sitting at 0°N 0°E.
        resetAisSlots();
    }

    // Reset every value to "no data" (NaN / zero) while KEEPING the mutex and
    // the arena buffers. Call with the lock held.
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
        lastGpsUpdate = 0;

        // ---- Wind ----
        awa = aws = twa = tws = twd = NAN;
        lastWindUpdate = 0;

        // ---- Depth ----
        depth = NAN;
        depthOffset = 0;
        lastDepthUpdate = 0;

        // ---- Engine ----
        rpm = oilPressure = coolantTemp = engineHours = fuelFlow = NAN;
        engineInstance = 0;
        lastEngineUpdate = 0;

        // ---- Electrical ----
        batteryVoltage = batteryCurrent = NAN;

        // ---- Rudder ----
        rudderAngle = NAN;
        lastRudderUpdate = 0;

        // ---- Attitude / motion ----
        roll = pitch = yaw = rateOfTurn = heave = NAN;
        lastAttitudeUpdate = 0;
        lastHeaveUpdate    = 0;
        waveHeight = wavePeriod = NAN;

        // ---- Environment ----
        airTemp = waterTemp = humidity = pressure = NAN;
        lastEnvUpdate = 0;

        // ---- Distance log ----
        logDistance = tripDistance = NAN;
        lastLogUpdate = 0;

        // ---- Time / date ----
        sysDays        = 0;
        sysSecOfDay    = 0;
        localOffsetMin = 0;
        lastTimeUpdate = 0;
        timeValid      = false;
        timeIsReal     = false;

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
        lastNavUpdate = 0;

        // ---- Autopilot ----
        apHeading = apTargetHeading = apRudder = NAN;
        apMode    = 0;
        apEngaged = false;
        lastApUpdate = 0;

        // ---- Media / audio ----
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

        // ---- AIS ----
        resetAisSlots();
        aisCount = 0;

        // ---- Tanks / battery banks ----
        for (int i = 0; i < MAX_TANKS; i++) tanks[i] = TankInfo{};
        tankCount = 0;
        for (int i = 0; i < MAX_BATT; i++) batteries[i] = BatteryBank{};
        battCount = 0;

        // ---- History rings ----
        // sizeof(ring) is DELIBERATELY not used below: these members are
        // pointers, so sizeof() would be 4 and each memset would clear one
        // sample instead of the whole buffer — while still looking correct.
        if (windHistory)  memset(windHistory,  0, sizeof(WindSample) * WIND_HIST);
        if (depthHistory) memset(depthHistory, 0, sizeof(float)      * DEPTH_HIST);
        if (speedHistory) memset(speedHistory, 0, sizeof(float)      * SPEED_HIST);
        if (heaveHistVal) memset(heaveHistVal, 0, sizeof(float)      * HEAVE_HIST);
        if (heaveHistMs)  memset(heaveHistMs,  0, sizeof(uint32_t)   * HEAVE_HIST);
        if (pressHistVal) memset(pressHistVal, 0, sizeof(float)      * PRESS_HIST);
        windHistIdx  = 0; windHistFull  = false;
        depthHistIdx = 0; depthHistFull = false;
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
    void pushWindSample(float twd_deg, float tws_kn) {
        if (!windHistory) return;
        windHistory[windHistIdx] = { twd_deg, tws_kn, (uint32_t)millis() };
        windHistIdx = (windHistIdx + 1) % WIND_HIST;
        if (windHistIdx == 0) windHistFull = true;
    }

    void pushDepthSample(float d) {
        if (!depthHistory) return;
        depthHistory[depthHistIdx] = d;
        depthHistIdx = (depthHistIdx + 1) % DEPTH_HIST;
        if (depthHistIdx == 0) depthHistFull = true;
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
    void calcCpa(AisTarget &t) const {
        if (isnan(lat) || isnan(lon) || isnan(sog) || isnan(cog)) return;
        if (isnan(t.lat) || isnan(t.lon) || isnan(t.sog) || isnan(t.cog)) return;
        float avgLat = (lat + t.lat) / 2.0f * DEG_TO_RAD;
        float dx = (t.lon - lon) * 60.0f * cosf(avgLat);  // nm
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
    AisTarget* findOrCreateAis(uint32_t mmsi) {
        if (!aisTargets) {
            // aisCount stays 0, so nothing ever reads this slot back out.
            aisScratch = AisTarget{};
            aisScratch.mmsi = mmsi;
            return &aisScratch;
        }
        for (int i = 0; i < aisCount; i++)
            if (aisTargets[i].mmsi == mmsi) return &aisTargets[i];
        if (aisCount < MAX_AIS) {
            aisTargets[aisCount] = AisTarget{};
            aisTargets[aisCount].mmsi = mmsi;
            return &aisTargets[aisCount++];
        }
        // Replace oldest
        int oldest = 0;
        for (int i = 1; i < aisCount; i++)
            if (aisTargets[i].lastSeen < aisTargets[oldest].lastSeen) oldest = i;
        aisTargets[oldest] = AisTarget{};
        aisTargets[oldest].mmsi = mmsi;
        return &aisTargets[oldest];
    }

private:
    bool buffersInited = false;   // initBuffers() has run (see there)

    // Bit bucket for findOrCreateAis() when there is no AIS table at all.
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
