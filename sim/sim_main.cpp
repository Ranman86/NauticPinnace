// ============================================================
// sim_main.cpp  –  NauticPinnace PC Simulator entry point
//
// Runs the same DisplayManager + screens on Windows/Linux via SDL2.
// Demo data animates automatically (no ESP32 needed).
//
// Build:  pio run -e simulator
// Run:    .pio/build/simulator/program.exe
// ============================================================

#include "sim_hal.h"
#include "arduino_stubs.h"

// Pull in the app headers
#include "../src/display/DisplayManager.h"
#include "../src/display/ConfigOverlay.h"
#include "../src/display/HomeOverlay.h"   // --shots: the launcher is part of the set
#include "../src/display/LicenseOverlay.h"
#include "../src/display/LanguageOverlay.h"
#include "../src/display/UpdateOverlay.h"   // --n2kupdate-selftest
#include "../src/display/BootScreen.h"
#include "../src/display/Theme.h"
#include "../src/display/UiConfig.h"
#include "../src/nmea/DataModel.h"
#include "../src/nmea/DemoData.h"
#include "../src/config/Config.h"
#include "../src/Entropy.h"
#include "../src/SunCalc.h"   // --set days: civil date <-> days since 1970
#include "../src/display/screens/AnchorScreen.h"   // --set anchortrack: g_simAnchorTrack

// ── Global instances (normally defined by main.cpp on ESP32) ─────────────────
// appConfig is defined in Config.cpp – do NOT redefine here
// data is defined here since main.cpp is excluded from the simulator build
DataModel  data;

// --n2kupdate-demo (sim_n2kupdate_demo.cpp): false once the demo is over.
bool simUpdDemoTick(uint32_t now, const char *shotsDir);
// --n2kupdate-selftest: -1 while running, then the exit code (0 = pass).
int simUpdHoldSelfTest(uint32_t now, lv_obj_t *installBtn);
// --ap-frames (sim_ap_frames.cpp): -1 while running, then the failed steps.
int simApFramesTick(uint32_t now, const char *shotsDir);

// ── getTickFps (simulator: counts presented SDL frames) ───────────────────────
// g_sim_frames is incremented in sim_flush_cb() when a full frame is presented.
extern uint32_t g_sim_frames;
uint32_t getTickFps(bool reset) {
    uint32_t v = g_sim_frames;
    if (reset) g_sim_frames = 0;
    return v;
}
#include "../src/config/Config.h"
#include "../src/PsramArena.h"
#include "../src/PolarTable.h"

#include <lvgl.h>
#include <SDL2/SDL.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>     // snprintf - used by the --shots file naming
#include <cstdlib>    // atoi
#include <cctype>     // tolower
#include <string>
#include <vector>     // --set ring specs: comma-separated fields

// ── LVGL draw buffer (SRAM on PC) ────────────────────────────────────────────
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t         s_buf1[UI_SCREEN_W * 20];
static lv_color_t         s_buf2[UI_SCREEN_W * 20];

// ── Load the REAL device config so the sim shows the device's colours ────────
// Without this the simulator ran on the code-default palette while the panel used
// the 56 roles stored in data/config.json — so the two never matched and every
// colour check in the sim was meaningless. The device does the same thing in
// Config::begin(): defaults first, file on top.
// The exe runs from .pio/build/simulator, hence the walk back up.
static bool sim_load_device_config(const char *explicitPath) {
    static const char *candidates[] = {
        "data/config.json",
        "../../../data/config.json",
        "../../data/config.json",
    };
    const char *tried[4];
    int nTried = 0;
    const char *paths[4];
    int nPaths = 0;
    if (explicitPath) paths[nPaths++] = explicitPath;
    for (const char *c : candidates) paths[nPaths++] = c;

    for (int i = 0; i < nPaths; i++) {
        FILE *f = fopen(paths[i], "rb");
        tried[nTried++] = paths[i];
        if (!f) continue;
        std::string json;
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) json.append(chunk, n);
        fclose(f);
        // Strip a UTF-8 BOM: ArduinoJson rejects it.
        if (json.size() >= 3 && (unsigned char)json[0] == 0xEF &&
            (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF)
            json.erase(0, 3);
        if (appConfig.fromJson(String(json.c_str()))) {
            printf("[sim] config loaded from %s  (theme='%s', lang='%s')\n",
                   paths[i], appConfig.cfg.themeActive, appConfig.cfg.lang);
            return true;
        }
        printf("[sim] %s found but could NOT be parsed — using defaults\n", paths[i]);
        return false;
    }
    printf("[sim] no data/config.json found (tried %d paths) — using code defaults\n", nTried);
    return false;
}

// ── Load the polar the device ships (data/polar.json), as for the config ─────
// First next to a config given with --cfg (data/config.json usually), then the
// same relative places as the config.
static void sim_load_polar(const char *cfgPath) {
    std::string beside;
    if (cfgPath) {
        beside = cfgPath;
        const size_t cut = beside.find_last_of("/\\");
        beside = (cut == std::string::npos ? std::string() : beside.substr(0, cut + 1)) + "polar.json";
    }
    const char *candidates[] = {
        beside.empty() ? nullptr : beside.c_str(),
        "data/polar.json",
        "../../../data/polar.json",
        "../../data/polar.json",
    };
    for (const char *p : candidates) {
        if (!p) continue;
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        std::string json;
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) json.append(chunk, n);
        fclose(f);
        if (json.size() >= 3 && (unsigned char)json[0] == 0xEF &&
            (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF)
            json.erase(0, 3);
        const bool ok = gPolar().fromJson(String(json.c_str()));
        printf("[sim] polar %s from %s\n", ok ? "loaded" : "could NOT be parsed", p);
        return;
    }
    printf("[sim] no data/polar.json found - Speed shows \"--\" for Polar and Perf\n");
}

// ── Demo tick thread ──────────────────────────────────────────────────────────
// Screen title -> file name stem for --shots. Titles are translated, so they
// carry German umlauts in UTF-8; those are transliterated rather than dropped,
// which keeps "Uebersicht" readable instead of turning it into "_bersicht".
// Everything else outside [A-Za-z0-9] collapses to a single underscore.
static void shotNameFromTitle(const char *title, char *out, size_t outSz) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)title; *p && o + 3 < outSz; p++) {
        const char *rep = nullptr;
        if (*p == 0xC3 && p[1]) {                 // two-byte Latin-1 supplement
            switch (*++p) {
                case 0x84: case 0xA4: rep = "ae"; break;   // A-umlaut / a-umlaut
                case 0x96: case 0xB6: rep = "oe"; break;   // O-umlaut / o-umlaut
                case 0x9C: case 0xBC: rep = "ue"; break;   // U-umlaut / u-umlaut
                case 0x9F:            rep = "ss"; break;   // sharp s
                default:              rep = "_";  break;
            }
        }
        if (rep) { while (*rep && o + 1 < outSz) out[o++] = *rep++; continue; }
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9')) out[o++] = (char)tolower(*p);
        else if (o > 0 && out[o - 1] != '_')      out[o++] = '_';
    }
    while (o > 0 && out[o - 1] == '_') o--;       // no trailing underscore
    out[o] = '\0';
    if (o == 0) snprintf(out, outSz, "screen");
}

static std::atomic<bool> s_running{true};

static std::atomic<bool> s_demo_enabled{false};
// --demo-stop S: the demo stops feeding the model S seconds after it started,
// and keeps its last values - which then age like a sensor that went quiet,
// until the bus warning comes up (BusWatch.h). Negative = never.
static double s_demoStopS = -1.0;

// ── Test harness: --freeze-demo / --set / --ais-seed ─────────────────────────
// What makes two --shots runs comparable pixel for pixel (tools/cmp_shots.py).
// --freeze-demo lives in DemoData (g_simFreezeT, its time base); the values
// below are laid over the demo's by simDemoOverride(), which DemoData::tick()
// calls at the end of its locked block. That is still the demo thread, right
// after the demo has written its own values, but under the SAME lock: a second
// lock taken after tick() returned would leave a window in which a screen
// reads the demo's rudder angle or date and paints a frame of it. All of these
// are set before the demo thread starts and only read afterwards.
static bool   s_setRudder  = false;    // --set rudder=DEG
static float  s_rudderDeg  = NAN;
static long   s_setDays    = -1;       // --set days=...: days since 1970-01-01, -1 = the demo's own
static double s_setSecBase = 43200.0;  // ... and the UTC time of day the demo time t is added to
static int    s_aisSeed    = 0;        // --ais-seed N
// --set depth/roll/pitch/rot=VALUE: a plain value over the demo's, "nan" for
// none. Written on every demo tick like rudder=.
static bool   s_setDepth = false, s_setRoll = false, s_setPitch = false, s_setRot = false;
static float  s_depthM   = NAN, s_rollDeg = NAN, s_pitchDeg = NAN, s_rotDegMin = NAN;
static bool   s_setNoFix = false;      // --set nofix=1: no own position, the rest of the demo runs on

