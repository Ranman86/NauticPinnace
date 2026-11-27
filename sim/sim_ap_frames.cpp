// ============================================================================
// sim_ap_frames.cpp - --ap-frames: the pilot computer's mode on the PC,
// without a bus.
//
// Plays synthetic example frames - built from the protocol description in
// ApDecode.h, NOT a recording from a bus - into the same entry points
// N2kHandler uses on the device: ApPipeline::addressClaim() for PGN 60928,
// ApPipeline::frame() for 65305/65340/65341, ApPipeline::begin() at a
// (simulated) start and persistDue() from the "bus task" loop. So the
// decoder, the NAME filter, the debounce, the priority, the staleness and the
// remembered pilot computer all run for real; only the CAN driver and the
// library's parsing are missing. The source picker's bookkeeping (srcGate)
// lives in N2kHandler.cpp and is not part of this.
//
// It starts with the decoder steps: single frames straight into
// ApDecode::decode(), each checked against what it must decode to - every
// rule of 65305, 65340 and 65341, and the values no rule knows (two mode
// bits at once, a bit the rules do not know, an unlisted 65341 mode byte),
// which must come out as ACTIVE (engaged, mode not known) and never as AUTO.
//
// Then the script: frames before any address claim (ignored), the claim of a
// pilot computer, then every mode in turn with its setpoint, a wind setpoint
// only a signed encoding could produce, mode words the rules cannot decide
// (ACTIVE, with the newer setpoint), the status report saying "manual",
// heading and wind setpoints mixed with an unlisted mode byte (the heading
// must stay), a foreign manufacturer's frames and frames from a device of
// the wrong class (none of them may change anything), a second pilot
// computer saying something else (the first keeps the mode), 65305 falling
// silent (65340 takes over after 10 s), a display restart without any claim
// (the remembered pilot computer is found again), everything falling silent
// ("--" after 10 s), a pilot that only sends PGN 127237, and STANDBY with
// 127237's heading-to-steer. After each step the model is checked against
// what the step expects; the run prints PASS/FAIL per step and the exit code
// is the number of failed steps, decoder steps included (0 = all pass).
//
// With --shots DIR it writes ap_NN_<step>.bmp of the Autopilot screen at the
// end of each step. Demo data is off for the run; the player supplies a
// compass heading and a rudder angle itself.
// ============================================================================
#include "sim_hal.h"
#include "arduino_stubs.h"
#include "../src/nmea/DataModel.h"
#include "../src/nmea/ApPipeline.h"
#include "../src/display/DisplayManager.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ApDecode;

