#include "WebConfig.h"
#include "../nmea/AnchorSyncNode.h"
#include "Config.h"
#include "../i18n/I18n.h"
#include "../nmea/DataModel.h"
#include "../nmea/N2kHandler.h"   // g_n2kSrcSeen for GET /api/sources
#include "../nmea/BusWatch.h"     // GET /api/data: bus state for the Live tab
#include "../DisplaySetup.h"
#include "../display/DisplayManager.h"
#include "../PolarTable.h"
#include "../PsramArena.h"   // GET /api/info: how the arena came out
#include <esp_heap_caps.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_wifi.h>   // esp_wifi_set_ps/get_ps - PS must be forced at IDF level
#include <LittleFS.h>
#include <esp_ota_ops.h>   // GET /api/info reports the running slot
#include <esp_system.h>    // esp_get_idf_version() for the same
#include "../Version.h"
#include "../WifiNaming.h"
#include "../n2kupdate/N2kUpdateNode.h"
#include "../n2kupdate/OtaGuard.h"
#include "../n2kupdate/OtaWriter.h"
#include "../n2kupdate/UpdateGate.h"
#include "../n2kupdate/UpdRegions.h"
#include "../n2kupdate/WebUiWriter.h"
#include "../LoopWatchdog.h"
#include "../UsbConsole.h"   // consoleFlushFull() before a restart
#include "OtaUploadSlot.h"

// main.cpp: set when the NMEA 2000 task was created at boot. Needed to tell a
// live demo-mode change apart from one that leaves the device with no data
// source at all.
extern volatile bool g_n2kTaskRunning;

// Demo data is what feeds the model right now - the condition the panel's
// demo banner shows on (DisplayManager::updateDemoBanner: demoMode &&
// !demoDeferred()), repeated here because demoDeferred() is file-static
// there. Switched on while an update session or its verify window keeps the
// N2K task parsing the bus, demo mode waits (N2K-UPDATE.md 5.1 rule 6) and the
// values are still the bus's; booted in demo mode there is no N2K task, so
// nothing to wait for. Keep the two in step.
static bool demoFeedsModel() {
    return appConfig.cfg.demoMode &&
           !(g_n2kTaskRunning && !UpdateGate::demoAllowed());
}

#ifdef PANIC_PATH_TEST
// Test build only (-DPANIC_PATH_TEST): the panic end of the restart path,
// __wrap_esp_restart_noos() in lib/NMEA2000_esp32, and the hangs the loop
// watchdog (LoopWatchdog.h) has to end. A normal restart can be asked for from
// the web interface; a panic or a hang has to be provoked. GET /api/panic-test
// answers with the reset reason of this boot and the watchdog's state ("wdt");
// ?kind= brings the device down:
//   null0/1   store to address 0 on core 0/1     abort0/1  abort()
//   loop      loop() spins at its top, interrupts on
//   lvgl      an LVGL timer callback spins, inside lv_timer_handler()
//   block25   one 25 s delay in loop() - must NOT reset (maxGapMs ~25000)
//   block35   one 35 s delay in loop() - resets after 30 s
//   irq1/0    a task on core 1/0 spins with its interrupts masked
//   both      irq0 and irq1 together (like OTA_TEST_HANG)
//   lock      a task on core 0 takes data.lock() and never gives it back
//   spin0     a priority-6 task on core 0 spins with interrupts ON: the N2K
//             task starves while loop() runs on (the N2K gate: 30 + 30 s)
//   stall0    a task on core 1 masks its interrupts and stalls core 0: the
//             reset must still boot (measured: rst 0x10 at stage-0 time,
//             which releases the stall; stage 1 is the fallback)
//   rsthang   the next restart hangs in a shutdown handler - then POST
//             /api/restart
//   lateabort loop() does not feed for 15 s, then calls abort(): the panic
//             handler re-arms the RTC watchdog for 10 s against a count that
//             already stands at 15 s. Expected: the panic output (backtrace,
//             core dump), rst 0x03, reason 4 - not an rst 0x10 right after
//             the abort line (LoopWatchdog.cpp, the panic wrap; 7B/5B)
// Until this module the hang kinds only froze a core for good (main.cpp turns
// the interrupt watchdog off, and on the 7B/5B with WiFi on the task watchdog).
// What a reset costs: after an update, before "[ota] marked valid", the first
// one rolls the image back. From then on OtaGuard counts and three in a row roll
// it back, so restart normally after every second one: a clean boot resets the
// count. A USB-flashed image is never rolled back.
#include <esp_rom_sys.h>
#include <esp_cpu.h>   // esp_cpu_stall()
enum : int { PT_NULL, PT_ABORT, PT_IRQ, PT_LOCK, PT_SPIN, PT_STALL0 };
static void panicTestTask(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(300));      // the reply leaves first
    volatile uint32_t n = 0;
    switch ((intptr_t)arg) {
        case PT_NULL: *(volatile int *)0 = 1; break;
        case PT_ABORT: abort(); break;
        case PT_IRQ:                     // levels 1-3 masked; the level-4 TG1 is off
            portDISABLE_INTERRUPTS();
            for (;;) n = n + 1;
        case PT_LOCK: {
            auto lk = data.lock();
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        case PT_SPIN:                    // interrupts on: the tick goes on
            // Not while the N2K task (priority 5, core 0) holds the data lock:
            // starved inside a message handler it would block the loop on that
            // lock as well, and the run would end as kind 'loop' (phase paint/
            // bsh) instead of testing the N2K gate. Taking the lock once waits
            // until nobody holds it - priority inheritance lets the N2K task
            // finish its handler - and a FreeRTOS give does not hand the mutex
            // on: a waiting N2K task would have to run on core 0 to take it,
            // and from here on it never does.
            { auto lk = data.lock(); }
            for (;;) n = n + 1;
        case PT_STALL0:                  // what a panic handler on core 1 does
            portDISABLE_INTERRUPTS();
            esp_cpu_stall(0);
            for (;;) n = n + 1;
    }
    vTaskDelete(nullptr);
}
#endif

static void pinProtocolBG();   // defined below, used from start()
static void applyWifiCountry();   // the same
#if ESP_IDF_VERSION_MAJOR >= 5
static void sendOwnedJson(AsyncWebServerRequest *req, const String &json);
#endif

// ---- WiFi update (/api/ota) ----------------------------------------------------
// The WiFi owner's writers. UpdateGate admits one upload at a time, and only
// the async_tcp task ever touches them.
// A restart asked for by a web handler. A handler must not restart the device
// itself: ESPAsyncWebServer's req->send() only stores the response, which goes
// onto the wire after the handler has returned - so `delay(500); ESP.restart()`
// inside the handler lost every reply. A firmware upload over WiFi then looked
// failed in the browser (and to curl) although it had worked. restartIfDue()
// restarts from the main loop once the reply has had time to leave.
static const uint32_t    RESTART_DELAY_MS = 1000;
static const uint32_t    OTA_RX_TIMEOUT_S = 20;
static volatile bool     s_restartDue = false;
static volatile uint32_t s_restartAt  = 0;

static void restartSoon(const char *why) {
    Serial.printf("%s - restarting in %u ms\n", why, (unsigned)RESTART_DELAY_MS);
    s_restartAt  = millis() + RESTART_DELAY_MS;
    s_restartDue = true;
}

void WebConfig::restartIfDue() {
    if (!s_restartDue || (int32_t)(millis() - s_restartAt) < 0) return;
    Serial.println("[web] restarting now"); consoleFlushFull();
    LoopWatchdog::phase(LoopWatchdog::PH_RESTART);   // a hang in there: the loop watch
    ESP.restart();
}

static OtaWriter   s_wifiFirmware(UpdateGate::OWNER_WIFI);
static WebUiWriter s_wifiWebUi(UpdateGate::OWNER_WIFI, nullptr);

// Which upload owns the two writers above, and when a new one may take them
// over from an upload whose client has gone silent: OtaUploadSlot.h, which
// also holds OtaUpload (per request) and the steps of an upload. What is
// left here only hands them the writers.
static OtaUploadSlot s_otaSlot;

struct OtaEnv {
    uint32_t now()        { return millis(); }
    bool     canStart()   { return UpdateGate::canStartWifiOta(); }
    uint32_t gateRetryS() { return UpdateGate::restartBlockedSeconds(); }
    uint8_t begin(uint8_t t, uint32_t &retryS) {
        if (t == OTA_TARGET_FW) return s_wifiFirmware.begin(0);
        uint32_t retryMs = 0;
        const uint8_t r = s_wifiWebUi.beginWifi(retryMs);
        retryS = (retryMs + 999) / 1000;
        return r;
    }
    uint8_t write(uint8_t t, const uint8_t *p, size_t n) {
        return t == OTA_TARGET_FW ? s_wifiFirmware.writeAny(p, n) : s_wifiWebUi.writeAny(p, n);
    }
    uint8_t finish(uint8_t t, uint8_t &detail) {
        return t == OTA_TARGET_FW ? s_wifiFirmware.finishAndActivate()
                                  : s_wifiWebUi.finishAndApply(detail);
    }
    void abort(uint8_t t) {
        if (t == OTA_TARGET_FW) s_wifiFirmware.abort();
        else                    s_wifiWebUi.abort();
    }
};

static void otaAbort(OtaUpload *u) {
    OtaEnv env;
    if (otaUploadRelease(s_otaSlot, u, env))
        Serial.println("[ota] upload aborted - writer released");
}

// The reason code as a stable token for the web interface, which shows its
// own text for it.
static const char *otaReasonName(uint8_t r) {
    switch (r) {
        case n2kupd::RSN_WRONG_BOARD:          return "WRONG_BOARD";
        case n2kupd::RSN_DESCRIPTOR_INVALID:   return "DESCRIPTOR_INVALID";
        case n2kupd::RSN_IMAGE_INVALID:        return "IMAGE_INVALID";
        case n2kupd::RSN_SIZE_MISMATCH:        return "SIZE_MISMATCH";
        case n2kupd::RSN_TOO_LARGE:            return "TOO_LARGE";
        case n2kupd::RSN_FLASH_ERROR:          return "FLASH_ERROR";
        case n2kupd::RSN_APPLY_FAILED:         return "APPLY_FAILED";
        case n2kupd::RSN_UNSUPPORTED_TARGET:   return "UNSUPPORTED_TARGET";
        case n2kupd::RSN_OUT_OF_MEMORY:        return "OUT_OF_MEMORY";
        default:                               return "ERROR";
    }
}

// ---- station disconnect reason (diagnosis) ------------------------------------
// Recorded in the core's event task, read by the boot sequence, the settings
// line and /api/info - plain words, a torn read costs nothing. The sequence
// number tells a waiting attempt whether a reason arrived DURING it, rather
// than one left over from the attempt before.
static volatile uint8_t  s_lastReason   = 0;
static volatile uint8_t  s_outageReason = 0;   // the same, cleared on link-up
static volatile uint32_t s_lastReasonMs = 0;
static volatile uint32_t s_reasonSeq    = 0;
static uint32_t          s_fbSeq0       = 0;   // s_reasonSeq when a background attempt began