// --set depthhist / windhist / anchortrack=PROFILE[,N[,IDX]][,live]: one of
// the history rings laid out by a fixed recipe, so the Depth echogram, the
// wind rose and the anchor breadcrumbs no longer depend on how long the sim
// has been running. N samples (default: a full ring), oldest first; IDX puts
// the oldest one of a FULL ring into that slot, i.e. a ring that has wrapped
// round (the write index then is IDX, not 0). The ring is rewritten on every
// tick and its own writer held off, so it stands; "live" lays it once and then
// lets the normal writer (demo depth every 5 s, the 1 Hz wind clock, the 3-s
// breadcrumb sampler) carry on from there - a quick way to a full ring for an
// unfrozen run, never for a reference picture.
struct SimRing {
    bool on      = false;
    int  profile = 0;       // index into the key's profile names
    int  n       = 0;       // samples
    int  idx     = 0;       // full ring: slot of the oldest sample
    bool live    = false;
    bool seeded  = false;   // live: laid already
};
static SimRing s_depthHist, s_windHist, s_anchorTrk;
static const char *const kDepthProfiles[] = { "flat", "rising", "falling", "spike",
                                              "shallow", "sine", "small" };
static const char *const kWindProfiles[]  = { "rose", "steady", "veer" };
static const char *const kTrackProfiles[] = { "swing", "drift" };
// AnchorScreen::TRACK_N, which is private; simAnchorTrack() checks it.
static constexpr int kSimTrackN = 240;

// --set anchor=DN,DE,RADIUS[,alarm]: an anchor DN m north and DE m east of the
// demo's swing centre (53.5 N, 10.0 E), RADIUS m of swing circle, the alarm
// armed or not. Set in the config at start, like a device that boots with an
// anchor down; heading-up stays the config file's anchor.north_up.
static bool   s_setAnchor = false, s_ancAlarm = false;
static float  s_ancDn = 0.f, s_ancDe = 0.f, s_ancR = 40.f;