namespace {

struct Frame { uint32_t pgn; uint8_t sa; uint8_t len; uint8_t d[8]; };

// Pilot computer at SA 3: NAME unique 0x12345, manufacturer 1857,
// function 150, class 40, industry 4, arbitrary address capable.
const Frame CLAIM_PILOT  = { 60928, 3, 8, { 0x45, 0x23, 0x21, 0xE8, 0x00, 0x96, 0x50, 0xC0 } };
// A second pilot computer at SA 5: unique 0x777, instance 1, otherwise the same.
const Frame CLAIM_PILOT2 = { 60928, 5, 8, { 0x77, 0x07, 0x20, 0xE8, 0x01, 0x96, 0x50, 0xC0 } };
// A device at SA 7 with the right manufacturer and function but class 25.
const Frame CLAIM_WRONG  = { 60928, 7, 8, { 0x42, 0x00, 0x20, 0xE8, 0x00, 0x96, 0x32, 0xC0 } };

Frame mode10(uint32_t w, uint8_t sa = 3) {
    return { PGN_MODE_REPORT, sa, 8, { 0x41, 0x9F, 0x10, 10, (uint8_t)w, (uint8_t)(w >> 8),
                                       (uint8_t)(w >> 16), (uint8_t)(w >> 24) } };
}
Frame status2(uint8_t v) {
    return { PGN_MODE_REPORT, 3, 8, { 0x41, 0x9F, 0x10, 2, v, 0xFF, 0xFF, 0xFF } };
}
Frame state(uint8_t b3, uint8_t b5, uint8_t sa = 3) {
    return { PGN_STATE, sa, 8, { 0x41, 0x9F, 0x02, b3, 0xFE, b5, 0x00, 0x80 } };
}
Frame setpointRaw(uint8_t mode, uint16_t raw, uint8_t sa = 3) {
    return { PGN_SETPOINT, sa, 8, { 0x41, 0x9F, 0xFF, 0xFF, mode, 0xFF, (uint8_t)raw,
                                    (uint8_t)(raw >> 8) } };
}
// Encoded the way ApDecode assumes by default: unsigned 0..2 pi, port above pi.
Frame setpoint(uint8_t mode, float deg, uint8_t sa = 3) {
    float rad = deg * 3.14159265f / 180.f;
    if (rad < 0) rad += 2.f * 3.14159265f;
    return setpointRaw(mode, (uint16_t)lroundf(rad * 10000.f), sa);
}
Frame foreign(Frame f) { f.d[0] = 0xA3; f.d[1] = 0x99; return f; }   // manufacturer 419

enum Claim : uint8_t { CLAIM_NONE, CLAIM_PILOT_AT_3, CLAIM_WRONG_AT_7, CLAIM_PILOT2_AT_5 };
const int ANY_SA = -1;

struct Step {
    const char  *name;
    Claim        claim;       // sent once when the step starts
    bool         restart;     // the display restarts first: NAME table and model lost
    uint32_t     holdMs;
    Frame        frames[4];   // sent every 250 ms for holdMs
    int          nFrames;
    bool         otherPilot;  // feed a heading-to-steer the way PGN 127237 would
    // Expected at the end of the step.
    Mode         mode;
    Source       source;
    SetpointKind spKind;
    float        spDeg;       // NaN: none
    Status       status;      // STATUS_UNKNOWN here = not checked
    int          sa;          // the pilot's address, ANY_SA = not checked
};

const float NONE = NAN;
const Status ANY = STATUS_UNKNOWN;

// Built at first use: the frame helpers are functions.
Step *script(int &n) {
    static Step s[] = {
        { "noclaim", CLAIM_NONE, false, 2500, { mode10(0x10), state(1, 0xFA) }, 2, false,
          MODE_UNKNOWN, SRC_NONE, SP_NONE, NONE, ANY, ANY_SA },
        { "standby", CLAIM_PILOT_AT_3, false, 2500, { mode10(0x08), state(0, 0xF8) }, 2, false,
          MODE_STANDBY, SRC_MODE_REPORT, SP_NONE, NONE, ANY, 3 },
        { "auto", CLAIM_NONE, false, 2500,
          { mode10(0x10), setpoint(2, 123.f), state(1, 0xFA), status2(16) }, 4, false,
          MODE_AUTO, SRC_MODE_REPORT, SP_HEADING, 123.f, STATUS_AUTOMATIC, ANY_SA },
        { "wind_stbd", CLAIM_NONE, false, 2500,
          { mode10(0x400), setpoint(3, 35.f), state(3, 0xFA) }, 3, false,
          MODE_WIND, SRC_MODE_REPORT, SP_WIND, 35.f, ANY, ANY_SA },
        { "wind_port", CLAIM_NONE, false, 2500, { mode10(0x400), setpoint(3, -35.f) }, 2, false,
          MODE_WIND, SRC_MODE_REPORT, SP_WIND, -35.f, ANY, ANY_SA },
        // Port 10 degrees as a signed int16 (-1745 = 0xF92F) - above 2 pi,
        // which the default unsigned encoding cannot produce: the wind
        // setpoint is cleared ("---") instead of shown wrong, and the log
        // says why. (Port angles above 15.5 degrees look valid either way -
        // see the on-board check in ApDecode.h.)
        { "wind_raw_signed", CLAIM_NONE, false, 2500,
          { mode10(0x400), setpointRaw(3, 0xF92F) }, 2, false,
          MODE_WIND, SRC_MODE_REPORT, SP_NONE, NONE, ANY, ANY_SA },
        { "nav", CLAIM_NONE, false, 2500,
          { mode10(0x40), setpoint(10, 250.f), state(4, 0xFA) }, 3, false,
          MODE_NAV, SRC_MODE_REPORT, SP_HEADING, 250.f, ANY, ANY_SA },
        { "no_drift", CLAIM_NONE, false, 2500, { mode10(0x100), setpoint(11, 30.f) }, 2, false,
          MODE_NO_DRIFT, SRC_MODE_REPORT, SP_HEADING, 30.f, ANY, ANY_SA },
        // Mode words the rules cannot decide: the pilot is engaged, the mode
        // is not known - ACTIVE, never a guessed mode. ACTIVE shows the newer
        // of the two setpoints.
        // The wind and the nav bit at once (two mode bits): ACTIVE, with the
        // wind angle sent next to it (the heading of NO DRIFT is older).
        { "active_two_bits", CLAIM_NONE, false, 2500, { mode10(0x440), setpoint(3, 140.f) }, 2, false,
          MODE_ACTIVE, SRC_MODE_REPORT, SP_WIND, 140.f, ANY, ANY_SA },
        // The heading bit with bit 16, which no rule knows: ACTIVE, not AUTO -
        // not even with a heading setpoint (mode byte 2) next to it, because
        // 65341 never decides the mode.
        { "active_bit16", CLAIM_NONE, false, 2500, { mode10(0x010010), setpoint(2, 123.f) }, 2, false,
          MODE_ACTIVE, SRC_MODE_REPORT, SP_HEADING, 123.f, ANY, ANY_SA },
        // Bit 11 alone: ACTIVE. With it a 65341 whose mode byte (12) is not
        // one of the four known values: no setpoint, both are left alone, so
        // the heading from the step before is still the newer one.
        { "active_bit11", CLAIM_NONE, false, 2500, { mode10(0x800), setpointRaw(12, 1234) }, 2, false,
          MODE_ACTIVE, SRC_MODE_REPORT, SP_HEADING, 123.f, ANY, ANY_SA },
        { "auto_manual", CLAIM_NONE, false, 2500,
          { mode10(0x10), setpoint(2, 123.f), status2(2) }, 3, false,
          MODE_AUTO, SRC_MODE_REPORT, SP_HEADING, 123.f, STATUS_MANUAL, ANY_SA },
        // AUTO with a heading AND a wind setpoint, then a 65341 whose mode
        // byte (0) names neither: the heading stays - no flip to the wind
        // angle, no blanking.
        { "mixed_setpoints", CLAIM_NONE, false, 2500,
          { mode10(0x10), setpoint(2, 123.f), setpoint(3, 40.f), setpointRaw(0, 1234) }, 4, false,
          MODE_AUTO, SRC_MODE_REPORT, SP_HEADING, 123.f, ANY, 3 },
        // Only frames that must be ignored: a foreign manufacturer's
        // "standby" from the pilot's own address, and "standby" from a device
        // of the wrong class. Accepted, two of them would switch to STANDBY.
        { "ignored", CLAIM_WRONG_AT_7, false, 2500,
          { foreign(mode10(0x08)), mode10(0x08, 7), state(0, 0xF8, 7), foreign(state(0, 0xF8)) },
          4, false,
          MODE_AUTO, SRC_MODE_REPORT, SP_HEADING, 123.f, STATUS_MANUAL, 3 },
        // A second pilot computer at SA 5 says STANDBY and another course: the
        // mode, and the setpoint with it, stay with SA 3.
        { "two_pilots", CLAIM_PILOT2_AT_5, false, 2500,
          { mode10(0x10), setpoint(2, 123.f), mode10(0x08, 5), setpoint(2, 300.f, 5) }, 4, false,
          MODE_AUTO, SRC_MODE_REPORT, SP_HEADING, 123.f, ANY, 3 },
        // 65305 falls silent, 65340 goes on: it takes over after 10 s.
        { "fallback_65340", CLAIM_NONE, false, 11500, { state(3, 0xFA), state(3, 0xFA, 7) }, 2, false,
          MODE_WIND, SRC_STATE, SP_NONE, NONE, ANY, 3 },
        // The display restarts (NAME table and model gone) and hears no claim:
        // the pilot computer remembered from before is accepted at once.
        { "restart", CLAIM_NONE, true, 2500, { mode10(0x400), setpoint(3, 35.f) }, 2, false,
          MODE_WIND, SRC_MODE_REPORT, SP_WIND, 35.f, ANY, 3 },
        // Everything silent: "--" after 10 s.
        { "stale", CLAIM_NONE, false, 11000, {}, 0, false,
          MODE_UNKNOWN, SRC_NONE, SP_NONE, NONE, ANY, ANY_SA },
        // A pilot that sends only PGN 127237: no mode, heading-to-steer shown.
        { "other_pilot", CLAIM_NONE, false, 2500, {}, 0, true,
          MODE_UNKNOWN, SRC_NONE, SP_NONE, NONE, ANY, ANY_SA },
        // STANDBY next to 127237: heading-to-steer shown as before.
        { "standby_127237", CLAIM_NONE, false, 2500, { mode10(0x08), state(0, 0xF8) }, 2, true,
          MODE_STANDBY, SRC_MODE_REPORT, SP_NONE, NONE, ANY, 3 },
    };
    n = (int)(sizeof(s) / sizeof(s[0]));
    return s;
}

// ---- Decoder steps ------------------------------------------------------------
// One frame each, straight into ApDecode::decode() with the pilot computer's
// NAME - no debounce, no tracker: what the rules make of that one frame.
struct DecodeStep {
    const char  *name;
    Frame        f;
    bool         used;     // false: the decoder must drop it as undecoded
    Mode         mode;     // 65305 report 10 and 65340: the mode;
                           // 65341: its mode byte as a Mode (setpointMode)
    SetpointKind kind;     // 65341: the setpoint it carries
    Status       status;   // 65305 report 2 (ANY elsewhere)
    float        spDeg;    // 65341: the setpoint in degrees, NaN: none
};

DecodeStep *decodeSteps(int &n) {
    static DecodeStep s[] = {
        // 65305 report 10: the five known bits, one at a time.
        { "w_standby",      mode10(0x08),     true, MODE_STANDBY,  SP_NONE, ANY, NONE },
        { "w_auto",         mode10(0x10),     true, MODE_AUTO,     SP_NONE, ANY, NONE },
        { "w_nav",          mode10(0x40),     true, MODE_NAV,      SP_NONE, ANY, NONE },
        { "w_no_drift",     mode10(0x100),    true, MODE_NO_DRIFT, SP_NONE, ANY, NONE },
        { "w_wind",         mode10(0x400),    true, MODE_WIND,     SP_NONE, ANY, NONE },
        // Standby wins, whatever else is set.
        { "w_standby_auto", mode10(0x18),     true, MODE_STANDBY,  SP_NONE, ANY, NONE },
        { "w_standby_b16",  mode10(0x010008), true, MODE_STANDBY,  SP_NONE, ANY, NONE },
        // What the rules cannot decide: ACTIVE, never AUTO or another mode.
        { "w_two_bits",     mode10(0x440),    true, MODE_ACTIVE,   SP_NONE, ANY, NONE },
        { "w_auto_b16",     mode10(0x010010), true, MODE_ACTIVE,   SP_NONE, ANY, NONE },
        { "w_b11_alone",    mode10(0x800),    true, MODE_ACTIVE,   SP_NONE, ANY, NONE },
        { "w_b16_alone",    mode10(0x010000), true, MODE_ACTIVE,   SP_NONE, ANY, NONE },
        // No bit at all says nothing.
        { "w_zero",         mode10(0),        false, MODE_UNKNOWN, SP_NONE, ANY, NONE },
        // 65305 report 2.
        { "r2_manual",      status2(2),       true, MODE_UNKNOWN,  SP_NONE, STATUS_MANUAL, NONE },
        { "r2_automatic",   status2(16),      true, MODE_UNKNOWN,  SP_NONE, STATUS_AUTOMATIC, NONE },
        { "r2_other",       status2(5),       false, MODE_UNKNOWN, SP_NONE, ANY, NONE },
        // 65340, as measured.
        { "s_standby",      state(0, 0xF8),   true, MODE_STANDBY,  SP_NONE, ANY, NONE },
        { "s_heading",      state(1, 0xFA),   true, MODE_AUTO,     SP_NONE, ANY, NONE },
        { "s_wind",         state(3, 0xFA),   true, MODE_WIND,     SP_NONE, ANY, NONE },
        { "s_nav",          state(4, 0xFA),   true, MODE_NAV,      SP_NONE, ANY, NONE },
        { "s_engaged",      state(0, 0xFA),   true, MODE_ACTIVE,   SP_NONE, ANY, NONE },
        { "s_other",        state(1, 0x00),   false, MODE_UNKNOWN, SP_NONE, ANY, NONE },
        // 65341: the four known mode bytes carry a setpoint ...
        { "sp_heading",     setpoint(2, 123.f),  true, MODE_AUTO,     SP_HEADING, ANY, 123.f },
        { "sp_wind",        setpoint(3, 35.f),   true, MODE_WIND,     SP_WIND,    ANY, 35.f },
        { "sp_nav",         setpoint(10, 250.f), true, MODE_NAV,      SP_HEADING, ANY, 250.f },
        { "sp_no_drift",    setpoint(11, 30.f),  true, MODE_NO_DRIFT, SP_HEADING, ANY, 30.f },
        // ... any other one none, and reads ACTIVE (not AUTO).
        { "sp_mode_12",     setpointRaw(12, 1234), true, MODE_ACTIVE, SP_NONE, ANY, NONE },
        { "sp_mode_0",      setpointRaw(0, 1234),  true, MODE_ACTIVE, SP_NONE, ANY, NONE },
    };
    n = (int)(sizeof(s) / sizeof(s[0]));
    return s;
}

void inject(const Frame &f, uint32_t now) {
    if (f.pgn == PGN_ADDRESS_CLAIM) { ApPipeline::addressClaim(f.sa, f.d, f.len); return; }
    auto lk = data.lock();
    ApPipeline::frame(f.pgn, f.sa, f.d, f.len, now);
}

bool near(float a, float b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return std::fabs(a - b) < 0.1f;
}

// Runs every decoder step once; returns how many failed.
int runDecodeSteps(int &n) {
    const DecodeStep *v = decodeSteps(n);
    uint64_t pilot = 0;
    ApDecode::nameFromClaim(CLAIM_PILOT.d, CLAIM_PILOT.len, pilot);
    int failed = 0;
    for (int i = 0; i < n; i++) {
        const DecodeStep &c = v[i];
        const Facts r = ApDecode::decode(c.f.pgn, c.f.sa, c.f.d, c.f.len, true, pilot);
        const Mode got = c.f.pgn == PGN_SETPOINT ? r.setpointMode : r.mode;
        const float deg = r.setpointRad * 57.2957795f;   // NaN stays NaN
        bool ok = r.ignored == (c.used ? IGN_NONE : IGN_UNDECODED);
        if (ok && c.used) {
            if (c.f.pgn == PGN_SETPOINT)
                ok = r.hasSetpoint && got == c.mode && r.setpointKind == c.kind;
            else if (c.status != ANY)
                ok = r.hasStatus && !r.hasMode && r.status == c.status;
            else
                ok = r.hasMode && got == c.mode && r.engaged == isEngaged(c.mode);
        }
        if (ok) ok = near(deg, c.spDeg);
        if (!ok) failed++;
        printf("[ap-frames] D%02d %-16s %s  ignored=%u mode=%s status=%u sp=%u/%.1f\n",
               i, c.name, ok ? "PASS" : "FAIL", (unsigned)r.ignored, modeKey(got),
               (unsigned)r.status, (unsigned)r.setpointKind, (double)deg);
    }
    return failed;
}

} // namespace

