// wind_vectors.cpp - test vectors for docs/WIND-AND-SAILS.md.
//
// Printed by the REAL firmware code wherever it can be linked on a PC, and by
// faithful restatements where it cannot (marked RESTATED below, each with the
// function it copies). A port to NauticPi reproduces these numbers.
//
// Build and run from the repository root (any C++11 compiler; no LVGL), as
// one command line each:
//   g++ -std=gnu++11 -O2 -Wall -Wextra -Wshadow -I src/nmea -I src/config
//       -I src/display/screens docs/wind_vectors.cpp src/nmea/WindCalc.cpp
//       src/display/screens/SpiGeom.cpp -o wind_vectors
//   ./wind_vectors > wind_vectors_out.txt
// The output is section 7.2 of WIND-AND-SAILS.md (how to run: 7.3), unchanged;
// after a change to any file below, regenerate it and paste it in again.
//
// Linked unchanged (the build compiles the repository files themselves):
//   src/nmea/WindCalc.cpp            norm180/norm360, apparentToTrue,
//                                    trueToApparent, lowPassAlpha,
//                                    ScalarLowPass, AngleLowPass, Pipeline
//                                    (sources, COG latch, received true wind,
//                                    expire)
//   src/config/WindDampCfg.h         header only: clamp(), apply() (migration)
//   src/display/screens/SpiGeom.cpp  the downwind sail geometry
// RESTATED (their files need LVGL or ArduinoJson):
//   WindScreen.cpp  camberForTwa(), computeSailState(), latchLeeSide(), the
//                   NaN handling and latch call at the top of drawInstrument(),
//                   the trim smoothing at the top of drawBoat(), spiRigSide(),
//                   resetRig(), the truncated stem-tip vertex of the hull (L4);
//                   the pole angle is the checker's definition
//   Config.cpp      spiTypeFromName() and the "sail.spi_type" block of fromJson()
//   index.html      spiTypeOf()
//   N2kHandler.cpp  m/s -> kn (/ 0.5144f)
//   LVGL 8.4        lv_color_mix() for RGB565 with LV_COLOR_MIX_ROUND_OFS 0
#include "WindCalc.h"
#include "WindDampCfg.h"
#include "SpiGeom.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

using WindCalc::BoatInputs;
using WindCalc::Pipeline;
using WindCalc::WindDamping;
using SpiGeom::Pt;

// ---------------------------------------------------------------- printing
static const char *F(float v) {
    static char buf[32][24];
    static int  n = 0;
    char *b = buf[n++ & 31];
    if (v != v) { snprintf(b, 24, "NaN"); return b; }
    if (v == 0.f) v = 0.f;                       // no "-0.0000"
    snprintf(b, 24, "%.4f", (double)v);
    return b;
}
static const char *B(bool v) { return v ? "1" : "0"; }
static void H(const char *t) { printf("\n==== %s\n", t); }

// ================================================================ RESTATED
// WindScreen.cpp

enum class SpiType : uint8_t { None = 0, Gores = 1, Balloon = 2, Gennaker = 3 };
static const char *spiName(SpiType t) {
    switch (t) {
        case SpiType::None:     return "none";
        case SpiType::Gores:    return "gores";
        case SpiType::Gennaker: return "gennaker";
        default:                return "balloon";
    }
}

struct SailState {
    float   boomAngleDeg     = 5.f;
    float   headsailAngleDeg = 20.f;
    float   flutterDeg       = 0.f;
    float   leewardSide      = -1.f;
    bool    luffing          = false;
    bool    running          = false;
    bool    useSpinnaker     = false;
    bool    useCodeZero      = false;
    int     reefCount        = 0;
    float   mainCamber       = 0.16f;
    float   jibCamber        = 0.14f;
    float   awaAbs           = 180.f;
    SpiType spiType          = SpiType::None;
};

// The AppConfig fields computeSailState() and drawBoat() read.
struct SailCfg {
    SpiType spiType   = SpiType::Balloon;   // sail.spi_type
    bool    codeZero  = false;              // sail.code_zero
    bool    butterfly = false;              // sail.butterfly
    float   lwl       = 9.f;                // sail.lwl_m
};

static float camberForTwa(float absTwa) {
    if (absTwa <  35.f) return 0.10f;
    if (absTwa <  60.f) return 0.15f;
    if (absTwa < 100.f) return 0.19f;
    if (absTwa < 150.f) return 0.24f;
    return 0.28f;
}

static float latchLeeSide(float sideAng, float fromSign, float prev) {
    if (isnan(sideAng)) return 0.f;
    if (fabsf(sideAng) > 175.f && prev != 0.f) return prev;
    return fromSign;
}

static SailState computeSailState(float absTwa, float twa, float awa, float tws,
                                  float stw, const SailCfg &cfg) {
    SailState s;
    const bool  haveTwa = !isnan(twa);
    const bool  haveAwa = !isnan(awa);
    const float sideAng = haveTwa ? twa : (haveAwa ? awa : 0.f);
    s.leewardSide = (sideAng >= 0.f) ? -1.f : 1.f;
    if (haveTwa) {
        s.luffing = (absTwa < 28.f);
        s.running = (absTwa > 150.f);
    } else if (haveAwa) {
        s.luffing = (fabsf(awa) < 18.f);
        s.running = (fabsf(awa) > 140.f);
    }
    float absAwa = isnan(awa) ? 0.f : fabsf(awa);
    s.awaAbs = haveAwa ? fabsf(awa) : (haveTwa ? absTwa : 180.f);
    float boom = absAwa - 13.f;
    if (boom <  4.f) boom =  4.f;
    if (boom > 86.f) boom = 86.f;
    s.boomAngleDeg     = boom;
    s.headsailAngleDeg = boom * 0.82f;
    s.flutterDeg       = 0.f;

    bool hasTws = !isnan(tws);
    const SpiType spi = cfg.spiType;
    s.spiType = spi;
    bool spiWindow = false;
    if (spi == SpiType::Gores || spi == SpiType::Balloon)
        spiWindow = (absTwa > 100.f && absTwa < 150.f && tws < 25.f) ||
                    (absTwa >= 150.f && tws < 20.f);
    else if (spi == SpiType::Gennaker)
        spiWindow = (absTwa > 90.f && absTwa < 150.f && tws < 25.f) ||
                    (absTwa >= 150.f && absTwa <= 160.f && tws < 20.f);
    s.useSpinnaker = spiWindow && !s.luffing && hasTws;
    s.useCodeZero  = !s.useSpinnaker && cfg.codeZero && hasTws &&
                     absTwa > 50.f && absTwa < 95.f && tws > 2.f && tws < 14.f;

    float lwl       = cfg.lwl;
    float hullSpeed = 2.43f * sqrtf(lwl > 0.f ? lwl : 9.f);
    float ratio     = (!isnan(stw) && hullSpeed > 0.f) ? stw / hullSpeed : 0.f;
    s.reefCount = 0;
    if (hasTws) {
        if      (tws > 28.f || ratio > 0.97f) s.reefCount = 3;
        else if (tws > 22.f || ratio > 0.93f) s.reefCount = 2;
        else if (tws > 16.f || ratio > 0.88f) s.reefCount = 1;
    }
    if (haveTwa) {
        s.mainCamber = camberForTwa(absTwa);
        s.jibCamber  = s.mainCamber * 0.85f;
    }
    return s;
}

// The per-screen state WindScreen keeps across frames, and one frame of it:
// the top of drawInstrument() (NaN handling, lee latch) and the top of
// drawBoat() (trim targets, smoothing, _spiDrawn).
struct Rig {
    float smBoom = 0.f, smJib = 0.f, smSpiRot = 0.f, smGen = 0.f;
    bool  trimInit = false;
    float leeSide  = 0.f;
    bool  spiDrawn = false;
    void  resetRig() { trimInit = false; leeSide = 0.f; }
};

