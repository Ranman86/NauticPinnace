#include "BshTide.h"
#include <Arduino.h>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR >= 5
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCoreWithCaps
#endif
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <math.h>
#include "../nmea/DataModel.h"
#include "../SunCalc.h"

// ── JSON pools -> PSRAM ──────────────────────────────────────────────────────
// The BSH body is huge: 433 KB for the query below (measured, limit=6), and
// what survives the filter is still 6 stations x ~23 HW/NW events x 4 fields,
// i.e. ~15-18 KB of ArduinoJson pool. Every pool block is <= 4 KB and therefore
// below SPIRAM_MALLOC_ALWAYSINTERNAL, so the DEFAULT allocator takes ALL of it
// from internal DRAM - while the TLS session is open and while the WiFi RX and
// lwIP buffers, which platformio.ini deliberately keeps internal, need that
// same DRAM. That is what killed this fetch: the RX path starved mid-body, the
// stream went >15 s without a byte, ArduinoJson reported IncompleteInput on a
// request that had already returned 200, and whichever task lost the next
// allocation took the device down with it. Same failure and same cure as the
// config JSON (Config.cpp). No DRAM fallback on purpose - it would reinstate
// exactly the spike this removes; a failed PSRAM block surfaces as NoMemory.
// IDF 5 only: the 4" stays on its proven DRAM path (its IDF 4.4 carries the
// documented OPI-PSRAM coherency bug).
#if ESP_IDF_VERSION_MAJOR >= 5
struct BshPsramAllocator final : ArduinoJson::Allocator {
    void *allocate(size_t n) override {
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override {
        return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};
static BshPsramAllocator s_bshJsonPsram;
#define BSH_JSON_DOC(name) JsonDocument name(&s_bshJsonPsram)
#else
#define BSH_JSON_DOC(name) JsonDocument name
#endif

// ── Configuration ────────────────────────────────────────────────────────────
static const char *BSH_URL_BASE =
    "https://gdi.bsh.de/ldproxy/rest/services/WaterLevelForecast"
    "/collections/waterlevelforecastdata/items";
// Europe/Berlin with automatic DST (CET/CEST).
static const char *TZ_BERLIN = "CET-1CEST,M3.5.0,M10.5.0/3";
// Fallback position (Cuxhaven) when no GPS fix is available yet.
static constexpr float FALLBACK_LAT = 53.870f, FALLBACK_LON = 8.720f;

static constexpr uint32_t FETCH_PERIOD_MS = 30UL * 60UL * 1000UL;  // 30 min
static constexpr int      RESYNC_EVERY    = 48;                    // ~24 h

// ── Time helpers ─────────────────────────────────────────────────────────────
// Parse "2026-06-11 16:31:00+02:00" -> Unix seconds (UTC). 0 on failure.
static uint32_t parseBshTime(const char *s) {
    if (!s) return 0;
    int Y, Mo, D, h, m, sec, oh = 0, om = 0; char sign = '+';
    int got = sscanf(s, "%d-%d-%d %d:%d:%d%c%d:%d",
                     &Y, &Mo, &D, &h, &m, &sec, &sign, &oh, &om);
    if (got < 6) return 0;
    long days     = scDaysFromCivil(Y, (unsigned)Mo, (unsigned)D);
    long localSec = days * 86400L + h * 3600L + m * 60L + sec;
    long offSec   = (long)(oh * 3600 + om * 60) * (sign == '-' ? -1 : 1);
    return (uint32_t)(localSec - offSec);
}

// Sync the system clock from SNTP and publish it to the DataModel. Returns the
// current Unix time on success, 0 on failure.
static uint32_t syncTimeFromSntp() {
    configTzTime(TZ_BERLIN, "pool.ntp.org", "de.pool.ntp.org", "time.nist.gov");
    time_t now = 0;
    for (int i = 0; i < 24; i++) {            // wait up to ~12 s
        now = time(nullptr);
        if (now > 1700000000) break;          // > 2023-11 → clock is set
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (now <= 1700000000) return 0;
    // Local (Berlin, DST-aware) minus UTC, derived from the two clock readings.
    struct tm lt, gt; localtime_r(&now, &lt); gmtime_r(&now, &gt);
    int diff = (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec)
             - (gt.tm_hour * 3600 + gt.tm_min * 60 + gt.tm_sec);
    if (diff < -43200) diff += 86400;
    if (diff >  43200) diff -= 86400;
    int offMin = diff / 60;
    {
        auto lk = data.lock();
        data.sysDays        = (uint16_t)(now / 86400);
        data.sysSecOfDay    = (double)(now % 86400);
        data.localOffsetMin = (int16_t)offMin;
        data.lastTimeUpdate = millis();
        data.timeValid      = true;
        data.timeIsReal     = true;
    }
    Serial.printf("[BSH] SNTP ok: unix=%ld  offset=%+dmin\n", (long)now, offMin);
    return (uint32_t)now;
}

// Transliterate UTF-8 German text to ASCII (the UI fonts have no umlauts).
static void translit(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < cap; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xC3 && in[i + 1]) {                       // 2-byte Latin-1
            const char *r = nullptr;
            switch ((unsigned char)in[i + 1]) {
                case 0xA4: r = "ae"; break; case 0x84: r = "Ae"; break;
                case 0xB6: r = "oe"; break; case 0x96: r = "Oe"; break;
                case 0xBC: r = "ue"; break; case 0x9C: r = "Ue"; break;
                case 0x9F: r = "ss"; break;
            }
            if (r) { while (*r && o + 1 < cap) out[o++] = *r++; }
            i += 2; continue;
        }
        if (c < 0x80) { out[o++] = (char)c; i++; continue; } // plain ASCII
        // Skip an unmapped multi-byte sequence, but never step OVER the
        // terminator: on a truncated UTF-8 tail (or a lone trailing 0xC3) the
        // blind "i += 2/3/4" landed past the NUL and the loop then kept reading
        // off the end of the string.
        size_t skip = (c < 0xE0) ? 2 : (c < 0xF0 ? 3 : 4);
        while (skip-- && in[i]) i++;
    }
    out[o] = 0;
}

// ── Fetch + parse ────────────────────────────────────────────────────────────
static bool fetchBsh() {
    float lat, lon; bool gpsOk;
    {
        auto lk = data.lock();
        lat = data.lat; lon = data.lon;
        gpsOk = !isnan(lat) && !isnan(lon);
    }
    if (!gpsOk) { lat = FALLBACK_LAT; lon = FALLBACK_LON; }

    const float d = 0.35f;       // bbox half-size in degrees
    char url[320];
    snprintf(url, sizeof(url),
             "%s?lang=de&bbox=%.3f,%.3f,%.3f,%.3f&limit=6&f=json",
             BSH_URL_BASE, lon - d, lat - d, lon + d, lat + d);
    Serial.printf("[BSH] GET %s\n", url);

    WiFiClientSecure client; client.setInsecure();   // public read-only data
    client.setTimeout(15000);
    HTTPClient http;
    if (!http.begin(client, url)) { Serial.println("[BSH] http.begin failed"); return false; }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.useHTTP10(true);     // disable chunked transfer so the JSON streams cleanly
    http.addHeader("Accept", "application/json");
    http.setTimeout(15000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[BSH] HTTP %d\n", code);
        http.end(); return false;
    }

    // Filter: keep only what we need; the huge per-station 'curve' is skipped.
    // It can only be skipped, not avoided: the service rejects both ?properties=
    // and ?skipGeometry= with 400 (verified against the live API), so the whole
    // 433 KB body still has to travel through the parser.
    BSH_JSON_DOC(filter);
    filter["features"][0]["geometry"]["coordinates"] = true;
    filter["features"][0]["properties"]["gauge_label"] = true;
    filter["features"][0]["properties"]["chartdatum_relative_to_gaugezero"] = true;
    filter["features"][0]["properties"]["high_water_low_water"][0]["event"] = true;
    filter["features"][0]["properties"]["high_water_low_water"][0]["event_timestamp"] = true;
    filter["features"][0]["properties"]["high_water_low_water"][0]["forecast_value"] = true;
    filter["features"][0]["properties"]["high_water_low_water"][0]["tidal_prediction_value"] = true;

    BSH_JSON_DOC(doc);
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    // A partial body is NEVER consumed. IncompleteInput means the stream died
    // mid-document (server closed early, or 15 s without a byte), which leaves
    // 'doc' holding a truncated tree - short arrays, missing members, a station
    // whose event list simply stops. Publishing that would put a wrong "next
    // tide" on the clock. Keep the previous forecast instead; the ClockScreen
    // ages it out by itself after 12 h. The heap figure is the diagnostic: this
    // parse used to starve internal DRAM, and that is what produced the stall.
    if (err) {
        Serial.printf("[BSH] JSON err: %s (internal heap free %u B)\n", err.c_str(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }

    JsonArray feats = doc["features"].as<JsonArray>();
    if (feats.isNull() || feats.size() == 0) { Serial.println("[BSH] no stations in bbox"); return false; }

    // Nearest USABLE station (equirectangular squared distance). Usability is
    // part of the ranking, not a check on the winner: several gauges carry no
    // chart datum at all (Meldorf and Neuwerk send null), and electing one of
    // those only to give up afterwards would hide a complete gauge a few
    // kilometres away.
    JsonObject best; double bestD = 1e30; const float D2R = 0.01745329f;
    int chartDatum = 0;                                   // cm, gauge zero -> chart datum
    for (JsonObject f : feats) {
        JsonArray c = f["geometry"]["coordinates"];
        if (c.isNull() || c.size() < 2) continue;
        JsonObject pr = f["properties"];
        // The datum arrives as a JSON FLOAT (313.0) and sometimes as null. It
        // must be read as a float: ArduinoJson's is<int>() is false for a float
        // variant, so the old "| 0" default won on EVERY station and every
        // height was published uncorrected - 302 cm too high at Cuxhaven.
        if (!pr["chartdatum_relative_to_gaugezero"].is<float>()) continue;
        JsonArray evs = pr["high_water_low_water"].as<JsonArray>();
        if (evs.isNull() || evs.size() == 0) continue;
        double flon = c[0].as<double>(), flat = c[1].as<double>();
        double dx = (flon - lon) * cos(lat * D2R), dy = (flat - lat);
        double dd = dx * dx + dy * dy;
        if (dd < bestD) {
            bestD = dd; best = f;
            chartDatum = (int)lroundf(pr["chartdatum_relative_to_gaugezero"].as<float>());
        }
    }
    if (best.isNull()) { Serial.println("[BSH] no usable station"); return false; }

    const char *label = best["properties"]["gauge_label"] | "";
    JsonArray evs     = best["properties"]["high_water_low_water"].as<JsonArray>();

    uint32_t nowUtc = (uint32_t)time(nullptr);
    DataModel::TideExtreme tmp[DataModel::MAX_TIDE_FC];
    int n = 0; uint32_t lastU = 0;
    for (JsonObject e : evs) {
        if (n >= DataModel::MAX_TIDE_FC) break;
        uint32_t u = parseBshTime(e["event_timestamp"] | (const char *)nullptr);
        if (!u || u + 1800 < nowUtc) continue;            // skip events >30 min past
        if (u <= lastU) continue;                         // the ClockScreen takes the
                                          // FIRST future entry as "next tide", so the
                                          // published list has to be strictly ascending
        const char *ev = e["event"] | "";
        // forecast_value exists only inside the official forecast horizon; past
        // it the API sends the astronomical tidal_prediction_value, and as a
        // STRING. Test for presence rather than treating 0 as "absent" - 0 cm is
        // a legal reading, the water standing exactly at gauge zero.
        int fv = 0;
        if (e["forecast_value"].is<float>())
            fv = (int)lroundf(e["forecast_value"].as<float>());       // cm above gauge zero
        else if (e["tidal_prediction_value"].is<const char *>())
            fv = atoi(e["tidal_prediction_value"].as<const char *>());
        else if (e["tidal_prediction_value"].is<float>())
            fv = (int)lroundf(e["tidal_prediction_value"].as<float>());
        else continue;                                    // no height at all
        long cm = (long)fv - chartDatum;
        if (cm < -3000 || cm > 3000) continue;            // +/-30 m is not a gauge reading
        tmp[n].unixUtc = u;
        tmp[n].cmCD    = (int16_t)cm;                     // cm above chart datum
        tmp[n].isHigh  = (ev[0] == 'H' || ev[0] == 'h');
        lastU = u;
        n++;
    }
    if (n == 0) { Serial.println("[BSH] station has no upcoming events"); return false; }

    char stClean[40]; translit(label, stClean, sizeof(stClean));
    {
        auto lk = data.lock();
        for (int i = 0; i < n; i++) data.tideFc[i] = tmp[i];
        data.tideFcCount = n;
        strncpy(data.tideStation, stClean, sizeof(data.tideStation) - 1);
        data.tideStation[sizeof(data.tideStation) - 1] = 0;
        data.tideIsBsh    = true;
        data.lastTideFcMs = millis();
    }
    Serial.printf("[BSH] '%s' chartDatum=%dcm events=%d:\n", stClean, chartDatum, n);
    for (int i = 0; i < n; i++)
        Serial.printf("[BSH]   %s u=%lu  %.2fm CD\n", tmp[i].isHigh ? "HW" : "NW",
                      (unsigned long)tmp[i].unixUtc, tmp[i].cmCD / 100.0);
    return true;
}

// ── Background task ──────────────────────────────────────────────────────────
static void bshTask(void *) {
    bool synced = false;
    int  iter   = 0;
    // Let boot settle first: screen creation + WiFi association allocate heap, and
    // the TLS fetch (~40 KB transient) must not collide with that peak.
    vTaskDelay(pdMS_TO_TICKS(25000));
    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            if (!synced || (iter % RESYNC_EVERY) == 0) {
                if (syncTimeFromSntp()) synced = true;
            }
            fetchBsh();
            // The 12 KB stack is sized for the TLS handshake and lives in PSRAM;
            // print what was left so a future mbedTLS/ArduinoJson change cannot
            // quietly eat the margin without anyone seeing it.
            Serial.printf("[BSH] stack low-water %u B, internal heap free %u B\n",
                          (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            iter++;
        } else {
            Serial.println("[BSH] WiFi not connected, retry later");
        }
        vTaskDelay(pdMS_TO_TICKS(FETCH_PERIOD_MS));
    }
}

void bshTideBegin() {
#if ESP_IDF_VERSION_MAJOR >= 5
    // 12 KB of stack for a task that does one HTTPS fetch every 30 minutes is
    // the single largest firmware-owned DRAM block. On IDF 5 the stack can
    // live in PSRAM (sdkconfig already has SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    // and FREERTOS_TASK_CREATE_ALLOW_EXT_MEM enabled): legal because this
    // task never writes flash and runs nothing ISR-ish - it fetches, parses,
    // and fills the DataModel. TLS itself is PSRAM-backed anyway (mbedTLS
    // EXTERNAL_MEM_ALLOC).
    if (xTaskCreatePinnedToCoreWithCaps(bshTask, "BSH", 12288, nullptr, 1,
                                        nullptr, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
            != pdPASS) {
        // PSRAM exhausted? Fall back rather than silently losing the feature.
        xTaskCreatePinnedToCore(bshTask, "BSH", 12288, nullptr, 1, nullptr, 1);
    }
#else
    xTaskCreatePinnedToCore(bshTask, "BSH", 12288, nullptr, 1, nullptr, 1);
#endif
    Serial.println("[setup] BSH tide task started");
}
