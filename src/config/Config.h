#pragma once
#include <ArduinoJson.h>
#include <LittleFS.h>

// ============================================================
// Config – persistent JSON configuration stored in LittleFS.
// All settings are loaded on boot and written on change.
// ============================================================
struct EngineConfig {
    int    rpmIdle     = 700;
    int    rpmCruise   = 2200;
    int    rpmMaxCont  = 3000;
    int    rpmMax      = 3600;
};

// User config for one tank, matched to a bus tank by (instance, fluidType).
// Calibration maps sender level [%] to real volume [L] (tanks are non-linear).
struct TankCfg {
    bool    used      = false;
    uint8_t instance  = 0;
    uint8_t fluidType = 0xFF;
    char    name[20]  = "";
    float   capacity  = NAN;        // L; NAN = use the capacity reported on the bus
    uint8_t calN      = 0;          // calibration points (0/1 = linear)
    float   calPct[6] = {0,0,0,0,0,0};   // sender % (ascending)
    float   calLit[6] = {0,0,0,0,0,0};   // real litres at that %

    // Litres for a given sender level [%]; piecewise-linear if calibrated.
    float liters(float level, float busCap) const {
        float cap = isnan(capacity) ? busCap : capacity;
        if (calN < 2 || isnan(level)) return isnan(cap) ? NAN : level / 100.f * cap;
        if (level <= calPct[0])        return calLit[0];
        if (level >= calPct[calN - 1]) return calLit[calN - 1];
        for (int i = 1; i < calN; i++) {
            if (level <= calPct[i]) {
                float t = (level - calPct[i-1]) / (calPct[i] - calPct[i-1]);
                return calLit[i-1] + t * (calLit[i] - calLit[i-1]);
            }
        }
        return calLit[calN - 1];
    }
};

// User config for one battery bank, matched by instance.
struct BatteryCfg {
    bool    used       = false;
    uint8_t instance   = 0;
    char    name[20]   = "";
    float   capacityAh = NAN;       // Ah (informational / time estimates)
    uint8_t nominalV   = 12;        // 12 or 24 V system
};

struct GridCell {
    char label[16]  = "";
    char pgn[16]    = "";    // e.g. "sog", "awa", "depth"
    char unit[8]    = "";
    int  decimals   = 1;
};

struct GridConfig {
    bool     active = false;   // slot in use? (drives screen creation at boot)
    char     name[24] = "";    // shown in nav + WebUI screen list
    int      rows   = 3;
    int      cols   = 3;
    // Layout key. Empty/"" or "RxC" = uniform rows×cols grid. Special "hero"
    // layouts put one wide large-font field on top:
    //   "hero1_2"   = wide top + 2 below      (3 cells)
    //   "hero1_3"   = wide top + 3 below       (4 cells)
    //   "hero1_2_2" = wide top + 2 + 2 below   (5 cells)
    char     layout[16] = "";
    GridCell cells[9];
};

// Themeable colour palette, stored as 0xRRGGBB. Member defaults = dark theme.
// The light variant is filled with the light defaults in Config::begin().
#include "../display/theme_colors.h"
struct ThemeColors {
    #define X(n,d,l) uint32_t n = d;
    THEME_COLOR_FIELDS(X)
    #undef X
};

#include "../display/theme_sizes.h"
struct ThemeSizes {
    #define X(n,d) int16_t n = d;
    THEME_SIZE_FIELDS(X)
    #undef X
};

#include "../display/theme_fonts.h"
struct ThemeFonts {   // font size (px) per role; mapped to a Montserrat font at boot
    #define X(r,d) int16_t r = d;
    THEME_FONT_FIELDS(X)
    THEME_BIGFONT_FIELDS(X)   // depth/gridHero -> custom 96/192 px numeric fonts
    #undef X
};

// Light-theme colour palette (called by Config::begin()); exposed so the PC
// simulator can populate themeLight without going through LittleFS/begin().
void fillLightTheme(ThemeColors &c);
void fillNightTheme(ThemeColors &c);   // red-preserving night mode

// ---- Screen model ----------------------------------------------------------
// 17 fixed instrument screens (ids 0..16) + up to MAX_GRIDS data-grid slots
// (ids GRID_ID_BASE..GRID_ID_BASE+MAX_GRIDS-1). See DisplayManager.
#define N_FIXED_SCREENS  17
#define MAX_GRIDS        6
#define GRID_ID_BASE     N_FIXED_SCREENS
#define MAX_SCREENS      (N_FIXED_SCREENS + MAX_GRIDS)   // 23

struct AppConfig {
    // WiFi
    char     wifiSsid[64]     = "";
    char     wifiPassword[64] = "";
    bool     apMode           = true;   // start as access-point if true
    bool     licenseAccepted  = false;  // licence notice confirmed on first start?
    bool     wifiEnabled      = false;  // WiFi radio OFF by default (boat has no AP); toggle on-screen when needed

    // UI language: "de" or "en". Drives both the panel and the WebUI.
    // Default "en": a fresh device boots in English and asks for the language
    // on first run (LanguageOverlay), before the licences are shown.
    char     lang[4]          = "en";

