// strings_screens_b.inc — see strings_screens.inc for the rules.
// One line per string:  X(KEY, "deutsch", "english")
// Keys are unique across ALL .inc files; prefix each with its screen.
// Write real UTF-8 (Ö, ü, ß) — never \xNN escapes (C hex escapes are greedy).
//
// Covers EngineScreen, FusionScreen, GridScreen, RouteScreen, RudderScreen,
// SpeedScreen. SpeedScreen and FusionScreen contribute nothing: their labels are
// unit/abbreviation chrome (SOG/STW/Polar/VMG/Perf, Master, Zone 1..3) or come
// off the N2K bus (Fusion source, title, artist, album).
#define I18N_STRINGS_SCREENS_B(X) \
    /* ── EngineScreen ─────────────────────────────────────────────────────── */ \
    X(ENG_OVER_REV,   "ÜBERDREHZAHL",  "OVER-REV")      \
    /* ── RouteScreen ──────────────────────────────────────────────────────── */ \
    X(ROUTE_NO_WP,    "Kein Wegpunkt", "No waypoint")   \
    X(ROUTE_WP,       "Wegpunkt",      "Waypoint")      \
    X(ROUTE_PORT,     "Bb",            "Pt")            \
    X(ROUTE_STBD,     "Stb",           "Stbd")          \
    X(ROUTE_BEARING,  "PEILUNG",       "BEARING")       \
    X(ROUTE_TTG,      "RESTZEIT",      "TTG")           \
    X(ROUTE_XTE,      "ABLAGE",        "XTE")           \
    /* ── RudderScreen ─────────────────────────────────────────────────────── */ \
    X(RUD_MIDSHIPS,   "Mittschiffs",   "Midships")      \
    X(RUD_PORT,       "Backbord",      "Port")          \
    X(RUD_STARBOARD,  "Steuerbord",    "Starboard")     \
    /* single-letter markers on the rudder dial: Backbord/Port, Steuerbord */ \
    X(RUD_PORT_MARK,  "B",             "P")             \
    X(RUD_STBD_MARK,  "S",             "S")             \
    /* ── FusionScreen ─────────────────────────────────────────────────────── */ \
    /* En dash (–), NOT em dash (—): only the former is contained in the
       latin_suppl fallback font — the long one renders as a box. */ \
    X(FUS_LISTEN_NOTE, "Nur mithören – Steuerung aus",  "Listen-only – control off")
