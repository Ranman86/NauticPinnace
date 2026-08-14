// DemoData.cpp – synthetic NMEA data for demo / presentation mode.
// All values are driven by millis() so the instrument animates automatically.

#include "DemoData.h"
#include "DataModel.h"
#include "FusionN2k.h"
#include "../SunCalc.h"
#include <math.h>
#include <string.h>

DemoDataSource demoData;

// Resolve a config field key to its live DataModel value (locks internally).
float dmFieldByKey(const char *key) {
    auto lk = data.lock();
    if (!strcmp(key,"sog"))       return data.sog;
    if (!strcmp(key,"cog"))       return data.cog;
    if (!strcmp(key,"hdg"))       return data.hdg;
    if (!strcmp(key,"stw"))       return data.stw;
    if (!strcmp(key,"awa"))       return data.awa;
    if (!strcmp(key,"aws"))       return data.aws;
    if (!strcmp(key,"twa"))       return data.twa;
    if (!strcmp(key,"tws"))       return data.tws;
    if (!strcmp(key,"twd"))       return data.twd;
    if (!strcmp(key,"depth"))     return data.depth;
    if (!strcmp(key,"rpm"))       return data.rpm;
    if (!strcmp(key,"oil"))       return data.oilPressure;
    if (!strcmp(key,"coolant"))   return data.coolantTemp;
    if (!strcmp(key,"hours"))     return data.engineHours;
    if (!strcmp(key,"fuel"))      return data.fuelFlow;
    if (!strcmp(key,"rudder"))    return data.rudderAngle;
    if (!strcmp(key,"roll"))      return data.roll;
    if (!strcmp(key,"pitch"))     return data.pitch;
    if (!strcmp(key,"yaw"))       return data.yaw;
    if (!strcmp(key,"rot"))       return data.rateOfTurn;
    if (!strcmp(key,"heave"))     return data.heave;
    if (!strcmp(key,"waveht"))    return data.waveHeight;
    if (!strcmp(key,"waveper"))   return data.wavePeriod;
    if (!strcmp(key,"battv"))     return data.batteryVoltage;
    if (!strcmp(key,"lat"))       return data.lat;
    if (!strcmp(key,"lon"))       return data.lon;
    if (!strcmp(key,"aptarget"))  return data.apTargetHeading;
    if (!strcmp(key,"variation")) return data.variation;      // PGN 127258
    if (!strcmp(key,"log"))       return data.logDistance;    // PGN 128275
    if (!strcmp(key,"trip"))      return data.tripDistance;   // PGN 128275
    return NAN;
}

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr float PI_F  = (float)M_PI;
static constexpr float D2R   = PI_F / 180.f;
static constexpr float R2D   = 180.f / PI_F;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Clamp to [lo, hi]
static inline float clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Smooth step between TWA zones via polar curve approximation.
// Returns STW in knots given absTwa (0–180) and TWS.
static float polarStw(float absTwa, float tws) {
    // Map to 0..1 efficiency (peak near 90–100°, minimum at extremes)
    float a = absTwa;
    float eff;
    if      (a <  28.f) eff = 0.05f;                           // No-Go
    else if (a <  50.f) eff = 0.05f + (a-28.f)/22.f * 0.50f; // 5→55%
    else if (a < 100.f) eff = 0.55f + (a-50.f)/50.f * 0.20f; // 55→75%
    else if (a < 150.f) eff = 0.75f - (a-100.f)/50.f * 0.20f; // 75→55%
    else                eff = 0.55f - (a-150.f)/30.f * 0.25f; // 55→30%
    eff = clamp(eff, 0.04f, 0.80f);
    // STW max ≈ hull speed * efficiency; cap at 1.05× hull speed
    float hullSpeed = 2.43f * sqrtf(9.0f); // default LWL 9 m → ~7.3 kn
    return clamp(eff * hullSpeed, 0.2f, hullSpeed * 1.05f);
}

// ── Main tick ─────────────────────────────────────────────────────────────────

