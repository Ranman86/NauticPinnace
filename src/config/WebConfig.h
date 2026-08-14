#pragma once
#include <ESPAsyncWebServer.h>

// ============================================================
// WebConfig – async web server serving the configuration UI
// and a REST API for reading / writing settings.
// ============================================================
class WebConfig {
public:
    void begin(bool apMode, const char *ssid, const char *password);

    // MUST be called periodically from the main loop. The async server itself
    // needs no servicing, but the STA link does: nothing else notices when the
    // association is lost. Without this the device sat at IP 0.0.0.0 forever —
    // reachable again only after a reboot — because setAutoReconnect(true) is
    // not a supervisor and gives up silently.
    void loop();

    // True while the STA link is up AND a DHCP lease is held. "Associated but
    // 0.0.0.0" is the failure this whole supervisor exists for, so both halves
    // are checked.
    static bool staLinkUp();

private:
    AsyncWebServer  _server{80};
    // (The unused SSE endpoint /events was removed — the UI polls /api/data.)

    // The UI page, loaded into PSRAM once at startup and served from there.
    // Streaming it from LittleFS ran file I/O inside the async_tcp task; a
    // handful of hard-dropped downloads (browser window closed mid-transfer)
    // left orphaned file handles there and deadlocked the whole server —
    // permanently, with a perfectly healthy heap. RAM responses cannot jam.
    uint8_t *_indexBuf = nullptr;
    size_t   _indexLen = 0;
    bool loadIndexToPsram();

    // ---- STA link supervision (see loop()) ----
    bool     _staMode      = false;   // supervise only when we joined a network
    uint32_t _lastCheck    = 0;       // millis of the last link check
    uint32_t _downSince    = 0;       // millis when the link was first seen down
    uint32_t _nextRetry    = 0;       // millis of the next reconnect attempt
    uint32_t _retryDelay   = 0;       // current backoff
    uint16_t _reconnects   = 0;       // how often we had to step in (logged)
    bool     _wasUp        = false;   // for edge-triggered logging

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
