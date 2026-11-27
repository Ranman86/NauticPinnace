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
    X(SCREEN_MEDIA,         "Medien",                   "Media")                  \
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
    /* The bus has delivered no data value for 10 s (BusWatch.h). The age in   */ \
    /* seconds is appended in code behind BANNER_SINCE.                         */ \
    X(BANNER_NO_BUS_DATA,   "KEINE DATEN VOM NMEA-2000-BUS",                      \
                            "NO DATA FROM NMEA 2000 BUS")                         \
    X(BANNER_SINCE,         "seit",                     "for")                    \
    /* Anchor watch armed, but no current position: it cannot see a drag.     */ \
    X(BANNER_ANCHOR_NO_POS, "ANKERWACHE OHNE POSITION", "ANCHOR WATCH: NO FIX")   \
    /* ── Boot progress ───────────────────────────────────────────────────── */ \
    X(BOOT_LOADING_SCREENS, "Lade Bildschirme...",      "Loading screens...")     \
    X(BOOT_READY,           "Bereit.",                  "Ready.")                 \
    /* WiFi start on the boot screen (BootScreen::wifi). Fragments, not format */ \
    /* strings: the specifiers stay in the code, see the rule in I18n.h. "..."  */ \
    /* is three dots - the panel fonts have no ellipsis glyph.                  */ \
    X(BOOT_WIFI_CONNECTING, "Verbinde mit WLAN",        "Connecting to WiFi")     \
    X(BOOT_WIFI_ATTEMPT,    "Versuch",                  "attempt")                \
    X(BOOT_WIFI_SCANNING,   "Suche WLAN",               "Looking for WiFi")       \
    /* The hotspot itself is named with CFG_HOTSPOT_SECTION, the heading of   */ \
    /* the settings section that holds its password and QR code.              */ \
    X(BOOT_WIFI_ACTIVE,     "aktiv",                    "active")                 \
    X(BOOT_WIFI_RETRY_IN,   "neuer Versuch in",         "retrying in")            \
    /* Quotes around a network name: German „…“, English “…” (both in the     */ \
    /* latin_suppl fallback fonts; the single ‚…‘ pair is not).                 */ \
    X(QUOTE_OPEN,           "„",                        "“")                      \
    X(QUOTE_CLOSE,          "“",                        "”")                      \
    /* Why the station did not join (config/WifiStatus.h). Short: the settings */ \
    /* line carries one next to the hotspot address, and on 1024x600 landscape */ \
    /* (18 px, 494 px) the longer "Sicherheitsart passt nicht" / "Verbindung   */ \
    /* abgelehnt" left room for the code only.                                  */ \
    X(WIFI_RSN_NOT_FOUND,   "nicht gefunden",           "not found")              \
    X(WIFI_RSN_AUTH,        "Passwort falsch?",         "wrong password?")        \
    X(WIFI_RSN_SECURITY,    "Sicherheitsart?",          "security type?")         \
    X(WIFI_RSN_REFUSED,     "abgelehnt",                "refused")                \
    X(WIFI_RSN_OTHER,       "nicht verbunden",          "not connected")          \
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
    X(FLD_CPU,              "CPU",                      "CPU")                    \
    X(FLD_RAM,              "RAM frei",                 "RAM free")               \
    X(FLD_LAT,              "Breite",                   "Latitude")               \
    X(FLD_LON,              "Länge",                    "Longitude")              \
    /* Hemisphere letters behind a coordinate (CoordFormat.h). German charts   */ \
    /* write east as "O" (Ost); the other three are the same in both.          */ \
    X(HEMI_N,               "N",                        "N")                      \
    X(HEMI_S,               "S",                        "S")                      \
    X(HEMI_E,               "O",                        "E")                      \
    X(HEMI_W,               "W",                        "W")                      \
    X(FLD_APTARGET,         "AP Ziel",                  "AP target")
