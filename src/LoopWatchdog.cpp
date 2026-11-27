// ============================================================================
// LoopWatchdog.cpp - see LoopWatchdog.h.
// ============================================================================
#ifndef SIMULATOR
#include "LoopWatchdog.h"
#include "Version.h"
#include "n2kupdate/OtaGuard.h"
#include "display/DisplayManager.h"
#include "UsbConsole.h"      // consoleFlushFull(): the test hang's last line
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_idf_version.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <nvs.h>
// The RTC watchdog through the WDT HAL, as the IDF's own panic handler arms
// it: rtc_wdt.h is ESP32/S2-only in IDF 5.5 and does not even compile for
// the S3 in 4.4 (its stage constants are missing there). The header calls
// itself "not public api" - re-check this file on any core or IDF upgrade.
// On IDF 5.5 the functions are in ROM (CONFIG_HAL_WDT_USE_ROM_IMPL), on 4.4
// in IRAM: no code of ours is involved in a feed but these three calls.
#include <hal/wdt_hal.h>
#include <soc/rtc.h>
#include <esp_private/esp_clk.h>   // esp_clk_slowclk_cal_get()
#include <string.h>

// main.cpp: the N2K task exists (it does not when the device booted in demo
// mode, or when creating it failed - then there is nothing to gate the feed on).
extern volatile bool g_n2kTaskRunning;

// ---- timeouts, each overridable with -D for bench builds ---------------------------
// Such a build prints "TEST BUILD LOOP_WDT_TIMES" at boot, which
// tools/release_checks.py refuses: a leftover -DLOOP_WDT_MS=5000 would reset
// healthy units, a -DUSB_REARM_MS=0 cut browser flashes.
#if defined(LOOP_WDT_MS) || defined(SETUP_WDT_MS) || defined(RTC_STAGE_MS) || defined(USB_REARM_MS)
#define LWD_TIMES_CHANGED 1
#else
#define LWD_TIMES_CHANGED 0
#endif
#ifndef LOOP_WDT_MS
#define LOOP_WDT_MS 30000       // the loop watch: >= 10x the longest pass found in code
#endif
#ifndef SETUP_WDT_MS
#define SETUP_WDT_MS 120000     // setup(): ~45-56 s worst case in code, 3.3-4.0 s measured
#endif
#ifndef RTC_STAGE_MS
#define RTC_STAGE_MS 2000       // stage 1, RTC reset 2 s after stage 0: a fallback, never reached (stage 0 measured as rst 0x10)
#endif
#ifndef USB_REARM_MS
#define USB_REARM_MS 10000      // no USB host for this long before the watch comes back
#endif

// The USB gate exists where the console is the S3's own USB-Serial/JTAG.
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE && !defined(LOOP_WDT_USB_OK)
#define LWD_USB_GATE 1
#else
#define LWD_USB_GATE 0
#endif