// The plain function-pointer form (WiFiEventSysCb): one signature on Arduino
// 2.0.17 and 3.3, and no std::function.
static void onStaDisconnected(arduino_event_t *ev) {
    if (!ev || ev->event_id != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
    const uint8_t r = ev->event_info.wifi_sta_disconnected.reason;
    if (r == WifiReason::ASSOC_LEAVE) return;   // our own WiFi.disconnect()
    s_lastReason   = r;
    s_outageReason = r;
    s_lastReasonMs = millis();
    s_reasonSeq    = s_reasonSeq + 1;
}

// Every place that sees the link come up calls this. Events arrive in order,
// so a reason from before the link came up has been recorded by then and is
// wiped here, not afterwards.
static void outageOver() { s_outageReason = 0; }

uint8_t  WebConfig::lastReason()      { return s_lastReason; }
uint8_t  WebConfig::outageReason()    { return s_outageReason; }
uint32_t WebConfig::lastReasonAgeMs() { return s_lastReason ? millis() - s_lastReasonMs : 0; }

uint32_t WebConfig::fallbackNextRetryS() const {
    // While deferred, the due time is pushed on by 5 s at every look - read
    // as a countdown it would say "next try in 3 s" for as long as the client
    // stays, and the one reading it is, as a rule, that very client.
    if (!_fbActive || _fbTrying || _fbDeferLog) return 0;
    const int32_t d = (int32_t)(_fbNextTry - millis());
    return d > 0 ? (uint32_t)(d + 999) / 1000 : 0;
}

// The window of one join attempt, at boot and in the background alike. Not
// lengthened on purpose (owner decision pending, see the WiFi report); what
// changed is that a failed boot is no longer the end of it.
static const uint32_t JOIN_WINDOW_MS = 6000;

// Background retries in the fallback hotspot: first after 30 s, then doubling,
// capped at 2 min. Short enough to catch a Pi hotspot that comes up a minute
// after the display, long enough that an absent network does not keep the
// radio scanning.
static const uint32_t FB_BACKOFF_FIRST_MS = 30000;
static const uint32_t FB_BACKOFF_MAX_MS   = 120000;

static const char *authName(uint8_t a) {
    // wifi_auth_mode_t, identical 0..8 on IDF 4.4 and 5.5.
    switch (a) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA_PSK";
        case 3: return "WPA2_PSK";
        case 4: return "WPA_WPA2_PSK";
        case 5: return "ENTERPRISE";
        case 6: return "WPA3_PSK";
        case 7: return "WPA2_WPA3_PSK";
        case 8: return "WAPI_PSK";
        default: return "OTHER";
    }
}

void WebConfig::start(bool apMode, const char *ssid, const char *password) {
    // ── WiFi hygiene (fixes the recurring AUTH_FAIL-on-boot + HTTP stalls) ──
    // persistent(false): don't store creds in NVS. The Arduino core otherwise
    //   auto-connects from stale NVS on boot, racing our begin() and emitting a
    //   spurious "Reason: 202 - AUTH_FAIL" before the real connect succeeds.
    // setSleep(false): disable WiFi modem power-save. With it on (the default in
    //   STA mode) the radio naps between DTIM beacons; on a busy/marginal link
    //   that adds latency and drops TCP segments, which is what made large HTTP
    //   responses (the UI page) stall and the async server appear "wedged".
    WiFi.persistent(false);

    // The hotspot name, logged on EVERY boot rather than only when the AP is
    // actually started. Two reasons: in station mode there was previously no
    // way to find out what this device's hotspot would be called without
    // reaching the settings screen, and printing it here - before any
    // WiFi.mode() call - proves the name no longer depends on the WiFi state.
    // Password deliberately not logged; serial output ends up in bug reports.
    Serial.printf("[wifi] hotspot name: %s\n", wifiApSsid().c_str());

    // Kept for connect() and the background retries. They point into
    // appConfig.cfg, which lives for the whole session.
    _ssid = ssid ? ssid : "";
    _pass = password ? password : "";

    if (apMode || strlen(_ssid) == 0) {
        _staWanted = false;
        WiFi.mode(WIFI_AP);
        applyWifiCountry();        // before softAP(): the configured hotspot's beacons name it
        WiFi.setSleep(false);
        // Internal hotspot: SSID "NauticPinnace<last6 MAC>", password random
        // per device (see WifiNaming.h / Entropy.h) — shown in the on-screen
        // config in plaintext + QR so a phone can join.
        String apS = wifiApSsid();
        String apP = wifiApPassword();
        WiFi.softAP(apS.c_str(), apP.c_str());
        // Password deliberately NOT logged: serial logs end up in bug reports
        // and photos. It is only readable on the display (settings / QR code).
        Serial.printf("AP started: %s  IP: %s\n",
                      apS.c_str(), WiFi.softAPIP().toString().c_str());
        return;
    }

    _staWanted = true;
    WiFi.mode(WIFI_STA);
    pinProtocolBG();               // see the comment at the helper
    // Covers everything that follows without a re-init: the joins, the boot
    // scan, the fallback's mode(WIFI_AP_STA) + softAP() and its background
    // retries - from STA, mode() changes the mode without a new init.
    applyWifiCountry();
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    // From here on every reason the station is disconnected for is kept (see
    // onStaDisconnected). Registered once per boot: start() runs once.
    WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.disconnect();          // clear any stale association from a prior boot
    delay(100);
}

bool WebConfig::attemptJoin(uint8_t attempt, uint8_t attempts, WifiProgressFn progress) {
    WifiProgress p;
    p.phase    = WifiProgress::CONNECTING;
    p.attempt  = attempt;
    p.attempts = attempts;
    strlcpy(p.ssid, _ssid, sizeof p.ssid);

    Serial.printf("[wifi] connect attempt %u to '%s'...\n", (unsigned)attempt, _ssid);
    const uint32_t seq0 = s_reasonSeq;
    WiFi.begin(_ssid, _pass);
    const uint32_t t = millis();
    while (millis() - t < JOIN_WINDOW_MS) {
        if (WiFi.status() == WL_CONNECTED) return true;
        // Polled every 50 ms plus one boot-screen frame (it was 150 ms of
        // plain delay): the window is the same, the panel stays alive.
        if (progress) progress(p);
        delay(50);
    }
    // The core logs every reason itself ("Reason: 201 - NO_AP_FOUND"), this
    // line says which one ENDED the attempt - the core retries once
    // internally, so one attempt can see two.
    if (s_reasonSeq != seq0)
        Serial.printf("[wifi] attempt %u failed: reason %u (%s)\n", (unsigned)attempt,
                      (unsigned)s_lastReason, WifiReason::name(s_lastReason));
    else
        Serial.printf("[wifi] attempt %u failed: no disconnect reason within %u s "
                      "(status %d - associated but no DHCP lease?)\n", (unsigned)attempt,
                      (unsigned)(JOIN_WINDOW_MS / 1000), (int)WiFi.status());
    // After the LAST attempt the core must not start a join of its own any
    // more. With auto-reconnect on, a NO_AP_FOUND still queued in its event
    // task at this moment makes it call begin() again right after our
    // disconnect - and then the scan that follows fails ("WiFi still
    // connecting"), or the station joins behind the fallback's back.
    // startFallback() keeps it off; a join turns it back on.
    if (attempt >= attempts) WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    const uint32_t t2 = millis();
    while (millis() - t2 < 250) {
        if (progress) progress(p);
        delay(50);
    }
    return false;
}

void WebConfig::scanTarget(WifiProgressFn progress) {
    WifiProgress p;
    p.phase = WifiProgress::SCANNING;
    strlcpy(p.ssid, _ssid, sizeof p.ssid);
    if (progress) progress(p);

    // One scan, so the log can tell "not there" from "there, but refused":
    // the reason codes alone cannot, because the station reports NO_AP_FOUND
    // for a network that is up but out of range just as for one that is off.
    // Directed - the name goes into the probe requests - so a hidden network
    // answers too, and the result holds only that network's entries (a few
    // bytes instead of every boat in the marina). Asynchronous, so the boot
    // screen keeps animating meanwhile.
    const uint32_t t0 = millis();
    int16_t n = WiFi.scanNetworks(true, true, false, 300, 0, _ssid);
    while (n == WIFI_SCAN_RUNNING && millis() - t0 < 8000) {
        if (progress) progress(p);
        delay(50);
        n = WiFi.scanComplete();
    }
    const unsigned ms = (unsigned)(millis() - t0);
    if (n < 0) {
        _scanSeen = -1;
        Serial.printf("[wifi] scan for '%s' failed (%d) after %u ms\n", _ssid, (int)n, ms);
        WiFi.scanDelete();
        return;
    }
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(WiFi.SSID(i).c_str(), _ssid) != 0) continue;
        if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best >= 0) {
        _scanSeen = 1;
        _scanRssi = (int8_t)WiFi.RSSI(best);
        _scanCh   = (uint8_t)WiFi.channel(best);
        _scanAuth = (uint8_t)WiFi.encryptionType(best);
        Serial.printf("[wifi] scan: '%s' seen - ch %u, RSSI %d dBm, auth %s "
                      "(%d entr%s, %u ms) -> it is there, the join was refused\n",
                      _ssid, (unsigned)_scanCh, (int)_scanRssi, authName(_scanAuth),
                      (int)n, n == 1 ? "y" : "ies", ms);
    } else {
        _scanSeen = 0;
        Serial.printf("[wifi] scan: '%s' NOT seen on any channel (%u ms) -> not up "
                      "(yet), out of range, or 5 GHz only\n", _ssid, ms);
    }
    WiFi.scanDelete();
}

void WebConfig::startFallback() {
    // No joins by the core itself from here on: with auto-reconnect on, every
    // NO_AP_FOUND makes it start the next join at once - a full channel scan
    // each time - and that drags the hotspot's channel round the band without
    // end. The retries are ours now, timed, and only on a free hotspot.
    WiFi.setAutoReconnect(false);
    // AP+STA instead of the plain WIFI_AP this used to be: the station
    // interface stays, so the network can be retried without tearing the
    // hotspot down. The hotspot itself is unchanged - same name, password and
    // address as in hotspot mode.
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    String apS = wifiApSsid();
    String apP = wifiApPassword();
    WiFi.softAP(apS.c_str(), apP.c_str());
    Serial.printf("WiFi failed, started AP: %s (password only on the display/QR)\n",
                  apS.c_str());
    fallbackArm(millis());
    Serial.printf("[wifi] fallback: AP+STA, last reason %u (%s), scan %s - '%s' is "
                  "retried in the background, first in %u s, only while no "
                  "client is on the hotspot\n",
                  (unsigned)s_lastReason, WifiReason::name(s_lastReason),
                  _scanSeen > 0 ? "saw it" : _scanSeen == 0 ? "did not see it" : "failed",
                  _ssid, (unsigned)(_fbBackoff / 1000));
}

