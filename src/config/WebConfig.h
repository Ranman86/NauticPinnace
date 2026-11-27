#pragma once
#include <ESPAsyncWebServer.h>
#include "WifiStatus.h"

// ============================================================
// WebConfig – async web server serving the configuration UI
// and a REST API for reading / writing settings.
// ============================================================
class WebConfig {
public:
    // The WiFi start comes in two halves, with the boot screen between them.
    //
    // start() brings the radio up (esp_wifi_init/start via WiFi.mode()) and,
    // in hotspot mode, the hotspot itself. It MUST run before the first LVGL
    // object exists - see the heap-race note at its call site in setup().
    //
    // connect() does the waiting: in station mode up to three join attempts,
    // then one scan for the network and, failing that, the own hotspot as a
    // fallback. `progress` is called about every 50 ms meanwhile, so the boot
    // screen can say what is going on - this used to be up to ~20 s of black
    // panel, which people took for a dead or hung device. Then the routes and
    // the web server. In hotspot mode there is nothing to wait for.
    void start(bool apMode, const char *ssid, const char *password);
    void connect(WifiProgressFn progress);

    // MUST be called periodically from the main loop. The async server itself
    // needs no servicing, but the STA link does: nothing else notices when the
    // association is lost. Without this the device sat at IP 0.0.0.0 forever —
    // reachable again only after a reboot — because setAutoReconnect(true) is
    // not a supervisor and gives up silently.
    void loop();

    // Carries out a restart asked for over the web (/api/restart, a firmware
    // upload) about a second later - never in the handler, whose reply would
    // otherwise never leave. Call on every main-loop pass, WiFi on or off: a
    // saved config may have switched WiFi off just before that restart.
    static void restartIfDue();

    // True while the STA link is up AND a DHCP lease is held. "Associated but
    // 0.0.0.0" is the failure this whole supervisor exists for, so both halves
    // are checked.
    static bool staLinkUp();

    // ---- diagnosis (settings WiFi line, /api/info) ----
    // The last reason the station was disconnected for, as the driver
    // reported it (wifi_err_reason_t; WifiStatus.h has the words), our own
    // disconnects excluded. 0 = none reported since boot.
    static uint8_t  lastReason();
    static uint32_t lastReasonAgeMs();     // since it was reported
    // The same, but only a reason of the CURRENT outage: cleared whenever the
    // link comes up (boot join, background join, supervisor recovery). This
    // is what the screens show - lastReason() alone kept, for instance, the
    // 201 of a failed first boot attempt after the second one had joined.
    static uint8_t  outageReason();
    // The fallback hotspot is up and the configured network is retried in
    // the background (see fallbackLoop()).
    bool    fallbackActive() const { return _fbActive; }
    // The one scan before the fallback, at boot - never refreshed by the
    // background retries, hence the name: 1 seen, 0 not seen, -1 no scan.
    int8_t  bootScanSeen()    const { return _scanSeen; }
    int8_t  bootScanRssi()    const { return _scanRssi; }
    uint8_t bootScanChannel() const { return _scanCh; }
    uint8_t bootScanAuth()    const { return _scanAuth; }
    uint16_t fallbackRetries() const { return _fbTries; }
    // A retry is due but waits, because a client is on the own hotspot.
    bool    fallbackDeferred() const { return _fbActive && _fbDeferLog; }
    // Seconds to the next background attempt; 0 while one runs, while one is
    // deferred (fallbackDeferred()), or when none is due.
    uint32_t fallbackNextRetryS() const;

private:
    // ---- the configured network (connect(), fallback retries) ----
    const char *_ssid      = "";      // point into appConfig.cfg, stable for the session
    const char *_pass      = "";
    bool        _staWanted = false;   // start() chose station mode

    // One join attempt of the boot sequence: WiFi.begin() and up to 6 s of
    // waiting, calling `progress` meanwhile. True when the station holds a lease.
    bool attemptJoin(uint8_t attempt, uint8_t attempts, WifiProgressFn progress);
    // The scan before the fallback; logs channel/RSSI/auth or "not seen".
    void scanTarget(WifiProgressFn progress);
    void startFallback();