static SailState frame(Rig &rig, float twa, float awa, float tws, float stw,
                       uint32_t tickMs, const SailCfg &cfg) {
    float nTwa = isnan(twa) ? 0.f : twa;
    while (nTwa >  180.f) nTwa -= 360.f;
    while (nTwa < -180.f) nTwa += 360.f;
    const float absTwa = fabsf(nTwa);
    const float nAwa   = isnan(awa) ? 0.f : awa;
    const bool  haveTwa = !isnan(twa), haveAwa = !isnan(awa);
    const float shownTwa = haveTwa ? nTwa : NAN;
    const float shownAwa = haveAwa ? nAwa : NAN;
    const float knownAbs = haveTwa ? absTwa : NAN;

    SailState sail = computeSailState(knownAbs, shownTwa, shownAwa, tws, stw, cfg);
    const float sideAng = haveTwa ? shownTwa : (haveAwa ? shownAwa : NAN);
    rig.leeSide = latchLeeSide(sideAng, sail.leewardSide, rig.leeSide);
    if (rig.leeSide != 0.f) sail.leewardSide = rig.leeSide;

    const float ss = sail.leewardSide;
    const bool butterfly = cfg.butterfly && sail.running;
    float tgtBoom, tgtJib;
    if (sail.luffing) {
        tgtBoom = sinf((float)tickMs * 0.018f)        * 15.f;
        tgtJib  = sinf((float)tickMs * 0.018f + 0.7f) * 17.f;
    } else {
        tgtBoom = ss * sail.boomAngleDeg;
        tgtJib  = butterfly ? (-ss * 84.f) : (ss * sail.headsailAngleDeg);
    }
    const float tgtSpiRot = ss * SpiGeom::symRotationDeg(sail.awaAbs);
    const float tgtGen    = ss * SpiGeom::genSheetDeg(sail.awaAbs);
    if (!rig.trimInit) {
        rig.smBoom = tgtBoom;     rig.smJib = tgtJib;
        rig.smSpiRot = tgtSpiRot; rig.smGen = tgtGen;
        rig.trimInit = true;
    } else {
        const float k = 0.18f;
        rig.smBoom   += (tgtBoom   - rig.smBoom)   * k;
        rig.smJib    += (tgtJib    - rig.smJib)    * k;
        rig.smSpiRot += (tgtSpiRot - rig.smSpiRot) * k;
        if ((tgtGen > 0.f) == (rig.smGen > 0.f))
            rig.smGen += (tgtGen - rig.smGen) * k;
        else
            rig.smGen = tgtGen;
    }
    rig.spiDrawn = !butterfly && sail.useSpinnaker;
    return sail;
}

static inline float spiRigSide(float smBoomDeg) { return smBoomDeg >= 0.f ? 1.f : -1.f; }

static const float DEG2RAD = 3.14159265f / 180.f;

// Pole angle off the bow [deg], mast (0, MAST_Y) -> tack (the checker's
// definition, docs/downwind_check.cpp).
static float poleAngle(Pt tack) {
    return atan2f(fabsf(tack.x), SpiGeom::MAST_Y - tack.y) / DEG2RAD;
}

// ---------------------------------------------------------------- Config.cpp
// A JSON value as fromJson() sees it.
enum class JKind { Missing, Null, Str, Num, Bool };
struct JVal { JKind k; const char *s; bool b; };
static JVal jMissing()          { JVal v = { JKind::Missing, nullptr, false }; return v; }
static JVal jNull()             { JVal v = { JKind::Null,    nullptr, false }; return v; }
static JVal jStr(const char *s) { JVal v = { JKind::Str,     s,       false }; return v; }
static JVal jNum()              { JVal v = { JKind::Num,     nullptr, false }; return v; }
static JVal jBool(bool b)       { JVal v = { JKind::Bool,    nullptr, b };     return v; }
static const char *jShow(const JVal &v) {
    static char buf[8][32];
    static int  n = 0;
    char *b = buf[n++ & 7];
    switch (v.k) {
        case JKind::Missing: return "(missing)";
        case JKind::Null:    return "null";
        case JKind::Num:     return "2";
        case JKind::Bool:    return v.b ? "true" : "false";
        case JKind::Str:     snprintf(b, 32, "\"%s\"", v.s); return b;
    }
    return "?";
}

static bool spiTypeFromName(const char *s, SpiType &out) {
    if (!s) return false;
    if (strcmp(s, "none") == 0)     { out = SpiType::None;     return true; }
    if (strcmp(s, "gores") == 0)    { out = SpiType::Gores;    return true; }
    if (strcmp(s, "balloon") == 0)  { out = SpiType::Balloon;  return true; }
    if (strcmp(s, "gennaker") == 0) { out = SpiType::Gennaker; return true; }
    return false;
}
// fromJson(), "sail.spi_type" block. as<const char*>() is null for anything
// that is not a string; isNull() is true for a missing key and for null.
static SpiType loadSpiType(SpiType cur, const JVal &spiType, const JVal &spinnaker) {
    SpiType t;
    if (spiTypeFromName(spiType.k == JKind::Str ? spiType.s : nullptr, t))
        return t;
    if (spinnaker.k == JKind::Bool)
        return spinnaker.b ? SpiType::Balloon : SpiType::None;
    if (!(spiType.k == JKind::Missing || spiType.k == JKind::Null))
        return SpiType::Balloon;
    return cur;
}
// index.html: spiTypeOf(s) - v = s?.spi_type; known -> v; else
// s?.spinnaker === false ? 'none' : 'balloon'.
static const char *webSpiTypeOf(const JVal &spiType, const JVal &spinnaker) {
    SpiType t;
    if (spiType.k == JKind::Str && spiTypeFromName(spiType.s, t)) return spiType.s;
    return (spinnaker.k == JKind::Bool && !spinnaker.b) ? "none" : "balloon";
}

// ---------------------------------------------------------------- LVGL mix
static uint16_t to565(uint32_t rgb) {
    const uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static uint16_t mix565(uint16_t c1, uint16_t c2, uint8_t mix) {
    const uint32_t m  = ((uint32_t)mix + 4) >> 3;
    const uint32_t bg = ((uint32_t)c2 | ((uint32_t)c2 << 16)) & 0x7E0F81F;
    const uint32_t fg = ((uint32_t)c1 | ((uint32_t)c1 << 16)) & 0x7E0F81F;
    const uint32_t r  = ((((fg - bg) * m) >> 5) + bg) & 0x7E0F81F;
    return (uint16_t)((r >> 16) | r);
}
// 8-bit ideal: round((fg * mix + bg * (255 - mix)) / 255) per channel.
static uint32_t mix888(uint32_t fg, uint32_t bg, int mix) {
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        const int f = (int)((fg >> sh) & 0xFF), b = (int)((bg >> sh) & 0xFF);
        const int c = (f * mix + b * (255 - mix) + 127) / 255;
        out |= (uint32_t)c << sh;
    }
    return out;
}

static inline float msToKn(float ms) { return ms / 0.5144f; }   // N2kHandler

// ================================================================ sections

static void secNorm() {
    H("A1 norm180 / norm360");
    const float in[] = { 0.f, 180.f, -180.f, 181.f, -181.f, 359.f, 360.f, 540.f,
                         -540.f, 720.5f, -1e-6f, NAN };
    printf("        in       norm180      norm360\n");
    for (float v : in)
        printf("%10s  %12s %12s\n", F(v), F(WindCalc::norm180(v)), F(WindCalc::norm360(v)));
}

static void secTrue() {
    H("B1 apparentToTrue(AWS, AWA, boat speed, prevTWA) -> TWS, TWA");
    struct C { float aws, awa, spd, prev; } cs[] = {
        { 10, 0, 0, NAN }, { 10, 0, 5, NAN }, { 10, 0, 10, NAN }, { 10, 0, 10, 45 },
        { 10, 0, 15, NAN }, { 10, 90, 5, NAN }, { 10, -90, 5, NAN },
        { 10, 180, 5, NAN }, { 10, -180, 5, NAN }, { 10, 45, 0, NAN },
        { 12, 30, 6, NAN }, { 15, -120, 6.5f, NAN }, { 8, 150, 4, NAN },
        { 6.8f, 11.8f, 7, NAN }, { 0, 0, 0, 30 }, { 0.04f, 10, 0, 30 },
        { 0.06f, 10, 0, 30 }, { NAN, 30, 5, 10 }, { 10, NAN, 5, 10 }, { 10, 30, NAN, 10 },
    };
    printf("       AWS        AWA        STW    prevTWA        TWS        TWA\n");
    for (const C &c : cs) {
        float tws, twa;
        WindCalc::apparentToTrue(c.aws, c.awa, c.spd, c.prev, tws, twa);
        printf("%10s %10s %10s %10s %10s %10s\n", F(c.aws), F(c.awa), F(c.spd), F(c.prev), F(tws), F(twa));
    }
    H("B2 trueToApparent(TWS, TWA, boat speed) -> AWS, AWA (the demo, and the checker's grid)");
    struct D { float tws, twa, spd; } ds[] = {
        { 14, 150, 5 }, { 14, -150, 5 }, { 14, 180, 5 }, { 14, 101, 5 }, { 14, 120, 5 },
        { 10, 150, 7 }, { 6, 178, 7 }, { 6, 178, 3 }, { 20, 178, 7 }, { 10, 90, 5 },
        { 20, 45, 6 }, { 5, 0, 0 }, { NAN, 90, 5 },
    };
    printf("       TWS        TWA        STW        AWS        AWA   back: TWS        TWA\n");
    for (const D &d : ds) {
        float aws, awa, tws, twa;
        WindCalc::trueToApparent(d.tws, d.twa, d.spd, aws, awa);
        WindCalc::apparentToTrue(aws, awa, d.spd, NAN, tws, twa);
        printf("%10s %10s %10s %10s %10s %16s %10s\n", F(d.tws), F(d.twa), F(d.spd), F(aws), F(awa), F(tws), F(twa));
    }
}

