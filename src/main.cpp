#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <esp_ota_ops.h>   // OTA rollback confirmation, see the end of setup()
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
// frozen fantasy numbers.
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

// ---- Setup -------------------------------------------------------------------

void setup() {
    // Disable the interrupt WDT (TG1 MWDT) early.  The 300 ms default threshold
    // is tight relative to the PSRAM zero-fill and SPI display init; disabling it
    // here keeps setup clean.  The Task WDT (TG0) is handled by unsubscribing all
    // known tasks below.  Unlock key for TG1 MWDT on ESP32-S3: 0x50D83AA1.
    TIMERG1.wdtwprotect.wdt_wkey = 0x50D83AA1U;
    TIMERG1.wdtconfig0.wdt_en    = 0;
    TIMERG1.wdtwprotect.wdt_wkey = 0;

    // Unsubscribe loopTask and Core-1 IDLE from TWDT before the long setup.
    // Core-0 IDLE cannot be unsubscribed yet – WiFi subscribes it during
    // esp_wifi_start(), which hasn't been called yet.  We handle it after WiFi.
    disableLoopWDT();
    disableCore1WDT();
    esp_task_wdt_delete(xTaskGetCurrentTaskHandle());  // belt-and-suspenders

    Serial.begin(115200);
    // USB-CDC on ESP32-S3: after a hard-reset the port takes ~2-4 s to reconnect.
    // CH32V003 also needs ~4-5 s to fully boot its own firmware before it accepts
    // I2C commands reliably.  8 s covers both.
    delay(8000);
    Serial.println("\n=== NauticPinnace booting ===");

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
    // Wind/Attitude buffer: 4.31 MB + draw-time scratches -> 4.4 MB leaves
    // ~0.53 MB PSRAM free for WiFi/TLS/JSON spikes (was 1.58 MB on the
    // 480 grid). Unshared demand would be 5.03 MB - does not fit.
    //
    // +20 KB (4.40 -> 4.42 MB): bulk data that used to sit in internal RAM now
    // sub-allocates from here as well - the DataModel rings below are ~10.8 KB
    // of it. That is paid out of the 0.53 MB PSRAM slack (down to ~0.51 MB),
    // which is the cheap side of the trade: the internal heap it relieves is
    // the one that starves WiFi/HTTP on these boards.
    PsramArena::init(4420000);
#else
    PsramArena::init(3350000);   // +500 KB: 2nd 480×480 Wind canvas (Schiffslage attitude screen)
    // Deliberately NOT raised for the 4-inch board: its canvas demand is
    // ~2.6 MB, so this arena already carries several hundred KB of unused
    // slack and swallows the DataModel rings without touching free PSRAM.
#endif

    // 1a-bis. The DataModel's AIS table and history rings are arena buffers
    //         too (~10.8 KB of internal RAM recovered on every board). This
    //         has to happen HERE: the global `data` was constructed before
    //         setup() ran, so its buffer pointers are still null, and the
    //         first thing that pushes a sample or renders a history screen
    //         would dereference them. Right after the arena exists and long
    //         before any task or screen can touch the model.
    data.initBuffers();

    // 1b. Display + LVGL – lv_init() must run before any lv_* calls
    displayInit();

    // 2. Filesystem + config (LittleFS can init after LVGL)
    appConfig.begin();
    Serial.println("Config loaded."); Serial.flush();

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
    //    (esp_wifi_start, which subscribes Core-0 IDLE to the TWDT, never runs).
    if (appConfig.cfg.wifiEnabled) {
        Serial.println("[setup] webCfg start"); Serial.flush();
        webCfg.begin(appConfig.cfg.apMode,
                     appConfig.cfg.wifiSsid,
                     appConfig.cfg.wifiPassword);
        Serial.println("[setup] webCfg done"); Serial.flush();

        // NOW unsubscribe Core-0 IDLE: esp_wifi_start() (called inside webCfg.begin)
        // subscribes it to the TWDT.  Calling disableCore0WDT() here – after WiFi
        // has actually started – ensures the task IS already in the watch list and
        // the unsubscription succeeds.
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

        // Tide forecast: when online, sync the clock (SNTP) and pull the nearest
        // German gauge's official HW/NW predictions from the BSH API.
        bshTideBegin();
    } else {
        Serial.println("[setup] WiFi DISABLED by config (Funk aus)"); Serial.flush();
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
    if (appConfig.cfg.demoMode) {
        xTaskCreatePinnedToCore(demoTask, "DEMO", 4096, nullptr, 2, nullptr, 0);
        Serial.println("[setup] Demo data task started"); Serial.flush();
    } else {
        xTaskCreatePinnedToCore(n2kTask, "N2K", 8192, nullptr, 5, nullptr, 0);
        // Remembered for the web handler: switching demo mode OFF at runtime
        // is useless while this task does not exist, because nothing would be
        // left reading the bus. See the reboot in WebConfig::handlePostConfig.
        g_n2kTaskRunning = true;
        // The pins are NOT printed here any more. They can now be overridden
        // from the config, and that is resolved inside the task in
        // N2kHandler::begin(), which logs the pair actually handed to the
        // driver. Printing the board defines here would have been the same
        // trap this comment used to warn about, one level further along.
        Serial.println("[setup] NMEA2000 task started (real bus)");
        Serial.flush();
    }

    // 5. Boot screen – now safe, WiFi heap already established
    bootScreen.show(appConfig.cfg.bootName);
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
                      (p0 && p1) ? "ok" : "TEILWEISE FEHLGESCHLAGEN");
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

    // ---- OTA rollback: declare THIS image healthy --------------------------
    // MEASURED 2026-08-21: rollback is NOT active in this build, despite
    // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in the env sdkconfig. The
    // pioarduino platform ships a PREBUILT bootloader.bin (no bootloader
    // object files are produced in the build dir), and rollback lives in the
    // bootloader - so the option only reaches the app-side libs and does
    // nothing. Proof: after a real OTA the image reports state=2 (VALID)
    // on its very first boot instead of 1 (PENDING_VERIFY).
    //
    // This call is therefore a no-op today, and deliberately kept: it costs
    // nothing, and the day the bootloader IS built from our config (own IDF
    // build, or a platform that rebuilds it) the safety net works without
    // anyone remembering to add it back.
    //
    // What IS protected without rollback: the bootloader validates the image,
    // so a truncated or corrupted upload never boots - it keeps the previous
    // slot. What is NOT protected: a complete image that crashes at runtime.
    // That needs USB access, which is why the WebUI says so plainly.
    //
    // Reaching this line is a meaningful health check, not a formality: the
    // panel is initialised, the config loaded and all screens built. An update
    // that crashes before this point rolls itself back, which is the whole
    // point of the mechanism on a boat where nobody can reach the USB port.
    // USB-flashed images are never in pending state, so this is a no-op there.
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        const bool haveState = running &&
            esp_ota_get_state_partition(running, &st) == ESP_OK;
        // Always log WHICH slot is running. Without this an OTA test is
        // unfalsifiable: both slots hold the same firmware, so a silent
        // rollback looks exactly like a successful update.
        Serial.printf("[ota] running from '%s' @ 0x%06X, state=%d\n",
                      running ? running->label : "?",
                      running ? (unsigned)running->address : 0u,
                      haveState ? (int)st : -1);
        if (haveState && st == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            Serial.println("[ota] image confirmed healthy - rollback cancelled");
        }
        Serial.flush();
    }

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
extern void windPhaseTake(uint32_t out[5]);

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
                     " ph bg=%.0f zone=%.0f tick=%.0f boat=%.0f ovl=%.0f"
