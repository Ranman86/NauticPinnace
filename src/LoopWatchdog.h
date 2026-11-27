#pragma once
// ============================================================================
// LoopWatchdog - a watchdog on the main loop.
//
// Until now a display whose loop() had hung stayed frozen until somebody cut
// the power: main.cpp switches the interrupt watchdog off, and on the 7B/5B
// with WiFi on the IDF task watchdog is gone as well. This module arms the RTC
// watchdog (RWDT, in the RTC domain, clocked from the ~136 kHz RC oscillator)
// and feeds it from loop() - at most every 100 ms, from ONE place, right after
// OtaGuard::loopTick(). A pass that does not come back within 30 s resets the
// chip. Stage 0 is programmed as a system reset (rst 0x09) with an RTC reset
// (rst 0x10) as stage 1, 2 s later - but on all three boards the bench
// (10.11.2026) only ever saw stage 0 end as rst 0x10 itself, at 30.0-30.3 s,
// which also boots a core a panic handler stalled; stage 1 stays as a fallback.
// Both read as ESP_RST_WDT and are classified alike (LoopWatchdog.cpp).
//
// On the 4" (IDF 4.4) the IDF task watchdog is still live for async_tcp and can
// end some hangs first: a task spinning with interrupts masked on core 1 gave a
// task-watchdog panic after ~5 s (reason 6), on core 0 or both a TG0 reset
// after ~15 s (rst 0x07). Those are reported as task-watchdog resets
// ("task watchdog (rst 0x03)" / "IDF task watchdog (rst 0x07)"), not this
// module's.
//
// It is the ONLY code that touches the RWDT. OtaGuard's update backstop
// (docs/N2K-UPDATE.md 10.3) goes through armBackstop() and is handed over to
// the loop watch at mark-valid (backstopToLoop()).
//
// Phases, stage 1 is always +2 s RTC reset (configured; not reached so far):
//   setup budget  120 s from bootEarly() to setupDone(), never fed
//   loop watch     30 s from setupDone(), fed from loop()
//   backstop      PENDING_VERIFY only: 300 s, 10 s around OtaGuard's own
//                 rollback restart, never fed - OtaGuard's, as before
//   off           4" and 5B while a USB host is attached (see below), and
//                 builds with -DLOOP_WDT_OFF
//
// USB gate (4" and 5B, console on the S3's own USB): ESP Web Tools / esptool-js
// does not switch the RWDT off, so a web flash of a running, plugged-in unit
// would be cut after at most 30 s. With a host on the bus the watch is off;
// it comes back after 10 s without one. The 7B's CH343 drives EN (a chip
// reset), it needs no gate. -DLOOP_WDT_USB_OK (bench builds flashed with
// esptool only) keeps it armed with a host.
//
// N2K gate: while the N2K task exists, a feed also needs its loop counter
// (OtaGuard::g_n2kLoopCount) to have advanced within the last 30 s. A task
// on core 0 that spins with interrupts on starves it without stopping loop().
//
// Panic handler (7B/5B): IDF 5 re-arms the RTC watchdog for 10 s when a panic
// starts. Linked through a wrapper here (-Wl,--wrap, platformio.ini) that
// feeds our count first, so a panic late in a watched phase no longer loses
// its backtrace and core dump to an RTC reset at once.
//
// Diagnostics: an RTC_NOINIT record (phase, screen, uptime, a core-0 stamp)
// classified at the next boot - "[wdt] last reset: ..." in the boot log and
// in bootSummary(), "reset" and "loopWatchdog" in GET /api/info, NVS
// "loopwd" (count, last) for the field. No setting, no config key.
// ============================================================================
#include <stdint.h>

namespace LoopWatchdog {

// Phase markers: where loop() was when the record was last written. One
// one-byte store into the RTC record each; the boot after a reset of ours
// names the last one. In setup(), PH_SETUP + the number of the boot mark.
enum : uint8_t {
    PH_NONE = 0,
    PH_TOP,        // the top of loop(): OtaGuard::loopTick(), this module
    PH_BUTTONS,    // dispMgr.handleButtons()
    PH_WIFI,       // webCfg.loop(): WiFi supervision and reconnect
    PH_RESTART,    // restartIfDue(), and right before every ESP.restart()
    PH_BSH,        // bshLocalOffsetTick()
    PH_PAINT,      // dispMgr.update(): canvas paint, overlays, deferred work
    PH_TICK,       // displayTick(): lv_timer_handler, render, flush, touch
    PH_LATE,       // the late expander check and the boot summary
    PH_HB,         // the heartbeat
    PH_DIAG,       // displayDiag()
    PH_DELAY,      // delay(5) at the end of the pass
    PH_TEST,       // a -DPANIC_PATH_TEST hang
    PH_SETUP = 0x80,
};

#if defined(PANIC_PATH_TEST)
// Test kinds that loop() itself has to carry out (GET /api/panic-test).
enum : uint8_t { T_NONE = 0, T_LOOP, T_LVGL, T_BLOCK25, T_BLOCK35, T_RSTHANG, T_LATEABORT };
#endif

}  // namespace LoopWatchdog

#ifndef SIMULATOR
#include <ArduinoJson.h>

namespace LoopWatchdog {

// main.cpp, in boot order.
void bootEarly(bool pending);   // right after OtaGuard::bootEarly()
void usbHost(bool present);     // after the USB console check (4", 5B)
void setupMark(uint8_t n);      // every bootMark(): phase, and the USB gate
void bootReport();              // once Serial is up: the [wdt] lines
void setupDone();               // right after OtaGuard::setupDone()
void loopTick();                // the ONE feed point, every loop() pass
void phase(uint8_t p);          // phase marker, see above

// The N2K task's loop (or the demo task): a core-0 stamp in the record,
// written about once a second.
void core0Stamp();

// OtaGuard (docs/N2K-UPDATE.md 10.3). armBackstop() programs the timer only;
// backstopToLoop() only raises a flag (and marks the record confirmed) that
// the next loopTick() acts on, so the RWDT has one writer at a time.
void armBackstop(uint32_t ms);
void backstopToLoop();

void     printLastResetLine();  // the "[wdt] last reset" line, again
uint32_t takeWindowGapMs();     // largest feed gap since the last call (HB)

// GET /api/info: "reset" {reason, rtc0, rtc1}, and the "loopWatchdog" block.
void fillReset(JsonObject o);
void fillInfo(JsonObject o);

#if defined(PANIC_PATH_TEST)
void    testRequest(uint8_t kind);   // web handler
uint8_t takeTestRequest();           // loop()
#endif

}  // namespace LoopWatchdog

#else  // SIMULATOR: no RTC watchdog on a PC.

namespace LoopWatchdog {
inline void bootEarly(bool) {}
inline void usbHost(bool) {}
inline void setupMark(uint8_t) {}
inline void bootReport() {}
inline void setupDone() {}
inline void loopTick() {}
inline void phase(uint8_t) {}
inline void core0Stamp() {}
inline void armBackstop(uint32_t) {}
inline void backstopToLoop() {}
inline void printLastResetLine() {}
inline uint32_t takeWindowGapMs() { return 0; }
template <class T> inline void fillReset(T) {}
template <class T> inline void fillInfo(T) {}
}  // namespace LoopWatchdog

#endif