static const uint32_t T0 = 100000;   // a clock that is not 0 ("never")

static BoatInputs boatAt(uint32_t now) {
    BoatInputs b;
    b.stw = 6.f; b.stwMs = now;
    return b;
}

static void secHeading() {
    H("C1 Pipeline::trueHeading (heading as received + deviation + variation), at t, stamp t unless noted");
    struct C { float hdg; bool mag; float dev, var; uint32_t age; } cs[] = {
        { 10, false, NAN, NAN, 0 }, { 10, false, 3, 5, 0 }, { 350, true, 3, 5, 0 },
        { 355, true, 2, 4, 0 }, { 5, true, -3, -4, 0 }, { 90, true, NAN, 5, 0 },
        { 90, true, 3, NAN, 0 }, { 90, true, NAN, NAN, 0 }, { 90, false, NAN, NAN, 4999 },
        { 90, false, NAN, NAN, 5000 }, { NAN, false, NAN, NAN, 0 },
    };
    printf("       hdg  magnetic        dev        var  age[ms]    trueHdg\n");
    for (const C &c : cs) {
        BoatInputs b;
        b.hdg = c.hdg; b.hdgMs = T0 - c.age; b.hdgMagnetic = c.mag;
        b.deviation = c.dev; b.variation = c.var;
        printf("%10s %9s %10s %10s %8u %10s\n", F(c.hdg), B(c.mag), F(c.dev), F(c.var),
               (unsigned)c.age, F(Pipeline::trueHeading(b, T0)));
    }
    H("C2 Pipeline::trueCourse (COG as a true heading; needs COG and SOG fresh)");
    struct K { float cog; bool mag; float var; float sog; uint32_t cogAge, sogAge; } ks[] = {
        { 200, false, NAN, 3, 0, 0 }, { 200, false, 5, 3, 0, 0 }, { 200, true, 5, 3, 0, 0 },
        { 358, true, 5, 3, 0, 0 }, { 200, true, NAN, 3, 0, 0 }, { 200, false, NAN, 0.2f, 0, 0 },
        { 200, false, NAN, NAN, 0, 0 }, { 200, false, NAN, 3, 5000, 0 }, { 200, false, NAN, 3, 0, 5000 },
    };
    printf("       COG  magnetic        var        SOG  cogAge  sogAge   trueCourse\n");
    for (const K &k : ks) {
        BoatInputs b;
        b.cog = k.cog; b.cogMs = T0 - k.cogAge; b.cogMagnetic = k.mag; b.variation = k.var;
        b.sog = k.sog; b.sogMs = T0 - k.sogAge;
        printf("%10s %9s %10s %10s %7u %7u %12s\n", F(k.cog), B(k.mag), F(k.var), F(k.sog),
               (unsigned)k.cogAge, (unsigned)k.sogAge, F(Pipeline::trueCourse(b, T0)));
    }
    H("C3 TWD through the pipeline, no damping: AWS 12, AWA 40, STW 6");
    const WindDamping d0 = { 0.f, 0.f, 0.f, 0.f };
    struct T { float hdg; bool mag; float dev, var; } ts[] = {
        { 350, true, 3, 5 }, { 10, false, NAN, NAN }, { 200, true, NAN, -4 }, { NAN, false, NAN, NAN },
    };
    printf("       hdg  magnetic        dev        var |       TWS        TWA        TWD\n");
    for (const T &t : ts) {
        Pipeline p; p.reset();
        BoatInputs b = boatAt(T0);
        b.hdg = t.hdg; b.hdgMs = isnan(t.hdg) ? 0 : T0; b.hdgMagnetic = t.mag;
        b.deviation = t.dev; b.variation = t.var;
        p.apparent(12.f, 40.f, b, T0, d0);
        printf("%10s %9s %10s %10s | %9s %10s %10s\n", F(t.hdg), B(t.mag), F(t.dev), F(t.var), F(p.tws), F(p.twa), F(p.twd));
    }
    H("C4 boat speed: STW if fresh, else SOG if fresh, else none (true wind NaN)");
    struct S { float stw; uint32_t stwAge; float sog; uint32_t sogAge; } ss[] = {
        { 6, 0, 7, 0 }, { 6, 5000, 7, 0 }, { NAN, 0, 7, 0 }, { 6, 5000, 7, 5000 }, { NAN, 0, NAN, 0 },
    };
    printf("       STW  stwAge        SOG  sogAge   boatSpeed |   TWS(AWS 12, AWA 40)        TWA\n");
    for (const S &s : ss) {
        BoatInputs b;
        b.stw = s.stw; b.stwMs = T0 - s.stwAge; b.sog = s.sog; b.sogMs = T0 - s.sogAge;
        Pipeline p; p.reset();
        p.apparent(12.f, 40.f, b, T0, d0);
        printf("%10s %7u %10s %7u %11s | %21s %10s\n", F(s.stw), (unsigned)s.stwAge, F(s.sog),
               (unsigned)s.sogAge, F(Pipeline::boatSpeed(b, T0)), F(p.tws), F(p.twa));
    }
}

// One apparent sample of the COG scenarios: AWS 10, AWA 60, STW 4 (fresh),
// COG 200 true; damping off so TWD shows raw. Prints SOG and TWD.
struct CogStep {
    uint32_t dtMs;      // since the previous step
    float    sogMs;     // SOG as sent on the bus [m/s]; NaN = "not available"
    bool     cogAvail;  // COG field available (else lastCogUpdate = 0)
    bool     hdg;       // a fresh true heading of 90 deg
    bool     opt;       // cal.twd_cog
    const char *note;
};
static void runCog(const char *title, const CogStep *st, int n) {
    printf("-- %s\n", title);
    printf("   t[s]  SOG[m/s]   SOG[kn]  cog  hdg  opt        TWA        TWD   note\n");
    Pipeline p; p.reset();
    const WindDamping d0 = { 0.f, 0.f, 0.f, 0.f };
    uint32_t t = T0;
    BoatInputs b;
    for (int i = 0; i < n; i++) {
        t += st[i].dtMs;
        b.stw = 4.f; b.stwMs = t;
        b.cog = 200.f; b.cogMs = st[i].cogAvail ? t : 0; b.cogMagnetic = false;
        if (!isnan(st[i].sogMs)) { b.sog = msToKn(st[i].sogMs); b.sogMs = t; }
        if (st[i].hdg) { b.hdg = 90.f; b.hdgMs = t; b.hdgMagnetic = false; } else b.hdgMs = 0;
        p.expire(t);
        p.apparent(10.f, 60.f, b, t, d0, st[i].opt);
        printf("%7.1f %9s %9s %4s %4s %4s %10s %10s   %s\n", (double)(t - T0) / 1000.0,
               F(st[i].sogMs), F(b.sog), B(st[i].cogAvail), B(st[i].hdg), B(st[i].opt),
               F(p.twa), F(p.twd), st[i].note);
    }
}

static void secCog() {
    H("D1 bus SOG steps around the COG limits (m/s -> kn as N2kHandler converts)");
    const float ms[] = { 0.40f, 0.41f, 0.42f, 0.50f, 0.51f, 0.52f };
    for (float m : ms)
        printf("   %s m/s -> %s kn   >= 0.99: %s   >= 0.79: %s\n", F(m), F(msToKn(m)),
               B(msToKn(m) >= WindCalc::COG_MIN_SOG_KN), B(msToKn(m) >= WindCalc::COG_OFF_SOG_KN));

    H("D2 COG latch: no heading, option on, samples 1 s apart (TWD NaN = latch off)");
    const float N = NAN;
    const CogStep a[] = {
        {    0, 0.45f, true, false, true, "below 1 kn: off" },
        { 1000, 0.50f, true, false, true, "0.972 kn: still off" },
        { 1000, 0.51f, true, false, true, "0.991 kn: ON" },
        { 1000, 0.45f, true, false, true, "0.875 kn: stays on" },
        { 1000, 0.41f, true, false, true, "0.797 kn: stays on" },
        { 1000, 0.40f, true, false, true, "0.778 kn: OFF" },
        { 1000, 0.45f, true, false, true, "0.875 kn: stays off" },
        { 1000, 0.52f, true, false, true, "1.011 kn: ON" },
        { 6000, 0.45f, true, false, true, "6 s without a sample: OFF (needs 1 kn)" },
        { 1000, 0.55f, true, false, true, "ON" },
        { 1000, 0.60f, false, false, true, "COG not available: OFF" },
        { 1000, 0.45f, true, false, true, "COG back at 0.875 kn: stays off" },
        { 1000, 0.60f, true, false, true, "ON" },
        { 1000, 0.60f, true, false, false, "option off: OFF" },
        { 1000, 0.45f, true, false, true, "option on again at 0.875 kn: off" },
        { 1000, N,     true, false, true, "SOG field n/a: the last SOG (1 s old) still counts" },
    };
    runCog("D2", a, (int)(sizeof(a) / sizeof(a[0])));

    H("D3 the latch follows the SOG behind a heading");
    const CogStep b[] = {
        {    0, 0.60f, true, true,  true, "heading: TWD = 90 + TWA" },
        { 1000, 0.47f, true, true,  true, "0.914 kn behind the heading" },
        { 1000, 0.47f, true, false, true, "heading gone: COG at once (reached 1 kn)" },
    };
    runCog("D3a came down from 1.17 kn", b, 3);
    const CogStep c[] = {
        {    0, 0.47f, true, true,  true, "heading: TWD = 90 + TWA" },
        { 1000, 0.47f, true, true,  true, "0.914 kn behind the heading" },
        { 1000, 0.47f, true, false, true, "heading gone: no TWD (never reached 1 kn)" },
    };
    runCog("D3b never reached 1 kn", c, 3);
}

