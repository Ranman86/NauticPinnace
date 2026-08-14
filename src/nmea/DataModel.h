#pragma once
#include <Arduino.h>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================
// DataModel – central store for all live NMEA 2000 data.
// Access always through the RAII lock guard.
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

    // ---- Fusion audio (NMEA 2000 entertainment, e.g. Fusion RA-670) --------
    static constexpr int FUSION_MAX_SOURCES = 8;
    static constexpr int FUSION_NUM_ZONES   = 3;     // Zone 1..3 (+ derived Master)
    char     fusionSourceName[FUSION_MAX_SOURCES][16] = {};  // source/input labels
    uint8_t  fusionSourceCount = 0;
    int8_t   fusionSource      = -1;       // index of current source (-1 = unknown)
    char     fusionTitle[48]   = {};       // track title / station name
    char     fusionArtist[40]  = {};
    char     fusionAlbum[40]   = {};
    uint32_t fusionElapsedMs   = 0;        // track elapsed time
    uint32_t fusionTotalMs     = 0;        // track length (0 = unknown / live)
    uint8_t  fusionPlayState   = 0;        // 0=stopped 1=playing 2=paused
    uint8_t  fusionZoneVol[FUSION_NUM_ZONES]  = {0,0,0};        // 0..100 per zone
    bool     fusionZoneMute[FUSION_NUM_ZONES] = {false,false,false};  // per-zone mute
    bool     fusionConnected   = false;    // have we heard from a radio on the bus?
    uint32_t lastFusionUpdate  = 0;

    // Master volume = loudest zone (the proportional reference level).
    uint8_t fusionMasterVol() const {
        uint8_t m = 0;
        for (int i = 0; i < FUSION_NUM_ZONES; i++) if (fusionZoneVol[i] > m) m = fusionZoneVol[i];
        return m;
    }
    // ---- AIS ---------------------------------------------------------------
    static constexpr int MAX_AIS = 50;
    AisTarget aisTargets[MAX_AIS];
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

    WindSample windHistory[WIND_HIST];
    int        windHistIdx  = 0;
    bool       windHistFull = false;

    float depthHistory[DEPTH_HIST];
    int   depthHistIdx  = 0;
    bool  depthHistFull = false;

    float speedHistory[SPEED_HIST];
    int   speedHistIdx  = 0;

    // Heave ring for the wave estimator (~16 s @ 5 Hz). Stores value + timestamp.
    static constexpr int HEAVE_HIST = 80;
    float    heaveHistVal[HEAVE_HIST];
    uint32_t heaveHistMs [HEAVE_HIST];
    int      heaveHistIdx  = 0;
    bool     heaveHistFull = false;

    // Barometric pressure trend ring (~1.5 h @ one sample/min on real data).
    static constexpr int PRESS_HIST = 90;
    float    pressHistVal[PRESS_HIST];
    int      pressHistIdx  = 0;
    bool     pressHistFull = false;
    uint32_t lastPressPushMs = 0;

    // ---- Mutex (acquire before any read/write) -----------------------------
    SemaphoreHandle_t mutex;

    DataModel() {
        mutex = xSemaphoreCreateMutex();
        memset(depthHistory, 0, sizeof(depthHistory));
        memset(speedHistory, 0, sizeof(speedHistory));
        memset(heaveHistVal, 0, sizeof(heaveHistVal));
        memset(heaveHistMs,  0, sizeof(heaveHistMs));
        memset(pressHistVal, 0, sizeof(pressHistVal));
    }

    // Reset every value to "no data" (NaN / zero) while KEEPING the mutex.
    // Call with the lock held. A plain `data = DataModel()` must never be used
    // for this: it would overwrite `mutex` with the temporary's fresh handle
    // while a Lock still holds the old one — the guard would then give back a
    // semaphore nobody waits on and every later lock would use a different
    // object. Preserving the handle here keeps that impossible.
    void clearValues() {
        SemaphoreHandle_t keep = mutex;
        SemaphoreHandle_t fresh;
        {
            DataModel blank;          // all members at their declared defaults
            fresh = blank.mutex;      // ... including a semaphore we do not want
            blank.mutex = keep;       // don't let the copy clobber the live one
            *this = blank;
        }
        mutex = keep;
        if (fresh) vSemaphoreDelete(fresh);   // no leak per switch
    }

    // RAII guard – use: { auto lock = data.lock(); ... }
    struct Lock {
        SemaphoreHandle_t m;
        Lock(SemaphoreHandle_t m) : m(m) { xSemaphoreTake(m, portMAX_DELAY); }
        ~Lock() { xSemaphoreGive(m); }
    };
    Lock lock() { return Lock(mutex); }

    // ---- Helpers -----------------------------------------------------------
    void pushWindSample(float twd_deg, float tws_kn) {
        windHistory[windHistIdx] = { twd_deg, tws_kn, (uint32_t)millis() };
        windHistIdx = (windHistIdx + 1) % WIND_HIST;
        if (windHistIdx == 0) windHistFull = true;
    }

    void pushDepthSample(float d) {
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

    // Find or allocate an AIS target slot by MMSI
    AisTarget* findOrCreateAis(uint32_t mmsi) {
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
};

extern DataModel data;

// Resolve a config field key ("sog","depth","oil",…) to its live value.
// Takes the data lock internally. Shared by GridScreen + EngineScreen.
float dmFieldByKey(const char *key);
