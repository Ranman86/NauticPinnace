#pragma once
#include <Arduino.h>

// ============================================================
// PolarTable – runtime-configurable boat polar.
//
// Loaded from a JSON file in LittleFS (default /polar.json):
//   { "name": "...", "tws":[...], "twa":[...], "speed":[[...],[...]] }
//   - tws[]   : true wind speeds (knots), columns,  ascending
//   - twa[]   : true wind angles (deg),   rows,     ascending (0..180)
//   - speed[] : speed[twaRow][twsCol] target boat speed (knots)
//
// speedAt() does bilinear interpolation over |twa| and tws (clamped to the
// table edges). Used by SpeedScreen and WindScreen so the on-device polar
// readouts match the configured data. Replaces the old hard-coded tables.
// ============================================================
class PolarTable {
public:
    static constexpr int MAX_TWA = 32;   // max rows
    static constexpr int MAX_TWS = 16;   // max columns

    char  name[32] = "Boat";
    int   ntwa = 0;
    int   ntws = 0;
    float twa[MAX_TWA];
    float tws[MAX_TWS];
    float spd[MAX_TWA][MAX_TWS];
    bool  loaded = false;

    // Read + parse a polar JSON file from LittleFS. Returns true on success.
    bool load(const char *path);
    // Parse a JSON string into this table (validates dimensions). Returns true on success.
    bool fromJson(const String &json);
    // Validate a polar JSON string without allocating a PolarTable. Used by the
    // web POST handler to keep its DRAM/stack footprint small.
    static bool validateJson(const String &json);
    // Serialise the current table back to the {name,tws,twa,speed} JSON shape.
    String toJson() const;
    // Persist the current table to LittleFS (pretty JSON). Returns true on success.

    // Interpolated target boat speed for a given TWA (deg, sign ignored) and
    // TWS (knots). Returns NAN if the table is not loaded / too small.
    float speedAt(float twaDeg, float twsKn) const;
};

// The polar table (~2 KB) lives in PSRAM, not DRAM: internal RAM is tight
// (~25 KB free) and the async web server stalls multi-segment file responses
// when it runs low. Allocated once in setup() via PolarTable::init().
// `polar` is a macro so existing `polar.foo()` call sites are unchanged.
extern PolarTable *polarPtr;
static inline PolarTable &gPolar() { return *polarPtr; }   // access the PSRAM instance

namespace PolarInit { void init(); }   // allocate the PSRAM instance (call early in setup)