namespace LoopWatchdog {
namespace {

constexpr uint32_t FEED_MIN_MS  = 100;     // at most ten feeds a second
// A loop that has run this long after setup() is healthy: inARow starts over,
// so "in a row" means a reboot loop and isolated resets keep reaching NVS.
constexpr uint32_t HEALTHY_RUN_MS = 600000;
// The N2K gate. Evidence that the N2K task's loop never legitimately stops
// for this long: every unit of N2K update work is bounded (one 4 KB sector
// erase <= 0.4 s, a TX-idle wait <= 250 ms, esp_ota_end / set_boot_partition
// "well under 1 s" - N2K-UPDATE.md 9.1), a send waits at most 5 ms a frame,
// the bus-off recovery never blocks, and the demo branch keeps it turning as
// well. OtaGuard's H4 already rolls an update back after 5 + 15 s of it.
constexpr uint32_t N2K_STALL_MS = 30000;

// ---- the reset record ------------------------------------------------------------------
// RTC slow memory: survives both RTC watchdog resets (rst 0x09 and 0x10),
// random after a power-on - hence the magic and the image id (RTC_NOINIT
// addresses change between builds). Core 1 (the loop task) writes everything
// but c0S, confirmed and panicked; core 0 writes c0S and confirmed, each in
// bytes of their own: c0S the N2K or the demo task, confirmed the esp_timer
// task (markValid). panicked is the panic wrapper's (7B/5B), from whichever
// core panics; bootEarly() clears it with the rest.
struct Record {
    uint32_t magic;
    uint8_t  id[8];      // OtaGuard::runningImageId()
    uint8_t  mode;       // M_*
    uint8_t  phase;      // PH_*: one-byte stores from phase()
    uint8_t  screen;     // dispMgr.currentIndex(), once a second
    uint8_t  flags;      // F_*
    uint8_t  inARow;     // resets of ours in a row, cleared after a healthy run
    uint8_t  confirmed;  // backstopToLoop() ran: the image was marked valid
    uint8_t  panicked;   // a panic handler re-armed the RTC watchdog (see the end)
    uint8_t  pad;
    uint32_t upS;        // the loop's last second
    uint32_t c0S;        // core 0's last second (N2K task / demo task)
};
static_assert(sizeof(Record) == 28, "the reset record layout");
constexpr uint32_t REC_MAGIC = 0x4E505744UL;   // "NPWD"
RTC_NOINIT_ATTR Record s_rec;

enum : uint8_t { M_OFF = 0, M_SETUP, M_LOOP, M_BACKSTOP, M_USB };
const char *const MODE_NAMES[] = { "off", "setup", "loop", "backstop", "usb" };
constexpr uint8_t F_FEED_HELD = 0x01;          // the N2K gate held the feeds back

// What the last reset was. Ours: K_LOOP .. K_STAGE1 and K_HANDOVER (the
// backstop of an image that was confirmed but whose loop never took the
// watch over). New kinds only ever go at the end: NVS "last" stores the number.
// K_STAGE1 is no longer produced - stage 0 itself reads back as rst 0x10, so
// a 0x10 cannot tell the stages apart - and stays so that NVS records written
// by earlier 1.3.0-dev images still read as "stage1".
enum : uint8_t { K_NONE = 0, K_LOOP, K_N2K, K_SETUP, K_BACKSTOP, K_STAGE1,
                 K_RWDT_OTHER, K_RTC_OTHER, K_TASK_WDT, K_HANDOVER };
const char *const KIND_KEYS[] = { "none", "loop", "n2k", "setup", "backstop", "stage1",
                                  "rwdt", "rtc", "taskwdt", "handover" };
inline bool ours(uint8_t k) { return (k >= K_LOOP && k <= K_STAGE1) || k == K_HANDOVER; }

// ROM reset causes (soc/reset_reasons.h on 5.5, esp32s3/rom/rtc.h on 4.4).
// Both RTC watchdog codes can be our stage 0: measured on 7B, 5B and 4" it
// comes back as 0x10, never as the 0x09 it is programmed for (see the timer).
// 0x10 is also the bootloader's watchdog and the one a panic handler arms.
constexpr uint8_t RST_TG0_CORE  = 0x07;   // TG0 (IDF task watchdog) core reset
constexpr uint8_t RST_RWDT_CORE = 0x09;   // RTC watchdog, system reset
constexpr uint8_t RST_RWDT_RTC  = 0x10;   // RTC watchdog, RTC reset

const char *const PHASE_NAMES[] = { "-", "top", "buttons", "wifi", "restart", "bsh", "paint",
                                    "tick", "late", "hb", "diag", "delay", "test" };

// The NVS record ("loopwd"/"last"), for the field: survives a power cycle.
struct NvsLast {
    char     version[16];
    uint32_t upS;
    uint8_t  kind, rtc0, mode, phase, screen, flags, inARow, pad;
};
static_assert(sizeof(NvsLast) == 28, "the NVS record layout");
const char *const NVS_NS = "loopwd";

// ---- state -----------------------------------------------------------------------------
wdt_hal_context_t s_ctx;                 // filled by wdt_hal_init()
volatile uint8_t  s_mode = M_OFF;
volatile uint32_t s_timeoutMs = 0;       // stage 0 as armed, 0 = off
volatile bool     s_handoverDue = false; // backstopToLoop(), consumed by loopTick()

// loop task only
uint32_t   s_lastPassMs = 0, s_lastFeedMs = 0, s_lastSecMs = 0, s_setupDoneMs = 0;
uint32_t   s_winGapMs = 0;               // largest feed gap since the last heartbeat
uint32_t   s_maxGapMs = 0;               // ... since boot
uint8_t    s_maxGapPhase = PH_NONE;      // the slowest section of that pass
TickType_t s_segAt = 0, s_segMax = 0;    // the section running, the slowest one so far
uint8_t    s_segPhase = PH_NONE, s_segMaxPhase = PH_NONE;
uint32_t   s_n2kLast = 0, s_n2kAt = 0;   // the N2K loop counter and when it last moved
volatile bool s_feedHeld = false;
#if LWD_USB_GATE
uint32_t   s_hostSeenMs = 0;
#endif

TickType_t s_c0Tick = 0;                 // core 0 only

// the last reset, from bootEarly()
Record   s_prev;
bool     s_prevValid = false;
uint8_t  s_kind = K_NONE, s_rtc0 = 0, s_rtc1 = 0, s_reason = 0;
uint8_t  s_row = 0;                      // resets of ours in a row that led to THIS boot

// NVS, read in setupDone()
uint32_t s_total = 0;
NvsLast  s_last;
bool     s_haveLast = false;

#if defined(PANIC_PATH_TEST)
volatile uint8_t s_testReq = T_NONE;
volatile bool    s_testRstHang = false;
#endif

// ---- the timer -----------------------------------------------------------------------------
// Programmed: stage 0 resets the digital system with the CPUs, TWAI and the
// GPIO matrix (RESET_SYSTEM, rst 0x09) - the same reset level as the restart
// wrapper's, so no bus storm. Stage 1, 2 s later, resets the RTC domain as
// well (rst 0x10): the reset that releases a core a panic handler stalled,
// whose stall bit lives there. Never RESET_CPU (it leaves TWAI running: the
// bus storm of 04.11.) and never the chip-reset option (it would read back as
// a power-on).
//
// Measured (10.11.2026, 7B, 5B and 4", every hang kind of PANIC_PATH_TEST):
// stage 0 always ended as rst 0x10 (RTCWDT_RTC_RST) by itself, 30.0-30.3 s
// after the last feed, with a single ROM banner and no 0x09 before it; stage 1
// was never reached. Also on the 4", whose WDT code is not the ROM's, so it is
// the chip with these settings (flashboot is the suspect, not verified), not
// the code that writes them. That reset boots a stalled core as well (stall0
// came back on all three boards, also at stage-0 time), the reset record
// survives it, and a listening display counted no bus errors at the 7B/5B
// resets (on a near-idle bus). Stage 1 stays as a harmless fallback;
// classify() takes 0x09 and 0x10 alike.
//
// Flashboot AND enable: IDF's own restart arms exactly these two stages with
// flashboot alone, and only that form had been seen to boot through stage 1
// on hardware; the enable bit is what the 4.4 panic handler asks before it
// installs a RWDT setup of its own (7 s, no stage 1). -DLOOP_WDT_FLASHBOOT_ONLY
// drops the enable on IDF 5 (7B/5B), whose panic handler installs its own 10 s
// RTC reset anyway - not needed so far, as stall0 never stayed dark. Never on
// the 4": its panic handler would then replace ours, and a hung 4.4 panic with
// core 0 stalled would hang for good again.
//
// Ticks from the boot's calibration of the RC slow clock (the period in us,
// Q13.19 - measured by the IDF's startup), not from the nominal 136 kHz the
// IDF's own RWDT code uses: the oscillator is only approximately that, and
// the 25 s / 30 s margin of the bench and the 270 s / 300 s one of the
// backstop are not wide. Nominal only should the calibration look wrong.
uint32_t slowClockCal() {                 // 0: use the nominal frequency
    const uint32_t nom = (uint32_t)((1000000ULL << RTC_CLK_CAL_FRACT) / rtc_clk_slow_freq_get_hz());
    const uint32_t cal = esp_clk_slowclk_cal_get();
    return (cal > nom / 2 && cal < nom * 2) ? cal : 0;
}

uint32_t slowClockHz() {
    const uint32_t cal = slowClockCal();
    return cal ? (uint32_t)((1000000ULL << RTC_CLK_CAL_FRACT) / cal) : rtc_clk_slow_freq_get_hz();
}

uint32_t msToTicks(uint32_t ms) {
    const uint64_t us = (uint64_t)ms * 1000ULL;
    const uint32_t cal = slowClockCal();
    return cal ? (uint32_t)((us << RTC_CLK_CAL_FRACT) / cal)
               : (uint32_t)(us * rtc_clk_slow_freq_get_hz() / 1000000ULL);
}

void arm(uint32_t ms, uint8_t mode, bool toRecord) {
    const uint32_t t0 = msToTicks(ms);
    const uint32_t t1 = msToTicks(RTC_STAGE_MS);
    // wdt_hal_init() clears the enable bit and turns every stage off, but
    // not flashboot - which an earlier arm() left set, so the count runs on
    // (rwdt_ll.h: flashboot alone keeps it counting). Re-armed from the
    // unfed setup budget or backstop, it can stand past the new hold already
    // (a 35 s setup with the network out of range, a backstop at 60-270 s
    // handed over or cut to 10 s): a shorter stage-0 hold written before a
    // feed could fire at once. Hence the feed first, while every stage is off,
    // and 30 us (> 3 ticks of the ~136 kHz slow clock the RWDT runs on) for it
    // to land before the hold is written: how fast a write crosses into that
    // clock domain is not documented. The panic wrap below does the same.
    wdt_hal_init(&s_ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&s_ctx);
    wdt_hal_feed(&s_ctx);
    esp_rom_delay_us(30);
    wdt_hal_config_stage(&s_ctx, WDT_STAGE0, t0, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_config_stage(&s_ctx, WDT_STAGE1, t1, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_set_flashboot_en(&s_ctx, true);
#if !(defined(LOOP_WDT_FLASHBOOT_ONLY) && ESP_IDF_VERSION_MAJOR >= 5)
    wdt_hal_enable(&s_ctx);
#endif
    wdt_hal_feed(&s_ctx);                         // the count starts now
    wdt_hal_write_protect_enable(&s_ctx);
    s_timeoutMs = ms;
    s_mode = mode;
    if (toRecord) s_rec.mode = mode;
}

// Both bits: wdt_hal_disable() leaves flashboot set, and flashboot alone keeps
// the timer counting.
void disarm(uint8_t mode) {
    if (!s_ctx.rwdt_dev) wdt_hal_init(&s_ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&s_ctx);
    wdt_hal_disable(&s_ctx);
    wdt_hal_set_flashboot_en(&s_ctx, false);
    wdt_hal_write_protect_enable(&s_ctx);
    s_timeoutMs = 0;
    s_mode = mode;
    s_rec.mode = mode;
}

// Exactly unlock, feed, lock - nothing else in between. ACCEPTED RESIDUAL
// RISK (IDF 5.5): a panic on core 0 reprograms the RWDT without any lock
// against this core (panic_handler.c -> panic.c: unlock, wdt_hal_init,
// unlock, 10 s RTC reset, enable, lock). Should this lock land between its
// second unlock and its stage write, the write protection drops the rest of
// its writes and the RWDT stays off - wdt_hal_init() cleared it - for the
// rest of that panic: a panic handler that then hung would hang for good, as
// before this module. Three register writes keep that window a few cycles.
//
// The panic handler's re-arm itself (wdt_hal_init, then a 10 s RESET_RTC
// stage-0 hold, then the feed inside wdt_hal_enable) runs while our flashboot
// bit is still set, so our count is not stopped and would be fed only after
// that hold is written: a panic while the count already stood at 10 s or more
// - the never-fed phases (setup budget, backstop), the N2K gate holding the
// feeds, a pass longer than 10 s - could end at once in rst 0x10, without
// backtrace or core dump. On 7B/5B the wrap at the end of this file now feeds
// first. What can still cut a panic's report short, unchanged: the race above;
// a third panic entry, which IDF restarts before any re-arm; and a panic
// handler that runs longer than its own 10 s (print + core dump). On the 4"
// there is no wrap and none is needed for this: its 4.4 panic handler leaves an
// enabled RWDT alone - but it does not feed ours either, so a panic within a
// core dump's time of our own pending reset can still be cut off. Bench: the
// PANIC_PATH_TEST kind lateabort (15 s without a feed, then abort()) must give
// a backtrace, rst 0x03 and reason 4, not an immediate rst 0x10.
inline void feed() {
    wdt_hal_write_protect_disable(&s_ctx);
    wdt_hal_feed(&s_ctx);
    wdt_hal_write_protect_enable(&s_ctx);
}

// ---- the last reset --------------------------------------------------------------------------
// rst 0x09 and 0x10 alike: our stage 0 comes back as 0x10 (see the timer), so
// the code tells neither the stage nor - on its own - whose watchdog it was.
// The record does: armed by this image, and in which mode. A panic handler
// that re-armed the RWDT (7B/5B, `panicked`, set by the wrap at the end) took
// it over from our stages - what fired then was its 10 s, not one of ours.
// What the record cannot see, so a line of ours is no proof of what hung: two
// other RWDT resets also come back as 0x10 while this image's record still
// says armed, and read as our kind (loop / n2k / setup / backstop), counted
// in a row. One is the bootloader's own watchdog (CONFIG_BOOTLOADER_WDT_TIME_MS,
// 9 s) should the bootloader hang after one of our resets or restarts - the
// record is rewritten only by bootEarly(). The other, on the 4" only (IDF 4.4:
// no wrap, no `panicked`), is a panic handler that hangs while our stage runs
// on - it leaves our RWDT counting. Both rare. Phase, screen and core-0 stamp
// in the line are always from the record's last write before the reset. With
// the hung panic handler our count did run out, and they show where the image
// stood when it stopped writing (before the panic). With the bootloader's
// watchdog our count never ran out: they are from the record's last writes
// before that earlier reset or restart, not from a hang of this boot - phase,
// screen and seconds since start from when the loop last wrote them, which
// after one of our own loop resets is up to 30 s before it; only the core-0
// stamp (N2K or demo task) can run on until such a reset.
uint8_t classify() {
    const uint8_t m = s_prevValid ? s_prev.mode : (uint8_t)M_OFF;
    const bool armed = m == M_SETUP || m == M_LOOP || m == M_BACKSTOP;
    switch (s_rtc0) {
        case RST_RWDT_CORE:
        case RST_RWDT_RTC:
            if (!armed || s_prev.panicked)
                return s_rtc0 == RST_RWDT_CORE ? K_RWDT_OTHER : K_RTC_OTHER;
            if (m == M_LOOP) return (s_prev.flags & F_FEED_HELD) ? K_N2K : K_LOOP;
            if (m == M_SETUP) return K_SETUP;
            // Confirmed, but no loop() pass consumed the hand-over: the image
            // is valid and boots again, and its loop (or setup) was dead.
            return s_prev.confirmed ? K_HANDOVER : K_BACKSTOP;
        case RST_TG0_CORE:
            return K_TASK_WDT;
        default:
            return K_NONE;
    }
}

const char *phaseText(uint8_t p, char *buf, size_t n) {
    if (p >= PH_SETUP) { snprintf(buf, n, "setup, after boot mark #%u", (unsigned)(p - PH_SETUP)); return buf; }
    return p < sizeof(PHASE_NAMES) / sizeof(PHASE_NAMES[0]) ? PHASE_NAMES[p] : "?";
}

const char *reasonName(uint8_t r) {
    static const char *const N[] = { "unknown", "power-on", "external pin", "software restart",
        "panic", "interrupt watchdog", "task watchdog", "other watchdog", "deep sleep",
        "brownout", "SDIO", "USB", "JTAG", "eFuse", "power glitch", "CPU lockup" };
    return r < sizeof(N) / sizeof(N[0]) ? N[r] : "?";
}

// What core 0 did meanwhile, from its stamp: the loop's record alone cannot
// tell a dead core 0 (whose tick also stops the loop in delay(5)) from a loop
// that stopped by itself. The stamp is the N2K task's (or the demo task's) and
// moves only with the FreeRTOS tick, which only core 0 advances: a stamp that
// ran on proves core 0 and its tick alive, not what the loop was doing. Only
// stuck in delay(5) with the tick alive does it follow that something above
// the loop held core 1.
void core0Text(uint8_t ph, char *b, size_t n) {
    b[0] = 0;
    if (!s_prev.c0S) return;
    const int32_t lead = (int32_t)(s_prev.c0S - s_prev.upS);
    if (lead >= 3 && ph == PH_DELAY)
        snprintf(b, n, "; the core-0 stamp ran on for %d s (tick alive) - a task above the "
                 "loop held core 1", (int)lead);
    else if (lead >= 3)
        snprintf(b, n, "; the core-0 stamp ran on for %d s (core 0 alive) - the loop itself "
                 "stopped: busy, blocked, or held off core 1", (int)lead);
    else if (lead >= -2)
        snprintf(b, n, "; the core-0 stamp stopped at the same time (core 0's tick, the chip, "
                 "or a lock both wait for)");
    else
        snprintf(b, n, "; the core-0 stamp (N2K/demo task) had stopped %d s earlier, while "
                 "core 0's tick still ran", (int)-lead);
}

void lineText(char *b, size_t n) {
    char ph[32], c0[128];
    const unsigned row = s_row;
    const unsigned rst = s_rtc0;          // 0x09 or 0x10 for the RWDT kinds
    switch (s_kind) {
        case K_LOOP:
            core0Text(s_prev.phase, c0, sizeof c0);
            if (s_prev.phase == PH_DELAY)
                snprintf(b, n, "loop watchdog (rst 0x%02x) - loop parked in delay(5) (core 0's tick "
                         "stopped, or a task above the loop held core 1) on screen %u, %u s after "
                         "start%s, %u in a row", rst, (unsigned)s_prev.screen, (unsigned)s_prev.upS,
                         c0, row);
            else
                snprintf(b, n, "loop watchdog (rst 0x%02x) - loop stuck in '%s' on screen %u, %u s "
                         "after start%s, %u in a row", rst, phaseText(s_prev.phase, ph, sizeof ph),
                         (unsigned)s_prev.screen, (unsigned)s_prev.upS, c0, row);
            return;
        case K_N2K:
            snprintf(b, n, "loop watchdog (rst 0x%02x) - the N2K task stopped (its loop counter "
                     "stood still for %u s) while the loop ran, %u s after start, %u in a row",
                     rst, (unsigned)(N2K_STALL_MS / 1000), (unsigned)s_prev.upS, row);
            return;
        case K_SETUP:
            snprintf(b, n, "setup budget (rst 0x%02x) - setup() not done after %u s, %s, %u in a row",
                     rst, (unsigned)(SETUP_WDT_MS / 1000), phaseText(s_prev.phase, ph, sizeof ph),
                     row);
            return;
        case K_BACKSTOP:
            snprintf(b, n, "update backstop (rst 0x%02x) - the image was neither confirmed nor "
                     "rolled back in time, phase '%s', %u in a row", rst,
                     phaseText(s_prev.phase, ph, sizeof ph), row);
            return;
        case K_HANDOVER:
            snprintf(b, n, "update backstop (rst 0x%02x) - the image was confirmed, but no loop() "
                     "pass took the watch over: the loop (or setup) was dead - usually a failed "
                     "health check with no image to roll back to; %u in a row", rst, row);
            return;
        case K_RWDT_OTHER:
        case K_RTC_OTHER:
            if (s_prevValid && s_prev.panicked)
                snprintf(b, n, "RTC watchdog (rst 0x%02x) during a panic - the panic handler hung "
                         "or ran past the 10 s it re-arms the watchdog for; not this module's "
                         "timeouts (armed as %s, phase '%s', %u s after start)", rst,
                         MODE_NAMES[s_prev.mode < 5 ? s_prev.mode : 0],
                         phaseText(s_prev.phase, ph, sizeof ph), (unsigned)s_prev.upS);
            else
                snprintf(b, n, "RTC watchdog (rst 0x%02x), not armed by this image - another "
                         "image's update backstop, the bootloader's watchdog, or a hung panic "
                         "handler", rst);
            return;
        case K_TASK_WDT:
            snprintf(b, n, "IDF task watchdog (rst 0x07), not this module's");
            return;
        default:
            // Also the IDF's own watchdog panics (reason 5/6 at rst 0x03): not ours.
            snprintf(b, n, "%s (rst 0x%02x), not this module's", reasonName(s_reason), (unsigned)s_rtc0);
            return;
    }
}

#if defined(PANIC_PATH_TEST)
// Registered in bootEarly(), before WiFi: the lowest slot, so it runs last,
// after esp_wifi_stop() and the other shutdown handlers (esp_system.c).
void testShutdown() {
    if (!s_testRstHang) return;
    phase(PH_RESTART);
    Serial.println("[wdt-test] rsthang: the restart hangs in a shutdown handler");
    consoleFlushFull();
    for (;;) delay(10);
}
#endif

}  // namespace

// ---- boot ----------------------------------------------------------------------------------
void bootEarly(bool pending) {
    s_reason = (uint8_t)esp_reset_reason();
    s_rtc0 = (uint8_t)esp_rom_get_reset_reason(0);
    s_rtc1 = (uint8_t)esp_rom_get_reset_reason(1);
    const uint8_t *id = OtaGuard::runningImageId();
    s_prevValid = s_rec.magic == REC_MAGIC && memcmp(s_rec.id, id, 8) == 0;
    if (s_prevValid) memcpy(&s_prev, &s_rec, sizeof s_prev);
    else memset(&s_prev, 0, sizeof s_prev);
    s_kind = classify();
    s_row = ours(s_kind) ? (uint8_t)(s_prev.inARow < 255 ? s_prev.inARow + 1 : 255) : 0;

    memset(&s_rec, 0, sizeof s_rec);
    s_rec.magic = REC_MAGIC;
    memcpy(s_rec.id, id, 8);
    s_rec.inARow = s_row;
    s_rec.phase = PH_SETUP;
#if defined(PANIC_PATH_TEST)
    esp_register_shutdown_handler(testShutdown);
#endif
    if (pending) {
        // OtaGuard::bootEarly() armed its backstop a moment ago (armBackstop,
        // which leaves this record alone). It stays OtaGuard's until mark-valid.
        s_rec.mode = s_mode;
        return;
    }
#if !defined(LOOP_WDT_OFF)
    arm(SETUP_WDT_MS, M_SETUP, true);    // never fed: setup() has 120 s
#endif
}

void usbHost(bool present) {
#if LWD_USB_GATE
    if (!present) return;
    s_hostSeenMs = millis();
    if (s_mode == M_SETUP) disarm(M_USB);
#else
    (void)present;
#endif
}

void setupMark(uint8_t n) {
    *(volatile uint8_t *)&s_rec.phase = (uint8_t)(PH_SETUP + (n < 0x7F ? n : 0x7F));
    s_rec.upS = millis() / 1000;
#if LWD_USB_GATE
    // A host that came up later in setup: the gate can only switch it off here.
    if (s_mode == M_SETUP && HWCDC::isPlugged()) {
        s_hostSeenMs = millis();
        disarm(M_USB);
    }
#endif
}

void bootReport() {
    // Fixed strings, so tools/release_checks.py can refuse such an image.
#if defined(PANIC_PATH_TEST)
    Serial.println("[wdt] TEST BUILD PANIC_PATH_TEST - not for release");
#endif
#if defined(LOOP_WDT_OFF)
    Serial.println("[wdt] TEST BUILD LOOP_WDT_OFF - not for release");
#endif
#if defined(LOOP_WDT_USB_OK)
    Serial.println("[wdt] TEST BUILD LOOP_WDT_USB_OK - not for release");
#endif
#if LWD_TIMES_CHANGED
    Serial.printf("[wdt] TEST BUILD LOOP_WDT_TIMES - not for release (loop %u ms, setup %u ms, "
                  "stage 1 %u ms, USB re-arm %u ms)\n", (unsigned)LOOP_WDT_MS,
                  (unsigned)SETUP_WDT_MS, (unsigned)RTC_STAGE_MS, (unsigned)USB_REARM_MS);
#endif
    printLastResetLine();
    switch (s_mode) {
        case M_SETUP:
            Serial.printf("[wdt] setup budget armed: %u s (+%u s RTC reset), RC slow clock %u Hz\n",
                          (unsigned)(SETUP_WDT_MS / 1000), (unsigned)(RTC_STAGE_MS / 1000),
                          (unsigned)slowClockHz());
            break;
        case M_USB:
            Serial.printf("[wdt] USB host attached - watch off until %u s without one\n",
                          (unsigned)(USB_REARM_MS / 1000));
            break;
        case M_BACKSTOP:
            Serial.printf("[wdt] health check running - OtaGuard's backstop: %u s (+%u s RTC "
                          "reset), RC slow clock %u Hz\n", (unsigned)(s_timeoutMs / 1000),
                          (unsigned)(RTC_STAGE_MS / 1000), (unsigned)slowClockHz());
            break;
        default:
            Serial.println("[wdt] off");
            break;
    }
}

void printLastResetLine() {
    char b[288];
    lineText(b, sizeof b);
    Serial.printf("[wdt] last reset: %s\n", b);
}

void setupDone() {
    const uint32_t now = millis();
    s_lastPassMs = s_lastFeedMs = s_lastSecMs = s_n2kAt = s_setupDoneMs = now;
    s_n2kLast = OtaGuard::g_n2kLoopCount;
    s_segAt = xTaskGetTickCount();
    s_segMax = 0;
    s_rec.upS = now / 1000;
    if (s_mode == M_SETUP) {
        arm(LOOP_WDT_MS, M_LOOP, true);
        Serial.printf("[wdt] loop watch armed: %u s (+%u s RTC reset)%s\n",
                      (unsigned)(LOOP_WDT_MS / 1000), (unsigned)(RTC_STAGE_MS / 1000),
                      g_n2kTaskRunning ? ", N2K task watched" : "");
    }

    // NVS "loopwd": the count of our resets and the last one. Written once
    // per boot after a reset of ours and only up to three in a row, so a
    // reboot loop cannot wear the flash (the row starts over after 10 min of
    // healthy loop, see loopTick()). The raw nvs API: Preferences would log
    // an error for the namespace on every boot until the first entry.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof s_last;
        if (nvs_get_u32(h, "n", &s_total) != ESP_OK) s_total = 0;
        s_haveLast = nvs_get_blob(h, "last", &s_last, &len) == ESP_OK && len == sizeof s_last;
        nvs_close(h);
    }
    if (ours(s_kind) && s_row <= 3) {
        NvsLast l;
        memset(&l, 0, sizeof l);
        strncpy(l.version, FW_VERSION, sizeof l.version);
        l.upS = s_prev.upS;
        l.kind = s_kind;
        l.rtc0 = s_rtc0;
        l.mode = s_prev.mode;
        l.phase = s_prev.phase;
        l.screen = s_prev.screen;
        l.flags = s_prev.flags;
        l.inARow = s_row;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            const bool ok = nvs_set_u32(h, "n", s_total + 1) == ESP_OK &&
                            nvs_set_blob(h, "last", &l, sizeof l) == ESP_OK &&
                            nvs_commit(h) == ESP_OK;
            nvs_close(h);
            if (ok) { s_total++; s_last = l; s_haveLast = true; }
        }
    }
}

// ---- runtime -------------------------------------------------------------------------------
void phase(uint8_t p) {
    *(volatile uint8_t *)&s_rec.phase = p;
    // Which section of the pass took longest - for maxGapPhase. Ticks are
    // milliseconds here (CONFIG_FREERTOS_HZ 1000) and cost one load.
    const TickType_t t = xTaskGetTickCount();
    if ((TickType_t)(t - s_segAt) > s_segMax) { s_segMax = t - s_segAt; s_segMaxPhase = s_segPhase; }
    s_segAt = t;
    s_segPhase = p;
}

void loopTick() {
    const uint32_t now = millis();

    // The pass that just ended: the feed gap and its slowest section. The
    // section running now (TOP: OtaGuard::loopTick(), its NVS write once a
    // minute) is part of this gap, so it is closed here as phase() would.
    const TickType_t t = xTaskGetTickCount();
    if ((TickType_t)(t - s_segAt) > s_segMax) { s_segMax = t - s_segAt; s_segMaxPhase = s_segPhase; }
    s_segAt = t;
    const uint32_t gap = now - s_lastPassMs;
    s_lastPassMs = now;
    if (gap > s_winGapMs) s_winGapMs = gap;
    if (gap > s_maxGapMs) { s_maxGapMs = gap; s_maxGapPhase = s_segMaxPhase; }
    s_segMax = 0;
    s_segMaxPhase = s_segPhase;

    // Mark-valid happened (OtaGuard, esp_timer task): the backstop becomes
    // the loop watch, written from here, so the RWDT has one writer at a time.
    if (s_handoverDue) {
        s_handoverDue = false;
#if defined(LOOP_WDT_OFF)
        disarm(M_OFF);
        Serial.println("[wdt] image valid - backstop off");
#else
        arm(LOOP_WDT_MS, M_LOOP, true);
        s_lastFeedMs = now;
        Serial.printf("[wdt] image valid - backstop handed over to the loop watch (%u s)\n",
                      (unsigned)(LOOP_WDT_MS / 1000));
#endif
    }

    // N2K gate: no feed while the N2K task's loop has stood still for 30 s.
    bool n2kOk = true;
    if (g_n2kTaskRunning) {
        const uint32_t c = OtaGuard::g_n2kLoopCount;
        if (c != s_n2kLast) {
            s_n2kLast = c;
            s_n2kAt = now;
            if (s_feedHeld) {
                s_feedHeld = false;
                s_rec.flags = (uint8_t)(s_rec.flags & ~F_FEED_HELD);
                Serial.println("[wdt] N2K task running again - feeding resumed");
            }
        } else if (now - s_n2kAt >= N2K_STALL_MS) {
            n2kOk = false;
            if (!s_feedHeld) {
                s_feedHeld = true;
                s_rec.flags = (uint8_t)(s_rec.flags | F_FEED_HELD);
                Serial.printf("[wdt] N2K task stalled for %u s%s\n", (unsigned)(N2K_STALL_MS / 1000),
                              s_mode == M_LOOP ? " - feeds held back, the loop watch will reset" : "");
            }
        }
    }

#if LWD_USB_GATE
    // A host on the bus: off at once. Back only after 10 s without one at
    // all - isPlugged() can read false for a moment on a healthy link, its
    // "plugged" is reliable. Never while OtaGuard owns the timer.
    if (HWCDC::isPlugged()) {
        s_hostSeenMs = now;
        if (s_mode == M_LOOP) {
            disarm(M_USB);
            Serial.println("[wdt] USB host attached - loop watch off");
        }
    } else if (s_mode == M_USB && now - s_hostSeenMs >= USB_REARM_MS) {
        arm(LOOP_WDT_MS, M_LOOP, true);
        s_lastFeedMs = now;
        Serial.printf("[wdt] no USB host for %u s - loop watch armed (%u s)\n",
                      (unsigned)(USB_REARM_MS / 1000), (unsigned)(LOOP_WDT_MS / 1000));
    }
#endif

    if (s_mode == M_LOOP && n2kOk && now - s_lastFeedMs >= FEED_MIN_MS) {
        feed();
        s_lastFeedMs = now;
    }

    if (now - s_lastSecMs >= 1000) {
        s_lastSecMs = now;
        s_rec.upS = now / 1000;
        s_rec.screen = (uint8_t)dispMgr.currentIndex();
        // After a healthy run the next reset of ours is a first one again.
        if (s_rec.inARow && now - s_setupDoneMs >= HEALTHY_RUN_MS) s_rec.inARow = 0;
    }
}

void core0Stamp() {
    const TickType_t t = xTaskGetTickCount();
    if ((TickType_t)(t - s_c0Tick) < pdMS_TO_TICKS(1000)) return;
    s_c0Tick = t;
    s_rec.c0S = millis() / 1000;
}

void armBackstop(uint32_t ms) { arm(ms, M_BACKSTOP, false); }

// The esp_timer task (OtaGuard::markValid()): two flags, no timer access.
// "confirmed" is a byte of its own in the record, a plain one-byte store -
// the loop task never writes it, so nothing is lost to a read-modify-write.
void backstopToLoop() {
    *(volatile uint8_t *)&s_rec.confirmed = 1;
    s_handoverDue = true;
}

uint32_t takeWindowGapMs() {
    const uint32_t g = s_winGapMs;
    s_winGapMs = 0;
    return g;
}

// ---- /api/info -----------------------------------------------------------------------------
void fillReset(JsonObject o) {
    o["reason"] = s_reason;
    o["rtc0"]   = s_rtc0;
    o["rtc1"]   = s_rtc1;
}

void fillInfo(JsonObject o) {
    const uint8_t m = s_mode;
    o["armed"]       = m == M_SETUP || m == M_LOOP || m == M_BACKSTOP;
    o["mode"]        = MODE_NAMES[m < 5 ? m : 0];
    o["timeoutS"]    = s_timeoutMs / 1000;
    o["maxGapMs"]    = s_maxGapMs;
    o["maxGapPhase"] = PHASE_NAMES[s_maxGapPhase < sizeof(PHASE_NAMES) / sizeof(PHASE_NAMES[0])
                                   ? s_maxGapPhase : 0];
    o["feedHeld"]    = (bool)s_feedHeld;
    o["inARow"]      = s_row;
    o["total"]       = s_total;
    if (!s_haveLast) { o["last"] = nullptr; return; }
    JsonObject l = o["last"].to<JsonObject>();
    char ph[32], v[17];
    memcpy(v, s_last.version, 16);
    v[16] = 0;
    l["kind"]    = KIND_KEYS[s_last.kind < sizeof(KIND_KEYS) / sizeof(KIND_KEYS[0]) ? s_last.kind : 0];
    l["rtc0"]    = s_last.rtc0;
    l["phase"]   = String(phaseText(s_last.phase, ph, sizeof ph));
    l["screen"]  = s_last.screen;
    l["upS"]     = s_last.upS;
    l["version"] = String(v);
}

#if defined(PANIC_PATH_TEST)
void testRequest(uint8_t kind) {
    if (kind == T_RSTHANG) s_testRstHang = true;
    else s_testReq = kind;
}

uint8_t takeTestRequest() {
    const uint8_t k = s_testReq;
    if (k) s_testReq = T_NONE;
    return k;
}
#endif

}  // namespace LoopWatchdog

