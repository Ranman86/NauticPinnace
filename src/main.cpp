#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <WiFi.h>   // WiFi.RSSI() for the heartbeat log
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <soc/timer_group_struct.h>
#include <soc/timer_group_reg.h>
#if defined(BOARD_PANEL_1024X600)
#include <esp_timer.h>   // frame instrumentation, see the heartbeat in loop()
#endif

// ROM cache flush – writes dirty D-cache lines back to PSRAM/Flash.
extern "C" void Cache_WriteBack_All(void);

#include "BoardConfig.h"
#include "DisplaySetup.h"
#include "nmea/DataModel.h"
#include "nmea/N2kHandler.h"
#include "config/Config.h"
#include "i18n/I18n.h"
#include "config/WebConfig.h"
#include "PolarTable.h"
#include "display/DisplayManager.h"
#include "display/BootScreen.h"
#include "display/LicenseOverlay.h"
#include "display/LanguageOverlay.h"
#include "net/BshTide.h"
#include "PsramArena.h"
#include "n2kupdate/OtaGuard.h"
#include "n2kupdate/UpdRegions.h"
#include "n2kupdate/N2kUpdateNode.h"
#include "LoopWatchdog.h"
#include "UsbConsole.h"    // the 5B's console TX timeout, consoleFlushFull()
#include "driver/twai.h"   // twai_get_status_info() for the CAN line in the heartbeat
#if !(ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE)
#include <esp_rom_uart.h>  // 7B: let the early log leave UART0 before Serial.begin()
#endif

// Global instances
DataModel  data;
N2kHandler n2k;
WebConfig  webCfg;

// True once the NMEA 2000 task exists. setup() creates EITHER that task OR the
// demo task, and only once - so a device booted in demo mode has nothing
// reading the bus, and clearing demo mode from the web UI would leave it with
// no data source at all until it restarts. WebConfig reads this to decide
// whether such a change needs a reboot.
volatile bool g_n2kTaskRunning = false;

// ---- FreeRTOS task: NMEA 2000 on Core 0 ------------------------------------

