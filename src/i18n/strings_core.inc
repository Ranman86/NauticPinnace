// strings_core.inc — shared UI chrome: screen catalogue, alarms, banners, boot.
//
// One line per string:  X(KEY, "deutsch", "english")
// Rules:
//   - KEY is unique across ALL .inc files (they share one enum).
//   - Write real UTF-8 (Ö, ü, ß). Do NOT use \xNN escapes: C hex escapes are
//     greedy, so "Stra\xC3\x9Fe" silently swallows the 'e' into the escape.
//   - Keep English no longer than German where the text sits in a tight layout.
#define I18N_STRINGS_CORE(X) \
    /* ── Screen catalogue (DisplayManager::SCREEN_NAMES, shown in the WebUI) ── */ \
    X(SCREEN_WIND,          "Wind & Trimm",             "Wind & Trim")            \
    X(SCREEN_SPEED,         "Geschwindigkeit & Polar",  "Speed & Polar")          \
    X(SCREEN_DEPTH,         "Tiefe",                    "Depth")                  \
    X(SCREEN_ENGINE,        "Motor",                    "Engine")                 \
    X(SCREEN_RUDDER,        "Ruder",                    "Rudder")                 \
    X(SCREEN_AIS,           "AIS-Radar",                "AIS Radar")              \
    X(SCREEN_WINDPLOT,      "Wind-Verlauf",             "Wind History")           \
    X(SCREEN_AUTOPILOT,     "Autopilot",                "Autopilot")              \
    X(SCREEN_FUSION,        "Fusion Radio",             "Fusion Radio")           \
    X(SCREEN_ATTITUDE,      "Schiffslage",              "Attitude")               \
    X(SCREEN_ANCHOR,        "Ankerwache",               "Anchor Watch")           \
    X(SCREEN_TANK,          "Tanks",                    "Tanks")                  \
    X(SCREEN_BATTERY,       "Batterie",                 "Battery")                \
    X(SCREEN_WEATHER,       "Wetter",                   "Weather")                \
    X(SCREEN_CLOCK,         "Uhr",                      "Clock")                  \
    X(SCREEN_VMG,           "VMG",                      "VMG")                    \
    X(SCREEN_ROUTE,         "Route",                    "Route")                  \
    X(SCREEN_GRID_DEFAULT,  "Datenraster",              "Data Grid")              \
    /* ── Alarms and banners (drawn on the panel — Montserrat + latin_suppl) ── */ \
    X(ALARM_ANCHOR_DRAG,    "ANKERALARM",               "ANCHOR ALARM")           \
    X(BANNER_DEMO_MODE,     "DEMO-MODUS  -  Keine echten Sensordaten",            \
                            "DEMO MODE  -  No real sensor data")                  \
    /* ── Boot progress ───────────────────────────────────────────────────── */ \
    X(BOOT_LOADING_SCREENS, "Lade Bildschirme...",      "Loading screens...")     \
    X(BOOT_READY,           "Bereit.",                  "Ready.")                 \
    /* ── Shared units: only those that genuinely differ by language ──────── */ \
    /* German nautical charts abbreviate the nautical mile as "sm" (Seemeile). */ \
    X(UNIT_NM,              "sm",                       "NM")                     \
    /* ── Data-field names (engine bottom cards + data grid cells) ────────── */ \
    /* Keyed by the field id stored in config.json (GridCell::pgn), and used   */ \
    /* ONLY when the user has not typed a label of their own. The wording       */ \
    /* mirrors the FIELDS table in data/index.html so panel and browser agree.  */ \
    X(FLD_SOG,              "SOG",                      "SOG")                    \
    X(FLD_COG,              "COG",                      "COG")                    \
    X(FLD_HDG,              "Kurs",                     "Heading")                \
    X(FLD_STW,              "STW",                      "STW")                    \
    X(FLD_AWA,              "AWA",                      "AWA")                    \
    X(FLD_AWS,              "AWS",                      "AWS")                    \
    X(FLD_TWA,              "TWA",                      "TWA")                    \
    X(FLD_TWS,              "TWS",                      "TWS")                    \
    X(FLD_TWD,              "TWD",                      "TWD")                    \
    X(FLD_DEPTH,            "Tiefe",                    "Depth")                  \
    X(FLD_RPM,              "RPM",                      "RPM")                    \
    X(FLD_OIL,              "Öldruck",                  "Oil pressure")           \
    X(FLD_COOLANT,          "Kühlwasser",               "Coolant")                \
    X(FLD_HOURS,            "Betriebsstd.",             "Engine hrs")             \
    X(FLD_FUEL,             "Kraftstoff",               "Fuel")                   \
    X(FLD_RUDDER,           "Ruder",                    "Rudder")                 \
    X(FLD_BATTV,            "Batt V",                   "Batt V")                 \
    X(FLD_LAT,              "Breite",                   "Latitude")               \
    X(FLD_LON,              "Länge",                    "Longitude")              \
    X(FLD_APTARGET,         "AP Ziel",                  "AP target")
