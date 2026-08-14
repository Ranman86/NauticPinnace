// strings_screens_a.inc — see strings_screens.inc for the rules.
// One line per string:  X(KEY, "deutsch", "english")
// Keys are unique across ALL .inc files; prefix each with its screen.
// Write real UTF-8 (Ö, ü, ß) — never \xNN escapes (C hex escapes are greedy).
//
// Owned screens: AisScreen, AnchorScreen, AutopilotScreen, BatteryScreen,
//                ClockScreen, DepthScreen.
// Screen TITLES are not listed here — they reuse the STR_SCREEN_* catalogue in
// strings_core.inc so the nav-bar title and the WebUI screen list cannot drift.
#define I18N_STRINGS_SCREENS_A(X) \
    /* ── AisScreen ───────────────────────────────────────────────────────── */ \
    X(AIS_TARGET_ONE,       "Ziel",                 "target")               \
    X(AIS_TARGETS,          "Ziele",                "targets")              \
    X(AIS_UNKNOWN,          "Unbekannt",            "Unknown")              \
    /* ── AnchorScreen ────────────────────────────────────────────────────── */ \
    X(ANCH_SET_BTN,         "Anker setzen",         "Set anchor")           \
    X(ANCH_ALARM_ON,        "Alarm AN",             "Alarm ON")             \
    X(ANCH_ALARM_OFF,       "Alarm AUS",            "Alarm OFF")            \
    X(ANCH_DISTANCE,        "ABSTAND",              "DISTANCE")             \
    X(ANCH_BEARING,         "PEILUNG",              "BEARING")              \
    X(ANCH_NOT_SET,         "Kein Anker gesetzt",   "No anchor set")        \
    X(ANCH_NO_GPS,          "Kein GPS-Fix",         "No GPS fix")           \
    /* ── AutopilotScreen ─────────────────────────────────────────────────── */ \
    X(AP_SET_HEADING,       "Sollkurs",             "Set Heading")          \
    X(AP_DEVIATION,         "Abw.",                 "Err")                  \
    X(AP_RUDDER,            "Ruder",                "Rudder")               \
    /* ── BatteryScreen ───────────────────────────────────────────────────── */ \
    X(BATT_NO_DATA,         "Keine Batteriedaten",  "No battery data")      \
    X(BATT_TO_FULL,         "bis voll",             "to full")              \
    X(BATT_TO_EMPTY,        "bis leer",             "to empty")             \
    /* ── ClockScreen: weekday names, index 0 = Sunday ────────────────────── */ \
    X(CLOCK_WDAY_SUN,       "Sonntag",              "Sunday")               \
    X(CLOCK_WDAY_MON,       "Montag",               "Monday")               \
    X(CLOCK_WDAY_TUE,       "Dienstag",             "Tuesday")              \
    X(CLOCK_WDAY_WED,       "Mittwoch",             "Wednesday")            \
    X(CLOCK_WDAY_THU,       "Donnerstag",           "Thursday")             \
    X(CLOCK_WDAY_FRI,       "Freitag",              "Friday")               \
    X(CLOCK_WDAY_SAT,       "Samstag",              "Saturday")             \
    /* ── ClockScreen: time, sun, moon ────────────────────────────────────── */ \
    X(CLOCK_NO_TIME,        "Keine Zeit (PGN 126992)",                      \
                            "No time (PGN 126992)")                         \
    X(CLOCK_SUN,            "Sonne",                "Sun")                  \
    X(CLOCK_POLAR_DAY,      "Polartag",             "Polar day")            \
    X(CLOCK_POLAR_NIGHT,    "Polarnacht",           "Polar night")          \
    X(CLOCK_NO_GPS,         "kein GPS",             "no GPS")               \
    X(CLOCK_MOON,           "Mond",                 "Moon")                 \
    X(CLOCK_MOON_NEW,       "Neumond",              "New moon")             \
    X(CLOCK_MOON_WAX_CRESC, "zunehmende Sichel",    "Waxing crescent")      \
    X(CLOCK_MOON_FIRST_Q,   "erstes Viertel",       "First quarter")        \
    X(CLOCK_MOON_WAX_GIBB,  "zunehmender Mond",     "Waxing gibbous")       \
    X(CLOCK_MOON_FULL,      "Vollmond",             "Full moon")            \
    X(CLOCK_MOON_WANE_GIBB, "abnehmender Mond",     "Waning gibbous")       \
    X(CLOCK_MOON_LAST_Q,    "letztes Viertel",      "Last quarter")         \
    X(CLOCK_MOON_WANE_CRESC,"abnehmende Sichel",    "Waning crescent")      \
    /* ── ClockScreen: tide. DE Hochwasser/Niedrigwasser = HW/NW, EN HW/LW ── */ \
    X(CLOCK_TIDE_HW,        "HW",                   "HW")                   \
    X(CLOCK_TIDE_LW,        "NW",                   "LW")                   \
    X(CLOCK_TIDE_LEVEL,     "Pegel",                "Level")                \
    X(CLOCK_TIDE_RISING,    "steigend",             "rising")               \
    X(CLOCK_TIDE_FALLING,   "fallend",              "falling")              \
    X(CLOCK_TIDE_STATION,   "Tidestation",          "Tide station")         \
    X(CLOCK_TIDE_THEN,      "danach",               "then")                 \
    X(CLOCK_TIDE_SPRING,    "Springzeit",           "Springs")              \
    X(CLOCK_TIDE_NEAP,      "Nippzeit",             "Neaps")                \
    X(CLOCK_TIDE_MEAN,      "mittlerer Hub",        "mean range")           \
    X(CLOCK_TIDE_NO_GPS,    "Tide: kein GPS",       "Tide: no GPS")         \
    X(CLOCK_TIDE_EST_NOTE,  "grobe Schätzung - ohne lokale Pegeldaten",     \
                            "rough estimate - no local gauge data")         \
    /* Attribution lines: only Quelle/Source differs, source name + licence stay verbatim */ \
    X(CLOCK_SRC_N2K,        "Quelle: NMEA2000 (PGN 130320)",                \
                            "Source: NMEA2000 (PGN 130320)")                \
    X(CLOCK_SRC_BSH,        "Quelle: BSH (CC BY 4.0)",                      \
                            "Source: BSH (CC BY 4.0)")                      \
    /* ── DepthScreen ─────────────────────────────────────────────────────── */ \
    X(DEPTH_CAPTION,        "T I E F E",            "D E P T H")            \
    X(DEPTH_SHALLOW,        "FLACH",                "SHALLOW")              \
    X(DEPTH_NOW,            "JETZT",                "NOW")
