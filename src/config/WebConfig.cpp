#include "WebConfig.h"
#include "Config.h"
#include "../i18n/I18n.h"
#include "../nmea/DataModel.h"
#include "../nmea/N2kHandler.h"   // g_n2kSrcSeen for GET /api/sources
#include "../DisplaySetup.h"
#include "../display/DisplayManager.h"
#include "../PolarTable.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_wifi.h>   // esp_wifi_set_ps/get_ps - PS must be forced at IDF level
#include <LittleFS.h>
#include <Update.h>   // OTA: /api/ota streams straight into the inactive slot
#include <esp_ota_ops.h>   // GET /api/info reports the running slot
#include <esp_system.h>    // esp_get_idf_version() for the same
#include "../Version.h"
#include "../WifiNaming.h"

// main.cpp: set when the NMEA 2000 task was created at boot. Needed to tell a
// live demo-mode change apart from one that leaves the device with no data
// source at all.
extern volatile bool g_n2kTaskRunning;

static void pinProtocolBG();   // defined below, used from begin()
#if ESP_IDF_VERSION_MAJOR >= 5
static void sendOwnedJson(AsyncWebServerRequest *req, const String &json);
#endif

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
        pinProtocolBG();               // see the comment at the helper
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
// esp_ping runs its own small task per session; callbacks fire in that task's
// context, so they only touch the volatile flags below - loop() consumes them.
#include "ping/ping_sock.h"

static volatile bool s_probePending = false;   // a probe is in flight
static volatile bool s_probeGotReply = false;  // it got an echo reply

static void probe_on_success(esp_ping_handle_t h, void *args) {
    s_probeGotReply = true;
}
static void probe_on_end(esp_ping_handle_t h, void *args) {
    esp_ping_delete_session(h);
    s_probePending = false;
}

