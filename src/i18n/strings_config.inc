// strings_config.inc — the on-panel settings modal (ConfigOverlay) and the
// licence overlay's chrome.
//
// One line per string:  X(KEY, "deutsch", "english")
// Rules:
//   - KEY is unique across ALL .inc files (they share one enum). Prefix with
//     CFG_ for settings, LIC_ for the licence overlay.
//   - Write real UTF-8 (Ö, ü, ß). Do NOT use \xNN escapes: C hex escapes are
//     greedy, so "Stra\xC3\x9Fe" silently swallows the 'e' into the escape.
//   - The licence BODY text (src/display/LicenseText.h) is NOT translated — an
//     unofficial translation of a licence is not the licence. Only the chrome
//     around it (heading, intro line, buttons) belongs here.
//   - "SSID", "BT", "DE", "EN" are the same in both languages and stay literal.
#define I18N_STRINGS_CONFIG(X) \
    /* ── Settings modal ──────────────────────────────────────────────────── */ \
    X(CFG_TITLE,          "Konfiguration",            "Settings")                 \
    X(CFG_WIFI_SECTION,   "WLAN-Verbindung",          "WiFi connection")          \
    X(CFG_WIFI_SHORT,     "WLAN",                     "WiFi")                     \
    X(CFG_WIFI_OFF,       "WLAN aus",                 "WiFi off")                 \
    X(CFG_WIFI_AP_ACTIVE, "Hotspot-Modus aktiv",      "Hotspot mode active")      \
    X(CFG_WIFI_CONNECTED, "Verbunden",                "Connected")                \
    X(CFG_WIFI_NOT_CONN,  "Nicht verbunden",          "Not connected")            \
    X(CFG_NETWORK_NAME,   "Netzwerkname",             "Network name")             \
    X(CFG_PASSWORD_SHORT, "Passw.",                   "Pass.")                    \
    X(CFG_PASSWORD,       "Passwort",                 "Password")                 \
    X(CFG_CONNECT_REBOOT, "Verbinden & Neustart",     "Connect & restart")        \
    X(CFG_HOTSPOT_SECTION,"Interner Hotspot",         "Internal hotspot")         \
    X(CFG_SCAN_QR,        "QR scannen zum Verbinden", "Scan the QR code to connect") \
    X(CFG_HOTSPOT_REBOOT, "Hotspot & Neustart",       "Hotspot & restart")        \
    X(CFG_DISPLAY_SECTION,"Anzeige",                  "Display")                  \
    X(CFG_THEME_AUTO,     "Auto",                     "Auto")                     \
    X(CFG_THEME_LIGHT,    "Hell",                     "Light")                    \
    X(CFG_THEME_DARK,     "Dunkel",                   "Dark")                     \
    X(CFG_THEME_NIGHT,    "Nacht",                    "Night")                    \
    X(CFG_LICENSES_BTN,   "Lizenzen und Module",      "Licences and modules")     \
    /* Listen-only switch — space is tight (label + switch in the               */ \
    /* bottom row), hence deliberately short.                                   */ \
    X(CFG_N2K_LISTEN,     "Nur hören",                "RX only")                  \
    X(CFG_N2K_LISTEN_ON,  "Nur mithören...",          "Listen-only...")           \
    X(CFG_N2K_LISTEN_OFF, "Senden aktiv...",          "TX enabled...")            \
    /* ── Licence overlay chrome (the licence TEXT itself is never translated) */ \
    X(LIC_TITLE,          "Verwendete Lizenzen",      "Licences in use")          \
    X(LIC_SUBTITLE,       "Bitte einmalig zur Kenntnis nehmen.",                  \
                          "Please acknowledge once.")                             \
    X(LIC_ACCEPT,         "Verstanden",               "Understood")               \
    X(LIC_CLOSE,          "Schließen",                "Close")                    \
    X(LIC_FULL_IN_NOTICES,"Vollständig in THIRD-PARTY-NOTICES.md",                \
                          "In full in THIRD-PARTY-NOTICES.md")                    \
    /* Reboot notices — these appear full-screen while the device restarts and  */\
    /* were hardcoded German, so an English first-run showed German text.       */\
    X(CFG_RB_CONNECT,     "Verbinde...",              "Connecting...")            \
    X(CFG_RB_HOTSPOT,     "Hotspot startet...",       "Starting hotspot...")      \
    X(CFG_RB_WIFI_ON,     "WLAN ein...",              "WiFi on...")               \
    X(CFG_RB_WIFI_OFF,    "WLAN aus...",              "WiFi off...")