static void secReceived() {
    H("E1 received true wind (another device), no apparent wind on the bus; no damping");
    const WindDamping d0 = { 0.f, 0.f, 0.f, 0.f };
    Pipeline p; p.reset();
    uint32_t t = T0;
    BoatInputs b; b.hdg = 100.f; b.hdgMs = t;
    printf("   t[s]  ref       TWS      angle   used        TWS        TWA        TWD   note\n");
    struct R { uint32_t dt; uint8_t ref; float tws, ang; bool app; bool hdg; const char *note; } rs[] = {
        {    0, 3, 11, 50,  false, true,  "ref 3 alone: used" },
        {  200, 4, 12, 45,  false, true,  "ref 4: used (best)" },
        {  200, 3, 11, 50,  false, true,  "ref 3 while 4 talks: stands by" },
        {  200, 0, 12, 140, false, true,  "ref 0 while 4 talks: stands by, TWD kept" },
        { 5000, 3, 11, 50,  false, true,  "4 silent 5 s: ref 3 used" },
        {  200, 0, 12, 140, false, true,  "ref 0 while 3 talks: stands by" },
        { 5200, 0, 12, 140, false, true,  "only ref 0: TWA = TWD - heading" },
        {  200, 0, 12, 140, false, false, "ref 0, no heading: TWA NaN, TWD 140" },
        {  200, 0, 0, 0,    true,  false, "apparent sample (AWS 12 AWA 40 STW 6): TWD from ref 0" },
        {  200, 4, 12, 45,  false, false, "ref 4 while apparent is fresh: ignored" },
    };
    for (const R &r : rs) {
        t += r.dt;
        if (r.hdg) { b.hdg = 100.f; b.hdgMs = t; } else b.hdgMs = 0;
        b.stw = 6.f; b.stwMs = t;
        p.expire(t);
        bool used;
        if (r.app) used = p.apparent(12.f, 40.f, b, t, d0);
        else       used = p.receivedTrue(r.ref, r.tws, r.ang, b, t, d0);
        printf("%7.1f %4s %9s %10s %6s %10s %10s %10s   %s\n", (double)(t - T0) / 1000.0,
               r.app ? "app" : (r.ref == 0 ? "0" : r.ref == 3 ? "3" : "4"),
               r.app ? "12(AWS)" : F(r.tws), r.app ? "40(AWA)" : F(r.ang), B(used),
               F(p.tws), F(p.twa), F(p.twd), r.note);
    }
    H("E2 expire(): blank after STALE_MS = 10 s, the next sample starts afresh (tau 2 s)");
    const WindDamping d2 = { 2.f, 2.f, 2.f, 2.f };
    Pipeline q; q.reset();
    t = T0;
    BoatInputs c; c.stw = 6.f; c.stwMs = t; c.hdg = 0.f; c.hdgMs = t;
    q.apparent(10.f, 50.f, c, t, d2);
    printf("   t=0      AWS %s AWA %s TWS %s TWA %s TWD %s\n", F(q.aws), F(q.awa), F(q.tws), F(q.twa), F(q.twd));
    q.expire(t + 9999);
    printf("   t=9.999  AWS %s AWA %s TWS %s TWA %s TWD %s (expire, still fresh)\n", F(q.aws), F(q.awa), F(q.tws), F(q.twa), F(q.twd));
    q.expire(t + 10000);
    printf("   t=10.0   AWS %s AWA %s TWS %s TWA %s TWD %s (expire: blanked)\n", F(q.aws), F(q.awa), F(q.tws), F(q.twa), F(q.twd));
    t += 10500;
    c.stwMs = t; c.hdgMs = t;
    q.apparent(20.f, -30.f, c, t, d2);
    printf("   t=10.5   AWS %s AWA %s TWS %s TWA %s TWD %s (new sample taken as is)\n", F(q.aws), F(q.awa), F(q.tws), F(q.twa), F(q.twd));
}

static void secDamping() {
    H("F1 lowPassAlpha(dt, tau) = 1 - exp(-dt/tau); tau <= 0 or NaN -> 1; dt <= 0 -> 0");
    struct A { float dt, tau; } as[] = {
        { 0.1f, 2 }, { 0.5f, 2 }, { 1, 2 }, { 2, 2 }, { 6, 2 }, { 0.5f, 0 }, { 0.5f, -1 },
        { 0.5f, NAN }, { 0, 2 }, { -0.1f, 2 }, { 0.6931472f, 1 },
    };
    for (const A &a : as) printf("   dt %s tau %s -> %s\n", F(a.dt), F(a.tau), F(WindCalc::lowPassAlpha(a.dt, a.tau)));

    H("F2 ScalarLowPass, tau 2 s, step 0 -> 10 at uneven dt; exact = 10 (1 - exp(-t/2))");
    {
        WindCalc::ScalarLowPass f;
        printf("   first sample 0 -> %s\n", F(f.update(0.f, 0.f, 2.f)));
        const float dts[] = { 0.1f, 0.25f, 0.05f, 0.6f, 1.0f, 0.5f, 1.5f, 3.0f };
        float t = 0.f;
        for (float dt : dts) {
            t += dt;
            const float y = f.update(10.f, dt, 2.f);
            printf("   t %s dt %s -> %s   exact %s\n", F(t), F(dt), F(y), F(10.f * (1.f - expf(-t / 2.f))));
        }
        const float yNan = f.update(NAN, 0.1f, 2.f);   // two statements: printf
        const float ySev = f.update(7.f, 0.1f, 2.f);    // arguments are unsequenced
        printf("   NaN -> %s (reset); next 7 -> %s (taken as is)\n", F(yNan), F(ySev));
        WindCalc::ScalarLowPass g, h;
        g.update(0.f, 0.f, 2.f); h.update(0.f, 0.f, 2.f);
        float yg = 0.f;
        for (int i = 0; i < 10; i++) yg = g.update(10.f, 0.1f, 2.f);
        printf("   ten samples 0.1 s apart -> %s; one sample 1 s later -> %s\n", F(yg), F(h.update(10.f, 1.f, 2.f)));
        WindCalc::ScalarLowPass z;
        z.update(3.f, 0.f, 0.f);
        printf("   tau 0: 3 then 8 -> %s (input passes through)\n", F(z.update(8.f, 0.1f, 0.f)));
    }

    H("F3 AngleLowPass, tau 2 s: wrap 359 -> 1 (goes through 0, never through 180)");
    {
        WindCalc::AngleLowPass f;
        printf("   first sample 359 -> %s\n", F(f.update(359.f, 0.f, 2.f)));
        const float dts[] = { 0.2f, 0.5f, 0.3f, 1.0f, 2.0f, 4.0f };
        float t = 0.f;
        for (float dt : dts) { t += dt; printf("   t %s x 1 -> %s\n", F(t), F(f.update(1.f, dt, 2.f))); }
    }
    H("F4 AngleLowPass, tau 2 s: following wind alternating +179 / -179");
    {
        WindCalc::AngleLowPass f;
        float x = 179.f;
        printf("   first 179 -> %s\n", F(f.update(x, 0.f, 2.f)));
        for (int i = 0; i < 6; i++) { x = -x; printf("   %s -> %s\n", F(x), F(f.update(x, 0.25f, 2.f))); }
    }
    H("F5 AngleLowPass: large steps, one sample after dt = tau (fraction of the step shown)");
    {
        const float steps[] = { 10.f, 45.f, 90.f, 135.f, 170.f, -170.f };
        for (float s : steps) {
            WindCalc::AngleLowPass f;
            f.update(0.f, 0.f, 2.f);
            const float y = f.update(s, 2.f, 2.f);
            printf("   0 -> %s: %s  (%s of the step)\n", F(s), F(y), F(y / s));
        }
        WindCalc::AngleLowPass f;
        f.update(0.f, 0.f, 2.f);
        float t = 0.f;
        const float dts[] = { 0.5f, 0.5f, 1.0f, 2.0f, 4.0f };
        printf("   170 deg step at uneven dt, tau 2:\n");
        for (float dt : dts) { t += dt; const float y = f.update(170.f, dt, 2.f); printf("     t %s -> %s (%s)\n", F(t), F(y), F(y / 170.f)); }
        WindCalc::AngleLowPass o;
        o.update(0.f, 0.f, 1.f);
        const float y = o.update(180.f, 0.6931472f, 1.f);
        printf("   exactly opposite, alpha 0.5 (0 -> 180, dt ln2, tau 1): vector (%s, %s), output %s (kept)\n", F(o.c), F(o.s), F(y));
        printf("   next 180 dt 1: %s\n", F(o.update(180.f, 1.f, 1.f)));
    }

    H("F6 Pipeline: each time constant drives only its own outputs");
    {
        // appSpd 1 s, appDir 2 s, trueSpd 4 s, trueDir 8 s. True heading 30,
        // STW 5. AWS 10 / AWA 40 at t = 0, then a step to AWS 15 / AWA 60.
        const WindDamping d = { 1.f, 2.f, 4.f, 8.f };
        Pipeline p; p.reset();
        const float ts[] = { 0.f, 0.3f, 0.7f, 1.2f, 2.0f, 3.5f, 6.0f, 10.0f, 20.0f };
        printf("   damping appSpd 1 appDir 2 trueSpd 4 trueDir 8; heading 30 true; STW 5\n");
        printf("      t    AWSin  AWAin |     rawTWS     rawTWA |        AWS        AWA        TWS        TWA        TWD\n");
        for (float t : ts) {
            const uint32_t now = T0 + (uint32_t)(t * 1000.f + 0.5f);
            BoatInputs b; b.stw = 5.f; b.stwMs = now; b.hdg = 30.f; b.hdgMs = now;
            const float aw = (t == 0.f) ? 10.f : 15.f, aa = (t == 0.f) ? 40.f : 60.f;
            float rtws, rtwa;
            WindCalc::apparentToTrue(aw, aa, 5.f, NAN, rtws, rtwa);
            p.apparent(aw, aa, b, now, d);
            printf("   %5.1f %8.1f %6.1f | %10s %10s | %10s %10s %10s %10s %10s\n", (double)t, (double)aw, (double)aa,
                   F(rtws), F(rtwa), F(p.aws), F(p.awa), F(p.tws), F(p.twa), F(p.twd));
        }
    }
    H("F7 Pipeline: calm keeps the previous TWA (AWS 5 AWA 0 STW 5 -> TWS 0)");
    {
        const WindDamping d0 = { 0.f, 0.f, 0.f, 0.f };
        Pipeline p; p.reset();
        BoatInputs b; b.stw = 5.f; b.stwMs = T0; b.hdg = 0.f; b.hdgMs = T0;
        p.apparent(8.f, 50.f, b, T0, d0);
        printf("   AWS 8 AWA 50:  TWS %s TWA %s TWD %s\n", F(p.tws), F(p.twa), F(p.twd));
        b.stwMs = T0 + 1000; b.hdgMs = T0 + 1000;
        p.apparent(5.f, 0.f, b, T0 + 1000, d0);
        printf("   AWS 5 AWA 0:   TWS %s TWA %s TWD %s (previous raw TWA held)\n", F(p.tws), F(p.twa), F(p.twd));
        Pipeline q; q.reset();
        q.apparent(5.f, 0.f, b, T0 + 1000, d0);
        printf("   same from a fresh pipeline: TWS %s TWA %s TWD %s (no previous angle)\n", F(q.tws), F(q.twa), F(q.twd));
        q.apparent(NAN, 30.f, b, T0 + 1200, d0);
        printf("   AWS NaN AWA 30: AWS %s AWA %s TWS %s TWA %s (half a sample clears the true wind)\n", F(q.aws), F(q.awa), F(q.tws), F(q.twa));
    }
}