    // Boot screen: title line and the optional boat name underneath it.
    // Both are shown by BootScreen; empty title falls back to the project name.
    char     bootTitle[24]    = "NauticPinnace";
    char     bootName[32]     = "";     // e.g. "S/V Albatross"; empty = hidden

    // Display
    uint8_t  brightness       = 200;    // 0-255
    // Screen navigation: order is the list of screen IDs in display sequence;
    // enabled is indexed BY SCREEN ID (screenEnabled[id]). Disabled screens are
    // skipped during navigation on the device. Normalised by DisplayManager.
    uint8_t  screenOrder[MAX_SCREENS]   = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22};  // screen IDs, in order
    bool     screenEnabled[MAX_SCREENS] = {true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true};
    uint8_t  screenCount      = N_FIXED_SCREENS;   // grids appended by DisplayManager when active

    // NMEA 2000
    int      canTxPin         = 17;
    int      canRxPin         = 18;
    // Listen only: N2km_ListenOnly instead of ListenAndNode. The device then
    // sends NOTHING on the bus (no address claim, no heartbeat, no Fusion
    // control) — for other people's boats, charter, or workshop appointments.
    // Takes effect on the next start because the mode is set in NMEA2000.Open().
    bool     n2kListenOnly    = false;
    // Hotspot password: randomly generated ONCE per device (see Entropy.h) —
    // replaces the old "MdPw"+MAC scheme that was derivable from the MAC.
    // Empty = regenerated on the next start (this way an existing device also
    // gets a random password automatically on update).
    char     apPass[20]       = "";

    // Engine
    EngineConfig engine;
    // Engine screen bottom fields – configurable count (1..6) + data point each.
    int      engineFieldCount = 4;
    GridCell engineFields[6];   // defaults set in Config::begin()

    // Data-grid screens (up to MAX_GRIDS independent slots)
    GridConfig grids[MAX_GRIDS];

    // Theme: active variant ("dark"/"light") + per-variant colour palettes.
    char        themeActive[8] = "dark";
    bool        themeAuto      = false;     // auto light/dark by sun (needs time+GPS)
    ThemeColors themeDark;                 // dark defaults (see struct)
    ThemeColors themeLight;                // light palette set in Config::begin()
    ThemeColors themeNight;                // night palette set in Config::begin()
    ThemeSizes  themeSizes;                // shared sizes (defaults = current UI_*)
    ThemeFonts  themeFonts;                // shared font sizes per role

    // Polar file path in LittleFS
    char polarFile[32] = "/polar.json";

    // Depth alarm threshold (m, 0 = disabled)
    float depthAlarm = 0;
    // Depth display unit: "m" = metres, "ft" = feet (internal always metres)
    char  depthUnit[4] = "m";

    // AIS
    int  aisRange   = 5;    // nm, display range
    bool aisAlarm   = true; // CPA/TCPA alarm
    float aisCpaAlarm  = 0.5f;  // nm
    float aisTcpaAlarm = 10.0f; // min

    // Anchor watch (drift alarm). Position persisted so the watch resumes after a
    // reboot while still at anchor. Evaluated globally in DisplayManager::update().
    bool   anchorSet     = false;   // true once an anchor position is captured
    float  anchorLat     = NAN;     // captured anchor position [deg]
    float  anchorLon     = NAN;
    float  anchorRadius  = 40.0f;   // drift-alarm radius [m]
    bool   anchorAlarmOn = false;   // drag-alarm armed
    bool   anchorNorthUp = true;    // true = North-up view, false = heading-up

    // Tank / battery user config (names, capacity, tank calibration). Matched to
    // bus instances. Sized to DataModel MAX_TANKS (6) / MAX_BATT (4).
    TankCfg     tankCfg[6];
    BatteryCfg  battCfg[4];

    // Demo mode (simulates all NMEA data)
    bool  demoMode          = false;

    // Performance overlay (FPS + CPU% in top-right corner)
    bool  showPerfOverlay   = false;

    // Sail / WindScreen
    float waterlineLengthM  = 9.0f;   // LWL in metres  (hull-speed = 2.43*sqrt(LWL))
    bool  allowSpinnaker    = true;    // show spinnaker when running / broad reach
    bool  allowCodeZero     = false;   // show Code Zero on close reach in light air
    int16_t foresailPercent = 100;    // headsail size %: 30=storm jib … 140=genoa (scales jib graphic)
    bool  allowButterfly    = false;   // wing-on-wing: jib poled out opposite the main on a run
    int16_t noGoAngle       = 30;      // No-Go half-angle (deg): |TWA| < this = in irons (configurable)
    bool  windLinesApparent = false;   // wind-flow lines: false=true wind (TWA), true=apparent (AWA)
    uint8_t headsailSizePct = 100;     // headsail size percentage (50–150 %), future use
};

class Config {
public:
    AppConfig cfg;

    bool begin();
    bool save();
    bool load();

    // Import/export raw JSON string
    String toJson() const;
    bool   fromJson(const String &json);

    // N2K config-transfer (stub – fill with proprietary PGN later)
    void sendViaN2k();
    bool receiveViaN2k(const uint8_t *data, size_t len);

private:
    static constexpr const char *PATH = "/config.json";
};

extern Config appConfig;
