#pragma once

// ============================================================
// PaintPhase.h - per-phase paint stopwatch for the canvas screens that have
// no instrumentation of their own (Clock, Rudder, AIS).
//
// MEASUREMENT BUILDS ONLY. Active when BOARD_PANEL_1024X600 && PERF_PAINT_PHASES
// (-DPERF_PAINT_PHASES, passed through PLATFORMIO_BUILD_FLAGS - never put it
// in platformio.ini). In every other build each PP_* macro expands to
// do { } while (0), its arguments are dropped unseen, and PaintPhaseSet,
// PaintPhaseLap and ppReport() do not exist. The released 4" and the normal
// 7B/5B builds therefore compile to the same code as before this header.
//
// What it answers: dispPaintTake() (DisplayManager.cpp) only says how long a
// screen's whole update() took. This says WHICH part of it, as max and mean
// per phase, over the window between two reports.
//
// How it works:
//   * A screen owns one static PaintPhaseSet. Its update() opens the frame
//     with PP_BEGIN; every PP_END(s, ix) charges the time since the previous
//     stamp to phase ix and restarts the stamp, so consecutive PP_ENDs tile
//     the frame without gaps. PP_FRAME closes the frame.
//   * The stamp lives in the SET, not in a local as with WindScreen's
//     WS_PHASE_BEGIN, so a phase may start in update() and end inside a draw
//     function, or the other way round.
//   * PP_SKIP restarts the stamp without charging anything - at the entry of
//     a draw function that does not run every frame, and on an early return,
//     so the gap is never billed to whichever phase happens to come next.
//   * PP_ADD charges a duration that was summed separately, in pieces, with a
//     PaintPhaseLap (e.g. one call inside a loop). It does not touch the stamp,
//     so that time is ALSO inside the enclosing PP_END phase.
//   * PP_AUX keeps the maximum of a plain counter (targets drawn, ...).
//   * PP_ONLY(stmt) is a statement that exists only in measurement builds,
//     for the odd local counter that feeds PP_AUX.
//
// Times are wall clock from esp_timer_get_time(), so they INCLUDE any
// renderYield() taken inside a phase and any interrupt time stolen from the
// core - that is what the phase really costs the frame. A stamp itself costs
// about a microsecond at most; a lap inside a long loop adds its two stamps
// per iteration to the enclosing phase (the tide's 565 columns: ~1 ms).
//
// WindScreen keeps its own WS_PHASE_* (WindScreen.cpp), which feed the [HB]
// line; nothing here touches them.
// ============================================================

#if defined(BOARD_PANEL_1024X600) && defined(PERF_PAINT_PHASES)

#define PP_ENABLED 1

#include <Arduino.h>      // Serial (sim: sim/Arduino.h)
#include <esp_timer.h>    // esp_timer_get_time() (sim: sim/esp_timer.h)
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static constexpr int PP_MAX_PHASES = 8;
static constexpr int PP_MAX_AUX    = 2;

struct PaintPhaseSet {
    int64_t  t, t0;                     // running stamp, start of the frame
    uint32_t maxUs[PP_MAX_PHASES];      // per phase: worst single charge
    uint64_t sumUs[PP_MAX_PHASES];      //            sum of all charges
    uint32_t cnt[PP_MAX_PHASES];        //            number of charges
    uint32_t frames, frameMaxUs;        // PP_FRAME count, worst frame
    uint32_t auxMax[PP_MAX_AUX];        // PP_AUX maxima
};

// A duration collected in pieces and charged once with PP_ADD.
struct PaintPhaseLap { int64_t t; uint32_t us; };

static inline void ppCharge(PaintPhaseSet &s, int ix, uint32_t us) {
    if (us > s.maxUs[ix]) s.maxUs[ix] = us;
    s.sumUs[ix] += us;
    s.cnt[ix]++;
}

#define PP_BEGIN(s)       do { (s).t = (s).t0 = esp_timer_get_time(); } while (0)
#define PP_SKIP(s)        do { (s).t = esp_timer_get_time(); } while (0)
#define PP_END(s, ix)     do {                                              \
        const int64_t _ppNow = esp_timer_get_time();                        \
        ppCharge((s), (ix), (uint32_t)(_ppNow - (s).t));                    \
        (s).t = _ppNow;                                                     \
    } while (0)
#define PP_ADD(s, ix, us) do { ppCharge((s), (ix), (uint32_t)(us)); } while (0)
#define PP_FRAME(s)       do {                                              \
        const uint32_t _ppD = (uint32_t)(esp_timer_get_time() - (s).t0);    \
        (s).frames++;                                                       \
        if (_ppD > (s).frameMaxUs) (s).frameMaxUs = _ppD;                   \
    } while (0)
