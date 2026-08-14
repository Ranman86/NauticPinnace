// strings_screens_c.inc — see strings_screens.inc for the rules.
// One line per string:  X(KEY, "deutsch", "english")
// Keys are unique across ALL .inc files; prefix each with its screen.
// Write real UTF-8 (Ö, ü, ß) — never \xNN escapes (C hex escapes are greedy).
//
// Owned screens: TankScreen, VmgScreen, WeatherScreen, WindPlotScreen,
//                WindScreen, BootScreen.
#define I18N_STRINGS_SCREENS_C(X) \
    /* ── TankScreen ──────────────────────────────────────────────────────── */ \
    X(TANK_NO_DATA,         "Keine Tankdaten",  "No tank data")                  \
    /* Fluid-type DEFAULT names (PGN 127505). Only shown when the tank has no  */ \
    /* user name in config.json — these are never written back to the config.  */ \
    X(TANK_FT_DIESEL,       "Diesel",           "Diesel")                        \
    X(TANK_FT_FRESH,        "Frischwasser",     "Fresh water")                   \
    X(TANK_FT_GREY,         "Grauwasser",       "Grey water")                    \
    X(TANK_FT_LIVEWELL,     "Livewell",         "Livewell")                      \
    X(TANK_FT_OIL,          "Öl",               "Oil")                           \
    X(TANK_FT_BLACK,        "Schwarzwasser",    "Black water")                   \
    X(TANK_FT_PETROL,       "Benzin",           "Petrol")                        \
    X(TANK_FT_OTHER,        "Tank",             "Tank")                          \
    /* ── VmgScreen ───────────────────────────────────────────────────────── */ \
    X(VMG_LOW_WIND,         "zu wenig Wind",    "Low wind")                      \
    X(VMG_UPWIND,           "VMG Luv",          "VMG upwind")                    \
    X(VMG_DOWNWIND,         "VMG Lee",          "VMG downwind")                  \
    X(VMG_T_TARGET,         "ZIEL-VMG",         "TARGET VMG")                    \
    X(VMG_T_TARGET_ANGLE,   "ZIEL-WINKEL",      "TARGET ANGLE")                  \
    X(VMG_T_TWA_NOW,        "AKT. TWA",         "TWA NOW")                       \
    X(VMG_HIGHER,           "Höher",            "Head up")                       \
    X(VMG_LOWER,            "Tiefer",           "Bear away")                     \
    X(VMG_OPTIMAL,          "Optimal",          "Optimal")                       \
    /* ── WeatherScreen ───────────────────────────────────────────────────── */ \
    X(WEA_PRESSURE,         "LUFTDRUCK",        "PRESSURE")                      \
    X(WEA_AIR,              "LUFT °C",          "AIR °C")                        \
    X(WEA_WATER,            "WASSER °C",        "WATER °C")                      \
    X(WEA_HUMIDITY,         "FEUCHTE %",        "HUMIDITY %")                    \
    X(WEA_TREND_RISING,     "steigend",         "rising")                        \
    X(WEA_TREND_FALLING,    "fallend",          "falling")                       \
    X(WEA_TREND_STEADY,     "stabil",           "steady")                        \
    /* ── WindPlotScreen ──────────────────────────────────────────────────── */ \
    /* 8-point rose labels — German Ost = O, English East = E.                 */ \
    X(WPLOT_C_N,            "N",                "N")                             \
    X(WPLOT_C_NE,           "NO",               "NE")                            \
    X(WPLOT_C_E,            "O",                "E")                             \
    X(WPLOT_C_SE,           "SO",               "SE")                            \
    X(WPLOT_C_S,            "S",                "S")                             \
    X(WPLOT_C_SW,           "SW",               "SW")                            \
    X(WPLOT_C_W,            "W",                "W")                             \
    X(WPLOT_C_NW,           "NW",               "NW")                            \
    X(WPLOT_SAMPLES,        "Werte",            "samples")                       \
    X(WPLOT_AVG_TWS,        "TWS Mittel",       "avg TWS")                       \
    /* ── WindScreen ──────────────────────────────────────────────────────── */ \
    /* Side abbreviations — shared by WindScreen and VmgScreen (same layer).   */ \
    X(WIND_STB,             "Stb",              "Stb")                           \
    X(WIND_PT,              "Bb",               "Pt")                            \
    X(WIND_BOW,             "Bug",              "Bow")                           \
    X(WIND_STERN,           "Heck",             "Stern")                         \
    /* Compass card inside the inner circle — German Ost = O, English East = E */ \
    X(WIND_CARD_N,          "N",                "N")                             \
    X(WIND_CARD_E,          "O",                "E")                             \
    X(WIND_CARD_S,          "S",                "S")                             \
    X(WIND_CARD_W,          "W",                "W")                             \
    /* Attitude-mode corner KPIs */ \
    X(WIND_KPI_ROLL,        "KRÄNGUNG",         "HEEL")                          \
    X(WIND_KPI_PITCH,       "TRIMM",            "PITCH")                         \
    X(WIND_KPI_WAVE,        "WELLE",            "WAVE")                          \
    X(WIND_KPI_PERIOD,      "PERIODE",          "PERIOD")                        \
    /* Point of sail (drawn inside the circle, max ~190 px at FONT_MED) */ \
    X(WIND_POS_NOGO,        "Im Wind",          "Head to wind")                  \
    X(WIND_POS_CLOSE_HAULED,"Am Wind",          "Close hauled")                  \
    X(WIND_POS_CLOSE_REACH, "Halb-am-Wind",     "Close reach")                   \
    X(WIND_POS_BEAM_REACH,  "Halbwind",         "Beam reach")                    \
    X(WIND_POS_BROAD_REACH, "Raumwind",         "Broad reach")                   \
    X(WIND_POS_RUNNING,     "Vorwind",          "Running")                       \
    /* Reef badge (56 px pill at FONT_TINY) */ \
    X(WIND_REEF1,           "1. Reff",          "Reef 1")                        \
    X(WIND_REEF2,           "2. Reff",          "Reef 2")                        \
    X(WIND_REEF3,           "3. Reff",          "Reef 3")                        \
    /* ── BootScreen (STR_BOOT_LOADING_SCREENS / STR_BOOT_READY live in core) ─ */ \
    X(BOOT_INIT,            "Initialisierung...",   "Initialising...")
