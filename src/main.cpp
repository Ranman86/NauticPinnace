#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <WiFi.h>   // WiFi.RSSI() for the heartbeat log
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <soc/timer_group_struct.h>
#include <soc/timer_group_reg.h>

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

static void lvglTickTask(void *) {
    for (;;) {
        lv_tick_inc(1);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

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
    PsramArena::init(3350000);   // +500 KB: 2nd 480×480 Wind canvas (Schiffslage attitude screen)

    // 1b. Display + LVGL – lv_init() must run before any lv_* calls
    displayInit();

    // 2. Filesystem + config (LittleFS can init after LVGL)
    appConfig.begin();
    Serial.println("Config loaded."); Serial.flush();

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
        disableCore0WDT();
        Serial.println("[setup] Core0 WDT disabled"); Serial.flush();

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
        Serial.println("[setup] NMEA2000 task started (real bus TX=6 RX=0)"); Serial.flush();
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

    // LVGL tick task – started HERE (after boot screen is gone) so the
    // high-priority task does not interfere with the boot screen's delay()
    // calls, which would allow the WiFi task watchdog to fire (TG1WDT).
    xTaskCreatePinnedToCore(lvglTickTask, "LVTICK", 4096, nullptr, 6, nullptr, 1);
    Serial.println("LVTICK task started."); Serial.flush();
}

// ---- Loop (Core 1) -----------------------------------------------------------

static uint32_t lastUpdate  = 0;
static uint32_t lastButtons = 0;

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


    // Render with WiFi active.  The flush_cb vTaskDelay(1) yields SPI0 between
    // strips so the WiFi beacon ISR can complete its ~0.1 ms DMA.
    // Update at up to 10 fps (100 ms) so animations and touch responses are
    // smooth.  The canvas renderers are fast enough for this rate; NMEA data
    // refreshes at 1–10 Hz anyway.  pendingUpdate() triggers an immediate extra
    // frame whenever data or a screen-switch occurs.
    if (dispMgr.pendingUpdate() || now - lastUpdate >= 100) {
        lastUpdate = now;
        esp_task_wdt_reset();
        dispMgr.update();   // writes new pixel data to PSRAM canvas buffers
        displayTick();      // LVGL flush → RGB frame-buffer
    } else {
        displayTick();
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
        Serial.printf("[HB] t=%u DRAM=%u PSRAM=%u n2kRx=%u rssi=%d ip=%s screen=%s\n",
            now,
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            g_n2kRxCount,
            rssi,
            ip.c_str(),
            dispMgr.currentTitle());
        Serial.flush();
        // Print CH32V003 pin state every other heartbeat (every 10 s).
        // gt911Diag() used to run here too — a bring-up probe that dumps the
        // touch controller's raw status/noise/config registers. It answered the
        // "is the panel wired up?" question long ago and only pads the log now;
        // the function stays for the next hardware revision.
        diagTick++;
        if (diagTick % 2 == 0) displayDiag();
    }

    delay(5);
}