// --set days=VALUE. An integer N (negative too) is N days from DemoData's
// demo date 2024-06-21; YYYY-MM-DD is that civil date. Either may carry
// THH:MM or THH:MM:SS, the UTC time of day that replaces the demo's 12:00.
static bool parseSetDays(const char *v, long &days, double &secBase) {
    int y = 0, n = 0; unsigned mo = 0, d = 0;
    const char *rest;
    if (sscanf(v, "%d-%u-%u%n", &y, &mo, &d, &n) == 3) {
        if (y < 1970 || y > 2149 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
        days = scDaysFromCivil(y, mo, d);
        // 2024-02-31 would quietly become 2024-03-02: refuse what does not
        // come back as the same date.
        int y2; unsigned mo2, d2;
        scCivilFromDays(days, y2, mo2, d2);
        if (y2 != y || mo2 != mo || d2 != d) return false;
        rest = v + n;
    } else {
        char *end;
        const long off = strtol(v, &end, 10);
        if (end == v) return false;
        days = scDaysFromCivil(2024, 6, 21) + off;
        rest = end;
    }
    if (days < 0 || days > 65535) return false;   // DataModel::sysDays is 16 bit
    secBase = 43200.0;
    if (*rest == 'T') {
        int hh = 0, mm = 0, ss = 0, m = 0;
        if (sscanf(rest + 1, "%d:%d%n", &hh, &mm, &m) != 2) return false;
        rest += 1 + m;
        if (*rest == ':') {
            if (sscanf(rest + 1, "%d%n", &ss, &m) != 1) return false;
            rest += 1 + m;
        }
        if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return false;
        secBase = hh * 3600.0 + mm * 60.0 + ss;
    }
    return *rest == '\0';
}

// A --set value: a number within +-lim, or "nan" (no value) where allowed.
static bool parseSetFloat(const char *v, float lim, float &out) {
    char *end;
    const float f = strtof(v, &end);
    if (end == v || *end) return false;
    if (!isnan(f) && (!std::isfinite(f) || fabsf(f) > lim)) return false;
    out = f;
    return true;
}

static bool parseSetCount(const std::string &s, long lo, long hi, int &out) {
    char *end;
    const long v = strtol(s.c_str(), &end, 10);
    if (s.empty() || *end || v < lo || v > hi) return false;
    out = (int)v;
    return true;
}

static std::vector<std::string> splitCommas(const char *v) {
    std::vector<std::string> t;
    const std::string s(v);
    for (size_t p = 0;;) {
        const size_t c = s.find(',', p);
        t.push_back(s.substr(p, c == std::string::npos ? std::string::npos : c - p));
        if (c == std::string::npos) return t;
        p = c + 1;
    }
}

// PROFILE[,N[,IDX]][,live] for a ring of `cap` samples (see SimRing).
static bool parseRingSpec(const char *v, const char *const *names, int nNames,
                          int cap, SimRing &r) {
    std::vector<std::string> t = splitCommas(v);
    SimRing s;
    s.profile = -1;
    for (int i = 0; i < nNames; i++) if (t[0] == names[i]) s.profile = i;
    if (s.profile < 0) return false;
    if (t.size() > 1 && t.back() == "live") { s.live = true; t.pop_back(); }
    if (t.size() > 3) return false;
    s.n = cap;
    if (t.size() > 1 && !parseSetCount(t[1], 0, cap, s.n)) return false;
    // A ring that is not full has never wrapped: its oldest sample is slot 0.
    if (t.size() > 2 && (s.n != cap || !parseSetCount(t[2], 0, cap - 1, s.idx))) return false;
    s.on = true;
    r = s;
    return true;
}

// DN,DE,RADIUS[,alarm]
static bool parseSetAnchor(const char *v) {
    const std::vector<std::string> t = splitCommas(v);
    if (t.size() < 3 || t.size() > 4) return false;
    float dn, de, r;
    if (!parseSetFloat(t[0].c_str(), 5000.f, dn) || isnan(dn)) return false;
    if (!parseSetFloat(t[1].c_str(), 5000.f, de) || isnan(de)) return false;
    if (!parseSetFloat(t[2].c_str(), 500.f, r) || isnan(r) || r < 5.f) return false;
    if (t.size() == 4 && t[3] != "alarm") return false;
    s_setAnchor = true;
    s_ancDn = dn; s_ancDe = de; s_ancR = r;
    s_ancAlarm = t.size() == 4;
    return true;
}

// --set KEY=VALUE; false for an unknown key or a value that does not parse.
static bool parseSetArg(const char *kv) {
    const char *eq = strchr(kv, '=');
    if (!eq || !eq[1]) return false;
    const std::string key(kv, eq - kv);
    const char *v = eq + 1;
    if (key == "rudder") {
        char *end;
        const float deg = strtof(v, &end);   // "nan" parses too: no angle
        if (end == v || *end) return false;
        if (!isnan(deg) && (!std::isfinite(deg) || fabsf(deg) > 90.f)) return false;
        s_setRudder = true; s_rudderDeg = deg;
        return true;
    }
    if (key == "days") return parseSetDays(v, s_setDays, s_setSecBase);
    if (key == "depth") return s_setDepth = parseSetFloat(v, 1000.f, s_depthM);
    if (key == "roll")  return s_setRoll  = parseSetFloat(v, 180.f, s_rollDeg);
    if (key == "pitch") return s_setPitch = parseSetFloat(v, 90.f, s_pitchDeg);
    if (key == "rot")   return s_setRot   = parseSetFloat(v, 3600.f, s_rotDegMin);
    if (key == "depthhist")
        return parseRingSpec(v, kDepthProfiles, 7, DataModel::DEPTH_HIST, s_depthHist);
    if (key == "windhist")
        return parseRingSpec(v, kWindProfiles, 3, DataModel::WIND_HIST, s_windHist);
    if (key == "anchortrack")
        return parseRingSpec(v, kTrackProfiles, 2, kSimTrackN, s_anchorTrk);
    if (key == "anchor") return parseSetAnchor(v);
    if (key == "nofix")  return s_setNoFix = strcmp(v, "1") == 0;
    return false;
}

// --ais-seed N: N synthetic targets next to the demo's own one, laid out
// around own ship as the demo has just left it. A target with a COG is placed
// BACKWARDS from the CPA/TCPA it is meant to have - relative motion along a
// bearing, passing `cpa` off to one side, `tcpa` minutes from now - so its
// colour class holds whatever course and speed the demo gives own ship and
// whatever alarm thresholds the config sets (AisScreen: red below cpa/tcpa,
// yellow below twice that). The track length to the CPA is capped so that a
// target starts inside the radar range; only a range below about 3.5 times
// the CPA alarm (the green ones pass at 3 times it) pushes one off. The pattern
// repeats every nine targets: red, yellow and green, each with and without a
// name, then dots without a COG and a triangle without a SOG, which have no
// CPA and so stay green. Bearings follow the golden angle, so any N spreads
// round the radar. MMSI 200000001.. - MID 200 belongs to no country.
enum SeedKind : uint8_t { SK_RED, SK_YELLOW, SK_GREEN, SK_NO_COG, SK_NO_SOG };
static const struct { SeedKind kind; bool named; } kSeed[] = {
    { SK_RED,    true  }, { SK_YELLOW, false }, { SK_GREEN,  true  },
    { SK_NO_COG, false }, { SK_RED,    false }, { SK_YELLOW, true  },
    { SK_GREEN,  false }, { SK_NO_COG, true  }, { SK_NO_SOG, true  },
};

static void simSeedAis(uint32_t nowMs) {
    const float ownLat = data.lat, ownLon = data.lon;
    if (isnan(ownLat) || isnan(ownLon)) return;
    const float D2R = (float)(M_PI / 180.0);
    const float v0  = isnan(data.sog) ? 0.f : data.sog;
    const float c0  = isnan(data.cog) ? 0.f : data.cog * D2R;
    const float v0e = v0 * sinf(c0), v0n = v0 * cosf(c0);   // own velocity, kn east/north
    float cpaAl = appConfig.cfg.aisCpaAlarm, tcpaAl = appConfig.cfg.aisTcpaAlarm;
    if (!(cpaAl  > 0.f)) cpaAl  = 0.5f;
    if (!(tcpaAl > 0.f)) tcpaAl = 10.f;
    const float range = appConfig.cfg.aisRange > 0 ? (float)appConfig.cfg.aisRange : 5.f;
    const int   nKinds = (int)(sizeof(kSeed) / sizeof(kSeed[0]));

    for (int k = 0; k < s_aisSeed; k++) {
        const SeedKind kind = kSeed[k % nKinds].kind;
        AisTarget *a = data.findOrCreateAis(200000001u + (uint32_t)k);
        const float b  = fmodf(k * 137.5f + 25.f, 360.f) * D2R;
        const float ue = sinf(b), un = cosf(b);   // unit vector along b
        float e, n;                               // position off own ship, nm east/north
        a->sog = a->cog = a->hdg = NAN;
        a->cpa = a->tcpa = NAN;
        if (kind == SK_RED || kind == SK_YELLOW || kind == SK_GREEN) {
            const float cpa  = (kind == SK_RED) ? 0.5f * cpaAl
                             : (kind == SK_YELLOW) ? 1.5f * cpaAl : 3.0f * cpaAl;
            const float tcpa = (kind == SK_RED) ? 0.5f * tcpaAl
                             : (kind == SK_YELLOW) ? 1.2f * tcpaAl : 0.8f * tcpaAl;
            float d = (6.f + 2.f * (float)(k % 4)) * tcpa / 60.f;   // 6..12 kn relative
            // Start no further out than 0.85 of the range (sqrt(cpa^2 + d^2)),
            // but keep some relative motion, or there is no CPA to classify.
            const float rMax = 0.85f * range;
            const float dMax = rMax > cpa ? sqrtf(rMax * rMax - cpa * cpa) : 0.f;
            if (d > dMax) d = dMax;
            if (d < 0.1f) d = 0.1f;
            const float vrel = d * 60.f / tcpa;
            const float side = (k & 1) ? 1.f : -1.f;
            // Now: `cpa` off the track to one side, `d` short of the CPA
            // along it; relative velocity u * vrel.
            e =  side * un * cpa - ue * d;
            n = -side * ue * cpa - un * d;
            const float ve = v0e + ue * vrel, vn = v0n + un * vrel;
            a->sog = sqrtf(ve * ve + vn * vn);
            a->cog = fmodf(atan2f(ve, vn) / D2R + 360.f, 360.f);
            a->hdg = a->cog;
        } else {
            const float r = (0.3f + 0.05f * (float)((k * 7) % 10)) * range;   // 0.30..0.75
            e = ue * r; n = un * r;
            if (kind == SK_NO_SOG) a->hdg = a->cog = fmodf(k * 137.5f + 115.f, 360.f);
        }
        a->lat = ownLat + n / 60.f;
        const float avgLat = (ownLat + a->lat) / 2.f * D2R;   // as calcCpa() averages
        a->lon = ownLon + e / (60.f * cosf(avgLat));
        a->shipType  = (kind == SK_NO_COG) ? 36 : 70;   // sailing vessel : cargo
        a->navStatus = (kind == SK_NO_COG) ? 1  : 0;    // at anchor : under way
        if (kSeed[k % nKinds].named) snprintf(a->name, sizeof(a->name), "SIM %02d", k + 1);
        else                         a->name[0] = '\0';
        snprintf(a->callsign, sizeof(a->callsign), "SIM%04d", k + 1);
        a->lastSeen = nowMs;
        data.calcCpa(*a);
    }
}

// ── The ring recipes (--set depthhist / windhist / anchortrack) ─────────────
// Sample k of n, oldest first. Plain arithmetic on k only - no clock, no
// libm in the depth and wind ones - so a recipe gives the same floats in every
// build of the harness. u runs 0 (oldest) .. 1 (newest); tri is a 0..1..0
// triangle over 24 samples, the cheap wobble.
static void simRingPhase(int k, int n, float &u, float &tri) {
    u = n > 1 ? (float)k / (float)(n - 1) : 1.f;
    const int p = k % 24;
    tri = (float)(p < 12 ? p : 24 - p) / 12.f;
}

// Depth in metres (kDepthProfiles). Picked for the echogram's cases: a profile
// that deepens and one that shoals, a single deep spike that sets maxM with a
// zero and a negative reading (drawn as 0) further on, a shallow tail - the
// seabed-heavy picture, most of the 400 rows seabed - and one that stays under
// maxM's 2 m floor.
static float simDepthSample(int profile, int k, int n) {
    float u, tri;
    simRingPhase(k, n, u, tri);
    switch (profile) {
    case 0:  return 10.f;                                     // flat
    case 1:  return 3.f + 15.f * u;                           // rising: 3 -> 18 m
    case 2:  return 18.f - 15.f * u;                          // falling: 18 -> 3 m
    case 3:                                                   // spike
        if (k == n / 3)             return 35.f;
        if (k == n / 3 + 1)         return 30.f;
        if (k == 2 * n / 3)         return 0.f;
        if (k == 2 * n / 3 + 1)     return -1.5f;
        return 8.f + 3.f * tri;
    case 4:  return (k >= n - n * 15 / 100) ? 1.2f            // shallow tail
                                            : 12.f + 2.f * tri;
    case 5:  return 6.f + 8.f * tri;                          // sine-ish 6..14 m
    default: return 0.4f + 1.f * u;                           // small: 0.4 -> 1.4 m
    }
}

// TWD/TWS (kWindProfiles). "rose" spreads the samples over 29 of the 36
// sectors by the weights below - petals from a sliver to the full radius,
// gaps between them, sector 0 and 35 both used - with each sector's TWS
// picked so that neighbours fall into different colour classes (green < 8 kn
// ... red >= 20); "steady" stays in a few sectors; "veer" turns through north,
// the sector wrap (with an odd N its middle sample is exactly 360 -> 0 deg).
static const uint8_t kRoseWeight[36] = { 2, 1, 0, 1, 2, 3,  0, 0, 1, 2, 3, 4,
                                         2, 1, 0, 1, 2, 3,  5, 7, 9, 6, 4, 3,
                                         2, 1, 0, 1, 1, 2,  0, 0, 1, 1, 2, 3 };
static void simWindSample(int profile, int k, int n, float &twd, float &tws) {
    float u, tri;
    simRingPhase(k, n, u, tri);
    switch (profile) {
    case 0: {
        int total = 0;
        for (uint8_t w : kRoseWeight) total += w;   // 76, prime to the stride 31
        int p = (k * 31) % total, s = 0;
        while (p >= kRoseWeight[s]) p -= kRoseWeight[s++];
        twd = 10.f * (float)s + (float)((k * 7) % 10) + 0.25f;
        tws = 3.f + (float)(s * 7 % 24) + (float)((k * 3) % 5 - 2) * 0.25f;
        break;
    }
    case 1:
        twd = 245.f + (float)((k * 7) % 21 - 10) * 0.9f;
        tws = 15.f + (float)((k * 5) % 11 - 5) * 0.3f;
        break;
    default:
        twd = 330.f + 60.f * u;
        if (twd >= 360.f) twd -= 360.f;
        tws = 6.f + 10.f * u;
        break;
    }
}

// Breadcrumb offset from the anchor in metres, north/east (kTrackProfiles),
// scaled to the swing radius R so it lands the same on the screen whatever
// radius the scenario sets. "swing": an ellipse round a point off the anchor,
// with six dots on one spot (the blend order of overlapping dots), dots under
// the anchor glyph, on the alarm circle and off the canvas (culled). "drift":
// a straight line corner to corner, through the corner readouts.
static void simTrackSample(int profile, int k, int n, float R, float &nM, float &eM) {
    float u, tri;
    simRingPhase(k, n, u, tri);
    if (profile == 1) { nM = eM = (-1.5f + 3.f * u) * R; return; }
    if (k >= 10 && k < 16)  { nM = 0.3f * R;  eM = -0.2f * R; return; }
    if (k % 23 == 5)        { nM = eM = 0.f; return; }
    if (k % 31 == 7)        { nM = 2.8f * R;  eM = -2.8f * R; return; }
    const float a = (float)(k % 60) * (6.2831853f / 60.f);
    if (k % 19 == 3)        { nM = R * cosf(a); eM = R * sinf(a); return; }
    nM = 0.25f * R + 0.55f * R * cosf(a);
    eM = 0.10f * R + 0.35f * R * sinf(a);
}

// Chronological sample k -> ring slot, and the write index / full flag the
// ring has after those samples.
static int  ringSlot(const SimRing &r, int k, int cap) { return r.n == cap ? (r.idx + k) % cap : k; }
static int  ringIdx (const SimRing &r, int cap)        { return r.n == cap ? r.idx : r.n; }

// Both run inside the demo lock (simDemoOverride). The unused slots are
// zeroed too, so nothing the demo pushed before stays behind in them.
//
// The depth ring is laid into a scratch copy and written only where it
// differs, with DataModel::depthHistSeq bumped then: the Depth screen skips
// every frame whose sequence is unchanged, so a lay that changes the ring
// without a bump would leave the old echogram standing - and bumping on every
// tick would repaint a held ring 5 times a second for nothing.
static void simLayDepthHist() {
    SimRing &r = s_depthHist;
    if (!data.depthHistory || (r.live && r.seeded)) return;
    const int cap = DataModel::DEPTH_HIST;
    static float ring[DataModel::DEPTH_HIST];
    for (int i = 0; i < cap; i++) ring[i] = 0.f;
    for (int k = 0; k < r.n; k++)
        ring[ringSlot(r, k, cap)] = simDepthSample(r.profile, k, r.n);
    const int  idx  = ringIdx(r, cap);
    const bool full = r.n == cap;
    if (memcmp(ring, data.depthHistory, sizeof ring) != 0 ||
        data.depthHistIdx != idx || data.depthHistFull != full) {
        memcpy(data.depthHistory, ring, sizeof ring);
        data.depthHistIdx  = idx;
        data.depthHistFull = full;
        data.depthHistSeq++;
    }
    r.seeded = true;
}

static void simLayWindHist(uint32_t nowMs) {
    SimRing &r = s_windHist;
    if (!data.windHistory || (r.live && r.seeded)) return;
    const int cap = DataModel::WIND_HIST;
    for (int i = 0; i < cap; i++) data.windHistory[i] = { 0.f, 0.f, 0u };
    for (int k = 0; k < r.n; k++) {
        float twd, tws;
        simWindSample(r.profile, k, r.n, twd, tws);
        data.windHistory[ringSlot(r, k, cap)] = { twd, tws, (uint32_t)(k + 1) * DataModel::WIND_HIST_MS };
    }
    data.windHistIdx  = ringIdx(r, cap);
    data.windHistFull = r.n == cap;
    // WindPipeline::tick() appends a sample whenever its 1 Hz clock is due -
    // on the display thread, between two demo ticks, i.e. where a screen can
    // paint it. Keeping the clock a minute ahead keeps it from ever being due.
    if (!r.live) data.windHistNextMs = nowMs + 60000u;
    r.seeded = true;
}

// Installed as g_simAnchorTrack; AnchorScreen::update() calls it right before
// its 3-s sampler, on the UI thread. Lays the track once, on the first frame
// with an anchor down, and then - unless "live" - keeps the sampler's clock
// current so that it never takes a sample.
static void simAnchorTrack(float *trkN, float *trkE, int cap, int &idx, bool &full,
                           float &maxDist, uint32_t &lastTrkMs) {
    SimRing &r = s_anchorTrk;
    if (!appConfig.cfg.anchorSet) return;      // no anchor, no track - as the sampler
    if (cap != kSimTrackN) {
        static bool told = false;
        if (!told) printf("[sim] anchortrack: the track ring has %d slots, expected %d\n",
                          cap, kSimTrackN);
        told = true;
        return;
    }
    if (!r.seeded) {
        const float R = appConfig.cfg.anchorRadius > 1.f ? appConfig.cfg.anchorRadius : 40.f;
        for (int i = 0; i < cap; i++) trkN[i] = trkE[i] = 0.f;
        maxDist = 0.f;
        for (int k = 0; k < r.n; k++) {
            float n, e;
            simTrackSample(r.profile, k, r.n, R, n, e);
            const int s = ringSlot(r, k, cap);
            trkN[s] = n; trkE[s] = e;
            const float d = sqrtf(n * n + e * e);   // as the sampler keeps _maxDist
            if (d > maxDist) maxDist = d;
        }
        idx  = ringIdx(r, cap);
        full = r.n == cap;
        r.seeded = true;
    }
    if (!r.live) lastTrkMs = millis();
}

// Installed as g_simDemoOverride. Runs INSIDE DemoData::tick()'s data lock:
// write `data` directly, never lock it (the mutex is not recursive).
static void simDemoOverride(uint32_t nowMs, float t) {
    if (s_setRudder) data.rudderAngle = s_rudderDeg;
    if (s_setDepth)  data.depth       = s_depthM;
    if (s_setRoll)   data.roll        = s_rollDeg;
    if (s_setPitch)  data.pitch       = s_pitchDeg;
    if (s_setRot)    data.rateOfTurn  = s_rotDegMin;
    if (s_depthHist.on) simLayDepthHist();
    if (s_windHist.on)  simLayWindHist(nowMs);
    // The same gate and the same formula as the demo's own date: only the
    // day and the base time of day change, the demo time t still runs on
    // (or stands, under --freeze-demo).
    if (s_setDays >= 0 && !data.timeIsReal) {
        data.sysDays     = (uint16_t)s_setDays;
        data.sysSecOfDay = s_setSecBase + (double)t;
    }
    if (s_aisSeed > 0) simSeedAis(nowMs);
    // After the seed, which lays its targets around the demo's own position.
    if (s_setNoFix) { data.lat = NAN; data.lon = NAN; }
}

// --selftest: the middle of the n-th button of the overlay on top, in window
// coordinates. Read from the object instead of written down, so the same walk runs
// on every panel and in every rotation. false until there is such a button.
static bool simOverlayButton(int n, int *x, int *y) {
    lv_obj_t *top = lv_layer_top();
    for (int32_t i = (int32_t)lv_obj_get_child_cnt(top) - 1; i >= 0; i--) {
        lv_obj_t *root = lv_obj_get_child(top, i);
        int seen = 0;
        for (uint32_t k = 0; k < lv_obj_get_child_cnt(root); k++) {
            lv_obj_t *c = lv_obj_get_child(root, k);
            if (!lv_obj_check_type(c, &lv_btn_class) || seen++ != n) continue;
            lv_area_t a;
            lv_obj_get_coords(c, &a);
            sim_hal_logical_to_window((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, x, y);
            return true;
        }
    }
    return false;
}

static void demo_thread_fn() {
    const auto t0 = std::chrono::steady_clock::now();
    while (s_running) {
        if (s_demoStopS >= 0.0 && s_demo_enabled &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= s_demoStopS) {
            // As switching demo mode off on a device would: the demo banner
            // goes, and the bus strip may come up 10 s later.
            s_demo_enabled = false;
            appConfig.cfg.demoMode = false;
            printf("[sim] --demo-stop: demo data stopped after %.1f s\n", s_demoStopS);
        }
        // --set / --ais-seed are applied inside tick(), right after the demo's
        // own values (simDemoOverride above).
        if (s_demo_enabled) demoData.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: keep last marker on crash

    // --selftest  : walk the first-run sequence with fast clicks and a swipe,
    //               assert each step lands, exit 0/1. Regression guard for the
    //               lost-click bug (see sim_hal.cpp).
    // --firstrun  : show the first-run sequence (language picker -> licences).
    //               Off by default: the PC sim has no LittleFS and so cannot
    //               remember the consent, which would block the UI every start.
    // --de / --en : force the start language. Without either, the sim uses the
    //               same default as a fresh device (cfg.lang, i.e. English).
    bool selfTest = false, firstRun = false, openConfig = false, openHome = false;
    const char *forceLang = nullptr, *forceTheme = nullptr, *cfgPath = nullptr;
    int  startScreen = -1;      // --screen N : open that screen id straight away
    bool noDemo      = false;   // --nodemo   : no demo data at all (= no GPS fix)
    // --demo-stop S : demo data for S seconds, then none - what a bus that goes
    //                 quiet looks like: the values age out one by one, then the
    //                 "no data from the bus" strip comes up (BusWatch.h).
    bool noPerf      = false;   // --noperf   : hide the fps/CPU overlay (for docs)
    // --shots DIR : walk the whole screen catalogue plus the home launcher and
    //               the config overlay, write one BMP each into DIR, then quit.
    //               A runtime flag rather than the old CAPTURE_ALL_SCREENS
    //               #define, because a documentation set has to be regenerated
    //               for every theme and every orientation - six runs - and
    //               rebuilding in between only invites a stale binary.
    // --settle MS : render time granted per shot. The canvas screens are slow in
    //               the PC software renderer, and Depth and Wind-Plot need to
    //               build up their history before they show anything at all.
    const char *shotsDir = nullptr;
    int  shotSettleMs    = 4000;
    // --n2kupdate-demo : play an update over NMEA 2000 through the real
    //                    overlay (sim_n2kupdate_demo.cpp). Interactive; with
    //                    --shots DIR it captures each state as upd_*.bmp
    //                    instead of the screen catalogue, then quits.
    bool updDemo = false;
    // --n2kupdate-selftest : the press-and-hold rules of the update dialog
    //                        with injected mouse events; exit code 0 = pass.
    bool updSelfTest = false;
    // --ap-frames : plays synthetic pilot-computer example frames, built from
    //               the protocol description - not a bus recording (address
    //               claim, every mode, ignored foreign/wrong-class frames,
    //               a second pilot, a display restart, staleness)
    //               through the real decoder path onto the Autopilot screen
    //               (sim_ap_frames.cpp); exit code = failed steps. With
    //               --shots DIR it captures each step as ap_*.bmp instead of
    //               the screen catalogue. No demo data during the run.
    bool apFrames = false;
    // Test harness - what makes two --shots runs comparable pixel for pixel
    // with tools/cmp_shots.py. All three work on the demo data, so none of
    // them has any effect with --nodemo (or --ap-frames).
    // --freeze-demo T : stop the demo scenario at T seconds. Every demo value
    //                   (wind, boat, rudder, attitude, the demo AIS target,
    //                   the time of day ...) is then a pure function of T,
    //                   while the freshness stamps keep the real clock, so
    //                   nothing ages out. The Clock stands still as well, at
    //                   12:00:00 UTC + T on 2024-06-21 (or the --set days
    //                   date); its label shows that in the demo's local time,
    //                   UTC + 2 h. A T with a .5 fraction keeps the seconds
    //                   label clear of a rounding edge.
    // --set KEY=VALUE : lay a value over the demo's; repeatable.
    //                     rudder=DEG  rudder angle, + = starboard. "nan" gives
    //                                 no angle while the rest of the demo runs.
    //                     days=N      the date the Clock sees: N days from the
    //                                 demo's 2024-06-21 (negative too) ...
    //                     days=YYYY-MM-DD  ... or that civil date.
    //                                 Either may end in THH:MM or THH:MM:SS,
    //                                 the UTC time of day in place of the
    //                                 demo's 12:00; the demo time t (T under
    //                                 --freeze-demo) is added to it, e.g.
    //                                 days=2024-12-21T06:30 --freeze-demo 0.5
    //                                 is 06:30:00 UTC, 08:30:00 on the label.
    //                     depth=M     depth below the transducer, "nan" none.
    //                     roll=DEG pitch=DEG rot=DEG/MIN  the attitude page's
    //                                 inputs (+ = starboard down / bow up /
    //                                 turning to starboard), "nan" none.
    //                     depthhist=PROFILE[,N[,IDX]][,live]
    //                                 the Depth echogram's history ring, laid
    //                                 out by a recipe instead of the demo's
    //                                 5-s clock: flat rising falling spike
    //                                 shallow sine small (simDepthSample());
    //                                 N samples, default a full ring (300);
    //                                 IDX (full ring only) = the slot of the
    //                                 oldest one, i.e. a ring that has wrapped.
    //                                 Held as laid, or with ",live" laid once
    //                                 and then left to the demo (see SimRing).
    //                     windhist=PROFILE[,N[,IDX]][,live]
    //                                 the same for the wind-history rose's
    //                                 1 Hz ring: rose steady veer (360).
    //                     anchor=DN,DE,RADIUS[,alarm]
    //                                 boot with the anchor down DN m north and
    //                                 DE m east of the demo's swing centre,
    //                                 a RADIUS m circle, the alarm armed or not.
    //                                 Needs no demo data (--nodemo: no GPS).
    //                     anchortrack=PROFILE[,N[,IDX]][,live]
    //                                 the anchor watch's breadcrumbs: swing
    //                                 drift (240), laid on its first frame with
    //                                 an anchor down. Needs no demo data either.
    //                     nofix=1     no own position, the rest of the demo runs on
    //                                 (the AIS radar's hint, with --ais-seed).
    //                   A malformed value is an error (exit 2), not ignored:
    //                   a reference set taken with the wrong value would
    //                   compare clean against itself.
    // --ais-seed N    : N synthetic AIS targets (0..49) next to the demo's own
    //                   one, at fixed places round own ship: red, yellow and
    //                   green CPA classes, each with and without a name, and
    //                   targets without a COG or a SOG (see simSeedAis()).
    // --shots-only LIST : with --shots, capture only these screen slots
    //                   (comma-separated, e.g. 2,6,9,10) and none of the
    //                   overlays - a scenario aimed at one page need not walk
    //                   the whole catalogue.
    // --run SECONDS   : quit after that long (exit 0), e.g. with --screen N to
    //                   keep one page up, unfrozen, for a self-check build. No
    //                   periodic sim_shot.bmp or boot_shot.bmp in that mode.
    // --exit-shot FILE: with --run, the screen as it stands at the end.
    const char *freezeArg = nullptr, *aisSeedArg = nullptr, *badSetArg = nullptr;
    const char *noValueArg = nullptr;   // a harness flag given last, without its value
    const char *shotsOnlyArg = nullptr, *runArg = nullptr, *exitShot = nullptr;
    for (int i = 1; i < argc; i++) {
        if (i + 1 == argc && (strcmp(argv[i], "--freeze-demo") == 0 ||
                              strcmp(argv[i], "--ais-seed") == 0 ||
                              strcmp(argv[i], "--set") == 0 ||
                              strcmp(argv[i], "--shots-only") == 0 ||
                              strcmp(argv[i], "--run") == 0 ||
                              strcmp(argv[i], "--exit-shot") == 0))
            noValueArg = argv[i];
        if (strcmp(argv[i], "--freeze-demo") == 0 && i + 1 < argc) freezeArg = argv[++i];
        if (strcmp(argv[i], "--ais-seed") == 0 && i + 1 < argc)    aisSeedArg = argv[++i];
        if (strcmp(argv[i], "--set") == 0 && i + 1 < argc && !parseSetArg(argv[++i]))
            badSetArg = argv[i];
        if (strcmp(argv[i], "--shots-only") == 0 && i + 1 < argc) shotsOnlyArg = argv[++i];
        if (strcmp(argv[i], "--run") == 0 && i + 1 < argc)        runArg = argv[++i];
        if (strcmp(argv[i], "--exit-shot") == 0 && i + 1 < argc)  exitShot = argv[++i];
        if (strcmp(argv[i], "--ap-frames") == 0) apFrames = true;
        if (strcmp(argv[i], "--n2kupdate-demo") == 0) updDemo = true;
        if (strcmp(argv[i], "--n2kupdate-selftest") == 0) updSelfTest = true;
        if (strcmp(argv[i], "--selftest") == 0) selfTest = true;
        if (strcmp(argv[i], "--firstrun") == 0) firstRun = true;
        if (strcmp(argv[i], "--config") == 0)   openConfig = true;
        if (strcmp(argv[i], "--home") == 0)     openHome   = true;   // 7B launcher overlay
        if (strcmp(argv[i], "--light") == 0)    forceTheme = "light";
        if (strcmp(argv[i], "--dark") == 0)     forceTheme = "dark";
        if (strcmp(argv[i], "--night") == 0)    forceTheme = "night";
        if (strcmp(argv[i], "--cfg") == 0 && i + 1 < argc) cfgPath = argv[++i];
        if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc) startScreen = atoi(argv[++i]);
        if (strcmp(argv[i], "--nodemo") == 0)  noDemo = true;
        if (strcmp(argv[i], "--demo-stop") == 0 && i + 1 < argc) s_demoStopS = atof(argv[++i]);
        if (strcmp(argv[i], "--noperf") == 0)  noPerf = true;
        if (strcmp(argv[i], "--en") == 0)       forceLang = "en";
        if (strcmp(argv[i], "--de") == 0)       forceLang = "de";
        if (strcmp(argv[i], "--shots") == 0 && i + 1 < argc) shotsDir = argv[++i];
        if (strcmp(argv[i], "--settle") == 0 && i + 1 < argc) shotSettleMs = atoi(argv[++i]);
    }
    if (shotsDir) noPerf = true;   // the fps counter has no place in a manual
    if (apFrames) noDemo = true;   // the demo would overwrite the pilot's mode
    if (selfTest) firstRun = true;   // the picker is the first click target

    // Test harness flags: checked strictly (see above), then reported, so the
    // log of a reference run says which scenario it shows.
    if (noValueArg) {
        fprintf(stderr, "[sim] %s: missing value\n", noValueArg);
        return 2;
    }
    if (badSetArg) {
        fprintf(stderr, "[sim] --set %s: unknown key or bad value "
                        "(rudder=DEG, days=N or days=YYYY-MM-DD[THH:MM[:SS]], depth=M, "
                        "roll=DEG, pitch=DEG, rot=DEG/MIN, depthhist=PROFILE[,N[,IDX]][,live], "
                        "windhist=..., anchor=DN,DE,RADIUS[,alarm], anchortrack=..., nofix=1)\n",
                badSetArg);
        return 2;
    }
    // --shots-only: a bit per screen slot.
    uint32_t shotsMask = 0;
    if (shotsOnlyArg) {
        int slot;
        for (const std::string &s : splitCommas(shotsOnlyArg)) {
            if (!parseSetCount(s, 0, 31, slot)) {
                fprintf(stderr, "[sim] --shots-only %s: expected slot numbers 0..31, "
                                "comma-separated\n", shotsOnlyArg);
                return 2;
            }
            shotsMask |= 1u << slot;
        }
    }
    uint32_t runMs = 0;   // --run: 0 = until the window is closed
    if (runArg) {
        char *end;
        const float s = strtof(runArg, &end);
        if (end == runArg || *end || !std::isfinite(s) || s <= 0.f || s > 86400.f) {
            fprintf(stderr, "[sim] --run %s: not a number of seconds\n", runArg);
            return 2;
        }
        runMs = (uint32_t)(s * 1000.f);
    }
    if (freezeArg) {
        char *end;
        const float T = strtof(freezeArg, &end);
        // The demo scenario clock starts at 0; a negative T is no state the
        // demo can ever be in.
        if (end == freezeArg || *end || !std::isfinite(T) || T < 0.f) {
            fprintf(stderr, "[sim] --freeze-demo %s: not a number of seconds\n", freezeArg);
            return 2;
        }
        g_simFreezeT = T;
    }
    if (aisSeedArg) {
        char *end;
        const long n = strtol(aisSeedArg, &end, 10);
        // One slot of the table stays the demo's own target.
        if (end == aisSeedArg || *end || n < 0 || n > DataModel::MAX_AIS - 1) {
            fprintf(stderr, "[sim] --ais-seed %s: expected 0..%d targets\n",
                    aisSeedArg, DataModel::MAX_AIS - 1);
            return 2;
        }
        s_aisSeed = (int)n;
    }
    printf("NauticPinnace Simulator\n");
    printf("Resolution: %d x %d\n", UI_SCREEN_W, UI_SCREEN_H);
    printf("Close window or press ESC to quit.\n\n");
    // Everything but anchor= and anchortrack= goes over the demo's values,
    // inside its tick.
    const bool overrides = s_setRudder || s_setDays >= 0 || s_aisSeed > 0 || s_setDepth ||
                           s_setRoll || s_setPitch || s_setRot || s_setNoFix || s_depthHist.on || s_windHist.on;
    const bool harness = !isnan(g_simFreezeT) || overrides;
    auto printRing = [](const char *key, const char *const *names, const SimRing &r) {
        if (!r.on) return;
        printf(" %s=%s,%d", key, names[r.profile], r.n);
        if (r.idx) printf(",%d", r.idx);
        if (r.live) printf(",live");
    };
    if (harness && noDemo) {
        printf("[sim] --freeze-demo/--set/--ais-seed act on the demo data - "
               "no effect without it\n");
    } else if (harness) {
        if (overrides) g_simDemoOverride = simDemoOverride;
        printf("[sim] harness:");
        if (!isnan(g_simFreezeT)) printf(" freeze-demo T=%.3f", g_simFreezeT);
        if (s_setRudder)          printf(" rudder=%.2f", s_rudderDeg);
        if (s_setDays >= 0) {
            int y; unsigned mo, d; scCivilFromDays(s_setDays, y, mo, d);
            const int sb = (int)s_setSecBase;
            printf(" date=%04d-%02u-%02u %02d:%02d:%02d UTC + t", y, mo, d,
                   sb / 3600, sb / 60 % 60, sb % 60);
        }
        if (s_aisSeed > 0)        printf(" ais-seed=%d", s_aisSeed);
        if (s_setDepth)           printf(" depth=%.2f", s_depthM);
        if (s_setRoll)            printf(" roll=%.2f", s_rollDeg);
        if (s_setPitch)           printf(" pitch=%.2f", s_pitchDeg);
        if (s_setRot)             printf(" rot=%.2f", s_rotDegMin);
        if (s_setNoFix)           printf(" nofix=1");
        printRing("depthhist", kDepthProfiles, s_depthHist);
        printRing("windhist",  kWindProfiles,  s_windHist);
        printf("\n");
    }
    // The anchor keys act on the config and on the anchor page itself, so
    // they work without the demo as well (and then show "no GPS").
    if (s_anchorTrk.on) g_simAnchorTrack = simAnchorTrack;
    if (s_setAnchor || s_anchorTrk.on) {
        printf("[sim] harness:");
        if (s_setAnchor) printf(" anchor=%.1f,%.1f,%.1f%s", s_ancDn, s_ancDe, s_ancR,
                                s_ancAlarm ? ",alarm" : "");
        printRing("anchortrack", kTrackProfiles, s_anchorTrk);
        printf("\n");
    }

    // Init SDL2 + LVGL display/input
    if (!sim_hal_init(UI_SCREEN_W, UI_SCREEN_H, "NauticPinnace Simulator")) {
        fprintf(stderr, "SDL2 init failed!\n");
        return 1;
    }

    // Init LVGL
    lv_init();

    // Register display driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2,
                          UI_SCREEN_W * 20);
    disp_drv.draw_buf   = &s_draw_buf;
    disp_drv.flush_cb   = sim_flush_cb;
    disp_drv.hor_res    = UI_SCREEN_W;
    disp_drv.ver_res    = UI_SCREEN_H;
    lv_disp_drv_register(&disp_drv);

    // Register touch/mouse input driver
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sim_mouse_cb;
    lv_indev_drv_register(&indev_drv);

    // Palette defaults FIRST, then the device config on top — exactly the order
    // Config::begin() uses on the panel, so the sim ends up with the same colours.
    fillLightTheme(appConfig.cfg.themeLight);
    fillNightTheme(appConfig.cfg.themeNight);
    const bool cfgLoaded = sim_load_device_config(cfgPath);

    // Screen rotation - AFTER the config load, which is where the value comes
    // from (the driver is registered before that). Same contract as on the
    // device: hor/ver stay PHYSICAL and LVGL swaps them for 90/270, so the SDL
    // window keeps the panel shape and the picture lies on its side inside it -
    // exactly what the real panel does when you mount it rotated.
    switch (appConfig.cfg.displayRotation) {
        case 90:  disp_drv.rotated = LV_DISP_ROT_90;  break;
        case 180: disp_drv.rotated = LV_DISP_ROT_180; break;
        case 270: disp_drv.rotated = LV_DISP_ROT_270; break;
        default:  disp_drv.rotated = LV_DISP_ROT_NONE; break;
    }
    disp_drv.sw_rotate = (disp_drv.rotated != LV_DISP_ROT_NONE) ? 1 : 0;
    if (disp_drv.sw_rotate) {
        lv_disp_drv_update(lv_disp_get_default(), &disp_drv);
        printf("[sim] rotation %u deg -> logical %dx%d\n",
               (unsigned)appConfig.cfg.displayRotation,
               (int)lv_disp_get_hor_res(NULL), (int)lv_disp_get_ver_res(NULL));
    }
    if (!cfgLoaded) {
        // No config to mirror: fall back to the light theme, which is what the
        // simulator has always started in.
        strncpy(appConfig.cfg.themeActive, "light", sizeof(appConfig.cfg.themeActive) - 1);
    }
    if (forceTheme)
        strncpy(appConfig.cfg.themeActive, forceTheme, sizeof(appConfig.cfg.themeActive) - 1);
    // --firstrun/--selftest must ALWAYS show the first-run sequence, a normal
    // sim start NEVER: data/config.json is genuine factory state
    // (license_ok=false), so without setting this here every start would show
    // the language picker before the screens.
    appConfig.cfg.licenseAccepted = !firstRun;

    // Ensure a hotspot password just like on the device (the sim does not run
    // through Config::begin()) — otherwise the settings overlay showed the
    // placeholder instead of a password in the QR code.
    if (!appConfig.cfg.apPass[0])
        Entropy::generateApPassword(appConfig.cfg.apPass, sizeof(appConfig.cfg.apPass));

    i18nSetLang(i18nLangFromCode(forceLang ? forceLang : appConfig.cfg.lang));
    // The DEMO banner hangs off cfg.demoMode, while the demo DATA comes from
    // s_demo_enabled further down - so a --shots run can keep the values and
    // drop the banner. That is the same argument --noperf makes: a device with
    // real sensors on the bus shows neither. It matters here because the banner
    // is drawn OVER the top of the instrument instead of pushing it down, so it
    // was covering screen titles and the first row of grid cells in a third of
    // the captured images. For a screenshot OF demo mode, use --screen N.
    appConfig.cfg.demoMode       = !noDemo && !shotsDir;
    // The sim has no radio; WiFi is "on" anyway so that the WiFi section of the
    // settings is shown in full (status + address instead of "WLAN aus").
    appConfig.cfg.wifiEnabled    = true;
    // On by default here (fps/CPU are useful while developing), but --noperf
    // turns it off — documentation screenshots must show what a device actually
    // ships with, and the factory config has it disabled.
    appConfig.cfg.showPerfOverlay = !noPerf;
    appConfig.cfg.waterlineLengthM = 9.0f;
    // The kite stays on in the sim, as before the type setting existed: a
    // config that picked a type keeps it, one that switched it off gets the
    // default balloon.
    if (appConfig.cfg.spiType == SpiType::None) appConfig.cfg.spiType = SpiType::Balloon;
    // --set anchor: over whatever the config file says. The position uses the
    // demo's own constants and formula (DemoData.cpp), so DN/DE are the metres
    // the anchor page measures from the demo's swing centre. The anchor fell
    // at 09:47 UTC on the demo date (11:47 on the page, the demo's UTC + 2 h).
    if (s_setAnchor) {
        const float D2R = (float)M_PI / 180.f;
        appConfig.cfg.anchorSet     = true;
        appConfig.cfg.anchorLat     = 53.500f + s_ancDn / (60.f * 1852.f);
        appConfig.cfg.anchorLon     = 10.000f + s_ancDe / (60.f * 1852.f * cosf(53.500f * D2R));
        appConfig.cfg.anchorRadius  = s_ancR;
        appConfig.cfg.anchorAlarmOn = s_ancAlarm;
        appConfig.cfg.anchorSetUtc  = (uint32_t)scDaysFromCivil(2024, 6, 21) * 86400u
                                    + 9u * 3600u + 47u * 60u;
    }

    // Populate the runtime theme/size/font globals (uiTheme/uiSz/uiFont). Without
    // this the FONT_*/CLR_* macros point at a NULL uiFont and the FIRST text
    // render segfaults — main.cpp calls this, the sim previously did not.
    applyThemeFromConfig();

    // Pre-allocate "PSRAM" arena on PC (normal heap)
    // Mirror the DEVICE arena sizes (main.cpp) so an over-budget canvas
    // OOMs here on the PC first, not on the boat. The per-alloc trace in
    // PsramArena.cpp (SIMULATOR only) is the sizing evidence.
#if defined(BOARD_PANEL_1024X600)
    PsramArena::init(4365000);   // main.cpp's np_psram_budget[0] without the update regions
#else
    PsramArena::init(3350000);
#endif

    // The DataModel's AIS table and history rings are arena buffers as well
    // (main.cpp does this right after the arena init). The sim MUST do it
    // itself: it never runs the device's setup(), and `data` is a global whose
    // constructor left those pointers null. Without this line the first render
    // segfaults — DepthScreen and WindPlotScreen memcpy the whole ring
    // unconditionally — taking the --shots documentation run with it.
    data.initBuffers();

    // Allocate the polar table (else gPolar() dereferences a null polarPtr and
    // the Wind/Speed screens segfault), then fill it from data/polar.json - the
    // file the device loads from LittleFS (main.cpp). Without it speedAt() was
    // NAN: the Speed screen showed "--" for Polar and Perf, and Wind and VMG ran
    // on their analytic fallback curve instead of the polar a device shows.
    PolarInit::init();
    sim_load_polar(cfgPath);

    // Build the display. Boat name comes from the config, exactly as on the
    // device (main.cpp), so the boot screen can be checked here.
    bootScreen.show(appConfig.cfg.bootName);
    dispMgr.begin();
    bootScreen.update("Simulator start...", 50);
    bootScreen.update("Bereit.", 100);

    // A quick lv_timer_handler flush to render boot screen
    for (int i = 0; i < 30; i++) {
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        sim_hal_tick();
    }
    // The boot screen is only up for ~1.5 s — capture it for visual checks.
    // Not under --run: that is an unattended run, and the file would land in
    // whatever directory it was started from.
    if (!runMs) sim_hal_screenshot("boot_shot.bmp");

    dispMgr.activate();
    bootScreen.dismiss();
    // First-run sequence on the device: language picker, then licences. The PC
    // sim has no LittleFS, so the consent cannot be remembered and this would
    // reappear on every start — opt in with --firstrun when you want to test it.
    if (firstRun && !appConfig.cfg.licenseAccepted) languageOverlay.requestOpen();
    // --config: open the settings straight away (layout check without a mouse click).
    if (openConfig) dispMgr.requestOpenConfig();
    // --home: open the 7B launcher overlay straight away (crash repro/layout).
    if (openHome) dispMgr.requestOpenHome();

    // Config overlay toggles for manual testing: C=open, X=close, K=keyboard.

    // --screen: jump straight to a screen id (layout checks without swiping).
    if (startScreen >= 0) dispMgr.showScreen(startScreen);

    // Start demo data in background thread. With --nodemo it never runs, so the
    // DataModel stays empty — that is how you reproduce "no GPS fix" on the PC.
    if (!noDemo) s_demo_enabled = true;
    std::thread demo_thread(demo_thread_fn);

    // --freeze-demo: the Clock (like every screen that shows the time) adds
    // millis() - lastTimeUpdate to the demo time, i.e. the age of the last
    // demo tick, 0..200 ms and more. That is enough to tick the seconds label
    // on in one run and not in the next, and to shift the terminator by a
    // fraction of a pixel. Re-stamped right before the screens read it (main
    // loop, below), the age is 0 and the time stands with the scenario.
    const bool clockFrozen = !noDemo && !isnan(g_simFreezeT);

    // Main loop
    uint32_t last_update = 0;
    const uint32_t runStart = millis_stub();   // --run counts from here
    while (s_running) {
        // Process SDL events
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { s_running = false; break; }
            if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE) { s_running = false; break; }
                if (ev.key.keysym.sym == SDLK_c) configOverlay.open();
                if (ev.key.keysym.sym == SDLK_x) configOverlay.close();
                if (ev.key.keysym.sym == SDLK_k) configOverlay.simShowKeyboard();
            }
            sim_hal_handle_event(&ev);
        }

        // LVGL tick
        sim_hal_tick();
        lv_timer_handler();

        // App update at ~10 fps
        uint32_t now = millis_stub();
        if (now - last_update >= 100) {
            last_update = now;
            if (clockFrozen) {
                auto lk = data.lock();
                if (data.timeValid && !data.timeIsReal) data.lastTimeUpdate = millis_stub();
            }
            dispMgr.update();   // refresh every 100 ms (matches the device loop)
        }

        // --n2kupdate-demo: the N2K task's part, on this thread like LVGL.
        if (updDemo && !simUpdDemoTick(now, shotsDir)) {
            updDemo = false;
            if (shotsDir) { printf("[shots] done\n"); s_running = false; }
        }
        if (apFrames) {
            const int rc = simApFramesTick(now, shotsDir);
            if (rc >= 0) {
                s_running = false;
                if (demo_thread.joinable()) demo_thread.join();
                sim_hal_deinit();
                return rc;
            }
        }
        if (updSelfTest) {
            const int rc = simUpdHoldSelfTest(now, updateOverlay.simInstallButton());
            if (rc >= 0) {
                s_running = false;
                if (demo_thread.joinable()) demo_thread.join();
                sim_hal_deinit();
                return rc;
            }
        }

        // Periodic screenshot for the dev visual-feedback loop. Note: the default
        // (Wind) screen's canvas render is heavy in the PC software renderer
        // (~250 ms/frame), so 40 iterations ≈ a fresh sim_shot.bmp every ~10 s.
        // Suppressed during --shots: it would scribble a stray sim_shot.bmp into
        // the working directory in the middle of a documentation run.
        if (!shotsDir && !runMs) {
            static int shotFrame = 0;
            if (++shotFrame == 14 || shotFrame % 40 == 0) sim_hal_screenshot("sim_shot.bmp");
        }
        // --run SECONDS: an unattended run ends by itself, through the normal
        // exit below (demo thread joined, SDL closed), so a self-check build
        // can print its tally on the way out.
        if (runMs && now - runStart >= runMs) {
            if (exitShot) sim_hal_screenshot(exitShot);
            printf("[sim] --run: %.1f s up, exit\n", runMs / 1000.0);
            s_running = false;
        }

        // ---- --selftest: guard against the lost-click regression -------------
        // Injects a FAST click (DOWN and UP in the same SDL_PollEvent drain, the
        // case that used to be swallowed) on the licence overlay's button and
        // asserts the overlay actually closed.
        // Walks the whole first-run sequence with FAST clicks (DOWN and UP in the
        // same SDL_PollEvent drain — the case that used to be swallowed):
        //   language picker -> "English" -> licences -> "Understood" -> screens
        if (selfTest) {
            auto fastClick = [](int x, int y) {
                SDL_Event d{}, u{};
                d.type = SDL_MOUSEBUTTONDOWN; d.button.button = SDL_BUTTON_LEFT;
                d.button.x = x; d.button.y = y;
                u.type = SDL_MOUSEBUTTONUP;   u.button.button = SDL_BUTTON_LEFT;
                u.button.x = x; u.button.y = y;
                SDL_PushEvent(&d); SDL_PushEvent(&u);
            };
            // EVENT-DRIVEN, not time-driven: the debug sim renders the canvas
            // screens at ~1 fps — fixed millisecond gates (previously
            // 2.5 s per step) then falsely reported "click swallowed" even
            // though LVGL simply had not sampled the click yet.
            // Each step waits for the OBSERVABLE effect of the previous one and
            // only fails after a generous timeout.
            enum Step { ST_WAIT_LANG, ST_LANG_CLICKED, ST_LIC_CLICKED,
                        ST_SETTLE, ST_SWIPE_SENT };
            static Step     step      = ST_WAIT_LANG;
            static uint32_t stepSince = now;
            static int      settle    = 0, swipeFrom = -1;
            const uint32_t  STEP_TIMEOUT_MS = 30000;

            auto finish = [&](bool ok) -> int {
                sim_hal_screenshot(ok ? "selftest_pass.bmp" : "selftest_fail.bmp");
                printf("[selftest] %s\n", ok ? "ALL PASS" : "FAILURES ABOVE");
                s_running = false;
                if (demo_thread.joinable()) demo_thread.join();
                sim_hal_deinit();
                return ok ? 0 : 1;
            };
            const bool stepTimedOut = (now - stepSince) > STEP_TIMEOUT_MS;

            // Clicks are REPEATED as long as the target overlay is still open:
            // right after it is built LVGL has not done a layout pass yet,
            // the buttons' coordinates only settle after the next
            // refresh — an immediate click would hit nothing (verified:
            // lv_indev_search_obj returned NULL). Re-clicking every few
            // iterations still exercises the fast-click delivery.
            static int  clickIter = 0;
            switch (step) {
            case ST_WAIT_LANG:
                if (languageOverlay.isOpen()) {
                    sim_hal_screenshot("firstrun_1_language.bmp");
                    const bool pickDe = forceLang && forceLang[0] == 'd';
                    printf("[selftest] language picker is up, clicking '%s' ...\n",
                           pickDe ? "Deutsch" : "English");
                    step = ST_LANG_CLICKED; stepSince = now; clickIter = 0;
                } else if (stepTimedOut) {
                    printf("[selftest] FAIL: language picker did not open.\n");
                    return finish(false);
                }
                break;
            case ST_LANG_CLICKED:
                if (!languageOverlay.isOpen() && licenseOverlay.isOpen()) {
                    sim_hal_screenshot("firstrun_2_licence.bmp");
                    printf("[selftest] language=%s, licences followed; accepting ...\n",
                           i18nLangCode(i18nLang()));
                    step = ST_LIC_CLICKED; stepSince = now; clickIter = 0;
                } else if (stepTimedOut) {
                    printf("[selftest] FAIL: %s\n", languageOverlay.isOpen()
                           ? "language click was swallowed."
                           : "licences did not follow the language choice.");
                    return finish(false);
                } else if (languageOverlay.isOpen() && (clickIter++ % 3) == 0) {
                    // --de picks Deutsch (the first button), otherwise English
                    // (the second). Both paths are worth testing: the German
                    // licence text is the one that can expose a missing glyph.
                    const bool pickDe = forceLang && forceLang[0] == 'd';
                    int bx, by;
                    if (simOverlayButton(pickDe ? 0 : 1, &bx, &by)) fastClick(bx, by);
                }
                break;
            case ST_LIC_CLICKED:
                if (!licenseOverlay.isOpen() && appConfig.cfg.licenseAccepted) {
                    printf("[selftest] first-run sequence completed — PASS\n");
                    step = ST_SETTLE; stepSince = now; settle = 0;
                } else if (stepTimedOut) {
                    printf("[selftest] FAIL: licence accept did not complete.\n");
                    return finish(false);
                } else if (licenseOverlay.isOpen() && (clickIter++ % 3) == 0) {
                    int bx, by;
                    if (simOverlayButton(0, &bx, &by)) fastClick(bx, by);   // "Understood"
                }
                break;
            case ST_SETTLE:
                // cbAccept triggers requestThemeReload (screens are rebuilt)
                // — wait a few iterations so the swipe does not land in the
                // middle of the rebuild.
                if (++settle >= 3) {
                    swipeFrom = dispMgr.currentIndex();
                    printf("[selftest] swiping left from screen %d (%s) ...\n",
                           swipeFrom, dispMgr.currentTitle());
                    // Stage 2: a drag must reach LVGL as a swipe. The whole
                    // gesture is pushed in ONE go, which is what really
                    // happens: the main loop drains the entire SDL queue
                    // before LVGL ever samples, so a real mouse swipe arrives
                    // as DOWN + all MOTION + UP in a single pass. An earlier
                    // version spread the events over several frames and missed
                    // a bug that discarded all the travel.
                    // Start inside the middle two thirds: the panel ignores
                    // swipes beginning in the outer sixths (nav arrows), and on
                    // 1024x600 anything outside the instrument block.
#if defined(BOARD_PANEL_1024X600)
                    const int ox = uiPortrait() ? 0 : UI7_CENTER_X;
                    const int oy = uiPortrait() ? UI7_RAIL_W : 0;
#else
                    const int ox = 0, oy = 0;
#endif
                    const int lx = ox + SCREEN_W * 380 / 480, ly = oy + SCREEN_H / 2;
                    int wx, wy;
                    sim_hal_logical_to_window(lx, ly, &wx, &wy);
                    SDL_Event d{};
                    d.type = SDL_MOUSEBUTTONDOWN; d.button.button = SDL_BUTTON_LEFT;
                    d.button.x = wx; d.button.y = wy;
                    SDL_PushEvent(&d);
                    for (int k = 1; k <= 6; k++) {
                        sim_hal_logical_to_window(lx - k * 45, ly, &wx, &wy);
                        SDL_Event m{};
                        m.type = SDL_MOUSEMOTION;
                        m.motion.x = wx;
                        m.motion.y = wy;
                        SDL_PushEvent(&m);
                    }
                    SDL_Event u{};
                    u.type = SDL_MOUSEBUTTONUP; u.button.button = SDL_BUTTON_LEFT;
                    u.button.x = wx; u.button.y = wy;
                    SDL_PushEvent(&u);
                    step = ST_SWIPE_SENT; stepSince = now;
                }
                break;
            case ST_SWIPE_SENT:
                if (dispMgr.currentIndex() != swipeFrom) {
                    printf("[selftest] swipe %d -> %d (%s)  — PASS\n", swipeFrom,
                           dispMgr.currentIndex(), dispMgr.currentTitle());
                    return finish(true);
                } else if (stepTimedOut) {
                    printf("[selftest] swipe — FAIL, screen did not change\n");
                    return finish(false);
                }
                break;
            }
        }

        // ---- --shots DIR: documentation capture ------------------------------
        // Walks the whole catalogue, then the two full-screen overlays, and
        // quits. Slots that hold no screen (unconfigured grid pages) are skipped
        // rather than captured blank: showScreen() refuses a null slot and
        // leaves _cur where it was, so a mismatching currentIndex() identifies
        // them exactly.
        if (shotsDir && !updDemo && !apFrames) {
            static int      capStep = -1;
            static uint32_t capT    = 0;
            const int total = dispMgr.screenTotal();
            // --shots-only: the slots not asked for are passed over as if
            // they were empty, but silently and without showing them.
            auto wanted = [&](int slot) { return !shotsMask || (slot < 32 && (shotsMask >> slot & 1u)); };
            if (capStep < 0) {
                capStep = 0;
                while (capStep < total && !wanted(capStep)) capStep++;
                capT = now;
                if (capStep < total) dispMgr.showScreen(capStep);
                else { printf("[shots] --shots-only: no such slot\n"); s_running = false; }
            }
            if (now - capT > (uint32_t)shotSettleMs) {
                capT = now;
                char path[512];
                if (capStep < total) {
                    if (dispMgr.currentIndex() == capStep) {
                        char name[64];
                        shotNameFromTitle(dispMgr.currentTitle(), name, sizeof(name));
                        snprintf(path, sizeof(path), "%s/scr_%02d_%s.bmp",
                                 shotsDir, capStep, name);
                        sim_hal_screenshot(path);
                        printf("[shots] %s\n", path);
                    } else {
                        printf("[shots] slot %d empty, skipped\n", capStep);
                    }
                    while (++capStep < total) {
                        if (!wanted(capStep)) continue;
                        dispMgr.showScreen(capStep);
                        if (dispMgr.currentIndex() == capStep) break;
                        printf("[shots] slot %d empty, skipped\n", capStep);
                    }
                    if (capStep >= total && shotsMask) {
                        printf("[shots] done\n");   // --shots-only: no overlays
                        s_running = false;
                    } else if (capStep >= total) dispMgr.requestOpenHome();
                } else if (capStep == total) {
                    snprintf(path, sizeof(path), "%s/ovl_00_home.bmp", shotsDir);
                    sim_hal_screenshot(path);
                    printf("[shots] %s\n", path);
                    homeOverlay.close();
                    dispMgr.requestOpenConfig();
                    capStep++;
                } else if (capStep == total + 1) {
                    snprintf(path, sizeof(path), "%s/ovl_01_config.bmp", shotsDir);
                    sim_hal_screenshot(path);
                    printf("[shots] %s\n", path);
                    configOverlay.close();
                    licenseOverlay.open();
                    capStep++;
                } else {
                    // The licence page belongs in the documentation like the
                    // other two overlays, and it is also the one screen whose
                    // layout depends on the label being unwrapped - see the
                    // note on LV_TEXT_FLAG_FIT in LicenseOverlay::build().
                    snprintf(path, sizeof(path), "%s/ovl_02_license.bmp", shotsDir);
                    sim_hal_screenshot(path);
                    printf("[shots] %s\n", path);
                    printf("[shots] done\n");
                    s_running = false;
                }
            }
        }

        SDL_Delay(5);
    }

    s_running = false;
    demo_thread.join();
    sim_hal_deinit();
    printf("Simulator closed.\n");
    return 0;
}