// ---- the panic handler's RTC watchdog re-arm (7B/5B) ------------------------------------
// IDF 5's panic handler arms the RWDT for its own run (panic_handler.c ->
// esp_panic_handler_enable_rtc_wdt() in panic.c): wdt_hal_init, a 10 s
// RESET_RTC stage-0 hold, then wdt_hal_enable, which feeds. wdt_hal_init leaves
// flashboot alone, which arm() sets, so our count runs on through it, and the
// 10 s hold is live before that feed: with the count already at 10 s or more
// (see feed() above) the chip could reset at once - no backtrace, no core dump.
// -Wl,--wrap=esp_panic_handler_enable_rtc_wdt (platformio.ini, 7B/5B only)
// routes the call through here first:
//   - feed, so the count is ~0 when the hold is written, and 30 us for the
//     feed to land in the slow clock domain (as in arm());
//   - flashboot off: what IDF's code expects (with flashboot off, an RWDT that
//     is disabled stops counting - rwdt_ll.h). It matters when the panic
//     handler disables the RWDT for an attached debugger, and in builds where
//     only flashboot keeps our count running (-DLOOP_WDT_FLASHBOOT_ONLY). The
//     bootloader of the next boot sets up its own anyway;
//   - mark the reset record: an RTC watchdog reset after this point is the
//     panic handler's 10 s, not one of our timeouts (classify()).
// A local context, as in panic.c: either core may get here, and s_ctx is still
// empty before the first arm() (LOOP_WDT_OFF boots, a panic before bootEarly).
// Should core 1's feed() lock the registers in between, these writes are
// dropped and the panic runs as it did without the wrap. No IRAM: panic_handler
// and the real function run from flash/PSRAM as well (CONFIG_ESP_PANIC_HANDLER_IRAM
// is off; should it ever be on, LWD_PANIC_ATTR follows). Outside every
// LOOP_WDT_OFF / PANIC_PATH_TEST guard, so the --wrap link always resolves.
// The post-link step (extra_script.py, tools/psram_budget.py) fails a 7B/5B
// image that lacks this symbol: --wrap gives no error when it matches nothing.
#if ESP_IDF_VERSION_MAJOR >= 5
#if CONFIG_ESP_PANIC_HANDLER_IRAM
#define LWD_PANIC_ATTR IRAM_ATTR
#else
#define LWD_PANIC_ATTR
#endif
extern "C" void __real_esp_panic_handler_enable_rtc_wdt(uint32_t timeout_ms);
extern "C" LWD_PANIC_ATTR void __wrap_esp_panic_handler_enable_rtc_wdt(uint32_t timeout_ms) {
    wdt_hal_context_t ctx;
    ctx.inst = WDT_RWDT;
    ctx.rwdt_dev = RWDT_DEV_GET();
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_feed(&ctx);
    esp_rom_delay_us(30);
    wdt_hal_set_flashboot_en(&ctx, false);
    wdt_hal_write_protect_enable(&ctx);
    *(volatile uint8_t *)&LoopWatchdog::s_rec.panicked = 1;
    __real_esp_panic_handler_enable_rtc_wdt(timeout_ms);
}
#endif
#endif  // SIMULATOR