static void secMigration() {
    H("G1 WindDampCfg::apply(cur, keys) - migration of the two old keys, clamp 0..30");
    struct M { float cur[4]; WindDampCfg::Keys k; const char *note; };
    const float N = NAN;
    WindDampCfg::Keys k0;
    M ms[12];
    int n = 0;
    auto add = [&](float c0, float c1, float c2, float c3, float as, float ad, float ts, float td,
                   float os, float od, const char *note) {
        M &m = ms[n++];
        m.cur[0] = c0; m.cur[1] = c1; m.cur[2] = c2; m.cur[3] = c3;
        m.k = k0;
        m.k.appSpd = as; m.k.appDir = ad; m.k.trueSpd = ts; m.k.trueDir = td;
        m.k.oldSpd = os; m.k.oldDir = od; m.note = note;
    };
    add(2, 2, 2, 2,  N, N, N, N,  N, N,  "no key: keep");
    add(2, 2, 2, 2,  N, N, N, N,  5, N,  "old speed only");
    add(2, 2, 2, 2,  N, N, N, N,  5, 7,  "both old keys");
    add(2, 2, 2, 2,  1, N, N, N,  5, 7,  "one new key + both old");
    add(2, 2, 2, 2,  1, 3, 4, 10, 5, 7,  "all new keys win");
    add(2, 2, 2, 2, -3, 45, N, 0, N, N,  "clamped; NaN = not a number keeps");
    add(2, 2, 2, 2,  N, N, N, N,  N, 40, "old dir 40 clamped to 30");
    add(1, 3, 4, 10, N, N, N, N,  N, N,  "patch without damping keys keeps");
    add(1, 3, 4, 10, N, N, N, N,  6, N,  "old page posts old speed key");
    add(2, 2, 2, 2,  0.25f, 0, 30, 30.5f, N, N, "0 = off, 30 max");
    printf("   cur(appSpd appDir trueSpd trueDir) | app_spd app_dir true_spd true_dir old_spd old_dir | result\n");
    for (int i = 0; i < n; i++) {
        const M &m = ms[i];
        const WindDamping cur = { m.cur[0], m.cur[1], m.cur[2], m.cur[3] };
        const WindDamping r = WindDampCfg::apply(cur, m.k);
        printf("   %s %s %s %s | %s %s %s %s %s %s | %s %s %s %s   %s\n",
               F(cur.appSpd), F(cur.appDir), F(cur.trueSpd), F(cur.trueDir),
               F(m.k.appSpd), F(m.k.appDir), F(m.k.trueSpd), F(m.k.trueDir), F(m.k.oldSpd), F(m.k.oldDir),
               F(r.appSpd), F(r.appDir), F(r.trueSpd), F(r.trueDir), m.note);
    }
}

static void secSpiConfig() {
    H("H1 sail.spi_type on load (Config::fromJson) and preselect (index.html spiTypeOf); current value gennaker");
    struct C { JVal t, s; } cs[] = {
        { jStr("gores"), jMissing() }, { jStr("none"), jBool(true) }, { jStr("gennaker"), jBool(false) },
        { jStr("balloon"), jMissing() }, { jMissing(), jBool(true) }, { jMissing(), jBool(false) },
        { jNull(), jBool(false) }, { jStr("Balloon"), jBool(false) }, { jStr("kite"), jBool(true) },
        { jStr("kite"), jMissing() }, { jNum(), jMissing() }, { jNull(), jMissing() },
        { jMissing(), jMissing() }, { jMissing(), jNum() },
    };
    printf("   spi_type     spinnaker  | firmware   web page\n");
    for (const C &c : cs)
        printf("   %-12s %-10s | %-10s %s\n", jShow(c.t), jShow(c.s),
               spiName(loadSpiType(SpiType::Gennaker, c.t, c.s)), webSpiTypeOf(c.t, c.s));
    printf("   save (toJson): spi_type = name, spinnaker = (type != none):");
    const SpiType all[] = { SpiType::None, SpiType::Gores, SpiType::Balloon, SpiType::Gennaker };
    for (SpiType t : all) printf("  %s/%s", spiName(t), t != SpiType::None ? "true" : "false");
    printf("\n");
}

