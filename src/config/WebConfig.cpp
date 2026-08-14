#include "WebConfig.h"
#include "Config.h"
#include "../i18n/I18n.h"
#include "../nmea/DataModel.h"
#include "../DisplaySetup.h"
#include "../display/DisplayManager.h"
#include "../PolarTable.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "../WifiNaming.h"

void WebConfig::begin(bool apMode, const char *ssid, const char *password) {
    // ── WiFi hygiene (fixes the recurring AUTH_FAIL-on-boot + HTTP stalls) ──
    // persistent(false): don't store creds in NVS. The Arduino core otherwise
    //   auto-connects from stale NVS on boot, racing our begin() and emitting a
    //   spurious "Reason: 202 - AUTH_FAIL" before the real connect succeeds.
    // setSleep(false): disable WiFi modem power-save. With it on (the default in
    //   STA mode) the radio naps between DTIM beacons; on a busy/marginal link
    //   that adds latency and drops TCP segments, which is what made large HTTP
    //   responses (the UI page) stall and the async server appear "wedged".
    WiFi.persistent(false);

    if (apMode || strlen(ssid) == 0) {
        WiFi.mode(WIFI_AP);
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
    } else {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        WiFi.disconnect();          // clear any stale association from a prior boot
        delay(100);

        // Up to 3 attempts × ~6 s. A single transient AUTH_FAIL no longer matters.
        bool ok = false;
        for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
            Serial.printf("[wifi] connect attempt %d to '%s'...\n", attempt, ssid);
            WiFi.begin(ssid, password);
            uint32_t t = millis();
            while (millis() - t < 6000) {
                if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
                delay(150);
            }
            if (!ok) { WiFi.disconnect(); delay(250); }
        }

        if (ok) {
            WiFi.setSleep(false);   // re-assert after association
            Serial.printf("WiFi connected. IP: %s  RSSI: %d dBm  ch: %d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
            _staMode = true;        // arm the link supervision in loop()
            _wasUp   = staLinkUp();
        } else {
            WiFi.mode(WIFI_AP);
            WiFi.setSleep(false);
            String apS = wifiApSsid();
            String apP = wifiApPassword();
            WiFi.softAP(apS.c_str(), apP.c_str());
            Serial.printf("WiFi failed, started AP: %s (Passwort nur am Display/QR)\n",
                          apS.c_str());
        }
    }
    loadIndexToPsram();
    setupRoutes();
    _server.begin();
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
    if (!_staMode) return;                    // AP mode has no association to lose
    const uint32_t now = millis();
    if (now - _lastCheck < 2000) return;      // cheap: check every 2 s
    _lastCheck = now;

    if (staLinkUp()) {
        if (!_wasUp) {                        // edge: report recovery once
            Serial.printf("[wifi] link up again: IP %s  RSSI %d dBm  ch %d"
                          "  (reconnects so far: %u)\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                          WiFi.channel(), (unsigned)_reconnects);
            _wasUp = true;
        }
        _downSince  = 0;
        _retryDelay = 0;
        _nextRetry  = 0;
        return;
    }

    // ---- link is down ----
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
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(appConfig.cfg.wifiSsid, appConfig.cfg.wifiPassword);

    // Backoff 3 s → 60 s so a genuinely absent network does not spam the log
    // or hog the radio, while a brief AP hiccup is caught almost immediately.
    _retryDelay = (_retryDelay < 60000) ? (_retryDelay * 2) : 60000;
    if (_retryDelay > 60000) _retryDelay = 60000;
    _nextRetry  = now + _retryDelay;
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
    _server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        {
            auto lk = data.lock();
            JsonArray jt = doc["tanks"].to<JsonArray>();
            for (int i = 0; i < data.tankCount; i++) {
                const TankInfo &t = data.tanks[i];
                if (isnan(t.level)) continue;
                JsonObject o = jt.add<JsonObject>();
                o["inst"] = t.instance; o["ft"] = t.fluidType; o["level"] = t.level;
                if (!isnan(t.capacity)) o["cap"] = t.capacity;
            }
            JsonArray jb = doc["batteries"].to<JsonArray>();
            for (int i = 0; i < data.battCount; i++) {
                const BatteryBank &b = data.batteries[i];
                if (isnan(b.voltage) && isnan(b.soc)) continue;
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
        req->send(200, "application/json", appConfig.toJson());
    });

    // REST: POST apply theme live (no reboot). Call after saving the theme.
    _server.on("/api/theme-apply", HTTP_POST, [](AsyncWebServerRequest *req) {
        dispMgr.requestThemeReload();
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // REST: POST restart device
    _server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", "Restarting...");
        delay(500);
        ESP.restart();
    });

    // REST: POST upload logo.bin (raw RGB565 with 4-byte header)
    // Body is streamed in chunks; we write directly to LittleFS.
    _server.on("/api/upload-logo", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            req->send(200, "application/json", "{\"ok\":true}");
        },
        [](AsyncWebServerRequest *req, const String &filename, size_t index,
           uint8_t *data, size_t len, bool final) {
            static File uploadFile;
            if (index == 0) {
                // create=true: explicitly allow file creation (ESP32 default is false)
                uploadFile = LittleFS.open("/logo.bin", "w", true);
                if (!uploadFile) Serial.println("[ws] LittleFS.open(/logo.bin, w) FAILED");
            }
            if (uploadFile) uploadFile.write(data, len);
            if (final && uploadFile) uploadFile.close();
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

    // Root: serve the UI from the PSRAM copy — NO filesystem I/O inside the
    // async_tcp task (see loadIndexToPsram in the header for the deadlock this
    // prevents). Falls back to LittleFS streaming only if the load failed.
    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *req){
        if (_indexBuf && _indexLen) {
            req->send(200, "text/html", _indexBuf, _indexLen);
        } else {
            req->send(LittleFS, "/index.html", "text/html");
        }
    });

    // Static files LAST – catch-all; must come after all /api/* routes.
    // (Only reached for paths other than "/": logo, polar downloads.)
    _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
}