static void n2kTask(void *) {
    n2k.begin();
    for (;;) {
        n2k.loop();
        OtaGuard::countN2kLoop();   // health criterion H4 (N2K-UPDATE.md 10.2)
        LoopWatchdog::core0Stamp(); // core 0 alive, for the next boot's diagnosis
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ---- FreeRTOS task: Demo data generator (when demoMode = true) ---------------
// Bypasses N2kHandler::begin() (which would configure TWAI on the wrong pins)
// and directly calls demoData.tick() at ~5 Hz to populate the DataModel.
//
// The flag is re-read EVERY tick, not just at startup. Previously this loop ran
// unconditionally once started, while the "DEMO MODE" banner did check the flag
// — so switching demo off in the web UI removed the warning but kept feeding
// synthetic values. Invented data that no longer announces itself is the worst
// possible state for an instrument, so: when the flag goes false the loop stops
// AND the model is wiped once, which makes every screen show "--" instead of
// frozen fantasy numbers (a clock set by SNTP stays: it is real time).
#include "nmea/DemoData.h"
static void demoTask(void *) {
    bool wasOn = true;
    for (;;) {
        const bool on = appConfig.cfg.demoMode;
        if (on) {
            demoData.tick();
        } else if (wasOn) {
            { auto lk = data.lock(); data.clearValues(); }   // back to all-NaN
            Serial.println("[demo] switched off — data model cleared "
                           "(reboot to read the real bus)");
        }
        wasOn = on;
        LoopWatchdog::core0Stamp();      // stands in for the N2K task's stamp
        vTaskDelay(pdMS_TO_TICKS(200));  // 5 Hz
    }
}


// ---- FreeRTOS task: LVGL tick (1 ms) ----------------------------------------
// Not compiled with LV_TICK_CUSTOM (7B): LVGL reads millis() itself there and
// lv_tick_inc() does not exist - see lv_conf.h.

#if !LV_TICK_CUSTOM
static void lvglTickTask(void *) {
    for (;;) {
        lv_tick_inc(1);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#endif

#if defined(BOARD_PANEL_1024X600)
// How much of core 1 is taken away from us by interrupts, in per mille.
//
// WHY IT IS WORTH KNOWING: this board drives the RGB panel with bounce buffers,
// which means the frame is not DMA'd out of PSRAM - the CPU memcpy's it, 20 KB
// at a time, inside the GDMA end-of-frame interrupt. At 1024x600x2 bytes and
// 41.3 Hz that is 50.8 MB/s moved permanently, 2,479 interrupts per second, one
// every 403 us. And it lands on core 1, the same core that runs loop() ->
// displayTick() -> lv_timer_handler(). Every millisecond this firmware measures
// is wall clock, so it already contains that theft. Until it is quantified,
// nobody knows how much of a 450 ms frame is even reachable by changing
// software.
//
// HOW: self-calibrating, so it needs no cycle counting and no sdkconfig change.
// The identical loop is timed twice - once with interrupts masked, which is the
// uncontended rate this core can actually deliver, and once normally. The ratio
// is the theft. Both halves run the same code, so the loop's own cost cancels.
//
// The masked half is deliberately kept to 100 us: the panel's bounce buffer has
// to be refilled every 403 us or the picture tears, so blocking that interrupt
// for a quarter of its budget is safe while a full millisecond would not be.
static uint32_t coreStealPerMille() {
    volatile uint32_t sink = 0;
    uint32_t nRef = 0, nRun = 0;

    portDISABLE_INTERRUPTS();
    const int64_t r0 = esp_timer_get_time();
    while (esp_timer_get_time() - r0 < 100) { sink += nRef; nRef++; }
    portENABLE_INTERRUPTS();

    const int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < 20000) { sink += nRun; nRun++; }

    if (!nRef) return 0;
    // Rates per microsecond, scaled to per mille of the uncontended rate.
    const float ref = (float)nRef / 100.0f;
    const float run = (float)nRun / 20000.0f;
    if (ref <= 0.0f) return 0;
    const float lost = 1.0f - (run / ref);
    if (lost <= 0.0f) return 0;
    if (lost >= 1.0f) return 1000;
    return (uint32_t)(lost * 1000.0f + 0.5f);
}
#endif

// ---- Boot milestones -----------------------------------------------------------
// One grep-able line per step that decides when the panel shows something,
// with the time since the app started. The question "how long is the screen
// dark, and why" used to need a stopwatch next to the device.
//
// Kept as well (pointer + time, `what` is always a string literal), so that
// bootSummary() can print them all again later: on the 4" and the 5B the
// console is the S3's own USB port, which comes back only some seconds after
// a power-on, and until a terminal has picked it up again the core keeps just
// the last 256 bytes - the expander line and the first marks are gone by then.
struct BootMarkRec { const char *what; uint32_t ms; };
static BootMarkRec s_bootMarks[10];
static uint8_t     s_bootMarkN = 0;
static uint8_t     s_bootMarkSeq = 0;   // every mark, also past the ten kept

static void bootMark(const char *what) {
    const uint32_t ms = millis();
    Serial.printf("[boot] %6u ms  %s\n", (unsigned)ms, what);
    if (s_bootMarkN < sizeof s_bootMarks / sizeof s_bootMarks[0]) {
        s_bootMarks[s_bootMarkN].what = what;
        s_bootMarks[s_bootMarkN].ms   = ms;
        s_bootMarkN++;
    }
    // The setup budget's phase: should setup() hang, the next boot names the
    // last mark reached (its number in the list below). Also the USB gate.
    if (s_bootMarkSeq < 0x7F) s_bootMarkSeq++;
    LoopWatchdog::setupMark(s_bootMarkSeq);
}

static bool consoleSettle(uint32_t t0);   // "A USB console nobody reads", below

// The start-up in a few lines, once a console can see it (loop(), after the
// late expander check). Answers a cold-start test without having to catch
// the first second of the log. false (5B only): the host stopped reading
// after the first line - the summary is then printed again for the next
// terminal that reads, instead of each line waiting 0.1 s for nobody.
static bool bootSummary(const char *why) {
    const ExpanderWait &e = displayExpanderWait();
    const uint32_t t0 = millis();
    Serial.printf("[boot] summary (%s): expander %s after %u ms (at t=%u ms; first ACK "
                  "after %u ms, %u round%s), late check: %s\n", why,
                  e.confirmed ? "ready" : "!!! NOT confirmed", (unsigned)e.tookMs,
                  (unsigned)e.atMs, (unsigned)e.firstAckMs, (unsigned)e.rounds,
                  e.rounds == 1 ? "" : "s",
                  e.late < 0 ? "not run" : e.late ? "settings held"
                                                  : "!!! settings LOST, written again");
#if USB_CONSOLE_BOUNDED
    if (!consoleSettle(t0) || !Serial) return false;
#else
    (void)t0;
#endif
    for (uint8_t i = 0; i < s_bootMarkN; i++)
        Serial.printf("[boot]   %6u ms  %s\n", (unsigned)s_bootMarks[i].ms, s_bootMarks[i].what);
    // On the USB-console boards the first one is usually lost (see above).
    LoopWatchdog::printLastResetLine();
    return true;
}

// WebConfig::connect() reports here while it waits for the network; the boot
// screen turns it into text and renders a frame, so the spinner keeps turning.
static void bootWifiProgress(const WifiProgress &p) {
    static int8_t s_lastPhase = -1;
    if ((int8_t)p.phase != s_lastPhase) {   // one log line per phase, not per frame
        s_lastPhase = (int8_t)p.phase;
        static const char *const names[] = { "wifi: joining", "wifi: scanning",
                                             "wifi: joined", "wifi: fallback hotspot" };
        bootMark(names[p.phase]);
    }
    bootScreen.wifi(p);
}

// ---- A USB console nobody reads (5B) -----------------------------------------
// The 5B's console is the S3's own USB-Serial/JTAG, driven by Arduino 3.x's
// HWCDC. A host on the socket that does not read - a terminal closed with the
// cable left in, a PC with no terminal open - is no disconnect to the core:
// the host still sends its start-of-frame packets, isPlugged() stays true, and
// the full TX ring counts as backpressure. HWCDC::write() then retries each
// chunk up to 20 times, waiting tx_timeout_ms (100 ms by default) each time,
// and returns short with `connected` still set: about 2 s in every write that
// does not fit, and the next one waits again. Measured on the bench: a loop
// pass of 2107 ms in the heartbeat after the test harness had let go of the
// port - by the core's code 2 s in the line and 0.1 s in the flush after it.
// Only HWCDC::flush() ends that state: after tx_timeout_ms polls without
// progress it drops the queue and clears `connected`, and writes cost nothing
// from then on - the core keeps the newest bytes that fit, at most 256 (of a
// single longer write its FIRST 256, and every 5B heartbeat is one) - until
// the host takes a packet again, which sets `connected` in its interrupt.
//
// What keeps a loop pass short (5B only):
//  - setTxTimeoutMs(USB_TX_TIMEOUT_MS) at the end of setup() (UsbConsole.h):
//    a write into a host that does not read gives up after 20 x 5 ms =
//    100 ms. setup() keeps the default, so the boot log keeps the full margin.
//  - consoleSettle() after the heartbeat, in place of a plain flush, and
//    after the first line of the 10-s block (late expander check, boot
//    summary): a host that has taken nothing of the line USB_SILENT_MS after
//    it started does not read, and only then comes the flush that switches
//    the core over (<= 5 ms more). A host that stops reading so costs one
//    pass about 0.1 s, once: the heartbeat is longer than the ring and always
//    waits the full 20 x 5 ms for its last part - on top of the pass's own
//    paint and tick, up to ~150 ms on the 5B, so up to ~250 ms in all - and a
//    line that fits waits USB_SILENT_MS. A host that reads takes a packet
//    within a millisecond and never gets that flush.
//  - A boot summary whose first line found no reader is printed again for
//    the next terminal that reads. Should the switch-over itself fail - its
//    flush did not get the console's lock within 5 ms because another task's
//    write held it - the heartbeat leaves out its paint table and expander
//    line, each of which would wait another 0.1 s, and the next one retries.
// Not covered: a line another task writes in the same moment can hide a stall
// from consoleSettle() once; single lines elsewhere on the loop still give up
// after 0.1 s each (a println() is two writes) until the next heartbeat.
//
// The 5 ms are also what every other Serial.flush() and every wait for the
// console's lock get from then on, on every task:
//  - a flush drops what is still queued once 5 of its 1-ms polls - counted
//    over the whole flush, not in a row - saw no progress (it was 100). A host
//    that reads loses nothing unless it falls that far behind. The last lines
//    before a restart, which a drop would lose for good, get the 100 back:
//    consoleFlushFull() (UsbConsole.h).
//  - a task that cannot get the lock within 5 ms - another task's line still
//    going into the ring - loses its own line whole, where it used to wait up
//    to 100 ms and then, with no reader, block on the ring for up to 2 s
//    itself. With a host that reads, a line holds the lock while whatever of
//    it does not fit into the ring's free space drains: for the heartbeat,
//    longer than the ring, up to all of its ~340 bytes, six USB packets. A
//    host that needs more than 5 ms for those costs the other task its line.
// Panic output and the core dump do not use HWCDC (panic.c writes the USB FIFO
// directly). The 4" (Arduino 2.0.17) needs none of this: its write() gives up
// after tx_timeout_ms one-millisecond polls without progress and clears
// `connected` itself - about 0.1 s, once. The 7B's console is a UART behind a
// USB bridge chip and never waits for a host.
#if USB_CONSOLE_BOUNDED
static const int      USB_TX_RING_BYTES = 256;   // HWCDC::begin()'s default TX ring
static const uint32_t USB_SILENT_MS     = 20;
#endif

// After a line that should get out: Serial.flush(), except on the 5B, see
// above. t0 = millis() taken just before the line was printed. false only on
// the 5B, when the console still waits for a host that does not read - the
// switch-over failed - so that any further line in this pass would wait too.
static bool consoleSettle(uint32_t t0) {
#if USB_CONSOLE_BOUNDED
    if (!Serial) return true;                     // no host, or already switched over
    int room = Serial.availableForWrite();
    if (room >= USB_TX_RING_BYTES) return true;   // all of it is out already
    while (millis() - t0 < USB_SILENT_MS) {
        delay(1);
        // Room grows when the USB interrupt moves data on, which it does only
        // after the host took a packet; less room means another task queued
        // a line meanwhile.
        const int fresh = Serial.availableForWrite();
        if (fresh > room) return true;            // the host reads
        room = fresh;
    }
    Serial.flush();                               // nothing taken: nobody reads
    // Switched over, or the host took it all during the flush after all;
    // otherwise the flush did not get the lock and nothing has changed.
    return !Serial || Serial.availableForWrite() >= USB_TX_RING_BYTES;
#else
    (void)t0;
    Serial.flush();
    return true;
#endif
}

#if defined(BOARD_PANEL_1024X600)
// ---- PSRAM budget (7B, 5B) -----------------------------------------------------
// What setup() asks of PSRAM, by name in firmware.elf: tools/psram_budget.py
// reads it from there after every build, so the check cannot drift from what
// the firmware really asks for. VOLATILE so that setup() really reads it from
// memory: folded into constants, nothing would refer to the array and the
// linker would drop it (this toolchain ignores __attribute__((retain))).
// Layout fixed: [0] the canvas arena (see setup(), 1a), [1] the frame
// buffer's room, [2] PSRAM kept free beside both, [3] a tag the script checks.
//
// [2] is what the running firmware needs on top: malloc sends every request
// from 4 KB up to PSRAM first (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), and the
// web interface's configuration document lives there on these boards
// (Config.cpp, SpiRamAllocator). Measured 03.11.2026 on the 7B: with 28 KB of
// PSRAM free, GET /api/config answered 200 with an EMPTY body and the web
// interface could not load its settings; the 5B with 95 KB was fine. 16 KB
// had been the figure here, which let the build check call that state "one
// page to spare". 64 KB, plus the ~16 KB the frame buffer hands back.
extern "C" const volatile uint32_t np_psram_budget[4] = {
    4365000 + UpdRegions::ARENA_BYTES,
    DISPLAY_FB_ROOM,
    64 * 1024,
    0x50534254u,   // "PSBT"
};
#endif

// ---- Setup -------------------------------------------------------------------

void setup() {
    // FIRST, before anything that could hang or crash: OtaGuard decides
    // whether this image is a fresh update on probation (PENDING_VERIFY). If
    // so it starts the health check and arms the RTC watchdog backstop, so a
    // broken update rolls back by itself (N2K-UPDATE.md 10.2, 10.3). It also
    // anchors erased otadata, runs the crash-loop guard and reads the last
    // rollback for the report. Serial is not up yet; bootReport() prints it.
    OtaGuard::bootEarly();
    // The loop watchdog (LoopWatchdog.h): what the last reset was, from its
    // RTC record, and - unless the image is on probation, where the backstop
    // just armed is OtaGuard's - the setup budget of 120 s from here.
    LoopWatchdog::bootEarly(OtaGuard::pendingVerify());

    // Disable the interrupt WDT (TG1 MWDT) early.  The 300 ms default threshold
    // is tight relative to the PSRAM zero-fill and SPI display init; disabling it
    // here keeps setup clean.  The Task WDT (TG0) is handled by unsubscribing all
    // known tasks below.  Unlock key for TG1 MWDT on ESP32-S3: 0x50D83AA1.
    // Read back, up to five times: IDF's tick hook on core 0 rewrites TG1's
    // stages every tick with a read-modify-write of this same register
    // (int_wdt.c -> mwdt_ll_config_stage), and a hook already inside one when
    // this write lands would put the enable bit back.
    bool tg1On = true;
    for (int i = 0; i < 5 && tg1On; i++) {
        TIMERG1.wdtwprotect.wdt_wkey = 0x50D83AA1U;
        TIMERG1.wdtconfig0.wdt_en    = 0;
        TIMERG1.wdtwprotect.wdt_wkey = 0;
        delayMicroseconds(50);   // past any hook that read the register before
        tg1On = TIMERG1.wdtconfig0.wdt_en;
    }

    // Unsubscribe loopTask and Core-1 IDLE from TWDT before the long setup.
    // None of the three finds anything to do (kept as a guard): Arduino never
    // subscribes the loop task, and the core-1 idle task is not watched in
    // this sdkconfig. Core-0 IDLE IS subscribed - by the IDF itself at startup
    // (app_startup.c), not by esp_wifi_start() as this comment used to say -
    // and is dealt with after the WiFi start below; with WiFi off it stays
    // watched (5 s, panic).
    //
    // The two removals are therefore only made for a task the TWDT really
    // holds, asked first with esp_task_wdt_status(), which says nothing either
    // way (with the TWDT up, as it always is here). Asked blind, they put an
    // error line into EVERY boot log for a task that was simply not there:
    //   E (837) task_wdt: delete_entry(240): task not found
    //     IDF 5's esp_task_wdt_delete() (7B/5B; on the 7B cut short to
    //     "E (8x" because Serial.begin() resets UART0 while it is going out),
    //   [E][esp32-hal-misc.c] disableCore1WDT(): Failed to remove Core 1 IDLE task from WDT
    //     the Arduino helper, on all three boards.
    // What is subscribed afterwards is exactly what it was. disableLoopWDT()
    // stays as it is: it only acts on a loop task Arduino itself subscribed.
    disableLoopWDT();
    {
#if ESP_IDF_VERSION_MAJOR >= 5
        TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
#else
        TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCPU(1);
#endif
        if (idle1 && esp_task_wdt_status(idle1) == ESP_OK) disableCore1WDT();
        TaskHandle_t self = xTaskGetCurrentTaskHandle();
        if (esp_task_wdt_status(self) == ESP_OK) esp_task_wdt_delete(self);
    }

#if !(ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE)
    // 7B: the console is UART0, and Serial.begin() resets its TX FIFO
    // (uartBegin -> uart_param_config -> uart_hal_txfifo_rst), throwing away
    // what the early log still had in it - up to 128 bytes, the last line
    // before this point cut off, as the "E (8x" above was. Let it go out
    // first: at most ~11 ms at 115200 baud, usually nothing.
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_rom_output_tx_wait_idle(0);
#else
    esp_rom_uart_tx_wait_idle(0);
#endif
#endif
    Serial.begin(115200);
    // There used to be a blind delay(8000) here, for two reasons: the USB
    // console needs a moment after a reset, and the IO expander - which
    // switches the panel's power - was said to need 4-5 s before it takes I2C
    // commands. Eight seconds of black panel on every start, and on a failed
    // WiFi join another ~20 s on top: people took the device for dead.
    //
    // The expander is now polled until it answers AND holds what it was told
    // (displayInit(), "[boot] expander ready after N ms"), capped at the old
    // 8 s, so the worst case is what it always was. The console gets a short
    // wait of its own below - only on the USB-console boards, and only while
    // a host is actually on the bus; on the boat nobody is.
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    {
        // 4" and 5B: the console is the S3's own USB-Serial/JTAG. isPlugged()
        // watches the host's start-of-frame packets (one per ms) and reads
        // "plugged" until ~6 ms have passed without one, hence the short
        // settle first. Without a host there is nothing to wait for; with one,
        // up to 1.5 s for its terminal to pick the port up again, so the first
        // lines of the boot log are not lost. A cold power-on re-enumerates the
        // port and usually takes longer than that; for that case the lines that
        // matter come again in bootSummary() (loop(), once a terminal is there)
        // instead of the board waiting any longer.
        const uint32_t t0 = millis();
        delay(20);
        const bool host = HWCDC::isPlugged();
        if (host) while (!Serial && millis() - t0 < 1500) delay(10);
        Serial.printf("\n[boot] USB console: %s after %u ms\n",
                      host ? (Serial ? "terminal attached" : "host on the bus, no terminal")
                           : "no host",
                      (unsigned)(millis() - t0));
        // With a host on the bus the watchdog goes off: a web flash (ESP Web
        // Tools) does not stop it and would be cut off (LoopWatchdog.h).
        LoopWatchdog::usbHost(host);
    }
#endif
    Serial.println("\n=== NauticPinnace booting ===");
#if defined(BOARD_PANEL_1024X600) && defined(PERF_PAINT_PHASES)
    // Says so up front, so a log from a stopwatch build (display/screens/
    // PaintPhase.h) is never mistaken for one from a release image.
    Serial.println("[perf] PERF_PAINT_PHASES build - not for release");
#endif
    bootMark("setup start (no blind wait any more)");
    OtaGuard::bootReport();
    LoopWatchdog::bootReport();
    if (tg1On) Serial.println("[wdt] !!! the interrupt watchdog (TG1) did not switch off "
                              "after 5 tries");

    // 1a. PSRAM arena – one heap_caps_malloc(SPIRAM) before any LVGL activity.
    //     Canvas pixel buffers are sub-allocated from this arena with pointer
    //     arithmetic (state in DRAM).  This avoids the OPI PSRAM TLSF cache-
    //     coherency bug where heavy DRAM realloc evicts PSRAM free-list metadata,
    //     causing subsequent heap_caps_malloc(SPIRAM) to assert in block_locate_free.
    //
    //     Size budget – sum of all screen canvases (RGB565 = w*h*2), created in
    //     DisplayManager::activate():
    //       Wind 480x430=412800, Depth 480x480=460800, Rudder 480x480=460800,
    //       AIS 390x390=304200, WindPlot 400x400=320000, Autopilot 480x170=163200
    //       => ~2.12 MB total.  1.7 MB was too small: WindPlot + Autopilot failed
    //       to allocate (blank screens).  2.7 MB fits all with headroom and still
    //       leaves >5 MB PSRAM free for the RGB framebuffer / LVGL / WiFi.
#if defined(BOARD_PANEL_1024X600)
    // 600-grid canvases (stage 3). Measured demand with the shared
    // Wind/Attitude buffer: 4.31 MB + draw-time scratches. Unshared demand
    // would be 5.03 MB - does not fit. (The "~0.53 MB left free" once noted
    // here was counted before the code moved into PSRAM too; what is really
    // left is below.)
    //
    // 4.365 MB since 03.11.2026, down from 4.42: every screen visited on the
    // 7B (PERF_SCREEN_SWEEP) used 4,599,944 bytes of the whole arena, the
    // simulator with the tide curve's scratch as well 4,340,168 + the update
    // regions = 4,603,336; the AIS static cache (DataModel, 3,072 B, since
    // 11.11.2026) adds to both. About 21 KB of margin is left (simulator_7b:
    // 4,343,240 of 4,365,000 at the last allocation); the ~55 KB the old figure
    // kept unused were what the 7B's web interface was missing (see
    // np_psram_budget above). A screen that needs more prints "[Arena] OOM"
    // and comes up without its canvas - the simulator_7b mirrors this figure,
    // so it shows there first.
    //
    // +20 KB (4.40 -> 4.42 MB): bulk data that used to sit in internal RAM now
    // sub-allocates from here as well - the DataModel rings below are ~10.8 KB
    // of it. That is the cheap side of the trade: the internal heap it
    // relieves is the one that starves WiFi/HTTP on these boards.
    //
    // + UpdRegions::ARENA_BYTES (257 KB): the two 128 KB regions of the update
    // path - one holds the web UI cache, the other is the work region of an
    // update session - plus the receiver's session record. Reserved here, once,
    // because a runtime PSRAM malloc is not safe (see PsramArena.h). The web UI
    // cache used to be a separate ~49 KB malloc, which is gone, so free PSRAM
    // drops by about 0.2 MB net (N2K-UPDATE.md 9.1, hardware test H25).
    //
    // PSRAM IS FULL on these boards (measured 02.11.2026): the firmware's code
    // and read-only data run from PSRAM too (XIP), in whole 64 KB pages, and
    // the arena used to come first with the panel's frame buffer (1.2 MB)
    // after it. A build ~10 KB larger took one code page more, the frame
    // buffer no longer fitted, the panel stayed dark and setup() never
    // finished. So the frame buffer's room is now held before the arena takes
    // its share, and the arena gets smaller instead (PsramArena.cpp): a short
    // PSRAM then costs the screens built last their canvas, not the picture.
    // tools/psram_budget.py checks every build against these numbers and says
    // how many code pages are left before that happens.
#if defined(PSRAM_SQUEEZE_TEST)
    // Test builds only (-DPSRAM_SQUEEZE_TEST=<bytes>): stands in for that much
    // more code in PSRAM, so the arena giving way can be watched on a real
    // board. Taken first and never freed, exactly like code pages. Aligned,
    // so the heap holds the bytes asked for and not their whole size class.
    Serial.printf("[perf] PSRAM_SQUEEZE_TEST build (%u bytes held) - not for release\n",
                  (unsigned)(PSRAM_SQUEEZE_TEST));
    (void)heap_caps_aligned_alloc(16, PSRAM_SQUEEZE_TEST, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    PsramArena::initLeavingRoom(np_psram_budget[0], np_psram_budget[1],
                                DISPLAY_FB_ROOM_CAPS, np_psram_budget[2]);
#else
    PsramArena::init(3350000 + UpdRegions::ARENA_BYTES);   // +500 KB: 2nd 480×480 Wind canvas (Schiffslage attitude screen)
    // Deliberately NOT raised for the 4-inch board's canvases: their demand is
    // ~2.6 MB, so this arena already carries several hundred KB of unused
    // slack and swallows the DataModel rings without touching free PSRAM.
    // The update regions are added on top all the same, so none of that
    // slack is consumed by them (N2K-UPDATE.md 9.1).
#endif

    // 1a-bis. The DataModel's AIS table and history rings are arena buffers
    //         too (~10.8 KB of internal RAM recovered on every board), and so
    //         is the AIS static cache (3,072 B, arena only: without room for
    //         it the table works as it did before there was one). This
    //         has to happen HERE: the global `data` was constructed before
    //         setup() ran, so its buffer pointers are still null, and the
    //         first thing that pushes a sample or renders a history screen
    //         would dereference them. Right after the arena exists and long
    //         before any task or screen can touch the model.
    data.initBuffers();

    // 1a-ter. The update path's PSRAM: two regions and the receiver's record,
    //         taken from the arena before any other task can touch it. Then
    //         the receiver's boot self-tests (8.1), which need the work region.
    UpdRegions::reserve();
    N2kUpdateNode::bootPrepare();

    // 1b. Display + LVGL – lv_init() must run before any lv_* calls
    displayInit();
    bootMark("panel initialised");

    // 2. Filesystem + config (LittleFS can init after LVGL)
    appConfig.begin();
    Serial.println("Config loaded."); Serial.flush();

    // The web UI: a stale /index.html.gz.new of an interrupted update is
    // removed, /index.html.gz goes into its PSRAM region and its web UI id is
    // computed - with WiFi off as well, because INFO reports it (9.4).
    UpdRegions::bootLoad();

    // Screen rotation can only be applied NOW: displayInit() above had to run
    // before the filesystem was mounted, so it could not know the setting.
    // Doing it here, before any screen is built, means the whole UI is laid
    // out for the rotated resolution from the start.
    displayApplyRotation();

    // Bluetooth: this firmware contains no BT/BLE code at all, so the controller
    // memory is always released — that is pure DRAM gain on a board where the
    // LVGL pool and the WiFi stack compete for it. There used to be a config
    // switch for this; it could only ever make things worse (reserving memory
    // for a radio nothing drives), so it was removed.
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    Serial.println("[setup] Bluetooth controller memory released (no BT in this firmware)");
    Serial.flush();

    // Polar table – allocate in PSRAM (keeps ~2 KB out of the tight DRAM heap),
    // then load from LittleFS so SpeedScreen/WindScreen use the configured boat
    // polar instead of the old hard-coded table.
    PolarInit::init();
    gPolar().load(appConfig.cfg.polarFile);

    // Theme – fill the active runtime palette from config before any screen is
    // built in dispMgr.activate(). (Screens read colours via the CLR_* macros.)
    applyThemeFromConfig();

    // Flush dirty PSRAM cache lines created by the arena zero-fill before WiFi
    // starts its DMA.  Avoids an MSPI bus race that causes data corruption.
    Cache_WriteBack_All();

    // 3. WiFi FIRST – before any LVGL object creation so the heap is clean.
    //    LVGL style/object allocs corrupt a SpinLock address in the heap mgmt
    //    struct; WiFi malloc then spins on that lock → TG1WDT.  Initialising
    //    WiFi here avoids the race entirely.
    //    WiFi can be disabled in the on-screen config (cfg.wifiEnabled). When off
    //    we never start the radio (no web UI) and skip the Core-0 WDT unsubscribe
    //    below - so the IDF's own task watchdog keeps watching Core-0 IDLE (the
    //    IDF subscribes it at startup; esp_wifi_start() has nothing to do with it).
    //
    //    Only the RADIO START has to come first, and that is all start() does
    //    (esp_wifi_init/start via WiFi.mode(), plus the hotspot in hotspot
    //    mode). The join itself - up to three 6 s attempts - used to run here
    //    as well, with nothing on the panel. It is connect() now, after the
    //    boot screen is up (section 3b).
    if (appConfig.cfg.wifiEnabled) {
        Serial.println("[setup] webCfg start"); Serial.flush();
        webCfg.start(appConfig.cfg.apMode,
                     appConfig.cfg.wifiSsid,
                     appConfig.cfg.wifiPassword);
        Serial.println("[setup] webCfg radio up"); Serial.flush();
        bootMark("radio up");

        // NOW unsubscribe Core-0 IDLE. It has been in the TWDT's watch list since
        // the IDF's startup (app_startup.c) - not since esp_wifi_start(), as this
        // comment used to claim. What has to wait for the radio is only the
        // decision: with WiFi off nothing here runs and IDLE0 stays watched.
#if ESP_IDF_VERSION_MAJOR >= 5
        // IDF 5.x: do NOT call disableCore0WDT() here. esp_task_wdt_deinit()
        // unsubscribes the idle tasks ITSELF - and its internal
        // ESP_ERROR_CHECK aborts with ESP_ERR_NOT_FOUND (= instant boot loop)
        // if IDLE0 was already removed beforehand. Learned the hard way.
        //
        // Why deinit at all: merely unsubscribing IDLE0 (the 4.4-era approach
        // below) leaves the TWDT idle hook firing esp_task_wdt_reset() from
        // the unsubscribed IDLE task, which 5.x answers with a logged error
        // EVERY time - measured ~300 lines/s, saturating the UART and
        // starving the rest of setup. Deinit removes watch list AND hooks.
        // The TWDT was always deliberately inert in this firmware; the
        // rebuilt libs have CONFIG_ESP_TASK_WDT_PANIC=y, and a screen rebuild
        // can starve IDLE0 for >5 s, which would otherwise panic-reboot.
        {
            esp_err_t werr = esp_task_wdt_deinit();
            Serial.printf("[setup] TWDT deinit -> %s\n", esp_err_to_name(werr));
            Serial.flush();
        }
#else
        disableCore0WDT();
        Serial.println("[setup] Core0 WDT disabled"); Serial.flush();
#endif
    } else {
        Serial.println("[setup] WiFi DISABLED by config (radio off)"); Serial.flush();
    }

    // 3a. Boot screen - as early as the rule above allows: the radio is up,
    //     so LVGL objects are safe from here on. With WiFi off this is the
    //     very first thing after the configuration.
    bootScreen.show(appConfig.cfg.bootName);
    bootMark("boot screen up");

    // 3b. The WiFi join, now with the boot screen saying what is going on
    //     ("Verbinde mit WLAN „x“ ... (Versuch 2/3)", then the address or the
    //     fallback hotspot and why). The result stays readable for a moment:
    //     the next step would replace it within milliseconds otherwise.
    if (appConfig.cfg.wifiEnabled) {
        // Before the first DHCP lease: lwIP takes a time server from it only
        // while this is on (BshTide.cpp, clock sync).
        bshSntpPreferDhcp();
        webCfg.connect(bootWifiProgress);
        Serial.println("[setup] webCfg done"); Serial.flush();
        if (webCfg.fallbackActive())        bootScreen.hold(3500);
        else if (WebConfig::staLinkUp())    bootScreen.hold(1200);

        // Tide forecast: when online, sync the clock (SNTP) and pull the nearest
        // German gauge's official HW/NW predictions from the BSH API.
        bshTideBegin();
    }

    // 4. NMEA 2000 (real bus) or demo data.
    //    Rev 4 CAN pins are TX=GPIO6 / RX=GPIO0 (BoardConfig.h) — NOT the USB
    //    D−/D+ pins anymore, so TWAI no longer kills USB CDC. GPIO0 is the BOOT
    //    strapping pin: if the bus holds it LOW at power-on the chip can enter
    //    download mode, so a boot may occasionally need the bus briefly detached.
    //    Start EITHER the N2K task OR the demo task (never both — demoTask writes
    //    the DataModel directly and would clobber real bus data).
    //    demoMode comes from the config (WebUI switch). Booted in demo mode,
    //    turning it off requires a restart (task selection happens only here);
    //    booted on the bus the switch takes effect live (N2kHandler::loop branches).
    //    A new firmware under its health check (OtaGuard PENDING_VERIFY) always
    //    starts on the bus: demo mode is deferred until the image is valid
    //    (N2K-UPDATE.md 5.1 rule 6, N2kHandler::loop), and without the N2K
    //    task the check could neither watch the bus (H4) nor ever see the
    //    reception an image installed over NMEA 2000 has to prove (H5).
    if (appConfig.cfg.demoMode && !OtaGuard::pendingVerify()) {
        xTaskCreatePinnedToCore(demoTask, "DEMO", 4096, nullptr, 2, nullptr, 0);
        Serial.println("[setup] Demo data task started"); Serial.flush();
    } else {
        const bool n2kCreated =
            xTaskCreatePinnedToCore(n2kTask, "N2K", 8192, nullptr, 5, nullptr, 0) == pdPASS;
        // From here on health criterion H4 watches the task's loop counter -
        // also should it hang before N2kHandler::begin() is through, and also
        // should creating it have failed: a new image without a bus reader
        // rolls back.
        OtaGuard::setN2kTaskStarted();
        // Remembered for the web handler: switching demo mode OFF at runtime
        // is useless while this task does not exist, because nothing would be
        // left reading the bus. See the reboot in WebConfig::handlePostConfig.
        // Only when it exists: the loop watchdog's N2K gate reads it too, and
        // a task that was never created would reset a valid image every
        // minute instead of leaving it running without bus data.
        g_n2kTaskRunning = n2kCreated;
        // The pins are NOT printed here any more. They can now be overridden
        // from the config, and that is resolved inside the task in
        // N2kHandler::begin(), which logs the pair actually handed to the
        // driver. Printing the board defines here would have been the same
        // trap this comment used to warn about, one level further along.
        Serial.println(n2kCreated ? "[setup] NMEA2000 task started (real bus)"
                                  : "[setup] !!! NMEA2000 task NOT created (no memory) - no bus data");
        Serial.flush();
    }

    // 5. Building the screens (the boot screen itself is up since 3a).
    bootScreen.update(T(STR_BOOT_LOADING_SCREENS), 30);
    dispMgr.begin();
    bootScreen.update(T(STR_BOOT_READY), 100);
    Serial.println("Boot complete."); Serial.flush();

    // Short pause so "Bereit." is readable, then switch to main UI.
    // NOTE: activate() calls lv_scr_load() internally.  lv_scr_load_anim()
    // calls lv_obj_set_pos(lv_scr_act(), 0, 0) unconditionally; if the boot
    // screen has already been deleted, lv_scr_act() returns NULL → crash.
    // Fix: activate() first (boot screen is still the active screen), then
    // dismiss() the now-inactive boot screen.
    delay(600);
    Serial.println("Activating..."); Serial.flush();
    dispMgr.activate();
#if defined(BOARD_PANEL_1024X600)
    // Prime LVGL's temp-draw-buffer cache NOW, while the pool is still
    // unfragmented. lv_mem_buf_get() hands out a cached buffer for any
    // request <= a free cached size, so these two stay parked in the pool
    // forever and every later render is served without a fresh allocation.
    // Without this, the 600-grid renders allocated mid-frame from a pool
    // fragmented by overlay churn - measured: a 4808-byte request failed at
    // 11.9K free / 25% fragmentation and rebooted the device (menu crash).
    // 8K+5K, NOT more: the first attempt parked 21.5K and left only 9.2K for
    // the launcher's ~13K of widgets - the cure must not eat the patient.
    {
        void *p0 = lv_mem_buf_get(8192);
        void *p1 = lv_mem_buf_get(5120);
        // Rotation MAY need one MORE cached block: for a rotated flush
        // lv_refr.c pulls LV_DISP_ROT_MAX_BUF (10 KB) out of this very pool.
        // Reserving it here, while the pool is still unfragmented, is the same
        // medicine that cured the home-launcher crash - except a failure would
        // now hit on EVERY frame instead of on one screen.
        //
        // But that block is only ever asked for when LVGL itself rotates.
        // displayApplyRotation() normally transposes straight into the panel
        // frame buffer and leaves sw_rotate at 0, and then draw_buf_rotate()
        // never runs. Since lv_mem_buf_get() CACHES (release only clears the
        // `used` flag, it never frees), keying this on the config value would
        // park 10 KB of a 106 KB pool for the whole session for nothing - on a
        // board whose documented failure mode is exactly a starved pool.
        // So ask the flag that actually decides. displayApplyRotation() ran
        // long before this point (right after appConfig.begin()), and the
        // reservation is still made on the fallback path, where the frame
        // buffer was unreachable and sw_rotate went back to 1.
        lv_disp_t *rotDisp = lv_disp_get_default();
        const bool swRotate = rotDisp && rotDisp->driver && rotDisp->driver->sw_rotate;
        void *p2 = swRotate ? lv_mem_buf_get(LV_DISP_ROT_MAX_BUF) : nullptr;
        if (p2) lv_mem_buf_release(p2);
        if (p0) lv_mem_buf_release(p0);
        if (p1) lv_mem_buf_release(p1);
        Serial.printf("[lv_mem] draw-buf cache primed (8K+5K%s) %s\n",
                      swRotate ? "+10K rot" : "",
                      (p0 && p1) ? "ok" : "PARTLY FAILED");
    }
#endif
    // On the very first start, show the licences and have them confirmed.
    // Initial commissioning: first choose the language, then confirm the licences.
    // The language selector opens the licence screen itself once a choice was made.
    if (!appConfig.cfg.licenseAccepted) languageOverlay.requestOpen();
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        Serial.printf("[lv_mem] pool total=%u  used=%u  free=%u  frag=%u%%\n",
            mon.total_size, mon.total_size - mon.free_size,
            mon.free_size, mon.frag_pct);
        Serial.flush();
    }
    Serial.println("Activated – dismissing boot screen..."); Serial.flush();
    bootScreen.dismiss();
    Serial.println("Dismissed."); Serial.flush();
    bootMark("main UI built (drawn by the first loop pass)");

    // ---- OTA rollback: the verify window starts here -------------------------
    // CORRECTION of what this block used to say (N2K-UPDATE.md 10.1): rollback
    // IS active on all three boards. Their prebuilt bootloaders are built with
    // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE and do set PENDING_VERIFY on the
    // first boot of an update - the "state=2 on the first boot" measured on
    // 2026-08-21 proves exactly that: without bootloader rollback the state
    // would have stayed NEW. It read VALID because Arduino's initArduino()
    // confirmed the image BEFORE setup(), whenever the weak verifyRollbackLater()
    // returned false - the default. OtaGuard.cpp now defines it as true, so an
    // update is only confirmed once it has shown that it works.
    //
    // Reaching this line is health criterion H1 (setup within 90 s). From here
    // OtaGuard checks the loop, LVGL, the N2K task and - for an image that came
    // over NMEA 2000 - the reception of the update protocol, and confirms the
    // image 60 to 180 s later, or rolls it back. USB-flashed images are never
    // PENDING_VERIFY, so for them this only logs the running slot.
    OtaGuard::setupDone();
    // The setup budget becomes the loop watch: 30 s, fed from loop() from the
    // first pass on (the warm-up pass included). On probation the backstop
    // stays OtaGuard's until mark-valid hands it over.
    LoopWatchdog::setupDone();
    Serial.flush();

#if !LV_TICK_CUSTOM
    // LVGL tick task – started HERE (after boot screen is gone) so the
    // high-priority task does not interfere with the boot screen's delay()
    // calls, which would allow the WiFi task watchdog to fire (TG1WDT).
    xTaskCreatePinnedToCore(lvglTickTask, "LVTICK", 4096, nullptr, 6, nullptr, 1);
    Serial.println("LVTICK task started."); Serial.flush();
#else
    // 7B: LVGL ticks from millis() via LV_TICK_CUSTOM (see lv_conf.h) - no
    // dedicated task, 4 KB internal stack saved.
    Serial.println("LVGL tick: LV_TICK_CUSTOM(millis)."); Serial.flush();
#endif

#if USB_CONSOLE_BOUNDED
    // From the first loop() pass on, a console write that meets a host which
    // does not read gives up after 100 ms instead of ~2 s ("A USB console
    // nobody reads", above consoleSettle()).
    Serial.setTxTimeoutMs(USB_TX_TIMEOUT_MS);
#endif
}

// ---- Loop (Core 1) -----------------------------------------------------------

static uint32_t lastUpdate  = 0;
static uint32_t lastButtons = 0;

#if defined(BOARD_PANEL_1024X600)
// ---- Frame instrumentation (1024x600 boards only) ---------------------------
// Before this existed the only readout of frame cost was the on-screen perf
// overlay, and it rolled everything into a single "fps" number. The loop below
// runs two clearly separate phases, one after the other, on core 1:
//   paint - dispMgr.update() draws the instrument into its PSRAM canvas
//   tick  - lv_timer_handler() lets LVGL redraw the invalidated areas and
//           flush them into the panel frame buffer
// LVGL's monitor_cb can only ever see the second one, so measuring the two
// together would charge the render for time the canvas paint spent. Hence two
// worst-case accumulators here, and two more inside DisplaySetup_7B.cpp (one
// whole refresh, one flush_cb body). All of them are printed and cleared by the
// heartbeat, so every reading describes exactly one 5 s window.
static uint32_t s_paintMaxUs = 0;
static uint32_t s_tickMaxUs  = 0;

// Defined in DisplaySetup_7B.cpp. Declared here rather than in DisplaySetup.h
// because that header is shared with the 4" board, which has no such counters.
// stripsMax = flush_cb calls in the worst refresh of the window; multiplied by
// flushMaxUs it says how much of a frame the flush really is.
// flushSumUs = flush time SUMMED over that same worst refresh, so
// "rend - flushSum" is LVGL's own drawing. Both come from ONE frame.
extern void dispPerfTake(uint32_t *frames, uint32_t *renderMaxMs,
                         uint32_t *flushMaxUs, uint32_t *stripsMax,
                         uint32_t *flushSumUs, uint32_t *pxAtMax);
// Defined in display/DisplayManager.cpp. Splits the "paint" lump above into the
// current screen's own update() and everything else the manager does around it
// (sidebar, banners, perf overlay, anchor watch, deferred config work).
extern void dispPaintTake(uint32_t *screenMaxUs, uint32_t *chromeMaxUs);
// Defined in display/screens/WindScreen.cpp. Worst observed duration of EACH of
// the five draw phases of the wind instrument in the window, in microseconds:
// bg | zone | tick | boat | ovl. All five, not just the winner - reporting only
// the maximum meant the other four had never once been observed, so every
// conclusion about where the paint time goes rested on a single number.
// All zero whenever the wind screen was not on display.
extern void windPhaseTake(uint32_t out[6]);
#if defined(PERF_PAINT_PHASES)
// MEASUREMENT BUILD ONLY (-DPERF_PAINT_PHASES, see display/screens/PaintPhase.h).
// Defined in display/screens/ClockScreen.cpp, RudderScreen.cpp, AisScreen.cpp,
// DepthScreen.cpp, WindPlotScreen.cpp, AnchorScreen.cpp, WindScreen.cpp.
// Each prints one "[ph/<tag>]" line - max/mean of every paint phase of that
// screen since its last call - and clears it; nothing if it was not painted.
extern void clockPhaseReport();
extern void rudderPhaseReport();
extern void aisPhaseReport();
extern void depthPhaseReport();
extern void wplotPhaseReport();
extern void anchorPhaseReport();
// WindScreen.cpp prints up to six lines: wz/wb/wo for Wind & Trim, az/ac/ao
// for Schiffslage, each breaking down the [HB] ph fields above.
extern void windPhaseReport();
#endif

static inline void paintTimed() {
    const int64_t t0 = esp_timer_get_time();
    dispMgr.update();          // writes new pixel data to PSRAM canvas buffers
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_paintMaxUs) s_paintMaxUs = us;
}

static inline void tickTimed() {
    const int64_t t0 = esp_timer_get_time();
    displayTick();             // LVGL render + flush -> RGB frame buffer
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_tickMaxUs) s_tickMaxUs = us;
}

// The heartbeat line is shared with the 4" board, so the frame fields are
// appended through a compile-time-empty tail instead of by editing the format
// string in place. On the 4" build both macros expand to nothing - the empty
// string literal is concatenated away - and the line is emitted with exactly
// the format and arguments it has always had.
#define HB_PERF_FMT  " fps=%.1f paint=%.1f tick=%.1f rend=%u flush=%.2f" \
                     " str=%u px=%u fsum=%.0f draw=%.0f scr=%.1f chr=%.1f steal=%u%%" \
                     " ph bg=%.0f zone=%.0f tick=%.0f boat=%.0f trace=%.0f ovl=%.0f"
#define HB_PERF_ARGS , hbFps, hbPaintMs, hbTickMs, hbRendMs, hbFlushMs, \
                       hbStrips, hbPx, hbFsumMs, hbDrawMs, hbScrMs, hbChromeMs, hbStealPct, \
                       hbPh[0] / 1000.0f, hbPh[1] / 1000.0f, hbPh[2] / 1000.0f, \
                       hbPh[3] / 1000.0f, hbPh[4] / 1000.0f, hbPh[5] / 1000.0f
#else
#define HB_PERF_FMT  ""
#define HB_PERF_ARGS
#endif

void loop() {
    // Phase markers (LoopWatchdog::phase): one byte in the watchdog's RTC
    // record per section, so the boot after a hang names the section.
    LoopWatchdog::phase(LoopWatchdog::PH_TOP);
    // Health criterion H2 (the loop runs) and the crash-loop window's uptime
    // bookkeeping (N2K-UPDATE.md 10.2, 10.6).
    OtaGuard::loopTick();
    // The loop watchdog: its ONE feed point. A pass that does not come back
    // here within 30 s resets the device (LoopWatchdog.h).
    LoopWatchdog::loopTick();

#if defined(PANIC_PATH_TEST)
    // Test build only: the hang kinds of GET /api/panic-test that have to
    // happen in the loop itself (WebConfig.cpp). Their lines are the last
    // ones before the device goes away: consoleFlushFull() (UsbConsole.h).
    {
        const uint8_t t = LoopWatchdog::takeTestRequest();
        if (t == LoopWatchdog::T_LOOP) {
            Serial.println("[wdt-test] loop: spinning at the top of loop(), interrupts on");
            consoleFlushFull();
            LoopWatchdog::phase(LoopWatchdog::PH_TEST);
            volatile uint32_t n = 0;
            for (;;) n = n + 1;
        } else if (t == LoopWatchdog::T_LVGL) {
            // Runs in the next lv_timer_handler(), so the record says 'tick'.
            Serial.println("[wdt-test] lvgl: an LVGL timer callback spins");
            lv_timer_create([](lv_timer_t *) { volatile uint32_t n = 0; for (;;) n = n + 1; },
                            0, nullptr);
        } else if (t == LoopWatchdog::T_BLOCK25 || t == LoopWatchdog::T_BLOCK35) {
            // One long block, not a hang: 25 s must pass, 35 s must not.
            const uint32_t ms = t == LoopWatchdog::T_BLOCK25 ? 25000 : 35000;
            Serial.printf("[wdt-test] block: one %u ms delay in loop()\n", (unsigned)ms);
            consoleFlushFull();
            LoopWatchdog::phase(LoopWatchdog::PH_TEST);
            delay(ms);
            Serial.println("[wdt-test] block: back");
        } else if (t == LoopWatchdog::T_LATEABORT) {
            // A panic while the RTC watchdog's count already stands past the
            // panic handler's own 10 s hold. Expected: the panic output and
            // reason 4 at the next boot - no immediate rst 0x10.
            Serial.println("[wdt-test] lateabort: no feed for 15 s, then abort()");
            consoleFlushFull();
            LoopWatchdog::phase(LoopWatchdog::PH_TEST);
            delay(15000);
            Serial.println("[wdt-test] lateabort: abort() now");
            consoleFlushFull();
            abort();
        }
    }
#endif

    // On the very first loop() iteration, skip the heavy canvas render to let
    // WiFi complete any pending SPI0 transactions before we start writing to PSRAM.
    static bool _warmupDone = false;
    if (!_warmupDone) {
        _warmupDone = true;
        // Run 10 display ticks (~50 ms) without canvas updates so LVGL flushes
        // the initial UI (nav bar, hidden containers) safely over DRAM-only paths.
        LoopWatchdog::phase(LoopWatchdog::PH_TICK);
        for (int i = 0; i < 10; i++) { displayTick(); delay(5); }
        lastUpdate = millis();
        return;
    }

    uint32_t now = millis();

    // Button debounce check ~every 50 ms
    if (now - lastButtons >= BTN_DEBOUNCE_MS) {
        lastButtons = now;
        LoopWatchdog::phase(LoopWatchdog::PH_BUTTONS);
        dispMgr.handleButtons();
    }

    // Keep the WiFi station alive. This call used to be missing entirely, so a
    // lost association was never noticed: the device sat at IP 0.0.0.0 and only
    // a reboot brought the web UI back. Internally rate-limited to 2 s.
    LoopWatchdog::phase(LoopWatchdog::PH_WIFI);
    if (appConfig.cfg.wifiEnabled) webCfg.loop();
    LoopWatchdog::phase(LoopWatchdog::PH_RESTART);
    WebConfig::restartIfDue();   // /api/restart, firmware upload over WiFi
    LoopWatchdog::phase(LoopWatchdog::PH_BSH);
    bshLocalOffsetTick();        // DST switch between two SNTP syncs


    // Render with WiFi active.  This used to describe a vTaskDelay(1) inside
    // flush_cb that yielded SPI0 between strips for the WiFi beacon ISR; no
    // flush_cb in this firmware contains one any more, on either board, so the
    // note is gone rather than left standing as a false explanation.
    // Update at up to 10 fps (100 ms) so animations and touch responses are
    // smooth.  The canvas renderers are fast enough for this rate; NMEA data
    // refreshes at 1–10 Hz anyway.  pendingUpdate() triggers an immediate extra
    // frame whenever data or a screen-switch occurs.
    if (dispMgr.pendingUpdate() || now - lastUpdate >= 100) {
        lastUpdate = now;
        // esp_task_wdt_reset() removed: the loop task was never subscribed to
        // the TWDT (silent no-op on Arduino 2.x, "task not found" log flood at
        // ~100 lines/s on Arduino 3.x / the 7B).
#if defined(BOARD_PANEL_1024X600)
        LoopWatchdog::phase(LoopWatchdog::PH_PAINT);
        paintTimed();       // dispMgr.update(), timed as its own phase
        LoopWatchdog::phase(LoopWatchdog::PH_TICK);
        tickTimed();        // displayTick(), timed as its own phase
#else
        LoopWatchdog::phase(LoopWatchdog::PH_PAINT);
        dispMgr.update();   // writes new pixel data to PSRAM canvas buffers
        LoopWatchdog::phase(LoopWatchdog::PH_TICK);
        displayTick();      // LVGL flush → RGB frame-buffer
#endif
    } else {
        LoopWatchdog::phase(LoopWatchdog::PH_TICK);
#if defined(BOARD_PANEL_1024X600)
        tickTimed();
#else
        displayTick();
#endif
    }

    // Once, 10 s after start: has the IO expander kept what displayInit() told
    // it? Its wait proved it listening at one moment only, and the blind 8 s
    // it replaced also covered a firmware that applies its own defaults a few
    // seconds into a cold start (see displayLateCheck()). 10 s is past the
    // end of that old wait, so a unit that passes here is where the old code
    // would have started. Then the boot summary - on the USB-console boards
    // as soon as a terminal is there to read it, which may be much later.
    {
        LoopWatchdog::phase(LoopWatchdog::PH_LATE);
        static bool s_lateDone = false, s_summaryDue = false;
        if (!s_lateDone && now >= 10000) {
            s_lateDone   = true;
            const uint32_t t0 = millis();
            displayLateCheck();
#if USB_CONSOLE_BOUNDED
            // 5B: a host that stopped reading costs this pass once, here,
            // not in every line of the summary below ("A USB console nobody
            // reads"); the summary then waits for the next terminal.
            consoleSettle(t0);
#else
            (void)t0;
#endif
            s_summaryDue = true;
        }
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
        if (s_summaryDue && Serial) s_summaryDue = !bootSummary("console attached");
#else
        if (s_summaryDue) { s_summaryDue = false; bootSummary("10 s after start"); }
#endif
    }

    // Heartbeat every 5 s: confirms stable operation and shows memory.
    // Every 10 s also prints CH32V003 register state so we can confirm
    // SYS_EN=HIGH and backlight without needing to capture the boot log.
    static uint32_t lastHB   = 0;
    static uint32_t diagTick = 0;
    if (now - lastHB >= 5000) {
        lastHB = now;
        LoopWatchdog::phase(LoopWatchdog::PH_HB);
        extern volatile uint32_t g_n2kRxCount;   // N2kHandler.cpp
        // RSSI in the heartbeat: web trouble on this device has repeatedly
        // turned out to be the RADIO LINK, not the firmware (35-50 % packet
        // loss measured while the heap was perfectly healthy). With the value
        // in every heartbeat, "web UI is flaky" and "RSSI -75" line up in the
        // same log instead of needing a separate measurement session.
        int rssi = 0;
        if (appConfig.cfg.wifiEnabled && WiFi.status() == WL_CONNECTED)
            rssi = WiFi.RSSI();
        // The IP belongs in here too: "rssi=0" alone could not distinguish
        // "WiFi off" from "association lost", and an IP of 0.0.0.0 is exactly
        // the symptom of a link that associated but never got a DHCP lease.
        // In the fallback hotspot (AP+STA, station not joined) the address that
        // answers is the hotspot's - 0.0.0.0 there would read as "dead".
        const wifi_mode_t wm = appConfig.cfg.wifiEnabled ? WiFi.getMode() : WIFI_MODE_NULL;
        String ip = !appConfig.cfg.wifiEnabled ? String("off")
                  : (wm == WIFI_MODE_AP || (wm == WIFI_MODE_APSTA && !WebConfig::staLinkUp()))
                        ? WiFi.softAPIP().toString()
                        : WiFi.localIP().toString();
        // DRAM is printed as three numbers, not one. The total says nothing
        // about whether the next allocation succeeds: WiFi/HTTP asks for
        // several KB in ONE piece, so a heap with 20 KB free in 40 fragments
        // fails exactly like an empty one. blk = largest contiguous block is
        // therefore the number that predicts the stalls, and min = the
        // low-water mark since boot shows how close the worst moment came -
        // which a 5 s sample would otherwise always miss.
#if defined(BOARD_PANEL_1024X600)
        // Frame numbers for the window that just ended:
        //   fps   completed LVGL refreshes per second (monitor_cb, so it counts
        //         the same event in every screen orientation)
        //   paint worst dispMgr.update() - the canvas render into PSRAM
        //   tick  worst lv_timer_handler() - LVGL redraw + flush + timers
        //   rend  worst single LVGL refresh as LVGL itself measures it,
        //         flush included (whole ms; that is all lv_tick_get gives)
        //   flush worst single flush_cb body, in ms with two decimals because
        //         one strip is a fraction of a millisecond
        //   str   flush strips in the WORST refresh - flush x str is the real
        //         per-frame flush cost, which flush alone cannot say
        //   scr   worst dispMgr.update() spent in the current screen's own
        //         update(), i.e. the instrument itself
        //   chr   worst dispMgr.update() spent on everything else in the same
        //         call: sidebar, banners, perf overlay, anchor watch
        //   ph    worst single wind-instrument draw phase and which one it was
        //         (bg | zone | tick | boat | ovl; "-" when not on the wind screen)
        // scr and chr are per-call maxima, so scr + chr is normally the same
        // frame as paint, but does not have to add up to it exactly.
        // s_perfWinStart is kept apart from lastHB so fps is divided by the
        // window that was really measured; the first one runs from boot, which
        // is exactly the span the counters cover.
        static uint32_t s_perfWinStart = 0;
        uint32_t hbFrames = 0, hbRendMs = 0, hbFlushUs = 0, hbStrips = 0, hbFsumUs = 0;
        uint32_t hbPx = 0;
        uint32_t hbScrUs = 0, hbChromeUs = 0;
        uint32_t hbPh[6] = { 0, 0, 0, 0, 0, 0 };
        dispPerfTake(&hbFrames, &hbRendMs, &hbFlushUs, &hbStrips, &hbFsumUs, &hbPx);
        dispPaintTake(&hbScrUs, &hbChromeUs);
        windPhaseTake(hbPh);
        const uint32_t hbWinMs = now - s_perfWinStart;
        s_perfWinStart         = now;
        const float hbFps      = hbWinMs ? (hbFrames * 1000.0f / hbWinMs) : 0.0f;
        const float hbPaintMs  = s_paintMaxUs / 1000.0f;
        const float hbTickMs   = s_tickMaxUs  / 1000.0f;
        const float hbFlushMs  = hbFlushUs    / 1000.0f;
        // The split that matters: fsum is all the flush time inside the worst
        // refresh, draw is what is left of it - LVGL laying out and rasterising.
        // Both halves come from the same frame, so they add up to rend.
        const float hbFsumMs   = hbFsumUs     / 1000.0f;
        const float hbDrawMs   = (hbRendMs > hbFsumMs) ? (hbRendMs - hbFsumMs) : 0.0f;
        const float hbScrMs    = hbScrUs      / 1000.0f;
        const float hbChromeMs = hbChromeUs   / 1000.0f;
        // Measured once a minute, not every heartbeat: the probe busy-loops
        // for 20 ms and masks interrupts for 100 us of that.
        static uint32_t s_stealPm = 0; static uint8_t s_stealTick = 0;
        if ((s_stealTick++ % 12) == 0) s_stealPm = coreStealPerMille();
        const unsigned hbStealPct = (unsigned)((s_stealPm + 5) / 10);
        s_paintMaxUs = 0;
        s_tickMaxUs  = 0;
#endif
        // Ask the driver directly for the CAN state. Without it, "bus not
        // connected", "wrong pins" and "wrong baud rate" look exactly alike
        // from outside - namely all like n2kRx=0. The difference lies in
        // the error counters: real silence leaves them at 0, a misread bus
        // makes them climb.
        //   st=0 STOPPED | 1 RUNNING | 2 BUS_OFF | 3 RECOVERING; can=n/a = no driver
        //
        // The MEANING lies in the combination, not in any single number:
        //   rx=0 AND all error counters 0  -> the line is electrically silent
        //                                     (nothing connected / wrong pins)
        //   rx=0 BUT rxerr/buserr climbing -> bus present, but misread
        //                                     (baud rate, wiring, termination)
        char canTxt[72] = " can=n/a";
        {
            twai_status_info_t ti;
            if (twai_get_status_info(&ti) == ESP_OK)
                snprintf(canTxt, sizeof canTxt,
                         // buserr: bus errors seen since boot (error frames,
                         // any node's) - not bus-off episodes, which the
                         // driver logs as "[n2k] BUS-OFF" lines of their own.
                         " can=st%d rx=%u rxmiss=%u txerr=%u rxerr=%u buserr=%u",
                         (int)ti.state, (unsigned)ti.msgs_to_rx,
                         (unsigned)ti.rx_missed_count,
                         (unsigned)ti.tx_error_counter,
                         (unsigned)ti.rx_error_counter,
                         (unsigned)ti.bus_error_count);
        }
        // While an update over NMEA 2000 is running: its state and the N2K
        // task's stack high-water mark - flash work, the inflater and the
        // file system all run on that one 8 KB stack (N2K-UPDATE.md 9.1).
        char updTxt[48] = "";
        if (N2kUpdateNode::sessionActive())
            snprintf(updTxt, sizeof updTxt, " upd=st%u n2kstack=%u",
                     (unsigned)N2kUpdateNode::sessionState(),
                     (unsigned)N2kUpdateNode::n2kStackHighWater());
        else if (OtaGuard::pendingVerify())
            snprintf(updTxt, sizeof updTxt, " upd=verify%us",
                     (unsigned)OtaGuard::verifySecondsLeft());
        // wdt = the longest loop pass of the window, feed to feed, in ms: how
        // close a healthy device comes to the loop watchdog's 30 s - the
        // field data any change of that figure has to rest on.
        const uint32_t hbT0 = millis();   // consoleSettle(): how long the line has been out
        Serial.printf("[HB] t=%u DRAM=%u blk=%u min=%u PSRAM=%u n2kRx=%u%s%s rssi=%d ip=%s screen=%s"
                      HB_PERF_FMT " wdt=%u\n",
            now,
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            g_n2kRxCount,
            canTxt,
            updTxt,
            rssi,
            ip.c_str(),
            dispMgr.currentTitle()
            HB_PERF_ARGS,
            (unsigned)LoopWatchdog::takeWindowGapMs());
        // A flush on the 4" and 7B; on the 5B it also ends the waits of a USB
        // host that does not read (see consoleSettle()). consoleFree is false
        // only there, when that failed: the paint table and the expander line
        // (both nothing but log lines on the 5B) then wait for a heartbeat
        // whose lines get out, rather than waiting 0.1 s per write now.
        const bool consoleFree = consoleSettle(hbT0);
#if defined(BOARD_PANEL_1024X600)
        // Once a minute, so the log stays readable: what each screen visited so
        // far costs to paint. Cheap - it prints a table that update() fills in
        // anyway.
        { static uint8_t s_scrRepTick = 0;
          if (consoleFree && (s_scrRepTick++ % 12) == 0) {
              dispMgr.reportScreenPaint();
#if defined(PERF_PAINT_PHASES)
              // Same minute window, so each [ph/..] line sits next to the
              // [paint/screen] table it breaks down.
              clockPhaseReport();
              rudderPhaseReport();
              aisPhaseReport();
              depthPhaseReport();
              wplotPhaseReport();
              anchorPhaseReport();
              windPhaseReport();
              // The paint runs on this (the loop) task; its deepest chains
              // hold CdLvBatch draw contexts, so watch the headroom.
              Serial.printf("[ph/stack] loop task min free %u B\n",
                            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
#endif
          } }
#endif
        // Print CH32V003 pin state every other heartbeat (every 10 s).
        // gt911Diag() used to run here too — a bring-up probe that dumps the
        // touch controller's raw status/noise/config registers. It answered the
        // "is the panel wired up?" question long ago and only pads the log now;
        // the function stays for the next hardware revision.
        diagTick++;
        if (consoleFree && diagTick % 2 == 0) {
            LoopWatchdog::phase(LoopWatchdog::PH_DIAG);
            displayDiag();
        }
    }

#if defined(PERF_LICENSE_SCROLL)
    licenseOverlay.perfScrollTick();   // measurement build only, see the header
#endif

#if defined(BOARD_PANEL_1024X600) && defined(PERF_SCREEN_SWEEP)
    // Measurement build only (-DPERF_SCREEN_SWEEP). Steps through the carousel
    // by itself so every screen gets painted and lands in the per-screen table
    // without anyone standing at the panel swiping. Six seconds is long enough
    // for a screen to reach its steady state at 3 fps. Never in a shipping
    // build - it would make the panel change screens on its own.
    { static uint32_t s_sweepAt = 0;
      if (millis() - s_sweepAt >= 6000) { s_sweepAt = millis(); dispMgr.nextScreen(); } }
#endif

    // Stuck HERE means the loop was fine but did not get core 1 back: core
    // 0's tick stopped (only core 0 advances it), or a task above the loop's
    // priority held core 1. The next boot tells the two apart by a core-0
    // stamp (LoopWatchdog::core0Stamp).
    LoopWatchdog::phase(LoopWatchdog::PH_DELAY);
    delay(5);
}
