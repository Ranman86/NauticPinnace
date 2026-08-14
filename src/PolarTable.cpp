#include "PolarTable.h"
#include "PsramArena.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <new>

PolarTable *polarPtr = nullptr;

namespace PolarInit {
    void init() {
        // Prefer the pre-WiFi PSRAM arena; fall back to PSRAM heap, then DRAM.
        void *mem = PsramArena::alloc(sizeof(PolarTable));
        if (!mem) mem = heap_caps_malloc(sizeof(PolarTable), MALLOC_CAP_SPIRAM);
        if (!mem) mem = malloc(sizeof(PolarTable));
        polarPtr = new (mem) PolarTable();
    }
}

bool PolarTable::load(const char *path) {
    if (!LittleFS.exists(path)) {
        Serial.printf("[polar] file %s not found\n", path);
        loaded = false;
        return false;
    }
    File f = LittleFS.open(path, "r");
    if (!f) { loaded = false; return false; }
    String json = f.readString();
    f.close();
    bool ok = fromJson(json);
    Serial.printf("[polar] load %s -> %s  (%d twa x %d tws, name=\"%s\")\n",
                  path, ok ? "OK" : "FAIL", ntwa, ntws, name);
    return ok;
}

bool PolarTable::fromJson(const String &json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) { Serial.printf("[polar] JSON error: %s\n", err.c_str()); return false; }

    JsonArray jtws = doc["tws"].as<JsonArray>();
    JsonArray jtwa = doc["twa"].as<JsonArray>();
    JsonArray jspd = doc["speed"].as<JsonArray>();
    if (jtws.isNull() || jtwa.isNull() || jspd.isNull()) {
        Serial.println("[polar] missing tws/twa/speed");
        return false;
    }

    int nt = (int)jtws.size();
    int na = (int)jtwa.size();
    if (nt < 2 || na < 2 || nt > MAX_TWS || na > MAX_TWA) {
        Serial.printf("[polar] bad dimensions twa=%d tws=%d\n", na, nt);
        return false;
    }
    if ((int)jspd.size() != na) {
        Serial.printf("[polar] speed rows %d != twa %d\n", (int)jspd.size(), na);
        return false;
    }
    // Validate all row lengths BEFORE committing so a malformed row can't leave
    // the table half-written (no large staging buffer needed → saves DRAM).
    for (int r = 0; r < na; r++) {
        JsonArray row = jspd[r].as<JsonArray>();
        if (row.isNull() || (int)row.size() != nt) {
            Serial.printf("[polar] speed row %d wrong length\n", r);
            return false;
        }
    }

    // Commit directly into members.
    loaded = false;
    strlcpy(name, doc["name"] | "Boat", sizeof(name));
    ntws = nt; ntwa = na;
    for (int c = 0; c < nt; c++) tws[c] = jtws[c].as<float>();
    for (int r = 0; r < na; r++) twa[r] = jtwa[r].as<float>();
    for (int r = 0; r < na; r++) {
        JsonArray row = jspd[r].as<JsonArray>();
        for (int c = 0; c < nt; c++) spd[r][c] = row[c].as<float>();
    }
    loaded = true;
    return true;
}

// Lightweight validation used by the web POST handler so it does not need a
// second 2 KB PolarTable buffer just to check incoming data.
bool PolarTable::validateJson(const String &json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    JsonArray jtws = doc["tws"].as<JsonArray>();
    JsonArray jtwa = doc["twa"].as<JsonArray>();
    JsonArray jspd = doc["speed"].as<JsonArray>();
    if (jtws.isNull() || jtwa.isNull() || jspd.isNull()) return false;
    int nt = (int)jtws.size(), na = (int)jtwa.size();
    if (nt < 2 || na < 2 || nt > MAX_TWS || na > MAX_TWA) return false;
    if ((int)jspd.size() != na) return false;
    for (int r = 0; r < na; r++) {
        JsonArray row = jspd[r].as<JsonArray>();
        if (row.isNull() || (int)row.size() != nt) return false;
    }
    return true;
}

String PolarTable::toJson() const {
    JsonDocument doc;
    doc["name"] = name;
    JsonArray jtws = doc["tws"].to<JsonArray>();
    for (int c = 0; c < ntws; c++) jtws.add(tws[c]);
    JsonArray jtwa = doc["twa"].to<JsonArray>();
    for (int r = 0; r < ntwa; r++) jtwa.add(twa[r]);
    JsonArray jspd = doc["speed"].to<JsonArray>();
    for (int r = 0; r < ntwa; r++) {
        JsonArray row = jspd.add<JsonArray>();
        for (int c = 0; c < ntws; c++) row.add(spd[r][c]);
    }
    String out;
    serializeJsonPretty(doc, out);
    return out;
}


float PolarTable::speedAt(float twaDeg, float twsKn) const {
    if (!loaded || ntwa < 2 || ntws < 2) return NAN;
    if (isnan(twaDeg) || isnan(twsKn)) return NAN;

    float a = fabsf(twaDeg);

    // --- locate TWA row bracket (twa[] ascending) ---
    int r1 = 1;
    while (r1 < ntwa - 1 && a > twa[r1]) r1++;
    int r0 = r1 - 1;
    float denomA = (twa[r1] - twa[r0]);
    float tf = (denomA != 0.f) ? (a - twa[r0]) / denomA : 0.f;
    if (tf < 0.f) tf = 0.f; if (tf > 1.f) tf = 1.f;   // clamp beyond table edges

    // --- locate TWS column bracket (tws[] ascending) ---
    int c1 = 1;
    while (c1 < ntws - 1 && twsKn > tws[c1]) c1++;
    int c0 = c1 - 1;
    float denomS = (tws[c1] - tws[c0]);
    float sf = (denomS != 0.f) ? (twsKn - tws[c0]) / denomS : 0.f;
    if (sf < 0.f) sf = 0.f; if (sf > 1.f) sf = 1.f;

    float s00 = spd[r0][c0], s01 = spd[r0][c1];
    float s10 = spd[r1][c0], s11 = spd[r1][c1];
    return (s00 * (1 - tf) + s10 * tf) * (1 - sf) +
           (s01 * (1 - tf) + s11 * tf) * sf;
}