// Read /index.html into PSRAM once. ~112 KB out of >4.5 MB free — cheap
// insurance compared to a wedged web server on the boat.
bool WebConfig::loadIndexToPsram() {
    File f = LittleFS.open("/index.html", "r");
    if (!f) return false;
    const size_t len = f.size();
    uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { f.close(); return false; }
    const size_t got = f.read(buf, len);
    f.close();
    if (got != len) { heap_caps_free(buf); return false; }
    _indexBuf = buf;
    _indexLen = len;
    Serial.printf("[web] index.html cached in PSRAM (%u bytes)\n", (unsigned)len);
    return true;
}

void WebConfig::handleGetConfig(AsyncWebServerRequest *req) {
    req->send(200, "application/json", appConfig.toJson());
}

void WebConfig::handlePostConfig(AsyncWebServerRequest *req, uint8_t *body, size_t len, size_t index, size_t total) {
    static String buf;
    if (index == 0) {
        buf = "";
        buf.reserve(total + 1);   // one allocation instead of realloc-and-copy growth
    }
    buf += String((char *)body, len);
    if (index + len == total) {
        // Heap is the shared budget of lwIP + ArduinoJson on this board: log it
        // so a tight save is visible in the serial log instead of showing up
        // only as a mysteriously dropped connection.
        Serial.printf("[cfg] POST %u bytes, free heap %u\n",
                      (unsigned)total, (unsigned)ESP.getFreeHeap());
        const Lang langBefore = i18nLang();
        if (appConfig.fromJson(buf)) {
            buf = String();       // release the body buffer BEFORE save() builds
                                  // its own JsonDocument + output String
            appConfig.save();
            // Apply hardware settings immediately (no restart needed)
            setBrightness(appConfig.cfg.brightness);
            // Rebuild screen order/visibility live on the next display tick.
            dispMgr.requestApplyScreenConfig();
            // Screen labels are set when a screen is built, so a language change
            // only shows up after a rebuild — the same path the theme uses.
            if (i18nLang() != langBefore) dispMgr.requestThemeReload();
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            buf = String();
            req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        }
    }
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
    static String buf;
    if (index == 0) { buf = ""; buf.reserve(total + 1); }
    buf += String((char *)body, len);
    if (index + len == total) {
        // Validate the incoming JSON without allocating a 2 KB PolarTable, then
        // write the raw body straight to the file and reload on the display tick.
        if (PolarTable::validateJson(buf)) {
            File f = LittleFS.open(appConfig.cfg.polarFile, "w", true);
            if (f) {
                f.print(buf);
                f.close();
                dispMgr.requestPolarReload();
                req->send(200, "application/json", "{\"ok\":true}");
            } else {
                req->send(500, "application/json", "{\"error\":\"write failed\"}");
            }
        } else {
            req->send(400, "application/json", "{\"error\":\"Invalid polar data\"}");
        }
        buf = String();   // heap back to lwIP — the static would hold it forever
    }
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
    doc["apEngaged"] = data.apEngaged;
    add("apTarget", data.apTargetHeading);
    doc["aisCount"] = data.aisCount;
    String out; serializeJson(doc, out);
    return out;
}