void WebConfig::probeGateway() {
    if (s_probePending) return;                // previous probe still running
    IPAddress gw = WiFi.gatewayIP();
    if (gw == IPAddress(0, 0, 0, 0)) return;   // no gateway - nothing to probe

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t target;
    ip_addr_set_ip4_u32_val(target, (uint32_t)gw);
    cfg.target_addr = target;
    cfg.count = 1;
    cfg.timeout_ms = 1500;
    cfg.task_stack_size = 2560;                // transient, freed after the probe

    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = probe_on_success;
    cbs.on_ping_end     = probe_on_end;

    esp_ping_handle_t h;
    s_probeGotReply = false;
    if (esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK) {
        s_probePending = true;
        esp_ping_start(h);
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

void WebConfig::forceReassoc(const char *why) {
    Serial.printf("[wifi] forcing re-association: %s (reconnects so far: %u)\n",
                  why, (unsigned)_reconnects);
    _reconnects++;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    pinProtocolBG();
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
    if (!_staMode) return;                    // AP mode has no association to lose
    const uint32_t now = millis();
    if (now - _lastCheck < 2000) return;      // cheap: check every 2 s
    _lastCheck = now;

    if (staLinkUp()) {
        if (!_wasUp) {                        // edge: report recovery once
            // Every fresh association may come up with default power-save
            // (WIFI_PS_MIN_MODEM) - force it off at IDF level on the recovery
            // edge, or large HTTP responses stall again (see begin()).
            esp_wifi_set_ps(WIFI_PS_NONE);
            Serial.printf("[wifi] link up again: IP %s  RSSI %d dBm  ch %d"
                          "  (reconnects so far: %u)\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                          WiFi.channel(), (unsigned)_reconnects);
            _wasUp = true;
        }
        _downSince  = 0;
        _retryDelay = 0;
        _nextRetry  = 0;

        // Status says CONNECTED - but that is exactly what it also says while
        // the data path is dead. Verify end-to-end with a gateway ping every
        // 15 s; three consecutive misses (~45 s worst case) force a full
        // re-association even though the driver still claims all is well.
        if (now - _lastProbe >= 15000) {
            if (_lastProbe != 0) {             // evaluate the PREVIOUS probe
                if (s_probeGotReply) {
                    _probeMisses = 0;
                } else if (s_probePending || !s_probeGotReply) {
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
    pinProtocolBG();
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
        doc["chip"]    = ESP.getChipModel();
        doc["idf"]     = esp_get_idf_version();
        // Which OTA slot is running - the pair that matters when an update
        // misbehaves and the question is whether the rollback took.
        const esp_partition_t *run = esp_ota_get_running_partition();
        doc["slot"] = run ? run->label : "?";
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

    // REST: POST restart device
    _server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", "Restarting...");
        delay(500);
        ESP.restart();
    });

    // REST: OTA update over WiFi. ?target=fw writes the firmware into the
    // INACTIVE app slot, ?target=fs replaces the LittleFS image.
    //
    // Chunks go straight into Update.write() - the image is NEVER buffered.
    // A 2.3 MB firmware would not fit anywhere on this device: internal DRAM
    // runs at 10-21 KB free with WiFi up, and the PSRAM arena is fully spoken
    // for by the canvases. Streaming is not an optimisation here, it is the
    // only way this can work at all.
    //
    // Safety net: the bootloader is built with rollback enabled, so a firmware
    // that does not reach the end of setup() is reverted automatically on the
    // next boot (see the confirmation call in main.cpp). Nobody has to reach
    // the USB port on a boat.
    _server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            const bool ok = !Update.hasError();
            AsyncWebServerResponse *res = req->beginResponse(
                ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false}");
            res->addHeader("Connection", "close");
            req->send(res);
            if (ok) {
                // Give the reply time onto the wire before the reset. Same
                // shape as /api/restart above; deliberately NOT dispMgr's
                // reboot screen, which would build LVGL objects from the
                // async_tcp task.
                Serial.println("[ota] update written - restarting"); Serial.flush();
                delay(500);
                ESP.restart();
            }
        },
        [](AsyncWebServerRequest *req, const String &filename, size_t index,
           uint8_t *data, size_t len, bool final) {
            if (index == 0) {
                const bool fsTarget = req->hasParam("target") &&
                                      req->getParam("target")->value() == "fs";
                // Reject anything that is not an ESP32 image before touching
                // the flash: a firmware always starts with the 0xE9 magic byte.
                // Costs nothing and turns "user picked the wrong file" from a
                // brick into an error message.
                if (!fsTarget && len > 0 && data[0] != 0xE9) {
                    Serial.println("[ota] rejected: not an ESP32 image (magic != 0xE9)");
                    return;
                }
                Serial.printf("[ota] start %s '%s'\n",
                              fsTarget ? "filesystem" : "firmware", filename.c_str());
                Serial.flush();
                if (!Update.begin(UPDATE_SIZE_UNKNOWN,
                                  fsTarget ? U_SPIFFS : U_FLASH)) {
                    Update.printError(Serial);
                    return;
                }
            }
            if (Update.isRunning() && len) {
                if (Update.write(data, len) != len) Update.printError(Serial);
            }
            if (final && Update.isRunning()) {
                if (Update.end(true)) Serial.printf("[ota] %u bytes written\n",
                                                    (unsigned)(index + len));
                else                  Update.printError(Serial);
                Serial.flush();
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

    // Root: serve the UI from the PSRAM copy — NO filesystem I/O inside the
    // async_tcp task (see loadIndexToPsram in the header for the deadlock this
    // prevents). Falls back to LittleFS streaming only if the load failed.
    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *req){
        if (_indexBuf && _indexLen) {
            AsyncWebServerResponse *res =
                req->beginResponse(200, "text/html", _indexBuf, _indexLen);
            // Without this header the browser renders the raw gzip bytes.
            if (_indexGzip) res->addHeader("Content-Encoding", "gzip");
            req->send(res);
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
    // Prefer the pre-compressed copy (rebuilt by extra_script.py on every run,
    // so it can never go stale): ~127 KB of HTML becomes ~28 KB on the wire,
    // which is the difference between a 13 s and a ~3 s page load on a board
    // with a weak signal. Falls back to the plain file when the .gz is absent
    // (e.g. a filesystem image built before this existed).
    _indexGzip = true;
    File f = LittleFS.open("/index.html.gz", "r");
    if (!f) {
        _indexGzip = false;
        f = LittleFS.open("/index.html", "r");
    }
    if (!f) return false;
    const size_t len = f.size();
    uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { f.close(); return false; }
    const size_t got = f.read(buf, len);
    f.close();
    if (got != len) { heap_caps_free(buf); return false; }
    _indexBuf = buf;
    _indexLen = len;
    Serial.printf("[web] index.html%s cached in PSRAM (%u bytes)\n",
                  _indexGzip ? ".gz" : "", (unsigned)len);
    return true;
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
    const bool demoBefore = appConfig.cfg.demoMode;
    if (!appConfig.fromJson(json)) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    json = String();          // release the body BEFORE save() builds its own
                              // JsonDocument + output String
    appConfig.save();
    // Apply hardware settings immediately (no restart needed)
    setBrightness(appConfig.cfg.brightness);
    // Rebuild screen order/visibility live on the next display tick.
    dispMgr.requestApplyScreenConfig();
    // Screen labels are set when a screen is built, so a language change
    // only shows up after a rebuild — the same path the theme uses.
    if (i18nLang() != langBefore) dispMgr.requestThemeReload();
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
    doc["apEngaged"] = data.apEngaged;
    add("apTarget", data.apTargetHeading);
    doc["aisCount"] = data.aisCount;
    String out; serializeJson(doc, out);
    return out;
}