static void secWindows() {
    H("I1 downwind sail shown (useSpinnaker), steady, STW 5, AWA from the true wind");
    printf("   (after each TWS: four digits = useSpinnaker for none, gores, balloon, gennaker)\n");
    const float twas[] = { 89.9f, 90.f, 90.1f, 95.f, 99.9f, 100.f, 100.1f, 149.9f, 150.f, 155.f,
                           160.f, 160.1f, 175.f, 180.f, -100.1f, -160.f };
    const float twss[] = { 19.9f, 20.f, 24.9f, 25.f };
    for (float twa : twas) {
        printf("   TWA %9s:", F(twa));
        for (float tws : twss) {
            float aws, awa;
            WindCalc::trueToApparent(tws, twa, 5.f, aws, awa);
            printf("  TWS %s ", F(tws));
            const SpiType ty[] = { SpiType::None, SpiType::Gores, SpiType::Balloon, SpiType::Gennaker };
            for (SpiType t : ty) {
                SailCfg c; c.spiType = t;
                printf("%s", B(computeSailState(fabsf(twa), twa, awa, tws, 5.f, c).useSpinnaker));
            }
        }
        printf("\n");
    }
    printf("   no TWS (NaN), TWA 120: ");
    {
        SailCfg c; c.spiType = SpiType::Balloon;
        printf("balloon %s; ", B(computeSailState(120.f, 120.f, 100.f, NAN, 5.f, c).useSpinnaker));
        printf("no TWA (NaN), AWA 120, TWS 10: balloon %s\n", B(computeSailState(NAN, NAN, 120.f, 10.f, 5.f, c).useSpinnaker));
    }
    H("I2 Code Zero (sail.code_zero on): no downwind sail, 50 < |TWA| < 95, 2 < TWS < 14");
    const float czT[] = { 50.f, 50.1f, 80.f, 92.f, 94.9f, 95.f };
    const float czS[] = { 2.f, 2.1f, 13.9f, 14.f };
    for (float twa : czT) {
        printf("   TWA %s:", F(twa));
        for (float tws : czS) {
            SailCfg cn; cn.spiType = SpiType::None;     cn.codeZero = true;
            SailCfg cg; cg.spiType = SpiType::Gennaker; cg.codeZero = true;
            printf("  TWS %s none %s gennaker %s", F(tws),
                   B(computeSailState(twa, twa, twa - 15.f, tws, 5.f, cn).useCodeZero),
                   B(computeSailState(twa, twa, twa - 15.f, tws, 5.f, cg).useCodeZero));
        }
        printf("\n");
    }
    H("I3 luffing / running / lee side (before the latch)");
    struct L { float twa, awa; } ls[] = {
        { 27.9f, 20.f }, { 28.f, 21.f }, { 150.f, 138.f }, { 150.1f, 138.f }, { -40.f, -30.f },
        { NAN, 17.9f }, { NAN, 18.f }, { NAN, 140.f }, { NAN, 140.1f }, { NAN, -60.f }, { NAN, NAN },
        { 0.f, 0.f }, { 180.f, 180.f },
    };
    SailCfg c0;
    for (const L &l : ls) {
        const SailState s = computeSailState(fabsf(l.twa), l.twa, l.awa, 10.f, 5.f, c0);
        printf("   TWA %s AWA %s -> luffing %s running %s leewardSide %s\n", F(l.twa), F(l.awa),
               B(s.luffing), B(s.running), F(s.leewardSide));
    }
    H("I4 trim targets from |AWA|: boom = clamp(|AWA| - 13, 4, 86), headsail = 0.82 boom; awaAbs");
    const float aw[] = { 0.f, 10.f, 17.f, 20.f, 45.f, 90.f, 99.f, 100.f, 150.f, 180.f };
    for (float a : aw) {
        const SailState s = computeSailState(NAN, NAN, a, 10.f, 5.f, c0);
        printf("   |AWA| %s -> boom %s headsail %s awaAbs %s\n", F(a), F(s.boomAngleDeg), F(s.headsailAngleDeg), F(s.awaAbs));
    }
    {
        const SailState a = computeSailState(130.f, 130.f, NAN, 10.f, 5.f, c0);
        const SailState b = computeSailState(NAN, NAN, NAN, NAN, NAN, c0);
        printf("   AWA NaN, TWA 130 -> boom %s headsail %s awaAbs %s\n", F(a.boomAngleDeg), F(a.headsailAngleDeg), F(a.awaAbs));
        printf("   no wind at all  -> boom %s headsail %s awaAbs %s luffing %s running %s lee %s\n", F(b.boomAngleDeg),
               F(b.headsailAngleDeg), F(b.awaAbs), B(b.luffing), B(b.running), F(b.leewardSide));
    }
    H("I5 camber (belly / chord) from |TWA|; jib = 0.85 main; no TWA -> 0.16 / 0.14");
    const float ct[] = { 20.f, 34.9f, 35.f, 59.9f, 60.f, 99.9f, 100.f, 149.9f, 150.f, 180.f };
    for (float a : ct) {
        const SailState s = computeSailState(a, a, a, 10.f, 5.f, c0);
        printf("   |TWA| %s -> main %s jib %s\n", F(a), F(s.mainCamber), F(s.jibCamber));
    }
    {
        const SailState s = computeSailState(NAN, NAN, 60.f, 10.f, 5.f, c0);
        printf("   no TWA       -> main %s jib %s\n", F(s.mainCamber), F(s.jibCamber));
    }
    H("I6 reef count: TWS > 16/22/28 or STW / hull speed > 0.88/0.93/0.97; hull speed = 2.43 sqrt(LWL)");
    {
        SailCfg c; c.lwl = 9.f;
        printf("   LWL 9 -> hull speed %s kn\n", F(2.43f * sqrtf(9.f)));
        struct R { float tws, stw; } rs[] = {
            { 16.f, 0.f }, { 16.1f, 0.f }, { 22.f, 0.f }, { 22.1f, 0.f }, { 28.f, 0.f }, { 28.1f, 0.f },
            { 10.f, 6.41f }, { 10.f, 6.42f }, { 10.f, 6.78f }, { 10.f, 6.79f }, { 10.f, 7.07f }, { 10.f, 7.08f },
            { 10.f, NAN }, { NAN, 9.f },
        };
        for (const R &r : rs)
            printf("   TWS %s STW %s -> reef %d\n", F(r.tws), F(r.stw), computeSailState(90.f, 90.f, 70.f, r.tws, r.stw, c).reefCount);
        SailCfg z; z.lwl = 0.f;
        printf("   LWL 0 (treated as 9): TWS 10 STW 6.42 -> reef %d\n", computeSailState(90.f, 90.f, 70.f, 10.f, 6.42f, z).reefCount);
    }
}

// drawSailFoil()'s Bezier control point, in boat units (the firmware does the
// same in pixels and truncates every vertex to an integer).
static Pt foilCtrl(Pt tack, Pt clew, float leeSign, float camber) {
    const float lx = clew.x - tack.x, ly = clew.y - tack.y;
    const float len = sqrtf(lx * lx + ly * ly);
    float nx = -ly / len, ny = lx / len;
    if ((nx < 0.f) != (leeSign < 0.f)) { nx = -nx; ny = -ny; }
    Pt c;
    c.x = tack.x + lx * 0.45f + nx * camber * len;
    c.y = tack.y + ly * 0.45f + ny * camber * len;
    return c;
}

static void secRigGeometry() {
    H("I7 main, jib and Code Zero geometry, steady state, boat units (drawBoat / drawSailFoil / drawCodeZero)");
    printf("   TWS 10, STW 5, foresail_pct 100, lee from the sign; mast (0,-6), boom 32; jib tack (0,-30), length 30 fp\n");
    const float twas[] = { 45.f, 70.f, 90.f, 120.f, 150.f, 180.f };
    SailCfg c; c.spiType = SpiType::None; c.codeZero = true;
    for (float twa : twas) {
        float aws, awa;
        WindCalc::trueToApparent(10.f, twa, 5.f, aws, awa);
        Rig rig;
        const SailState s = frame(rig, twa, awa, 10.f, 5.f, 0, c);
        const float bss = rig.smBoom >= 0.f ? 1.f : -1.f, jss = rig.smJib >= 0.f ? 1.f : -1.f;
        const float bA = fabsf(rig.smBoom) * DEG2RAD, jA = fabsf(rig.smJib) * DEG2RAD;
        Pt mast; mast.x = 0.f; mast.y = SpiGeom::MAST_Y;
        Pt bow;  bow.x = 0.f;  bow.y = -30.f;
        Pt boomEnd; boomEnd.x = bss * 32.f * sinf(bA); boomEnd.y = mast.y + 32.f * cosf(bA);
        const float fp = 1.f;
        Pt jibClew; jibClew.x = jss * 30.f * fp * sinf(jA); jibClew.y = bow.y + 30.f * fp * cosf(jA);
        const Pt mc = foilCtrl(mast, boomEnd, bss, s.mainCamber);
        const Pt jc = foilCtrl(bow, jibClew, jss, s.jibCamber);
        printf("   TWA %s AWA %s: boom %s main clew (%s, %s) ctrl (%s, %s) camber %s",
               F(twa), F(awa), F(rig.smBoom), F(boomEnd.x), F(boomEnd.y), F(mc.x), F(mc.y), F(s.mainCamber));
        if (s.useCodeZero) {
            const float a = s.headsailAngleDeg * DEG2RAD, ss = s.leewardSide;
            Pt cz; cz.x = ss * 34.f * sinf(a); cz.y = bow.y + 34.f * cosf(a);
            const Pt cc = foilCtrl(bow, cz, ss, 0.13f);
            printf(" | Code Zero clew (%s, %s) ctrl (%s, %s)\n", F(cz.x), F(cz.y), F(cc.x), F(cc.y));
        } else if (!s.running) {
            printf(" | jib %s clew (%s, %s) ctrl (%s, %s) camber %s\n", F(rig.smJib), F(jibClew.x), F(jibClew.y), F(jc.x), F(jc.y), F(s.jibCamber));
        } else {
            printf(" | running: no jib\n");
        }
    }
}