#define HB_PERF_ARGS , hbFps, hbPaintMs, hbTickMs, hbRendMs, hbFlushMs, \
                       hbStrips, hbPx, hbFsumMs, hbDrawMs, hbScrMs, hbChromeMs, hbStealPct, \
                       hbPh[0] / 1000.0f, hbPh[1] / 1000.0f, hbPh[2] / 1000.0f, \
                       hbPh[3] / 1000.0f, hbPh[4] / 1000.0f
#else
#define HB_PERF_FMT  ""
#define HB_PERF_ARGS
#endif

void loop() {
    // On the very first loop() iteration, skip the heavy canvas render to let
    // WiFi complete any pending SPI0 transactions before we start writing to PSRAM.
    static bool _warmupDone = false;
    if (!_warmupDone) {
        _warmupDone = true;
        // Run 10 display ticks (~50 ms) without canvas updates so LVGL flushes
        // the initial UI (nav bar, hidden containers) safely over DRAM-only paths.
        for (int i = 0; i < 10; i++) { displayTick(); delay(5); }
        lastUpdate = millis();
        return;
    }

    uint32_t now = millis();

    // Button debounce check ~every 50 ms
    if (now - lastButtons >= BTN_DEBOUNCE_MS) {
        lastButtons = now;
        dispMgr.handleButtons();
    }

    // Keep the WiFi station alive. This call used to be missing entirely, so a
    // lost association was never noticed: the device sat at IP 0.0.0.0 and only
    // a reboot brought the web UI back. Internally rate-limited to 2 s.
    if (appConfig.cfg.wifiEnabled) webCfg.loop();


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
        paintTimed();       // dispMgr.update(), timed as its own phase
        tickTimed();        // displayTick(), timed as its own phase
#else
        dispMgr.update();   // writes new pixel data to PSRAM canvas buffers
        displayTick();      // LVGL flush → RGB frame-buffer
#endif
    } else {
#if defined(BOARD_PANEL_1024X600)
        tickTimed();
#else
        displayTick();
#endif
    }

    // Heartbeat every 5 s: confirms stable operation and shows memory.
    // Every 10 s also prints CH32V003 register state so we can confirm
    // SYS_EN=HIGH and backlight without needing to capture the boot log.
    static uint32_t lastHB   = 0;
    static uint32_t diagTick = 0;
    if (now - lastHB >= 5000) {
        lastHB = now;
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
        String ip = appConfig.cfg.wifiEnabled
                    ? (WiFi.getMode() == WIFI_MODE_AP ? WiFi.softAPIP().toString()
                                                      : WiFi.localIP().toString())
                    : String("off");
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
        uint32_t hbPh[5] = { 0, 0, 0, 0, 0 };
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
        Serial.printf("[HB] t=%u DRAM=%u blk=%u min=%u PSRAM=%u n2kRx=%u rssi=%d ip=%s screen=%s"
                      HB_PERF_FMT "\n",
            now,
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            g_n2kRxCount,
            rssi,
            ip.c_str(),
            dispMgr.currentTitle()
            HB_PERF_ARGS);
        Serial.flush();
#if defined(BOARD_PANEL_1024X600)
        // Once a minute, so the log stays readable: what each screen visited so
        // far costs to paint. Cheap - it prints a table that update() fills in
        // anyway.
        { static uint8_t s_scrRepTick = 0;
          if ((s_scrRepTick++ % 12) == 0) dispMgr.reportScreenPaint(); }
#endif
        // Print CH32V003 pin state every other heartbeat (every 10 s).
        // gt911Diag() used to run here too — a bring-up probe that dumps the
        // touch controller's raw status/noise/config registers. It answered the
        // "is the panel wired up?" question long ago and only pads the log now;
        // the function stays for the next hardware revision.
        diagTick++;
        if (diagTick % 2 == 0) displayDiag();
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

    delay(5);
}
