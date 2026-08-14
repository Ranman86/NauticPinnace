#include "BshTide.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include "../nmea/DataModel.h"
#include "../SunCalc.h"

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
        if (c < 0x80) out[o++] = (char)c;                   // plain ASCII
        i += (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
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
    JsonDocument filter;
    filter["features"][0]["geometry"]["coordinates"] = true;
    filter["features"][0]["properties"]["gauge_label"] = true;
    filter["features"][0]["properties"]["chartdatum_relative_to_gaugezero"] = true;
    filter["features"][0]["properties"]["high_water_low_water"][0]["event"] = true;
    filter["features"][0]["properties"]["high_water_low_water"][0]["event_timestamp"] = true;
    filter["features"][0]["properties"]["high_water_low_water"][0]["forecast_value"] = true;
    filter["features"][0]["properties"]["high_water_low_water"][0]["tidal_prediction_value"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) { Serial.printf("[BSH] JSON err: %s\n", err.c_str()); return false; }

    JsonArray feats = doc["features"].as<JsonArray>();
    if (feats.isNull() || feats.size() == 0) { Serial.println("[BSH] no stations in bbox"); return false; }

    // Nearest station (equirectangular squared distance).
    JsonObject best; double bestD = 1e30; const float D2R = 0.01745329f;
    for (JsonObject f : feats) {
        JsonArray c = f["geometry"]["coordinates"];
        if (c.isNull() || c.size() < 2) continue;
        double flon = c[0].as<double>(), flat = c[1].as<double>();
        double dx = (flon - lon) * cos(lat * D2R), dy = (flat - lat);
        double dd = dx * dx + dy * dy;
        if (dd < bestD) { bestD = dd; best = f; }
    }
    if (best.isNull()) { Serial.println("[BSH] no usable station"); return false; }

    const char *label = best["properties"]["gauge_label"] | "";
    int chartDatum    = best["properties"]["chartdatum_relative_to_gaugezero"] | 0;  // cm
    JsonArray evs     = best["properties"]["high_water_low_water"].as<JsonArray>();

    uint32_t nowUtc = (uint32_t)time(nullptr);
    DataModel::TideExtreme tmp[DataModel::MAX_TIDE_FC];
    int n = 0;
    for (JsonObject e : evs) {
        if (n >= DataModel::MAX_TIDE_FC) break;
        uint32_t u = parseBshTime(e["event_timestamp"] | (const char *)nullptr);
        if (!u || u + 1800 < nowUtc) continue;            // skip events >30 min past
        const char *ev = e["event"] | "";
        int fv = e["forecast_value"] | 0;                 // cm above gauge zero
        if (fv == 0) fv = atoi(e["tidal_prediction_value"] | "0");  // beyond the
                                          // official horizon: use the astronomical value
        tmp[n].unixUtc = u;
        tmp[n].cmCD    = (int16_t)(fv - chartDatum);      // cm above chart datum
        tmp[n].isHigh  = (ev[0] == 'H' || ev[0] == 'h');
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
            iter++;
        } else {
            Serial.println("[BSH] WiFi not connected, retry later");
        }
        vTaskDelay(pdMS_TO_TICKS(FETCH_PERIOD_MS));
    }
}

void bshTideBegin() {
    xTaskCreatePinnedToCore(bshTask, "BSH", 12288, nullptr, 1, nullptr, 1);
    Serial.println("[setup] BSH tide task started");
}