void WebConfig::fallbackArm(uint32_t now) {
    _fbActive   = true;
    _fbTrying   = false;
    _fbDeferLog = false;
    _fbTries    = 0;
    _fbBackoff  = FB_BACKOFF_FIRST_MS;
    _fbSince    = now;
    _fbNextTry  = now + _fbBackoff;
}

void WebConfig::connect(WifiProgressFn progress) {
    if (_staWanted) {
        // Up to 3 attempts × ~6 s. A single transient AUTH_FAIL no longer matters.
        bool ok = false;
        for (uint8_t attempt = 1; attempt <= 3 && !ok; attempt++)
            ok = attemptJoin(attempt, 3, progress);

        if (ok) {
            // Force power-save OFF at the IDF level and VERIFY it - do not
            // trust the Arduino wrapper here. The association often comes from
            // the NVS auto-connect race (not from our WiFi.begin), which
            // starts the modem with the default WIFI_PS_MIN_MODEM. With PS on,
            // single-segment replies (/api/data) still work but multi-segment
            // transfers (the 114 KB UI page) stall to zero bytes - measured:
            // AP+STA mode served the page fine (AP keeps the radio awake by
            // hardware), pure STA stalled it.
            esp_wifi_set_ps(WIFI_PS_NONE);
            wifi_ps_type_t ps = WIFI_PS_NONE;
            esp_wifi_get_ps(&ps);
            Serial.printf("WiFi connected. IP: %s  RSSI: %d dBm  ch: %d  ps=%d(0=NONE)\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                          WiFi.channel(), (int)ps);
            _staMode = true;        // arm the link supervision in loop()
            _wasUp   = staLinkUp();
            outageOver();           // a 201 from attempt 1 is history now
            if (progress) {
                WifiProgress p;
                p.phase = WifiProgress::CONNECTED;
                strlcpy(p.ssid, _ssid, sizeof p.ssid);
                strlcpy(p.ip, WiFi.localIP().toString().c_str(), sizeof p.ip);
                progress(p);
            }
        } else {
            scanTarget(progress);
            startFallback();
            if (progress) {
                WifiProgress p;
                p.phase    = WifiProgress::FALLBACK;
                // The last reason any of the three attempts reported. Not
                // only the third one's: an attempt that ends without a
                // reason (a slow handshake cut off by the window) would
                // otherwise hide the 202 the two before it gave. Where the
                // code and the scan disagree, WifiReason::text() lets the
                // scan decide the words.
                p.reason   = s_outageReason;
                p.seen     = _scanSeen;
                p.retryInS = (uint16_t)(_fbBackoff / 1000);
                strlcpy(p.ssid, _ssid, sizeof p.ssid);
                strlcpy(p.ip, WiFi.softAPIP().toString().c_str(), sizeof p.ip);
                strlcpy(p.apSsid, wifiApSsid().c_str(), sizeof p.apSsid);
                progress(p);
            }
        }
    }
    // The page itself was loaded into its PSRAM region by UpdRegions::bootLoad()
    // in setup(), WiFi or not: INFO reports its id over NMEA 2000 too.
    setupRoutes();
#if ESP_IDF_VERSION_MAJOR >= 5
    // Kill HTTP keep-alive on the 7B's AsyncWebServer (3.12): when a browser
    // aborts an in-flight fetch (tab switch) and immediately re-requests on
    // the SAME pooled connection, the server splices the second response's
    // bytes into the first response's content-length window. Reproduced
    // deterministically with a pipelined socket: /api/config restarted at
    // byte 5630 mid-body - the UI then fell back to defaults (empty screens
    // list, wrong IP shown). One response per connection makes the interleave
    // mechanically impossible. The 4" keeps its pinned server version and
    // behavior untouched.
    DefaultHeaders::Instance().addHeader("Connection", "close");
#endif
    _server.begin();
}

// ---- gateway probe (half-dead link detection) ---------------------------------
// One ICMP echo over a raw socket, sent from loop() and read back by it when the
// next probe is due. This used to be esp_ping, whose task ended after every probe,
// every 15 s. A task that has used a socket leaves a semaphore in its thread-local
// storage, and the idle task frees that for it. On a 1024-byte stack, with heap
// poisoning and an interrupt arriving at the deepest point, that overflowed (core
// dump of a 5B, 04.11.2026: prvIdleTask -> prvDeleteTCB -> sys_thread_sem_free ->
// vQueueDelete -> multi_heap_free -> _xt_lowint1, stack watchpoint in IDLE1). The
// more WiFi traffic, the likelier: a firmware upload brought it out. The idle
// stack is fixed in the 4"'s prebuilt libraries. loop() never ends, so nothing is
// left for the idle task, and the ping task's 2.5 KB stay in the internal heap.
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/sockets.h"

static const uint16_t PROBE_ID = 0x4E50;   // "NP"
static int      s_probeSock = -1;
static uint16_t s_probeSeq  = 0;
static bool     s_probeGotReply = false;   // the last probe got its echo reply
// Only a gateway that has answered before can show a link has gone half-dead. One
// that never does - no router in the lease, a firewall dropping pings - says nothing
// about the link, and its misses forced a re-association every 45 s for good. Kept
// across re-associations: one that did not heal the link must be tried again.
static uint32_t s_probedGw   = 0;          // where the last probe went; 0 = no gateway
static uint32_t s_answeredGw = 0;          // the gateway that has answered since boot

// Empties the socket, which also gets a copy of any other ICMP for this device.
static void probeCollect() {
    if (s_probeSock < 0) return;
    uint8_t buf[64];
    int n;
    while ((n = lwip_recvfrom(s_probeSock, buf, sizeof(buf), MSG_DONTWAIT, nullptr, nullptr)) > 0) {
        const int ipLen = (buf[0] & 0x0F) * 4;     // a raw socket keeps the IP header
        if (n < ipLen + (int)sizeof(icmp_echo_hdr)) continue;
        icmp_echo_hdr e;
        memcpy(&e, buf + ipLen, sizeof(e));
        if (ICMPH_TYPE(&e) == ICMP_ER && e.id == PROBE_ID && e.seqno == htons(s_probeSeq)) {
            s_probeGotReply = true;
            s_answeredGw    = s_probedGw;
        }
    }
}

void WebConfig::probeGateway() {
    IPAddress gw = WiFi.gatewayIP();
    s_probedGw = (uint32_t)gw;
    if (s_probedGw == 0) return;               // no gateway - nothing to probe

    if (s_probeSock < 0) s_probeSock = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    s_probeGotReply = false;
    if (s_probeSock < 0) return;               // counts as a miss, like a lost reply

    icmp_echo_hdr pkt = {};
    ICMPH_TYPE_SET(&pkt, ICMP_ECHO);
    ICMPH_CODE_SET(&pkt, 0);
    pkt.id     = PROBE_ID;
    pkt.seqno  = htons(++s_probeSeq);
    pkt.chksum = inet_chksum(&pkt, sizeof(pkt));

    sockaddr_in to = {};
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = (uint32_t)gw;
    if (lwip_sendto(s_probeSock, &pkt, sizeof(pkt), 0, (sockaddr *)&to, sizeof(to)) < 0) {
        lwip_close(s_probeSock);               // a fresh one for the next probe
        s_probeSock = -1;
    }
}

// Pin the station to 802.11b/g - no 11n/HT layer at all. Even with AMPDU
// disabled the link still wedged every 1-2 minutes under browser load
// (probe-heal-wedge oscillation, reconnect counter climbing); block-ACK and
// MCS rate negotiation only exist in HT, so dropping to b/g removes the whole
// machinery. A config UI does not miss the bandwidth. Must be re-applied
// after every WiFi stop: disconnect(true) powers the modem off, which resets
// the protocol mask to the default.
static void pinProtocolBG() {
    esp_err_t e = esp_wifi_set_protocol(WIFI_IF_STA,
                      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
    Serial.printf("[wifi] protocol pinned to 11b/g -> %s\n", esp_err_to_name(e));
}

// The WiFi country (wifi.country, web interface tab WiFi; list in
// WifiCountry.h). "" is the default and makes no call at all, so every board
// keeps the country its framework starts with - the 4" (IDF 4.4) channels
// 1-13, the 7B/5B (IDF 5.5) world safe mode "01", channels 1-11 - exactly as
// before the setting existed.
//
// A chosen code is set MANUAL (ieee80211d_enabled = false): that country's
// channels are scanned actively - in Europe 12 and 13 too, so the directed
// boot scan finds a hidden network there - and the own hotspot names that
// country in its beacon; a country an access point announces is ignored.
// AUTO would scan 12-14 only passively, which a hidden network never answers.
// The flip side: abroad the device keeps these channels until the setting is
// changed, which is what the web interface's hint says.
//
// RAM only, nothing reaches the WiFi library's NVS: WiFi.persistent(false) in
// start() makes the core select WIFI_STORAGE_RAM, under which ESP-IDF keeps
// the WiFi configuration, the country included, in RAM only (observed on IDF
// 4.4 and 5.5: after a restart the country is back at the default). So
// "Standard" plus a restart really is the default again, with no reset call.
//
// Like pinProtocolBG(): to be re-applied after every WiFi re-init.
// disconnect(true) de-initialises WiFi, and the next WiFi.mode() starts with
// the framework's country again. Takes effect at the next start - the WiFi
// tab's save restarts the device anyway, so there is no live apply.
static void applyWifiCountry() {
    const char *cc = appConfig.cfg.wifiCountry;
    if (!cc[0]) return;   // default: the framework's own country, untouched
    const esp_err_t e = esp_wifi_set_country_code(cc, false);   // false = MANUAL
    Serial.printf("[wifi] country %s (manual) -> %s\n", cc, esp_err_to_name(e));
}

void WebConfig::forceReassoc(const char *why) {
    Serial.printf("[wifi] forcing re-association: %s (reconnects so far: %u)\n",
                  why, (unsigned)_reconnects);
    _reconnects++;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    // A hotspot still lingering after a background join is gone with the two
    // calls above; nothing left for apOff() to wait for.
    _apLinger = false;
    pinProtocolBG();
    applyWifiCountry();   // disconnect(true) dropped it with the WiFi init
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(appConfig.cfg.wifiSsid, appConfig.cfg.wifiPassword);
    // Success is detected by the normal supervisor path; the recovery edge
    // there re-asserts esp_wifi_set_ps(WIFI_PS_NONE).
    _wasUp       = false;
    _downSince   = millis();
    _nextRetry   = millis() + 8000;   // give this attempt time before backoff
    _retryDelay  = 8000;
    _probeMisses = 0;
}

// ---- STA link supervision ----------------------------------------------------

bool WebConfig::staLinkUp() {
    return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

// The async web server is interrupt-driven and needs no servicing. The WiFi
// STATION does. Observed failure: after some hours the association was gone,
// the display showed IP 0.0.0.0, and the device stayed unreachable until a
// reboot — while other 2.4 GHz devices on the same AP were fine.
// setAutoReconnect(true) does not cover this: it is a one-shot hint, not a
// supervisor, and WiFi.disconnect() (which begin() calls in its retry loop)
// clears the credentials it would need. So we keep the credentials ourselves
// and re-run begin() with a capped backoff.
void WebConfig::loop() {
    if (_fbActive) { fallbackLoop(); return; }   // own hotspot, retrying the network
    if (!_staMode) return;                    // AP mode has no association to lose
    const uint32_t now = millis();
    if (now - _lastCheck < 2000) return;      // cheap: check every 2 s
    _lastCheck = now;

    // After a background join the own hotspot stays only as long as a phone
    // is still on it (see fallbackJoined()).
    if (_apLinger && WiFi.softAPgetStationNum() == 0) apOff("its last client left");

    if (staLinkUp()) {
        if (!_wasUp) {                        // edge: report recovery once
            // Every fresh association may come up with default power-save
            // (WIFI_PS_MIN_MODEM) - force it off at IDF level on the recovery
            // edge, or large HTTP responses stall again (see connect()).
            esp_wifi_set_ps(WIFI_PS_NONE);
            Serial.printf("[wifi] link up again: IP %s  RSSI %d dBm  ch %d"
                          "  (reconnects so far: %u)\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                          WiFi.channel(), (unsigned)_reconnects);
            _wasUp = true;
            // Nothing from before the outage counts on this association: a probe
            // sent then has no answer coming, and it would be a false miss, or the
            // third one, tearing down a link just up.
            _lastProbe   = 0;
            _probeMisses = 0;
            outageOver();
        }
        _downSince  = 0;
        _retryDelay = 0;
        _nextRetry  = 0;

        // Status says CONNECTED - but that is exactly what it also says while
        // the data path is dead. Verify end-to-end with a gateway ping every
        // 15 s; three consecutive misses (~45 s worst case) force a full
        // re-association even though the driver still claims all is well.
        if (now - _lastProbe >= 15000) {
            probeCollect();                    // before a first probe too: empties the socket
            if (_lastProbe != 0) {             // evaluate the PREVIOUS probe
                if (s_probeGotReply) {
                    _probeMisses = 0;
                } else if (s_probedGw != 0 && s_probedGw == s_answeredGw) {
                    _probeMisses++;
                    Serial.printf("[wifi] gateway probe miss %u/3\n",
                                  (unsigned)_probeMisses);
                }
            }
            _lastProbe = now;
            if (_probeMisses >= 3) {
                forceReassoc("link half-dead: 3 gateway probes unanswered "
                             "while status=CONNECTED");
                return;
            }
            probeGateway();
        }
        return;
    }

    // ---- link is down ----
    if (_apLinger) {
        // Joined in the background while a phone was on the own hotspot, and
        // the network went again before that phone left. The teardown below
        // (disconnect(true) + STA only) would take the hotspot from under it -
        // the very thing the retries are careful never to do. Hotspot up,
        // network gone: that is the fallback again, so it is handled as one.
        Serial.printf("[wifi] link DOWN (status=%d) while the own hotspot still has "
                      "%u client(s) - back to the fallback, retries only on a free "
                      "hotspot\n", (int)WiFi.status(), (unsigned)WiFi.softAPgetStationNum());
        _staMode  = false;
        _apLinger = false;
        WiFi.setAutoReconnect(false);         // as in startFallback()
        WiFi.disconnect();                    // the station only; the hotspot stays
        fallbackArm(now);
        return;
    }
    if (_wasUp || _downSince == 0) {
        _downSince = now;
        _nextRetry = now + 3000;              // brief grace: let the core retry
        _retryDelay = 3000;
        Serial.printf("[wifi] link DOWN (status=%d, ip=%s) — supervising\n",
                      (int)WiFi.status(), WiFi.localIP().toString().c_str());
        _wasUp = false;
    }
    if ((int32_t)(now - _nextRetry) < 0) return;

    _reconnects++;
    // Full re-association, not WiFi.reconnect(): after a lost link the stored
    // config can be empty (see above), and begin() with explicit credentials
    // also re-runs DHCP, which is the half that was actually missing.
    Serial.printf("[wifi] reconnect attempt %u to '%s' (down for %u s)\n",
                  (unsigned)_reconnects, appConfig.cfg.wifiSsid,
                  (unsigned)((now - _downSince) / 1000));
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    _apLinger = false;                        // see forceReassoc()
    pinProtocolBG();
    applyWifiCountry();                       // as in forceReassoc()
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(appConfig.cfg.wifiSsid, appConfig.cfg.wifiPassword);

    // Backoff 3 s → 60 s so a genuinely absent network does not spam the log
    // or hog the radio, while a brief AP hiccup is caught almost immediately.
    _retryDelay = (_retryDelay < 60000) ? (_retryDelay * 2) : 60000;
    if (_retryDelay > 60000) _retryDelay = 60000;
    _nextRetry  = now + _retryDelay;
}

// ---- fallback hotspot: background retries -------------------------------------
// Runs from loop() while the fallback is up. One attempt at a time, each with
// the same window as at boot; between attempts the backoff (30 s, 60 s, then
// every 120 s). Nothing here blocks: an attempt is started and then looked at
// on the following passes.
void WebConfig::fallbackLoop() {
    const uint32_t now = millis();
    // Every 250 ms while an attempt runs (the join is noticed promptly), once
    // a second otherwise.
    if (now - _lastCheck < (_fbTrying ? 250u : 1000u)) return;
    _lastCheck = now;

    // Joined - by our attempt, or by one we did not start: a connect the core
    // had already queued when auto-reconnect went off. Either way the link is
    // what counts, and the supervision has to take over; looking only while
    // an attempt runs left such a join unnoticed until the next retry - which
    // on Arduino 3 even tears a live link down first.
    if (staLinkUp()) { fallbackJoined(); return; }

    if (_fbTrying) {
        // A client came onto the hotspot WHILE the attempt runs - typically
        // the phone of someone who just read "http://192.168.4.1" on the boot
        // screen, and the first retry is due 30 s after that text. The guard
        // below only holds at the start of an attempt; the connect scan and
        // the join that may follow would move the channel under this phone
        // all the same. So the attempt stops here (looked at every 250 ms)
        // and waits for a free hotspot like any deferred one. Not counted as
        // a failure: the backoff stays where it was.
        const uint8_t c = WiFi.softAPgetStationNum();
        if (c) {
            WiFi.disconnect();
            _fbTrying   = false;
            _fbDeferLog = true;               // "free again - retrying now" follows
            _fbNextTry  = now + 5000;
            Serial.printf("[wifi] background retry %u aborted after %u ms: %u client%s "
                          "joined the own hotspot meanwhile - retried once it is free\n",
                          (unsigned)_fbTries, (unsigned)(now - _fbTryStart),
                          (unsigned)c, c == 1 ? "" : "s");
            return;
        }
        if (now - _fbTryStart < JOIN_WINDOW_MS) return;
        // Failed. Stop the station so it does not keep scanning on its own -
        // auto-reconnect is off, but the core still retries once internally.
        WiFi.disconnect();
        _fbTrying  = false;
        _fbBackoff = (_fbBackoff >= FB_BACKOFF_MAX_MS / 2) ? FB_BACKOFF_MAX_MS
                                                           : _fbBackoff * 2;
        _fbNextTry = now + _fbBackoff;
        if (s_reasonSeq != s_fbSeq0)
            Serial.printf("[wifi] background retry %u failed: reason %u (%s) - hotspot "
                          "stays up, next try in %u s\n", (unsigned)_fbTries,
                          (unsigned)s_lastReason, WifiReason::name(s_lastReason),
                          (unsigned)(_fbBackoff / 1000));
        else
            Serial.printf("[wifi] background retry %u failed: no disconnect reason "
                          "within %u s (status %d) - hotspot stays up, next try in %u s\n",
                          (unsigned)_fbTries, (unsigned)(JOIN_WINDOW_MS / 1000),
                          (int)WiFi.status(), (unsigned)(_fbBackoff / 1000));
        return;
    }

    if ((int32_t)(now - _fbNextTry) < 0) return;

    // Only on a free hotspot. The station has to scan to find the network,
    // and joining it moves the radio to that network's channel - the hotspot
    // goes along, and a phone on it loses its connection, in the middle of
    // whatever it was doing in the web interface.
    const uint8_t clients = WiFi.softAPgetStationNum();
    if (clients) {
        if (!_fbDeferLog) {
            Serial.printf("[wifi] background retry deferred: %u client%s on the own "
                          "hotspot (a scan would move its channel and drop them)\n",
                          (unsigned)clients, clients == 1 ? "" : "s");
            _fbDeferLog = true;
        }
        _fbNextTry = now + 5000;              // look again shortly
        return;
    }
    if (_fbDeferLog) {
        Serial.println("[wifi] own hotspot free again - retrying now");
        _fbDeferLog = false;
    }

    _fbTries++;
    Serial.printf("[wifi] background retry %u to '%s' (fallback for %u s)\n",
                  (unsigned)_fbTries, _ssid, (unsigned)((now - _fbSince) / 1000));
    pinProtocolBG();                          // as the other reconnect paths do
    s_fbSeq0 = s_reasonSeq;
    WiFi.begin(_ssid, _pass);
    _fbTrying   = true;
    _fbTryStart = now;
}

void WebConfig::fallbackJoined() {
    const uint32_t now = millis();
    esp_wifi_set_ps(WIFI_PS_NONE);            // see connect()
    WiFi.setAutoReconnect(true);              // back to the station settings
    if (_fbTrying)
        Serial.printf("[wifi] background retry %u: joined '%s' - IP %s  RSSI %d dBm  "
                      "ch %d (after %u s on the own hotspot)\n", (unsigned)_fbTries,
                      _ssid, WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                      WiFi.channel(), (unsigned)((now - _fbSince) / 1000));
    else
        Serial.printf("[wifi] joined '%s' without a retry of ours running (a connect the "
                      "driver had queued) - IP %s  RSSI %d dBm  ch %d (after %u s on the "
                      "own hotspot)\n", _ssid, WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                      WiFi.channel(), (unsigned)((now - _fbSince) / 1000));
    outageOver();
    _fbActive   = false;
    _fbTrying   = false;
    _fbDeferLog = false;

    // From here the normal link supervision takes over, exactly as after a
    // join at boot.
    _staMode     = true;
    _wasUp       = true;
    _downSince   = 0;
    _retryDelay  = 0;
    _nextRetry   = 0;
    _lastProbe   = 0;
    _probeMisses = 0;
    _lastCheck   = now;

    // The own hotspot goes now - unless a phone is on it, then once it left.
    const uint8_t clients = WiFi.softAPgetStationNum();
    if (clients == 0) {
        apOff("joined the network, no client on it");
    } else {
        _apLinger = true;
        Serial.printf("[wifi] own hotspot stays while its %u client%s %s on it\n",
                      (unsigned)clients, clients == 1 ? "" : "s",
                      clients == 1 ? "is" : "are");
    }
}

void WebConfig::apOff(const char *why) {
    // softAPdisconnect(true) leaves the station alone: AP+STA becomes STA.
    WiFi.softAPdisconnect(true);
    esp_wifi_set_ps(WIFI_PS_NONE);            // re-assert after the mode change
    _apLinger = false;
    Serial.printf("[wifi] own hotspot off (%s) - link %s, IP %s\n", why,
                  staLinkUp() ? "up" : "DOWN", WiFi.localIP().toString().c_str());
}

void WebConfig::setupRoutes() {
    // REST routes FIRST – more specific paths must be registered before the
    // catch-all serveStatic("/") handler, otherwise every /api/... request
    // falls through to LittleFS (which logs "does not exist" spam).

    // REST: GET current config as JSON
    _server.on("/api/config", HTTP_GET, handleGetConfig);

    // REST: POST new config (full or partial JSON)
    _server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *req){},
        nullptr, handlePostConfig);

    // REST: GET live sensor data
    _server.on("/api/data", HTTP_GET, handleGetData);

    // REST: GET firmware identity. Deliberately NOT part of /api/config: that
    // document is what Config::save() writes to config.json, so a version put
    // there would be persisted and then read back on the next boot as if it
    // described the running firmware - which after an update it would not.
    // This endpoint reports what is actually executing, every time it is asked.
    _server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["version"] = FW_VERSION;
        doc["model"]   = FW_MODEL_VERSION;
        doc["board"]   = FW_BOARD_NAME;
        doc["boardId"] = NP_BOARD_ID;
        doc["chip"]    = ESP.getChipModel();
        doc["idf"]     = esp_get_idf_version();
        // Which OTA slot is running - the pair that matters when an update
        // misbehaves and the question is whether the rollback took.
        const esp_partition_t *run = esp_ota_get_running_partition();
        doc["slot"] = run ? run->label : "?";
        // Limits and default of settings the WebUI edits, so the form takes
        // them from the firmware that enforces them instead of keeping a
        // second copy. Here rather than in /api/config for the reason given
        // above: that document IS config.json, and limits carried in it would
        // be stored, exported and imported as if they were settings.
        {
            JsonObject wd = doc["limits"]["wind_damp_s"].to<JsonObject>();
            wd["min"] = WindDampCfg::MIN_S;
            wd["max"] = WindDampCfg::MAX_S;
            wd["def"] = WindDampCfg::DEFAULT_S;
        }
        // N2K-UPDATE.md 11.4: the same facts INFO gives an update sender.
        auto hex = [](const uint8_t *p, size_t n) {
            char b[2 * 32 + 1];
            for (size_t i = 0; i < n; i++) snprintf(b + 2 * i, 3, "%02x", p[i]);
            b[2 * n] = 0;
            return String(b);
        };
        doc["otaState"] = OtaGuard::pendingVerify() ? "pending_verify" : "valid";
        const uint16_t vs = OtaGuard::verifySecondsLeft();
        if (vs != 0xFFFF) doc["verifySecondsLeft"] = vs;
        else              doc["verifySecondsLeft"] = nullptr;
        doc["runningSlot"]    = OtaGuard::runningSlot();
        doc["runningImageId"] = hex(OtaGuard::runningImageId(), 8);
        uint8_t rbId[8], rbVer[16], rbCause = 0, rbDetail = 0;
        if (OtaGuard::rollbackInfo(rbId, rbVer, rbCause, rbDetail)) {
            char v[17]; memcpy(v, rbVer, 16); v[16] = 0;
            doc["lastInvalidVersion"] = v;
            doc["rollbackCause"]      = rbCause;
            doc["rollbackDetail"]     = rbDetail;
        } else {
            doc["lastInvalidVersion"] = nullptr;
            doc["rollbackCause"]      = 0;
        }
        uint8_t webId[8], webVer[16];
        uint32_t webSize = 0;
        UpdRegions::webUiIdentity(webId, webSize, webVer);
        char wv[17]; memcpy(wv, webVer, 16); wv[16] = 0;
        doc["webUiId"]      = hex(webId, 8);
        doc["webUiVersion"] = wv;
        doc["webUiSize"]    = webSize;
        // A short arena shows here without a serial console: on the 7B/5B the
        // canvas arena gives way to the frame buffer when PSRAM runs out
        // (PsramArena::initLeavingRoom), and the screens built last stay empty.
        JsonObject pr = doc["psram"].to<JsonObject>();
        pr["arena"]       = PsramArena::size();
        pr["arenaWanted"] = PsramArena::wanted();
        pr["arenaUsed"]   = PsramArena::used();
        pr["free"]        = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        // The internal RAM, which WiFi, TCP and every small allocation live on.
        // "min" is the lowest it has been since boot.
        JsonObject hp = doc["heap"].to<JsonObject>();
        hp["free"]    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        hp["largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        hp["min"]     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        doc["uptimeS"] = millis() / 1000;
        // How this boot began, and the loop watchdog: armed or not (a USB
        // host switches it off on the 4" and 5B), the longest loop pass since
        // boot, and the last reset of its own, from NVS (LoopWatchdog.h).
        LoopWatchdog::fillReset(doc["reset"].to<JsonObject>());
        LoopWatchdog::fillInfo(doc["loopWatchdog"].to<JsonObject>());
        JsonObject n = doc["n2kUpdate"].to<JsonObject>();
        n["acceptUpdates"] = appConfig.cfg.n2kAcceptUpdates;
        n["protocol"]      = 1;
        n["sessionState"]  = N2kUpdateNode::sessionState();
        n["capabilities"]  = N2kUpdateNode::capabilities();
        n["selfTest"]      = N2kUpdateNode::selfTestsPassed();
        n["listenOnly"]    = appConfig.cfg.n2kListenOnly;
        n["gate"]          = UpdateGate::describe();
        // With gate "wifi": the seconds until a new WiFi upload may take the
        // writers over from the one that has them (OtaUploadSlot.h), 0 = now
        // or nobody has them. The web interface asks before it sends a file.
        n["wifiTakeoverS"] = s_otaSlot.takeoverInS(millis());
        // The WiFi state with the reason the network was last lost or not
        // joined - what a forum answer needs and a user rarely has a serial
        // log for. Names as in WifiStatus.h / the IDF.
        {
            JsonObject w = doc["wifi"].to<JsonObject>();
            const wifi_mode_t m = WiFi.getMode();
            w["mode"] = m == WIFI_MODE_APSTA ? "apsta" : m == WIFI_MODE_AP ? "ap"
                      : m == WIFI_MODE_STA   ? "sta"   : "off";
            w["staUp"]    = WebConfig::staLinkUp();
            // The WiFi country: the setting ("" = default, no call made), and
            // what the WiFi library reports it is using. Careful with the
            // latter under policy "auto" (the default): with the station
            // connected the IDF reports the ACCESS POINT's country from its
            // beacon, not the device's - so the web interface shows it only
            // for "manual", a country this device was given.
            w["countryCfg"] = appConfig.cfg.wifiCountry;
            wifi_country_t wc;
            if (m != WIFI_MODE_NULL && esp_wifi_get_country(&wc) == ESP_OK) {
                char cc[3] = { wc.cc[0], wc.cc[1], 0 };   // cc is not NUL-terminated
                JsonObject k = w["country"].to<JsonObject>();
                k["code"]     = cc;
                k["schan"]    = wc.schan;
                k["nchan"]    = wc.nchan;
                k["policy"]   = wc.policy == WIFI_COUNTRY_POLICY_MANUAL ? "manual" : "auto";
                k["maxTxDbm"] = wc.max_tx_power;
            }
            w["fallback"] = webCfg.fallbackActive();
            const uint8_t r = WebConfig::lastReason();
            w["lastReason"]     = r;
            w["lastReasonName"] = WifiReason::name(r);
            if (r) w["lastReasonAgeS"] = WebConfig::lastReasonAgeMs() / 1000;
            else   w["lastReasonAgeS"] = nullptr;
            // The reason of the outage now going on, if any (0 while the
            // link is up) - lastReason above is kept across a recovery.
            w["outageReason"] = WebConfig::outageReason();
            // The scan before the fallback, at BOOT: never refreshed by the
            // background retries - nested and named so, or it reads like the
            // current picture next to a link that came up an hour later.
            const int8_t seen = webCfg.bootScanSeen();
            if (seen < 0) {
                w["bootScan"] = nullptr;
            } else {
                JsonObject s = w["bootScan"].to<JsonObject>();
                s["seen"] = seen > 0;
                if (seen > 0) {
                    s["channel"] = webCfg.bootScanChannel();
                    s["rssi"]    = webCfg.bootScanRssi();
                    s["auth"]    = authName(webCfg.bootScanAuth());
                }
            }
            if (webCfg.fallbackActive()) {
                w["retries"] = webCfg.fallbackRetries();
                // Deferred: a client is on the own hotspot, and the retry
                // waits for it to leave - no countdown to show then.
                w["retryDeferred"] = webCfg.fallbackDeferred();
                if (webCfg.fallbackDeferred()) w["nextRetryS"] = nullptr;
                else                           w["nextRetryS"] = webCfg.fallbackNextRetryS();
            }
            if (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA)
                w["apClients"] = WiFi.softAPgetStationNum();
        }
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // REST: GET screen catalog (all known screens with their German labels).
    // The WebUI merges this with display.screens (order + enabled) from /api/config.
    _server.on("/api/screens", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < dispMgr.screenTotal(); i++) {
            if (!dispMgr.screenPresent(i)) continue;   // skip inactive grid slots
            JsonObject s = arr.add<JsonObject>();
            s["id"]   = i;
            s["name"] = dispMgr.screenName(i);
            s["type"] = dispMgr.screenType(i);          // wind|speed|...|grid
        }
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // REST: GET detected tanks + battery banks (drives the config editors).
    // Every occupied slot, whatever its values: BusWatch blanks a slot whose
    // sender went quiet (values NaN, instance kept), and skipping NaN here made
    // that tank or bank vanish from the editors - a save from the Display tab
    // then wrote tankcfg/battcfg without it and dropped its name, capacity and
    // calibration. An unknown reading is left out, the page shows no number.
    _server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        {
            auto lk = data.lock();
            JsonArray jt = doc["tanks"].to<JsonArray>();
            for (int i = 0; i < data.tankCount; i++) {
                const TankInfo &t = data.tanks[i];
                if (t.fluidType == 0xFF) continue;   // empty slot
                JsonObject o = jt.add<JsonObject>();
                o["inst"] = t.instance; o["ft"] = t.fluidType;
                if (!isnan(t.level))    o["level"] = t.level;
                if (!isnan(t.capacity)) o["cap"] = t.capacity;
            }
            JsonArray jb = doc["batteries"].to<JsonArray>();
            for (int i = 0; i < data.battCount; i++) {
                const BatteryBank &b = data.batteries[i];
                if (b.instance == 0xFF) continue;    // empty slot
                JsonObject o = jb.add<JsonObject>();
                o["inst"] = b.instance;
                if (!isnan(b.voltage)) o["v"] = b.voltage;
                if (!isnan(b.soc))     o["soc"] = b.soc;
            }
        }
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // REST: POST import config
    _server.on("/api/import", HTTP_POST, [](AsyncWebServerRequest *req){},
        nullptr, handleImport);

    // REST: GET / POST polar data ({name, tws[], twa[], speed[][]})
    _server.on("/api/polar", HTTP_GET, handleGetPolar);
    _server.on("/api/polar", HTTP_POST, [](AsyncWebServerRequest *req){},
        nullptr, handlePostPolar);

    // REST: GET export config
    _server.on("/api/export", HTTP_GET, [](AsyncWebServerRequest *req) {
#if ESP_IDF_VERSION_MAJOR >= 5
        sendOwnedJson(req, appConfig.toJson());   // same lifecycle bug shield as /api/config
#else
        req->send(200, "application/json", appConfig.toJson());
#endif
    });

    // REST: N2K senders seen per data category (source address, message
    // count, seconds since last message) - feeds the WebUI's source picker.
    // Lock-free read of monotonic counters; a torn read is harmless here.
    _server.on("/api/sources", HTTP_GET, [](AsyncWebServerRequest *req) {
        String out; out.reserve(512);
        out += '{';
        for (int c = 0; c < N2K_SRC_CAT_N; c++) {
            if (c) out += ',';
            out += '"'; out += N2K_SRC_KEYS[c]; out += "\":[";
            bool first = true;
            for (int i = 0; i < N2K_SRC_SLOTS; i++) {
                const N2kSrcSeen &s = g_n2kSrcSeen[c][i];
                if (!s.count) continue;
                if (!first) out += ',';
                first = false;
                out += "{\"sa\":";  out += String((unsigned)s.sa);
                out += ",\"n\":";   out += String((unsigned long)s.count);
                out += ",\"age\":"; out += String((unsigned long)((millis() - s.lastMs) / 1000));
                out += '}';
            }
            out += ']';
        }
        out += '}';
        req->send(200, "application/json", out);
    });

    // REST: POST apply theme live (no reboot). Call after saving the theme.
    _server.on("/api/theme-apply", HTTP_POST, [](AsyncWebServerRequest *req) {
        dispMgr.requestThemeReload();
        req->send(200, "application/json", "{\"ok\":true}");
    });

#ifdef PANIC_PATH_TEST
    // Test build only (see panicTestTask() above).
    _server.on("/api/panic-test", HTTP_GET, [](AsyncWebServerRequest *req) {
        // A task of its own per entry ("both" has two); the loop's kinds are
        // a request the next loop() pass carries out.
        static const struct { const char *name; int what, core, prio; } kinds[] = {
            { "null0", PT_NULL, 0, 5 },  { "null1", PT_NULL, 1, 5 },
            { "abort0", PT_ABORT, 0, 5 }, { "abort1", PT_ABORT, 1, 5 },
            { "irq0", PT_IRQ, 0, 5 },     { "irq1", PT_IRQ, 1, 5 },
            { "both", PT_IRQ, 0, 5 },     { "both", PT_IRQ, 1, 5 },
            { "lock", PT_LOCK, 0, 5 },    { "spin0", PT_SPIN, 0, 6 },
            { "stall0", PT_STALL0, 1, 5 },
        };
        static const struct { const char *name; uint8_t req; } loopKinds[] = {
            { "loop", LoopWatchdog::T_LOOP },       { "lvgl", LoopWatchdog::T_LVGL },
            { "block25", LoopWatchdog::T_BLOCK25 }, { "block35", LoopWatchdog::T_BLOCK35 },
            { "rsthang", LoopWatchdog::T_RSTHANG }, { "lateabort", LoopWatchdog::T_LATEABORT },
        };
        JsonDocument doc;
        doc["reason"] = (int)esp_reset_reason();
        doc["rtc0"]   = (int)esp_rom_get_reset_reason(0);
        doc["rtc1"]   = (int)esp_rom_get_reset_reason(1);
        doc["upMs"]   = (unsigned long)millis();
        LoopWatchdog::fillInfo(doc["wdt"].to<JsonObject>());
        if (req->hasParam("kind")) {
            const String k = req->getParam("kind")->value();
            bool known = false;
            for (const auto &e : kinds)
                if (k == e.name) {
                    known = true;
                    xTaskCreatePinnedToCore(panicTestTask, "panictest", 3072, (void *)(intptr_t)e.what,
                                            e.prio, nullptr, e.core);
                }
            for (const auto &e : loopKinds)
                if (k == e.name) { known = true; LoopWatchdog::testRequest(e.req); }
            if (known) doc["kind"] = k;
            else       doc["kind"] = nullptr;
        }
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });
#endif

    // REST: POST restart device
    _server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *req) {
        // N2K-UPDATE.md 10.4 / 11.5: not during an update session (it would cut
        // the transfer) and not during the verify window after one (it would
        // roll the new firmware back). Allowed while a restart is PENDING -
        // that restart is exactly what activates the update.
        if (!UpdateGate::canRestart()) {
            char j[96];
            snprintf(j, sizeof(j), "{\"ok\":false,\"error\":\"update\",\"gate\":\"%s\",\"seconds\":%u}",
                     UpdateGate::describe(), (unsigned)UpdateGate::restartBlockedSeconds());
            req->send(409, "application/json", j);
            return;
        }
        req->send(200, "text/plain", "Restarting...");
        restartSoon("[web] restart requested");
    });

    // REST: update over WiFi (N2K-UPDATE.md 11.5).
    //   ?target=fw     the firmware, into the INACTIVE app slot (OtaWriter)
    //   ?target=webui  the web interface, /index.html.gz (WebUiWriter)
    // ?target=fs no longer replaces the file system: it overwrote the whole
    // LittleFS image - config.json included - and could not be rolled back.
    // It is kept only as another name for webui, for the old web interface.
    //
    // Both go through the same writers as an update over NMEA 2000: streamed
    // in 256-byte pieces through the internal bounce buffer, never buffered
    // whole (a 2.4 MB image fits nowhere on this device), the firmware checked
    // against this board's descriptor as soon as its first 0x160 bytes are
    // in flash, the web interface swapped into the page cache without a
    // restart. Arduino's Update.h is no longer used: it held a 4 KB DRAM
    // buffer and erased 64 KB blocks.
    //
    // One owner at a time (UpdateGate): 409 during an update over NMEA 2000,
    // during the verify window after an update, and while a restart is
    // pending - the next update slot would then be the slot set to boot next.
    // And during another upload over WiFi, gate "wifi" - unless that one has
    // sent nothing for 10 s: a new upload then takes its writer over, and the
    // old one, should its client come back, gets 409 gate "replaced"
    // (OtaUploadSlot.h). A web interface upload within 120 s of the last web
    // interface install gets 409 gate "grace": its work region is still the
    // retired cache a page may be reading (UpdRegions.h).
    //
    // Safety net: a firmware installed this way boots PENDING_VERIFY and is
    // confirmed by OtaGuard only once it works; otherwise the bootloader
    // brings the previous one back (main.cpp, OtaGuard.h). No confirmation on
    // the touchscreen for WiFi uploads (owner decision 15.2 #7).
    _server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            const OtaUpload *up = static_cast<const OtaUpload *>(req->_tempObject);
            char j[160];
            int code = 200;
            const bool ok = up && up->finished && up->reason == 0;
            if (ok) {
                snprintf(j, sizeof(j), "{\"ok\":true,\"restart\":%s,\"detail\":%u}",
                         up->target == OTA_TARGET_FW ? "true" : "false", (unsigned)up->detail);
            } else if (!up) {
                code = 400;
                snprintf(j, sizeof(j), "{\"ok\":false,\"reason\":\"NO_FILE\"}");
            } else if (up->reason == n2kupd::RSN_BUSY) {
                // "wifi": seconds until a new upload may take over, "replaced":
                // a newer upload took this one's writer over, "grace": seconds
                // left of the web UI work region's grace (OtaUploadSlot.h).
                code = 409;
                snprintf(j, sizeof(j), "{\"ok\":false,\"reason\":\"BUSY\",\"gate\":\"%s\",\"seconds\":%u}",
                         otaBusyGate(*up, UpdateGate::describe()),
                         (unsigned)otaBusySeconds(*up, s_otaSlot, millis()));
            } else {
                code = up->reason == n2kupd::RSN_FLASH_ERROR ||
                       up->reason == n2kupd::RSN_INTERNAL_ERROR ? 500 : 400;
                snprintf(j, sizeof(j), "{\"ok\":false,\"reason\":\"%s\",\"code\":%u}",
                         otaReasonName(up->reason), (unsigned)up->reason);
            }
            AsyncWebServerResponse *res = req->beginResponse(code, "application/json", j);
            res->addHeader("Connection", "close");
            req->send(res);
            // Same as /api/restart above: the restart comes from the main
            // loop, once the reply is out. Deliberately NOT dispMgr's reboot
            // screen, which would build LVGL objects from the async_tcp task.
            if (ok && up->target == OTA_TARGET_FW) restartSoon("[ota] firmware written");
        },
        [](AsyncWebServerRequest *req, const String &filename, size_t index,
           uint8_t *data, size_t len, bool final) {
            OtaUpload *up = static_cast<OtaUpload *>(req->_tempObject);
            OtaEnv env;
            if (index == 0) {
                if (up) { otaAbort(up); free(up); }      // a retried upload
                // malloc, not new: ~AsyncWebServerRequest() free()s the slot.
                up = static_cast<OtaUpload *>(malloc(sizeof(OtaUpload)));
                req->_tempObject = up;
                if (!up) return;
                memset(up, 0, sizeof(*up));
                // The server drops a connection after 3 s without data, and on a
                // weak link that ended every other update (5B at -79 dBm: 12 KB/s,
                // cut off after 13 to 92 s).
                req->client()->setRxTimeout(OTA_RX_TIMEOUT_S);
                const String t = req->hasParam("target") ? req->getParam("target")->value()
                                                         : String("fw");
                // "fs" is what the web interface of 1.2.0 and older sends. It
                // no longer replaces the file system: it goes to the web UI
                // writer, which takes only a gzip file - index.html.gz picked
                // in that old field installs, a littlefs.bin is refused. So a
                // board updated firmware-first is not left without a way to
                // get the matching web interface.
                up->target = (t == "webui" || t == "fs") ? OTA_TARGET_WEBUI
                           : t == "fw" ? OTA_TARGET_FW : 0;
                Serial.printf("[ota] upload '%s' -> %s\n", filename.c_str(), t.c_str());
                if (!up->target) {
                    up->reason = n2kupd::RSN_UNSUPPORTED_TARGET;   // anything but fw, webui and fs
                } else if (otaUploadStart(s_otaSlot, up, env)) {
                    Serial.printf("[ota] the previous upload sent nothing for %u s - "
                                  "its writer was taken over\n",
                                  (unsigned)(OTA_TAKEOVER_MS / 1000));
                }
                if (up->started) {
                    // A client that vanishes mid-upload must not leave the
                    // writer claimed (and a half-written slot open). Once a
                    // newer upload has taken the writer over, this aborts
                    // nothing (otaAbort).
                    req->onDisconnect([req]() {
                        OtaUpload *u = static_cast<OtaUpload *>(req->_tempObject);
                        if (u) otaAbort(u);
                    });
                } else {
                    Serial.printf("[ota] refused (reason 0x%02X, gate %s)\n",
                                  (unsigned)up->reason,
                                  otaBusyGate(*up, UpdateGate::describe()));
                }
            }
            switch (otaUploadPiece(s_otaSlot, up, env, data, len, final)) {
                case OTA_PIECE_REPLACED:
                    Serial.println("[ota] upload taken over by a newer one - rest dropped");
                    break;
                case OTA_PIECE_DONE:
                    Serial.printf("[ota] %s %s (%u bytes)\n",
                                  up->target == OTA_TARGET_FW ? "firmware" : "web interface",
                                  up->reason ? "refused" : "installed", (unsigned)(index + len));
                    break;
                default:
                    break;
            }
        });

    // REST: POST upload logo.bin (raw RGB565 with 4-byte header)
    // Body is streamed in chunks; we write directly to LittleFS.
    _server.on("/api/upload-logo", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            req->send(200, "application/json", "{\"ok\":true}");
        },
        [](AsyncWebServerRequest *req, const String &filename, size_t index,
           uint8_t *data, size_t len, bool final) {
            // ONE file handle per REQUEST, not one shared static. With a static
            // handle two overlapping uploads truncate and interleave into the
            // same file, and an ABORTED upload leaks the handle forever -
            // orphaned LittleFS handles inside async_tcp have permanently
            // deadlocked this whole server before (see the note in WebConfig.h).
            // The request's own destructor cannot close a File, so the handle
            // is closed on the final chunk AND on the disconnect callback.
            struct LogoUpload { File f; };
            LogoUpload *up = static_cast<LogoUpload *>(req->_tempObject);
            if (index == 0) {
                if (up) { up->f.close(); delete up; }          // retried upload
                up = new LogoUpload();
                req->_tempObject = up;
                // create=true: explicitly allow file creation (ESP32 default is false)
                up->f = LittleFS.open("/logo.bin", "w", true);
                if (!up->f) Serial.println("[ws] LittleFS.open(/logo.bin, w) FAILED");
                // Runs if the client vanishes mid-upload; without it the handle
                // would stay open until reboot.
                req->onDisconnect([req]() {
                    LogoUpload *u = static_cast<LogoUpload *>(req->_tempObject);
                    if (!u) return;
                    if (u->f) { u->f.close(); Serial.println("[ws] logo upload aborted - handle closed"); }
                    delete u;
                    req->_tempObject = nullptr;   // stop the default free() of a non-POD
                });
            }
            if (!up) return;                       // first chunk never arrived
            if (up->f) up->f.write(data, len);
            if (final) {
                if (up->f) up->f.close();
                delete up;
                req->_tempObject = nullptr;
            }
        });

    // REST: DELETE logo
    _server.on("/api/delete-logo", HTTP_DELETE, [](AsyncWebServerRequest *req) {
        LittleFS.remove("/logo.bin");
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // REST: GET logo info (exists + size)
    _server.on("/api/logo-info", HTTP_GET, [](AsyncWebServerRequest *req) {
        bool exists = LittleFS.exists("/logo.bin");
        size_t size = 0;
        if (exists) { File f = LittleFS.open("/logo.bin","r"); size = f.size(); f.close(); }
        String j = "{\"exists\":" + String(exists?"true":"false") +
                   ",\"size\":" + String(size) + "}";
        req->send(200, "application/json", j);
    });

    _server.onNotFound([](AsyncWebServerRequest *req){
        req->send(404, "text/plain", "Not found");
    });

    // Root: serve the UI from the PSRAM copy - NO filesystem I/O inside the
    // async_tcp task (see serveIndex in the header for the deadlock this
    // prevents). /index.html gets the same answer, explicitly and BEFORE the
    // static handler below: the plain file there is 166 KB of the release's
    // original text and would otherwise be served stale after the web
    // interface was replaced over NMEA 2000 or WiFi (N2K-UPDATE.md 9.4.6).
    _server.on("/", HTTP_GET, serveIndex);
    _server.on("/index.html", HTTP_GET, serveIndex);

    // Static files LAST – catch-all; must come after all /api/* routes.
    // (Only reached for other paths: logo, polar downloads.)
    _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
}

// The page from its PSRAM region (UpdRegions). The descriptor is read through
// one pointer, so buffer, length and gzip flag always belong together, even
// while a new web interface is being published from the N2K task. A response
// keeps reading the buffer after this returns; the region it points into is
// therefore not reused for 120 s after a swap (9.1).
void WebConfig::serveIndex(AsyncWebServerRequest *req) {
    const UpdRegions::Cache *c = UpdRegions::liveCache();
    if (c && c->buf && c->len) {
        AsyncWebServerResponse *res = req->beginResponse(200, "text/html", c->buf, c->len);
        // Without this header the browser renders the raw gzip bytes.
        if (c->gzip) res->addHeader("Content-Encoding", "gzip");
        req->send(res);
        return;
    }
    // No cache (too large, or unreadable at boot): the compressed file from
    // flash, and only when that is missing as well the plain one - the
    // last-resort fallback every littlefs.bin ships (9.4.6).
    if (LittleFS.exists("/index.html.gz")) {
        AsyncWebServerResponse *res = req->beginResponse(LittleFS, "/index.html.gz", "text/html");
        res->addHeader("Content-Encoding", "gzip");
        req->send(res);
    } else {
        req->send(LittleFS, "/index.html", "text/html");
    }
}

#if ESP_IDF_VERSION_MAJOR >= 5
// Serve a large JSON from a response-owned immutable PSRAM buffer.
//
// Why not req->send(200, type, String): with two responses in flight (the UI
// polls every second) the 7B's AsyncWebServer served the FIRST response's
// remaining bytes out of the SECOND response's content - reproduced as
// /api/config breaking at the same TCP-window boundary on a FRESH connection
// while a browser was polling. Smells like a String-buffer lifecycle bug in
// the response object. This path hands the server a filler callback over a
// buffer that only the callback's captured shared_ptr owns - freed when the
// response is destroyed, immune to whatever the String path does.
static void sendOwnedJson(AsyncWebServerRequest *req, const String &json) {
    const size_t len = json.length();
    char *raw = (char *)heap_caps_malloc(len ? len : 1,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) {                       // PSRAM exhausted - degrade, don't die
        req->send(200, "application/json", json);
        return;
    }
    memcpy(raw, json.c_str(), len);
    std::shared_ptr<char> buf(raw, heap_caps_free);
    AsyncWebServerResponse *res = req->beginResponse(
        "application/json", len,
        [buf, len](uint8_t *dst, size_t maxLen, size_t index) -> size_t {
            const size_t n = (index + maxLen > len) ? (len - index) : maxLen;
            memcpy(dst, buf.get() + index, n);
            return n;
        });
    req->send(res);
}
#endif

void WebConfig::handleGetConfig(AsyncWebServerRequest *req) {
#if ESP_IDF_VERSION_MAJOR >= 5
    sendOwnedJson(req, appConfig.toJson());
#else
    req->send(200, "application/json", appConfig.toJson());
#endif
}

// ---- POST body accumulation --------------------------------------------------
//
// Bodies arrive in TCP-sized chunks and must be buffered until the last one.
// The buffer has to belong to the REQUEST, not to the handler. This used to be
// one `static String` per handler, which every concurrent POST appended into -
// and /api/config and /api/import even shared a single one, because
// handleImport() delegates to handlePostConfig(). Overlapping requests are the
// normal case here, not a corner case: the UI polls /api/data every second
// while a save is in flight and a browser re-sends a save it thinks was lost.
// The chunks then interleaved and the config was parsed from a mixture of two
// bodies - silent corruption, no error anywhere.
//
// request->_tempObject is the per-request slot the server itself uses for this
// (AsyncJson.cpp does exactly the same) and ~AsyncWebServerRequest() free()s it.
// That is what makes a cancelled upload leak-free without a cleanup hook of our
// own: on ESP-IDF free() IS heap_caps_free(), which finds the owning heap by
// address, so it releases the PSRAM block just as well as a DRAM one.
// AsyncCallbackWebHandler never touches the slot - only the static-file handler
// does, and that one claims GET requests only, so it can never see these routes.

// A bogus Content-Length must not be able to claim the heap: the old code
// reserve()d whatever the header claimed. 32 KB is ~2.5x the largest body that
// really occurs (a full config export on the 600 grid).
static constexpr size_t POST_BODY_MAX = 32 * 1024;

struct PostBody {
    size_t cap;      // payload bytes this block can hold
    size_t len;      // highest offset written so far
    bool   fits;     // cleared when a chunk did not fit - a torn body is never parsed
    char   data[1];  // cap + 1 bytes follow, NUL-terminated once complete
};

enum class BodyState : uint8_t { Pending, Complete, TooLarge, NoMemory };

static void postBodyFree(AsyncWebServerRequest *req) {
    if (!req->_tempObject) return;
    free(req->_tempObject);        // dispatches to the owning heap - see above
    req->_tempObject = nullptr;    // or the destructor would free it a second time
}

// Append one body chunk. Returns Pending until the chunk that completes the
// body; on Complete *out holds the whole body, valid until postBodyFree().
// The failure states are reported on that same completing chunk and nowhere
// else, so every request still produces exactly one response.
static BodyState postBodyCollect(AsyncWebServerRequest *req, const uint8_t *data,
                                 size_t len, size_t index, size_t total,
                                 const PostBody **out) {
    if (index == 0 && total <= POST_BODY_MAX) {
        postBodyFree(req);   // nothing may own the slot yet; never allocate over it
        // The 4" deliberately stays on DRAM: its IDF 4.4 has the OPI-PSRAM
        // coherency bug, so nothing allocates SPIRAM at runtime there. Same
        // split as sendOwnedJson() above and Config.cpp's JSON allocator.
#if ESP_IDF_VERSION_MAJOR >= 5
        void *blk = heap_caps_malloc(sizeof(PostBody) + total,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        void *blk = malloc(sizeof(PostBody) + total);
#endif
        if (blk) {
            PostBody *b = (PostBody *)blk;
            b->cap = total; b->len = 0; b->fits = true; b->data[0] = '\0';
            req->_tempObject = blk;
        }
    }

    PostBody *b = (PostBody *)req->_tempObject;
    if (b) {
        // Bound-checked even though the server clamps len to the remaining
        // Content-Length: a chunked body reports total = 0, so cap is 0 and an
        // unchecked memcpy would run straight off the block.
        if (index > b->cap || len > b->cap - index) {
            b->fits = false;
        } else {
            memcpy(b->data + index, data, len);
            if (index + len > b->len) b->len = index + len;
        }
    }

    if (index + len != total) return BodyState::Pending;
    if (!b) return (total > POST_BODY_MAX) ? BodyState::TooLarge : BodyState::NoMemory;
    if (!b->fits) return BodyState::TooLarge;
    b->data[b->len] = '\0';
    *out = b;
    return BodyState::Complete;
}

// Answer a rejected body. Frees first, so nothing is held while the reply is
// built. The WebUI only looks at "ok", so the text is for the serial log/curl.
static void postBodyReject(AsyncWebServerRequest *req, BodyState st, size_t total) {
    postBodyFree(req);
    if (st == BodyState::TooLarge) {
        Serial.printf("[web] POST body rejected: %u bytes (max %u)\n",
                      (unsigned)total, (unsigned)POST_BODY_MAX);
        req->send(413, "application/json", "{\"error\":\"Body too large\"}");
    } else {
        Serial.printf("[web] POST body buffer alloc failed (%u bytes)\n",
                      (unsigned)total);
        req->send(507, "application/json", "{\"error\":\"Out of memory\"}");
    }
}

void WebConfig::handlePostConfig(AsyncWebServerRequest *req, uint8_t *body, size_t len, size_t index, size_t total) {
    const PostBody *b = nullptr;
    const BodyState st = postBodyCollect(req, body, len, index, total, &b);
    if (st == BodyState::Pending) return;
    if (st != BodyState::Complete) { postBodyReject(req, st, total); return; }

    // Heap is the shared budget of lwIP + ArduinoJson on this board: log it
    // so a tight save is visible in the serial log instead of showing up
    // only as a mysteriously dropped connection.
    Serial.printf("[cfg] POST %u bytes, free heap %u\n",
                  (unsigned)total, (unsigned)ESP.getFreeHeap());

    // fromJson() takes a String, so one copy is unavoidable - but the block
    // goes back immediately afterwards, so the peak is the same as before and
    // the body no longer occupies DRAM for the whole duration of the upload.
    String json((const char *)b->data, b->len);
    postBodyFree(req);

    const Lang langBefore = i18nLang();
    const uint8_t iconSetBefore = appConfig.cfg.iconSet;   // OpenBridge icon trial
    const bool demoBefore = appConfig.cfg.demoMode;
    // The web form is the fifth place the anchor state can change (the other
    // four are the buttons on the anchor screen). Compared rather than always
    // stamped: every save from every tab comes through here, and stamping a
    // revision on a brightness change would let this device win an arbitration
    // it has no business winning.
    const bool     ancAlarmBefore  = appConfig.cfg.anchorAlarmOn;
    const float    ancRadiusBefore = appConfig.cfg.anchorRadius;
    const bool     ancSetBefore    = appConfig.cfg.anchorSet;
    const float    ancLatBefore    = appConfig.cfg.anchorLat;
    const float    ancLonBefore    = appConfig.cfg.anchorLon;
    const uint32_t ancSetUtcBefore = appConfig.cfg.anchorSetUtc;
    if (!appConfig.fromJson(json)) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    json = String();          // release the body BEFORE save() builds its own
                              // JsonDocument + output String
    // Position and set time are compared too: /api/import comes through this
    // same handler and can move the anchor without touching set/radius/alarm.
    // Unstamped, that position would never be broadcast AND could never be
    // corrected by the peer - its heartbeat carries the revision we already
    // hold - so the two devices would watch different spots indefinitely, each
    // convinced it was in sync. NaN != NaN, hence the isnan pair.
    auto latLonMoved = [](float a, float b) {
        if (isnan(a) && isnan(b)) return false;
        if (isnan(a) != isnan(b)) return true;
        return a != b;
    };
    if (appConfig.cfg.anchorAlarmOn != ancAlarmBefore  ||
        appConfig.cfg.anchorSet     != ancSetBefore    ||
        appConfig.cfg.anchorRadius  != ancRadiusBefore ||
        appConfig.cfg.anchorSetUtc  != ancSetUtcBefore ||
        latLonMoved(appConfig.cfg.anchorLat, ancLatBefore) ||
        latLonMoved(appConfig.cfg.anchorLon, ancLonBefore)) {
        AnchorSyncNode::noteLocalChange();
    }
    appConfig.save();
    // Apply hardware settings immediately (no restart needed)
    setBrightness(appConfig.cfg.brightness);
    // Rebuild screen order/visibility live on the next display tick.
    dispMgr.requestApplyScreenConfig();
    // Screen labels are set when a screen is built, so a language change
    // only shows up after a rebuild — the same path the theme uses.
    if (i18nLang() != langBefore) dispMgr.requestThemeReload();
    // OpenBridge icon trial: same live rebuild for a new icon set (see Icons.h).
    else if (appConfig.cfg.iconSet != iconSetBefore) dispMgr.requestThemeReload();
    // Leaving demo mode is a no-op unless something is actually reading the
    // bus. setup() creates EITHER the demo task OR the NMEA 2000 task, and
    // only at boot, so a device that booted in demo mode has no bus reader:
    // clearing the flag here would stop the demo data and put nothing in its
    // place. The result looks like broken hardware - every value null,
    // n2kRx stuck at 0, /api/sources empty - and it cost an afternoon of
    // hunting a perfectly healthy CAN bus. Restart instead, and say so on the
    // display. The answer is sent first so the browser is not left hanging.
    const bool needBusRestart =
        demoBefore && !appConfig.cfg.demoMode && !g_n2kTaskRunning;
    req->send(200, "application/json", "{\"ok\":true}");
    if (needBusRestart) dispMgr.requestReboot(T(STR_CFG_RB_DEMO_OFF));
}

void WebConfig::handleGetData(AsyncWebServerRequest *req) {
    req->send(200, "application/json", getDataJson());
}

void WebConfig::handleGetPolar(AsyncWebServerRequest *req) {
    // Serve the stored file if present (preserves exact formatting), else the
    // in-RAM table serialised.
    if (LittleFS.exists(appConfig.cfg.polarFile)) {
        req->send(LittleFS, appConfig.cfg.polarFile, "application/json");
    } else {
        req->send(200, "application/json", gPolar().toJson());
    }
}

void WebConfig::handlePostPolar(AsyncWebServerRequest *req, uint8_t *body, size_t len, size_t index, size_t total) {
    const PostBody *b = nullptr;
    const BodyState st = postBodyCollect(req, body, len, index, total, &b);
    if (st == BodyState::Pending) return;
    if (st != BodyState::Complete) { postBodyReject(req, st, total); return; }

    String json((const char *)b->data, b->len);
    postBodyFree(req);   // heap back to lwIP before the file write

    // Validate the incoming JSON without allocating a 2 KB PolarTable, then
    // write the raw body straight to the file and reload on the display tick.
    if (!PolarTable::validateJson(json)) {
        req->send(400, "application/json", "{\"error\":\"Invalid polar data\"}");
        return;
    }
    File f = LittleFS.open(appConfig.cfg.polarFile, "w", true);
    if (!f) {
        req->send(500, "application/json", "{\"error\":\"write failed\"}");
        return;
    }
    f.print(json);
    f.close();
    dispMgr.requestPolarReload();
    req->send(200, "application/json", "{\"ok\":true}");
}

void WebConfig::handleImport(AsyncWebServerRequest *req, uint8_t *body, size_t len, size_t index, size_t total) {
    handlePostConfig(req, body, len, index, total);  // same logic
}

String WebConfig::getDataJson() {
    auto lk = data.lock();
    JsonDocument doc;
    auto add = [&](const char *k, float v) {
        if (!isnan(v)) doc[k] = round(v * 100) / 100.0;
        else doc[k] = nullptr;
    };
    add("sog", data.sog);
    add("cog", data.cog);
    add("hdg", data.hdg);
    add("stw", data.stw);
    add("awa", data.awa);
    add("aws", data.aws);
    add("twa", data.twa);
    add("tws", data.tws);
    add("twd", data.twd);
    add("depth", data.depth);
    add("rpm", data.rpm);
    add("oilPressure", data.oilPressure);
    add("coolantTemp", data.coolantTemp);
    add("rudder", data.rudderAngle);
    add("battV", data.batteryVoltage);
    // Autopilot. apEngaged and apTarget (heading-to-steer, PGN 127237) keep
    // their old meaning; apEngaged is now false while the mode is unknown.
    // The rest is the pilot computer's own state (ApPipeline): apMode is
    // "unknown" with no fresh status - never a STANDBY nobody reported.
    doc["apEngaged"] = data.apEngaged;
    add("apTarget", data.apTargetHeading);
    doc["apMode"] = ApDecode::modeKey((ApDecode::Mode)data.apMode);
    switch (data.apModeSource) {
        case ApDecode::SRC_MODE_REPORT: doc["apModeSrc"] = "65305"; break;
        case ApDecode::SRC_STATE:       doc["apModeSrc"] = "65340"; break;
        case ApDecode::SRC_DEMO:        doc["apModeSrc"] = "demo";  break;
        default:                        doc["apModeSrc"] = nullptr; break;
    }
    if (data.apPilotSa == 0xFF) doc["apSa"] = nullptr;
    else doc["apSa"] = data.apPilotSa;
    switch (data.apStatus) {
        case ApDecode::STATUS_MANUAL:    doc["apStatus"] = "manual";    break;
        case ApDecode::STATUS_AUTOMATIC: doc["apStatus"] = "automatic"; break;
        default:                         doc["apStatus"] = nullptr;     break;
    }
    add("apSetpoint", data.apSetpoint);
    switch (data.apSetpointKind) {
        case ApDecode::SP_HEADING: doc["apSetpointKind"] = "heading"; break;
        case ApDecode::SP_WIND:    doc["apSetpointKind"] = "wind";    break;
        default:                   doc["apSetpointKind"] = nullptr;   break;
    }
    doc["aisCount"] = data.aisWithPosition();
    // The bus as a whole, the answer the panel's warning strip gives (see
    // BusWatch.h): "silent" / "never" mean the nulls above are a quiet bus,
    // not missing sensors, and the Live tab says so instead of a wall of
    // dashes. "demo" while demo data feeds the model, because BusWatch watches
    // the demo feed as well and would answer "ok" for it. busQuietS: whole
    // seconds since the last data value, only while Silent - Never has no
    // last value, Ok and Starting have no gap to report. From BusWatch's own
    // clock, not from the newest stamp: going silent blanks the values AND
    // their stamps, so there is no stamp left to measure from.
    const bool demo = demoFeedsModel();
    const BusWatch::State bus = BusWatch::state();
    doc["bus"] = demo ? "demo" : BusWatch::stateKey(bus);
    if (!demo && bus == BusWatch::State::Silent)
        doc["busQuietS"] = BusWatch::quietMs(millis()) / 1000;
    else
        doc["busQuietS"] = nullptr;
    String out; serializeJson(doc, out);
    return out;
}