static void secLatch() {
    H("J1 lee side held through a dead run (latchLeeSide in drawInstrument), TWA sequence");
    struct S { float twa, awa; const char *note; } ss[] = {
        { 170.f, 160.f, "first value: from the sign" },
        { 176.f, 172.f, "" }, { 179.f, 178.f, "" }, { 180.f, 180.f, "" },
        { -179.f, -178.f, "beyond 175: kept" }, { -177.f, -175.f, "" }, { -176.f, -173.f, "" },
        { -175.5f, -172.f, "" }, { -175.f, -171.f, "175 or less: new side" },
        { 179.f, 178.f, "kept" }, { 176.f, 172.f, "" }, { 175.f, 170.f, "new side" },
        { -175.1f, -171.f, "kept" }, { NAN, NAN, "no wind: forgotten" },
        { -179.f, -178.f, "first after no wind: from the sign" }, { 179.f, 178.f, "kept" },
        { NAN, 177.f, "TWA lost, AWA 177: kept (|AWA| > 175)" }, { NAN, -170.f, "AWA -170: new side" },
    };
    Rig rig;
    SailCfg c;
    for (const S &s : ss) {
        const SailState st = frame(rig, s.twa, s.awa, 10.f, 5.f, 0, c);
        const float sideAng = !isnan(s.twa) ? s.twa : s.awa;
        const float fromSign = (isnan(sideAng) ? 0.f : sideAng) >= 0.f ? -1.f : 1.f;
        printf("   TWA %9s AWA %9s  sign-side %s  kept %s  drawn %s   %s\n", F(s.twa), F(s.awa),
               F(fromSign), F(rig.leeSide), F(st.leewardSide), s.note);
    }
}

static void secSmoothing() {
    H("K1 smoothing, k = 0.18 per frame, balloon, TWS 10 STW 5: frame 0 TWA 60 (first frame: targets at once), then TWA 120");
    {
        Rig rig;
        SailCfg c; c.spiType = SpiType::Balloon;
        printf("   frame       TWA        AWA |   smBoom    smJib smSpiRot    smGen | drawn\n");
        struct W { float twa; } ws[] = { {60}, {120}, {120}, {120}, {120}, {120}, {120}, {120}, {120} };
        int i = 0;
        for (const W &w : ws) {
            float aws, awa;
            WindCalc::trueToApparent(10.f, w.twa, 5.f, aws, awa);
            frame(rig, w.twa, awa, 10.f, 5.f, 0, c);
            printf("   %5d %9s %10s | %8s %8s %8s %8s | %s\n", i++, F(w.twa), F(awa), F(rig.smBoom), F(rig.smJib),
                   F(rig.smSpiRot), F(rig.smGen), B(rig.spiDrawn));
        }
    }
    H("K2 gybe at TWA 150 -> -150 (TWS 10 STW 5): boom swings, gennaker jumps, pole side follows the boom");
    {
        Rig rig;
        SailCfg c; c.spiType = SpiType::Balloon;
        printf("   frame       TWA |   smBoom smSpiRot    smGen | lee  poleSide(spiRigSide) genSide\n");
        for (int i = 0; i < 12; i++) {
            const float twa = (i < 2) ? 150.f : -150.f;
            float aws, awa;
            WindCalc::trueToApparent(10.f, twa, 5.f, aws, awa);
            const SailState s = frame(rig, twa, awa, 10.f, 5.f, 0, c);
            printf("   %5d %9s | %8s %8s %8s | %s %s %s\n", i, F(twa), F(rig.smBoom), F(rig.smSpiRot), F(rig.smGen),
                   F(s.leewardSide), F(spiRigSide(rig.smBoom)), F(rig.smGen >= 0.f ? 1.f : -1.f));
        }
        rig.resetRig();
        float aws, awa;
        WindCalc::trueToApparent(10.f, 150.f, 5.f, aws, awa);
        frame(rig, 150.f, awa, 10.f, 5.f, 0, c);
        printf("   resetRig(), then TWA 150: smBoom %s smSpiRot %s smGen %s (targets at once)\n", F(rig.smBoom), F(rig.smSpiRot), F(rig.smGen));
    }
    H("K3 butterfly (sail.butterfly on, running): jib target -lee * 84, no kite drawn");
    {
        Rig rig;
        SailCfg c; c.spiType = SpiType::Balloon; c.butterfly = true;
        float aws, awa;
        WindCalc::trueToApparent(10.f, 165.f, 5.f, aws, awa);
        const SailState s = frame(rig, 165.f, awa, 10.f, 5.f, 0, c);
        printf("   TWA 165 AWA %s: running %s useSpinnaker %s smJib %s smBoom %s drawn %s\n", F(awa), B(s.running),
               B(s.useSpinnaker), F(rig.smJib), F(rig.smBoom), B(rig.spiDrawn));
    }
    H("K4 luffing (|TWA| < 28): flap targets boom 15 sin(0.018 t), jib 17 sin(0.018 t + 0.7), t = tick [ms]");
    {
        const uint32_t ticks[] = { 0, 50, 100, 175, 349 };
        for (uint32_t tk : ticks)
            printf("   t %4u ms: boom %s jib %s\n", (unsigned)tk, F(sinf((float)tk * 0.018f) * 15.f), F(sinf((float)tk * 0.018f + 0.7f) * 17.f));
    }
}

static void printSym(float awaAbs, float lee) {
    const float rot = lee * SpiGeom::symRotationDeg(awaAbs);
    Pt foot[SpiGeom::SYM_N + 1], lead[SpiGeom::SYM_N + 1];
    SpiGeom::symOutline(rot, foot, lead);
    const int iT = SpiGeom::symTackIndex(lee), iC = SpiGeom::symClewIndex(lee);
    float rmax = 0.f; int imax = 0;
    for (int i = 0; i <= SpiGeom::SYM_N; i++) {
        const float r = sqrtf(lead[i].x * lead[i].x + lead[i].y * lead[i].y);
        if (r > rmax) { rmax = r; imax = i; }
    }
    const Pt t = foot[iT], c = foot[iC];
    const float dx = t.x, dy = t.y - SpiGeom::MAST_Y;
    printf("   |AWA| %s lee %s: rot %s | tack[%d] (%s, %s) clew[%d] (%s, %s) | crest lead[12] (%s, %s) r %s | max r %s at lead[%d] / lead[%d] (mirror pair, equal radius)\n",
           F(awaAbs), F(lee), F(rot), iT, F(t.x), F(t.y), iC, F(c.x), F(c.y),
           F(lead[12].x), F(lead[12].y), F(sqrtf(lead[12].x * lead[12].x + lead[12].y * lead[12].y)), F(rmax),
           imax < SpiGeom::SYM_N - imax ? imax : SpiGeom::SYM_N - imax,
           imax < SpiGeom::SYM_N - imax ? SpiGeom::SYM_N - imax : imax);
    printf("        foot[12] (%s, %s) foot[6] (%s, %s) lead[6] (%s, %s) lead[18] (%s, %s) | pole %s deg, length %s\n",
           F(foot[12].x), F(foot[12].y), F(foot[6].x), F(foot[6].y), F(lead[6].x), F(lead[6].y),
           F(lead[18].x), F(lead[18].y), F(poleAngle(t)), F(sqrtf(dx * dx + dy * dy)));
}

static void printGen(float awaAbs, float lee) {
    const float a = lee * SpiGeom::genSheetDeg(awaAbs);
    SpiGeom::GenOutline o;
    const bool ok = SpiGeom::genOutline(a, o);
    const int N = SpiGeom::GEN_N;
    float rmax = 0.f; int imax = 0;
    for (int i = 0; i <= N; i++) {
        const float r = sqrtf(o.lee[i].x * o.lee[i].x + o.lee[i].y * o.lee[i].y);
        if (r > rmax) { rmax = r; imax = i; }
    }
    const float cx = o.lee[N].x - o.lee[0].x, cy = o.lee[N].y - o.lee[0].y;
    printf("   |AWA| %s lee %s: sheet %s ok %s | tack (%s, %s) clew (%s, %s) chord %s | lee[8] (%s, %s) luff[8] (%s, %s) panel[8] (%s, %s)\n",
           F(awaAbs), F(lee), F(a), B(ok), F(o.lee[0].x), F(o.lee[0].y), F(o.lee[N].x), F(o.lee[N].y),
           F(sqrtf(cx * cx + cy * cy)), F(o.lee[8].x), F(o.lee[8].y), F(o.luff[8].x), F(o.luff[8].y),
           F(o.panel[8].x), F(o.panel[8].y));
    printf("        lee[4] (%s, %s) lee[12] (%s, %s) | seams", F(o.lee[4].x), F(o.lee[4].y), F(o.lee[12].x), F(o.lee[12].y));
    for (int s = 0; s < SpiGeom::GEN_SEAMS; s++) {
        const int e = SpiGeom::genSeamEnd(s), b = e - SpiGeom::GEN_SEAM_BACK;
        printf(" panel[%d] (%s, %s) -> lee[%d] (%s, %s)", b, F(o.panel[b].x), F(o.panel[b].y), e, F(o.lee[e].x), F(o.lee[e].y));
    }
    printf(" | max r %s at lee[%d]\n", F(rmax), imax);
}