    // ---- fallback hotspot with background retries ----
    // Until now a failed join was final: the device sat in its own hotspot
    // until someone restarted it. On a boat that is exactly the common case -
    // the display and the Pi hotspot get power at the same moment, and the
    // Pi needs longer than the ~20 s the display tried for. So the fallback
    // runs AP+STA and keeps trying, with a backoff, but only while nobody is
    // on the own hotspot: a scan or a join moves the hotspot's channel and
    // would throw that phone off.
    bool     _fbActive   = false;
    bool     _fbTrying   = false;     // an attempt is running
    bool     _fbDeferLog = false;     // a retry waits for a free hotspot (logged once)
    uint16_t _fbTries    = 0;
    uint32_t _fbBackoff  = 0;         // current backoff, 30 s -> 60 s -> 120 s cap
    uint32_t _fbNextTry  = 0;         // millis of the next attempt
    uint32_t _fbTryStart = 0;         // millis the running attempt began
    uint32_t _fbSince    = 0;         // millis the fallback started
    void     fallbackLoop();
    void     fallbackJoined();
    void     fallbackArm(uint32_t now);   // retry bookkeeping for a fresh fallback
    // Joined while a phone was on the own hotspot: the hotspot stays until
    // that phone leaves, then goes (checked by the link supervisor). Should
    // the network go again before that, it is the fallback again - see loop().
    bool     _apLinger   = false;
    void     apOff(const char *why);

    // ---- result of the scan before the fallback ----
    int8_t   _scanSeen   = -1;
    int8_t   _scanRssi   = 0;
    uint8_t  _scanCh     = 0;
    uint8_t  _scanAuth   = 0;

    AsyncWebServer  _server{80};
    // (The unused SSE endpoint /events was removed — the UI polls /api/data.)

    // The UI page is served from a PSRAM copy, not from LittleFS. Streaming it
    // from LittleFS ran file I/O inside the async_tcp task; a handful of
    // hard-dropped downloads (browser window closed mid-transfer) left
    // orphaned file handles there and deadlocked the whole server -
    // permanently, with a perfectly healthy heap. RAM responses cannot jam.
    // The copy lives in one of the two update regions (UpdRegions, loaded at
    // boot) so that a new web interface - over NMEA 2000 or WiFi - can replace
    // it without a restart and without a runtime PSRAM allocation.
    static void serveIndex(AsyncWebServerRequest *req);

    // ---- STA link supervision (see loop()) ----
    bool     _staMode      = false;   // supervise only when we joined a network
    uint32_t _lastCheck    = 0;       // millis of the last link check
    uint32_t _downSince    = 0;       // millis when the link was first seen down
    uint32_t _nextRetry    = 0;       // millis of the next reconnect attempt
    uint32_t _retryDelay   = 0;       // current backoff
    uint16_t _reconnects   = 0;       // how often we had to step in (logged)
    bool     _wasUp        = false;   // for edge-triggered logging

    // ---- half-dead link detection (gateway probe) ----
    // WiFi.status() can stay WL_CONNECTED while the data path is gone: the
    // radio still hears beacons (RSSI updates), but ARP/ping/TCP all die.
    // Measured twice on the 7B - once after a crash-reboot, once simply under
    // web-traffic load. Only an end-to-end probe catches this state, so the
    // supervisor pings the gateway and forces a re-association after three
    // consecutive misses.
    uint32_t _lastProbe    = 0;       // millis of the last gateway ping
    uint8_t  _probeMisses  = 0;       // consecutive failed probes
    void     probeGateway();          // sends one ping, read at the next probe
    void     forceReassoc(const char *why);

    void setupRoutes();

    static void handleGetConfig(AsyncWebServerRequest *req);
    static void handlePostConfig(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total);
    static void handleGetData(AsyncWebServerRequest *req);
    static void handleImport(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total);
    static void handleGetPolar(AsyncWebServerRequest *req);
    static void handlePostPolar(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total);
    static String getDataJson();
};

extern WebConfig webCfg;