#define PP_AUX(s, k, v)   do {                                              \
        const uint32_t _ppV = (uint32_t)(v);                                \
        if (_ppV > (s).auxMax[k]) (s).auxMax[k] = _ppV;                     \
    } while (0)

#define PP_LAP_DECL(l)    PaintPhaseLap l = { 0, 0 }
#define PP_LAP_START(l)   do { (l).t = esp_timer_get_time(); } while (0)
#define PP_LAP_STOP(l)    do { (l).us += (uint32_t)(esp_timer_get_time() - (l).t); } while (0)

#define PP_ONLY(...)      __VA_ARGS__

// Bounded append for ppReport(); a line that would overflow is cut, not lost.
static inline void ppCat(char *buf, size_t cap, size_t &len, const char *fmt, ...) {
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (w > 0) len = (len + (size_t)w < cap) ? len + (size_t)w : cap - 1;
}

// Microseconds -> tenths of a millisecond, rounded; printed as "%u.%u".
static inline unsigned ppTenths(uint32_t us) { return (unsigned)((us + 50u) / 100u); }

// Prints ONE line for the window since the last call, then clears the set
// (reset on read - exactly one consumer per set, the heartbeat in main.cpp):
//
//   [ph/<tag>] frames=N frame=<max> <name>=<max>/<mean>[x<cnt>] ... [<aux>=<max> ...]
//
// All times in ms with 0.1 resolution. x<cnt> appears only when the phase ran
// in fewer (or more) frames than were counted - a draw that runs every 8 s,
// a frame that skipped the canvas - so the mean is always per run, never
// diluted by frames that did not run it. A phase that never ran prints
// "<name>=-". auxNames may be nullptr, and a nullptr entry is not printed.
// Nothing is printed for a window in which the screen was never painted.
static inline void ppReport(const char *tag, const char *const *names, int n,
                            PaintPhaseSet &s, const char *const *auxNames) {
    if (s.frames == 0) return;
    if (n > PP_MAX_PHASES) n = PP_MAX_PHASES;

    char line[480];
    size_t len = 0;
    line[0] = '\0';
    const unsigned fm = ppTenths(s.frameMaxUs);
    ppCat(line, sizeof line, len, "[ph/%s] frames=%u frame=%u.%u",
          tag, (unsigned)s.frames, fm / 10u, fm % 10u);
    for (int i = 0; i < n; i++) {
        if (!s.cnt[i]) { ppCat(line, sizeof line, len, " %s=-", names[i]); continue; }
        const unsigned mx = ppTenths(s.maxUs[i]);
        const unsigned mn = ppTenths((uint32_t)(s.sumUs[i] / s.cnt[i]));
        ppCat(line, sizeof line, len, " %s=%u.%u/%u.%u",
              names[i], mx / 10u, mx % 10u, mn / 10u, mn % 10u);
        if (s.cnt[i] != s.frames) ppCat(line, sizeof line, len, "x%u", (unsigned)s.cnt[i]);
    }
    if (auxNames)
        for (int k = 0; k < PP_MAX_AUX; k++)
            if (auxNames[k]) ppCat(line, sizeof line, len, " %s=%u",
                                   auxNames[k], (unsigned)s.auxMax[k]);
    Serial.println(line);
    Serial.flush();

    // The stamps are left alone: the report runs between frames, and the next
    // frame starts with PP_BEGIN anyway.
    memset(s.maxUs,  0, sizeof s.maxUs);
    memset(s.sumUs,  0, sizeof s.sumUs);
    memset(s.cnt,    0, sizeof s.cnt);
    memset(s.auxMax, 0, sizeof s.auxMax);
    s.frames = 0;
    s.frameMaxUs = 0;
}

#else

#define PP_ENABLED 0

#define PP_BEGIN(s)       do { } while (0)
#define PP_SKIP(s)        do { } while (0)
#define PP_END(s, ix)     do { } while (0)
#define PP_ADD(s, ix, us) do { } while (0)
#define PP_FRAME(s)       do { } while (0)
#define PP_AUX(s, k, v)   do { } while (0)
#define PP_LAP_DECL(l)    do { } while (0)
#define PP_LAP_START(l)   do { } while (0)
#define PP_LAP_STOP(l)    do { } while (0)
#define PP_ONLY(...)      do { } while (0)

#endif