static void secKite() {
    H("L1 rotation and sheeting laws");
    const float aw[] = { 180.f, 175.f, 172.f, 171.f, 168.1f, 165.f, 160.f, 150.f, 135.f, 120.f, 116.f,
                         110.f, 99.8f, 90.f, 80.f, 44.f, 11.8f, NAN };
    for (float a : aw)
        printf("   |AWA| %s -> symRotationDeg %s  genSheetDeg %s\n", F(a), F(SpiGeom::symRotationDeg(a)), F(SpiGeom::genSheetDeg(a)));
    printf("   balloonBandEdge j=0..5:");
    for (int j = 0; j <= SpiGeom::BALLOON_BANDS; j++) printf(" %s", F(SpiGeom::balloonBandEdge(j)));
    printf("\n   rig points lee +1: quarterBlock (%s, %s) sheetWinch (%s, %s) twing (%s, %s) guyWinch (%s, %s)\n",
           F(SpiGeom::quarterBlock(1.f).x), F(SpiGeom::quarterBlock(1.f).y), F(SpiGeom::sheetWinch(1.f).x),
           F(SpiGeom::sheetWinch(1.f).y), F(SpiGeom::twing(1.f).x), F(SpiGeom::twing(1.f).y),
           F(SpiGeom::guyWinch(1.f).x), F(SpiGeom::guyWinch(1.f).y));
    printf("   rig points lee -1: quarterBlock (%s, %s) twing (%s, %s)\n", F(SpiGeom::quarterBlock(-1.f).x),
           F(SpiGeom::quarterBlock(-1.f).y), F(SpiGeom::twing(-1.f).x), F(SpiGeom::twing(-1.f).y));

    H("L2 symmetric kite (gores, balloon), steady state, boat units");
    const float sa[] = { 180.f, 172.f, 165.f, 150.f, 135.f, 116.f, 100.f, 80.f };
    for (float a : sa) printSym(a, 1.f);
    printSym(135.f, -1.f);
    printSym(11.8f, 1.f);

    H("L2b panel seams (gores) and band edges (balloon) at sample 12, lee +1");
    const float rots[] = { 0.f, 14.f };
    for (float r : rots) {
        Pt foot[SpiGeom::SYM_N + 1], lead[SpiGeom::SYM_N + 1];
        SpiGeom::symOutline(r, foot, lead);
        printf("   rot %s gores seams foot[i] -> lead[i]:", F(r));
        for (int g = 1; g < SpiGeom::SYM_GORES; g++) {
            const int i = g * (SpiGeom::SYM_N / SpiGeom::SYM_GORES);
            printf(" i%d (%s, %s)->(%s, %s)", i, F(foot[i].x), F(foot[i].y), F(lead[i].x), F(lead[i].y));
        }
        printf("\n   rot %s balloon band edges j=0..5 at i=12:", F(r));
        for (int j = 0; j <= SpiGeom::BALLOON_BANDS; j++) {
            const Pt p = SpiGeom::lerp(foot[12], lead[12], SpiGeom::balloonBandEdge(j));
            printf(" (%s, %s)", F(p.x), F(p.y));
        }
        printf("\n");
    }

    H("L3 gennaker, steady state, boat units");
    const float ga[] = { 90.f, 99.8f, 116.f, 135.f, 150.f };
    for (float a : ga) printGen(a, 1.f);
    printGen(116.f, -1.f);

    H("L4 kites in pixels: (CX + x BS, CY + y BS); L2 kite at rot 0 with vertices rounded floor(v + 0.5); gennaker tack row floor(y), hull tip vertex truncated");
    struct Bd { const char *n; float sf; } bds[] = { { "4-inch", 1.0f }, { "7B/5B", 1.25f } };
    for (const Bd &bd : bds) {
        const float CX = 240.f * bd.sf, CY = 226.f * bd.sf, BS = 3.8f * bd.sf;
        Pt foot[SpiGeom::SYM_N + 1], lead[SpiGeom::SYM_N + 1];
        SpiGeom::symOutline(0.f, foot, lead);
        printf("   %-6s CX %s CY %s BS %s R_INNER %s | rot 0: tack px (%s, %s) -> (%d, %d), crest px (%s, %s) -> (%d, %d)\n",
               bd.n, F(CX), F(CY), F(BS), F(148.f * bd.sf), F(CX + foot[0].x * BS), F(CY + foot[0].y * BS),
               (int)floorf(CX + foot[0].x * BS + 0.5f), (int)floorf(CY + foot[0].y * BS + 0.5f),
               F(CX + lead[12].x * BS), F(CY + lead[12].y * BS),
               (int)floorf(CX + lead[12].x * BS + 0.5f), (int)floorf(CY + lead[12].y * BS + 0.5f));
        // Gennaker tack against the stem tip as drawBoat() draws it: the hull
        // station y = -32 truncated to a whole pixel (lv_coord_t), with the
        // 2-px outline through it (its front pixel row is one row further up).
        const float ty = CY + SpiGeom::GEN_TACK_Y * BS;
        const int   hv = (int)(short)(CY - 32.f * BS);
        printf("   %-6s gennaker tack px (%s, %s), pixel row %d | hull tip vertex row %d (= %s units), outline front row %d\n",
               bd.n, F(CX), F(ty), (int)floorf(ty), hv, F(((float)hv - CY) / BS), hv - 1);
    }
}

static void secColours() {
    H("M1 kite tones: mix(fg, bg, m) = (fg m + bg (255 - m)) / 255; 8-bit ideal and the device's RGB565");
    struct P { const char *n; uint32_t orange, inner; } ps[] = {
        { "dark",  0xFF8A3D, 0x081521 },
        { "light", 0xD2691E, 0xEEF2F6 },
        { "night", 0xC25E2A, 0x120404 },
    };
    for (const P &p : ps) {
        printf("   %-5s ORANGE #%06X  C_INNER #%06X\n", p.n, (unsigned)p.orange, (unsigned)p.inner);
        const uint16_t o5 = to565(p.orange), i5 = to565(p.inner);
        struct T { const char *n; int a, b; } ts[] = {   // b < 0: single mix
            { "gores p1 = mix(O,I,215)", 215, -1 },
            { "gores p2 = mix(mix(O,I,140),I,215)", 140, 215 },
            { "balloon band 0 = mix(O,I,140)", 140, -1 },
            { "balloon band 1 = mix(O,I,168)", 168, -1 },
            { "balloon band 2 = mix(O,I,197)", 197, -1 },
            { "balloon band 3 = mix(O,I,226)", 226, -1 },
            { "balloon band 4 = mix(O,I,255)", 255, -1 },
            { "balloon seam = mix(O,I,100)", 100, -1 },
            { "gennaker luff/seams c2 = mix(O,I,140)", 140, -1 },
        };
        for (const T &t : ts) {
            uint32_t c8 = mix888(p.orange, p.inner, t.a);
            uint16_t c5 = mix565(o5, i5, (uint8_t)t.a);
            if (t.b >= 0) { c8 = mix888(c8, p.inner, t.b); c5 = mix565(c5, i5, (uint8_t)t.b); }
            printf("      %-40s #%06X   RGB565 0x%04X\n", t.n, (unsigned)c8, (unsigned)c5);
        }
    }
}

int main() {
    printf("# Wind & sail test vectors - docs/WIND-AND-SAILS.md section 7\n");
    printf("# Real code: WindCalc.cpp, WindDampCfg.h, SpiGeom.cpp. Restated: WindScreen sail logic,\n");
    printf("# Config spi_type load, index.html spiTypeOf, lv_color_mix. Values %%.4f, NaN = not known.\n");
    printf("# Units: kn, deg, s, ms; boat units (x + starboard, y + aft, origin boat centre).\n");
    printf("# Constants: CALM_KN %s COG_MIN_SOG_KN %s COG_OFF_SOG_KN %s FRESH_MS %u STALE_MS %u\n",
           F(WindCalc::CALM_KN), F(WindCalc::COG_MIN_SOG_KN), F(WindCalc::COG_OFF_SOG_KN),
           (unsigned)WindCalc::FRESH_MS, (unsigned)WindCalc::STALE_MS);
    printf("# WindDampCfg MIN_S %s MAX_S %s DEFAULT_S %s\n", F(WindDampCfg::MIN_S), F(WindDampCfg::MAX_S), F(WindDampCfg::DEFAULT_S));
    secNorm();
    secTrue();
    secHeading();
    secCog();
    secReceived();
    secDamping();
    secMigration();
    secSpiConfig();
    secWindows();
    secRigGeometry();
    secLatch();
    secSmoothing();
    secKite();
    secColours();
    return 0;
}