void DemoDataSource::tick() {
    // Rate-limit to ~5 Hz – called from tight N2K task loop
    static uint32_t lastMs = 0;
    uint32_t now = millis();
    if (now - lastMs < 200) return;
    lastMs = now;

    float t = now / 1000.f;     // seconds since boot

    // ── Scenario clock (480 s = 8-min full cycle) ─────────────────────────
    float tCycle = fmodf(t, 480.f);
    float phase  = (tCycle < 240.f) ? tCycle : tCycle - 240.f;  // 0..240 within half
    float sign   = (tCycle < 240.f) ? 1.f : -1.f;               // Stb or Port tack

    // absTwa sweeps 20° → 165° → 20° with a sine curve (half period = 240 s)
    float absTwa = 20.f + 145.f * sinf(phase / 240.f * PI_F);
    absTwa = clamp(absTwa, 5.f, 175.f);
    float twa    = sign * absTwa;

    // ── Wind speed (slowly varying 8–18 kn, period 150 s) ────────────────
    float tws = 13.f + 5.f * sinf(t * 2.f * PI_F / 150.f);
    tws = clamp(tws, 8.f, 18.f);

    // ── Boat speed (polar model) ──────────────────────────────────────────
    float stw = polarStw(absTwa, tws);
    // Add gentle oscillation (~turbulence / waves)
    stw += 0.4f * sinf(t * 2.f * PI_F / 17.f);
    stw  = clamp(stw, 0.1f, 9.f);

    // ── Apparent wind (vector calculation) ───────────────────────────────
    // In boat frame: x = Stb, y = Fwd
    // True wind velocity: (TWS*sin(TWA), -TWS*cos(TWA))
    // Apparent: subtract boat velocity (0, STW)
    float twaRad = twa * D2R;
    float awx    = tws * sinf(twaRad);
    float awy    = -tws * cosf(twaRad) - stw;   // fwd component + boat speed headwind
    float aws    = sqrtf(awx * awx + awy * awy);
    float awa    = atan2f(awx, -awy) * R2D;      // positive = Stb, negative = Port

    // ── Heading (slow oscillation ±8° around 215°) ───────────────────────
    float hdg = 215.f + 8.f * sinf(t * 2.f * PI_F / 45.f);
    hdg = fmodf(hdg + 360.f, 360.f);

    // ── True wind direction (compass) ────────────────────────────────────
    float twd = fmodf(hdg + twa + 360.f, 360.f);

    // ── SOG / COG (≈ STW/HDG, small leeway) ──────────────────────────────
    float leeway = (absTwa < 50.f) ? 3.f * sinf(t / 11.f) : 0.f;
    float cog    = fmodf(hdg + leeway + 360.f, 360.f);
    float sog    = stw * 0.97f + 0.1f * sinf(t / 7.f);

    // ── Depth (6–14 m, period 200 s) ─────────────────────────────────────
    float depth = 10.f + 4.f * sinf(t * 2.f * PI_F / 200.f);

    // ── Rudder (±3°, period ~8 s) ─────────────────────────────────────────
    float rudder = 3.0f * sinf(t * 2.f * PI_F / 8.f);

    // ── Battery voltage (12.3–12.8 V, very slow drift) ───────────────────
    float battV = 12.55f + 0.25f * sinf(t * 2.f * PI_F / 600.f);

    // ── Attitude / motion (demo swell) ───────────────────────────────────
    // Heave: dominant ~6 s swell + a shorter chop component (peak ≈ ±0.75 m).
    const float Tswell = 6.0f;
    float heave  = 0.6f * sinf(t * 2.f * PI_F / Tswell)
                 + 0.15f * sinf(t * 2.f * PI_F / 2.7f);
    // Steady heel to leeward (grows with wind, peaks near a beam reach) plus a
    // wave-induced roll oscillation. roll>0 = starboard down; on starboard tack
    // (sign>0, wind from starboard) the boat heels to port -> negative.
    float steadyHeel = -sign * clamp(tws * 0.9f * sinf(absTwa * D2R), 0.f, 22.f);
    float roll  = clamp(steadyHeel + 6.0f * sinf(t * 2.f * PI_F / Tswell + 0.5f),
                        -32.f, 32.f);
    float pitch = 4.0f * sinf(t * 2.f * PI_F / Tswell + 1.2f) + 1.0f * sinf(t / 3.1f);
    // Rate of turn = d/dt of the ±8°/45 s heading wander, expressed in deg/min.
    float rotDegMin = 8.0f * (2.f * PI_F / 45.f) * cosf(t * 2.f * PI_F / 45.f) * 60.f;

    // ── AIS: one synthetic target circling the boat ───────────────────────
    // Target at ~0.5 nm, bearing rotates slowly
    const float OWN_LAT = 53.500f, OWN_LON = 10.000f;
    float aisBrg = fmodf(t / 3.f, 360.f);   // full circle every ~18 min
    float aisLat = OWN_LAT + 0.5f / 60.f * cosf(aisBrg * D2R);
    float aisLon = OWN_LON + 0.5f / 60.f / cosf(OWN_LAT * D2R) * sinf(aisBrg * D2R);

    // ── Push all values into DataModel ────────────────────────────────────
    {
        auto lk = data.lock();

        data.twa  = twa;
        data.tws  = tws;
        data.awa  = awa;
        data.aws  = aws;
        data.twd  = twd;
        data.stw  = stw;
        data.sog  = sog;
        data.cog  = cog;
        data.hdg  = hdg;
        data.depth      = depth;
        data.rudderAngle = rudder;
        data.batteryVoltage = battV;
        // Gentle anchor-swing wander (~±18 m, elliptical) so the Ankerwache shows
        // a realistic swing in demo mode; also keeps the GPS fix "fresh".
        {
            float wN = 18.f * sinf(t * 2.f * PI_F / 70.f);
            float wE = 18.f * sinf(t * 2.f * PI_F / 95.f + 1.f);
            data.lat = OWN_LAT + wN / (60.f * 1852.f);
            data.lon = OWN_LON + wE / (60.f * 1852.f * cosf(OWN_LAT * D2R));
            data.lastGpsUpdate = now;
        }

        // Attitude / motion + wave estimate (lock already held → push directly).
        data.roll       = roll;
        data.pitch      = pitch;
        data.yaw        = hdg;
        data.rateOfTurn = rotDegMin;
        data.heave      = heave;
        data.lastAttitudeUpdate = now;
        data.lastHeaveUpdate    = now;
        data.pushHeaveSample(heave, now);

        // Tanks (demo): supply tanks slowly drain/refill, waste tanks fill.
        auto setTank = [&](uint8_t inst, uint8_t ft, float lvl, float cap) {
            TankInfo *tk = data.findOrCreateTank(inst, ft);
            if (tk) { tk->level = lvl; tk->capacity = cap; tk->lastUpdate = now; }
        };
        setTank(0, 0, clamp(55.f + 30.f*sinf(t*2.f*PI_F/900.f),       5.f, 100.f), 200.f); // Diesel
        setTank(0, 1, clamp(50.f + 35.f*sinf(t*2.f*PI_F/700.f + 1.f), 0.f, 100.f), 150.f); // fresh water
        setTank(0, 5, clamp(40.f + 30.f*sinf(t*2.f*PI_F/650.f + 2.f), 0.f,  95.f),  80.f); // black water
        setTank(0, 2, clamp(45.f + 25.f*sinf(t*2.f*PI_F/500.f + 3.f), 0.f,  95.f),  60.f); // grey water

        // Battery banks (demo): Service charges from solar by "day", drains at night;
        // Starter floats full. Current sign: + = charging, - = discharging.
        float solarA = clamp(9.f * sinf(t * 2.f * PI_F / 240.f), 0.f, 9.f);   // 0..9 A solar
        float svcA   = solarA - 4.5f;                                          // net service current
        float svcSoc = clamp(70.f + 22.f * sinf(t * 2.f * PI_F / 800.f), 15.f, 100.f);
        float svcAh  = svcSoc / 100.f * 200.f;                                 // 200 Ah bank
        float svcMin = (fabsf(svcA) > 0.3f)
                     ? ((svcA < 0.f) ? svcAh / (-svcA) : (200.f - svcAh) / svcA) * 60.f
                     : NAN;
        auto setBatt = [&](uint8_t inst, float v, float a, float soc, float trMin) {
            BatteryBank *b = data.findOrCreateBattery(inst);
            if (b) { b->voltage = v; b->current = a; b->soc = soc;
                     b->timeRemMin = trMin; b->temperature = 24.f; b->lastUpdate = now; }
        };
        setBatt(0, 12.55f + svcA * 0.03f, svcA, svcSoc, svcMin);   // Service 200 Ah
        setBatt(1, 13.1f, 0.3f, 100.f, NAN);                       // Starter (floating, full)

        // Environment (demo): pre-seed a falling-pressure trend curve (demo only),
        // then live barometric wave + air/water temp + humidity.
        static bool envSeeded = false;
        if (!envSeeded) {
            envSeeded = true;
            for (int i = 0; i < DataModel::PRESS_HIST; i++)
                data.pressHistVal[i] = 1016.f + 4.f*sinf(i*0.12f)
                                     - 6.f*(i/(float)(DataModel::PRESS_HIST-1));
            data.pressHistIdx = 0; data.pressHistFull = true;
        }
        float press = 1011.f + 5.f*sinf(t*2.f*PI_F/600.f);
        data.pressure  = press;
        data.airTemp   = 18.f + 5.f*sinf(t*2.f*PI_F/240.f);
        data.waterTemp = 16.f + 2.f*sinf(t*2.f*PI_F/300.f + 1.f);
        data.humidity  = clamp(66.f + 16.f*sinf(t*2.f*PI_F/180.f + 2.f), 30.f, 99.f);
        data.lastEnvUpdate = now;
        data.pushPressureSample(press, now);

        // Time/date (demo): 2024-06-21 (summer solstice), noon UTC + real-time
        // tick, CEST (+2 h). Lets the clock run and sunrise/sunset compute.
        // Skipped once a real clock (SNTP/GPS) has set data.timeIsReal — then the
        // genuine current time stands so the BSH tide forecast lines up.
        if (!data.timeIsReal) {
            data.sysDays        = (uint16_t)scDaysFromCivil(2024, 6, 21);
            data.sysSecOfDay    = 43200.0 + (double)t;   // 12:00 UTC + seconds since boot
            data.localOffsetMin = 120;
            data.lastTimeUpdate = now;
            data.timeValid      = true;
        }

        // Waypoint navigation (demo): a mark roughly ahead, oscillating XTE.
        float btw = fmodf(cog + 6.f * sinf(t * 2.f * PI_F / 80.f) + 360.f, 360.f);
        data.navActive = true;
        data.navBtw    = btw;
        data.navDtw    = clamp(2.5f + 2.f * sinf(t * 2.f * PI_F / 300.f), 0.2f, 6.f);
        data.navXte    = 14.f * sinf(t * 2.f * PI_F / 45.f);                  // m, L/R
        data.navVmc    = clamp(sog * cosf((btw - cog) * D2R), 0.1f, 12.f);
        data.navWpNum  = 3;
        data.lastNavUpdate = now;

        // Engine (motor-sailing demo): RPM swings 700 ↔ 2200 over a 25 s cycle.
        float rpm     = 1450.f + 750.f * sinf(t * 2.f * PI_F / 25.f);
        float rpmNorm = clamp((rpm - 700.f) / 1500.f, 0.f, 1.f);
        data.rpm          = rpm;
        data.oilPressure  = 1600.f + 2900.f * rpmNorm;   // ~1.6-4.5 bar, field carries hPa
        data.coolantTemp  = 76.f + 11.f * rpmNorm + 1.5f * sinf(t / 30.f);// ~76–88 °C
        data.fuelFlow     = 0.8f + 9.0f * rpmNorm;                        // ~0.8–9.8 L/h
        data.engineHours  = 1287.4f + t / 3600.f;                         // creeps up

        // AP: standby
        data.apEngaged    = false;

        // Autopilot target: follow current COG
        data.apTargetHeading = hdg;

        // Push ring-buffer histories every ~5 s
        static uint32_t lastHist = 0;
        if (now - lastHist >= 5000) {
            data.pushWindSample(twd, tws);
            data.pushDepthSample(depth);
            lastHist = now;
        }

        // Update one synthetic AIS target
        if (data.aisCount == 0) {
            // First call: allocate the target
            AisTarget *t2 = data.findOrCreateAis(123456789);
            if (t2) {
                strncpy(t2->name,     "DEMO VESSEL", sizeof(t2->name)-1);
                strncpy(t2->callsign, "DM0001",      sizeof(t2->callsign)-1);
                t2->shipType  = 36;  // sailing vessel
                t2->navStatus = 0;   // under way
            }
        }
        // Update position of the target
        for (int i = 0; i < data.aisCount; i++) {
            if (data.aisTargets[i].mmsi == 123456789) {
                data.aisTargets[i].lat      = aisLat;
                data.aisTargets[i].lon      = aisLon;
                data.aisTargets[i].sog      = 5.2f;
                data.aisTargets[i].cog      = fmodf(aisBrg + 90.f, 360.f);
                data.aisTargets[i].hdg      = fmodf(aisBrg + 90.f, 360.f);
                data.aisTargets[i].lastSeen = now;
                data.calcCpa(data.aisTargets[i]);
                break;
            }
        }
    }

    // ── Fusion media demo (source list, now-playing, volumes) ─────────────
    static bool fusInit = false;
    if (!fusInit) { Fusion::demoInit(); fusInit = true; }
    Fusion::demoTick(now);
}