// -1 while running, then the number of failed steps.
int simApFramesTick(uint32_t now, const char *shotsDir) {
    static int      step = -1, nSteps = 0, nDecode = 0, failed = 0, claimedFor = -1;
    static uint32_t stepT = 0, lastSend = 0;
    static Step    *s = nullptr;
    if (step < 0) {
        s = script(nSteps);
        decodeSteps(nDecode);
        printf("[ap-frames] %d decoder steps, then %d steps\n", nDecode, nSteps);
        failed = runDecodeSteps(nDecode);
        ApPipeline::resetNames();
        ApPipeline::begin();          // a start: nothing remembered yet
        { auto lk = data.lock(); data.clearValues(); }
        dispMgr.showScreen(SCR_AUTOPILOT);
        step = 0; stepT = now; lastSend = 0;
    }
    const Step &st = s[step];

    if (claimedFor != step) {
        claimedFor = step;
        if (st.restart) {
            printf("[ap-frames] display restart: NAME table and model lost\n");
            ApPipeline::resetNames();
            { auto lk = data.lock(); data.clearValues(); }
            ApPipeline::begin();
        }
        if (st.claim == CLAIM_PILOT_AT_3)  inject(CLAIM_PILOT, now);
        if (st.claim == CLAIM_WRONG_AT_7)  inject(CLAIM_WRONG, now);
        if (st.claim == CLAIM_PILOT2_AT_5) inject(CLAIM_PILOT2, now);
    }

    // The boat around it: a compass heading near the setpoints, some rudder.
    {
        auto lk = data.lock();
        data.hdg = 118.f + 4.f * std::sin(now / 3000.f);
        data.lastHdgUpdate = data.lastHdgRx = now;   // the wind's and BusWatch's stamp
        data.apRudder = 2.5f * std::sin(now / 1700.f);
        data.lastApUpdate = now;                     // the rudder's stamp (BusWatch)
        if (st.otherPilot) data.apTargetHeading = 200.f;
    }

    if (st.nFrames && now - lastSend >= 250) {
        lastSend = now;
        for (int i = 0; i < st.nFrames; i++) inject(st.frames[i], now);
    }
    ApPipeline::persistDue();     // what the bus task's loop does

    if (now - stepT < st.holdMs) return -1;

    // End of step: check, capture, next.
    uint8_t mode, src, kind, status, sa;
    float sp;
    {
        auto lk = data.lock();
        mode = data.apMode; src = data.apModeSource;
        kind = data.apSetpointKind; sp = data.apSetpoint; status = data.apStatus;
        sa = data.apPilotSa;
    }
    const bool ok = mode == st.mode && src == st.source &&
                    kind == st.spKind && near(sp, st.spDeg) &&
                    (st.status == ANY || status == st.status) &&
                    (st.sa == ANY_SA || sa == st.sa);
    if (!ok) failed++;
    printf("[ap-frames] %02d %-16s %s  mode=%s src=%u sa=%u sp=%u/%.1f status=%u\n",
           step, st.name, ok ? "PASS" : "FAIL", modeKey((Mode)mode),
           (unsigned)src, (unsigned)sa, (unsigned)kind, (double)sp, (unsigned)status);
    if (shotsDir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/ap_%02d_%s.bmp", shotsDir, step, st.name);
        sim_hal_screenshot(path);
    }
    if (++step >= nSteps) {
        printf("[ap-frames] %s (%d of %d steps failed)\n",
               failed ? "FAILURES ABOVE" : "ALL PASS", failed, nDecode + nSteps);
        return failed;
    }
    stepT = now;
    lastSend = 0;
    return -1;
}
