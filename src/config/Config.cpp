#include "Config.h"
#include "WifiCountry.h"
#include "../i18n/I18n.h"
#include "../Entropy.h"

#ifndef SIMULATOR
#include <esp_idf_version.h>   // not among the sim stubs; leaving the macro
#endif                         // undefined there selects the plain-doc branch
#if ESP_IDF_VERSION_MAJOR >= 5
#include <esp_heap_caps.h>
// The ~12 KB config JSON costs, transiently and all at once: ArduinoJson pool
// blocks (~13 KB) + the pretty-printed output String (~12 KB) + the async
// response's own copy (~12 KB). On the 7B that exceeds the free internal heap
// (~24 KB with WiFi up) - and ArduinoJson/String fail SILENTLY on a lost
// allocation, so /api/config delivered structurally broken JSON with the
// break at a different offset every time (measured), and the WebUI fell back
// to defaults (empty screens list, wrong IP). Routing the doc pools through
// PSRAM removes the whole spike. 7B-only: the 4" stays on its proven DRAM
// path (its IDF 4.4 has the documented OPI-PSRAM coherency bug).
struct SpiRamAllocator final : ArduinoJson::Allocator {
    void *allocate(size_t n) override {
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override {
        return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};
static SpiRamAllocator s_jsonPsram;
#define CFG_JSON_DOC() JsonDocument doc(&s_jsonPsram)
#else
#define CFG_JSON_DOC() JsonDocument doc
#endif

#if defined(BOARD_PANEL_1024X600) && !defined(SIMULATOR)
#include <Preferences.h>
// "pio run -t uploadfs" replaces the WHOLE LittleFS partition with the image
// built from data/ - including config.json, whose repo copy deliberately holds
// no WiFi credentials. Every filesystem deploy therefore knocked the device
// off the network until someone retyped the WLAN on the on-screen keyboard.
// NVS lives in its own partition and survives uploadfs, so the WiFi section is
// mirrored there on save and pulled back when a factory config (empty SSID)
// shows up after a deploy. WiFi only: a full-config mirror will not reliably
// fit the small nvs partition alongside the WiFi stack's own storage.
//
// The WiFi country rides along as "cc". Read through isKey(): every display
// from before the setting has no "cc", and Preferences::getString() logs an
// error for a missing key - on every save, for good, since the default ""
// equals the missing key and is therefore never written. A display in
// hotspot mode without an SSID mirrors nothing, the country included
// (acceptable: in hotspot mode it only changes the country the beacon names).
static void wifiBackupStore(const AppConfig &cfg) {
    if (!cfg.wifiSsid[0]) return;   // never overwrite a good backup with factory-empty
    Preferences p;
    if (!p.begin("wifibak", false)) return;
    if (p.getString("ssid") != cfg.wifiSsid ||
        p.getString("pass") != cfg.wifiPassword ||
        p.getBool("en", false) != cfg.wifiEnabled ||
        p.getBool("ap", true)  != cfg.apMode ||
        (p.isKey("cc") ? p.getString("cc") : String()) != cfg.wifiCountry) {
        p.putString("ssid", cfg.wifiSsid);
        p.putString("pass", cfg.wifiPassword);
        p.putBool("en", cfg.wifiEnabled);
        p.putBool("ap", cfg.apMode);
        p.putString("cc", cfg.wifiCountry);
    }
    p.end();
}
static bool wifiBackupRestore(AppConfig &cfg) {
    if (cfg.wifiSsid[0]) return false;   // config already carries a network
    Preferences p;
    if (!p.begin("wifibak", true)) return false;   // read-only; absent on first boot
    String ssid = p.getString("ssid");
    if (!ssid.length()) { p.end(); return false; }
    strlcpy(cfg.wifiSsid,     ssid.c_str(),          sizeof(cfg.wifiSsid));
    strlcpy(cfg.wifiPassword, p.getString("pass").c_str(), sizeof(cfg.wifiPassword));
    cfg.wifiEnabled = p.getBool("en", false);
    cfg.apMode      = p.getBool("ap", true);
    if (p.isKey("cc")) {            // absent in a backup from before the setting
        String cc = p.getString("cc");
        if (wifiCountryValid(cc.c_str()))
            strlcpy(cfg.wifiCountry, cc.c_str(), sizeof(cfg.wifiCountry));
    }
    p.end();
    return true;
}
#endif

Config appConfig;

// "sail.spi_type" <-> SpiType. The strings are the stored and the WebUI's
// form; the enum values are never written anywhere.
static const char *spiTypeName(SpiType t) {
    switch (t) {
        case SpiType::None:     return "none";
        case SpiType::Gores:    return "gores";
        case SpiType::Gennaker: return "gennaker";
        default:                return "balloon";
    }
}
static bool spiTypeFromName(const char *s, SpiType &out) {
    if (!s) return false;
    if (strcmp(s, "none") == 0)     { out = SpiType::None;     return true; }
    if (strcmp(s, "gores") == 0)    { out = SpiType::Gores;    return true; }
    if (strcmp(s, "balloon") == 0)  { out = SpiType::Balloon;  return true; }
    if (strcmp(s, "gennaker") == 0) { out = SpiType::Gennaker; return true; }
    return false;
}

// Light-theme defaults (dark defaults live in the ThemeColors struct members).
void fillLightTheme(ThemeColors &c) {
    #define X(n,d,l) c.n = l;
    THEME_COLOR_FIELDS(X)
    #undef X
}

// ── Night mode (red-preserving) ──────────────────────────────────────────────
// Adopted from the NauticPi MFD palette (data-theme="night"):
//   deep #0a0202 · card #2a0f0f · edge #4a1c1c · ink #d35c3e · dim #aa4a2c
//   accent #e2643e · warn #e2543a · ok #b24c2c · amber #c25e2a
//
// Deliberately, EVERY role is set, not just the conspicuous ones: the point
// of night mode is preserving dark adaptation, and a single blue sounder
// trace or green zone destroys that completely. Everything therefore stays
// in red/brown tones and dark.
void fillNightTheme(ThemeColors &c) {
    // Base palette
    c.bg        = 0x0A0202;  c.surface   = 0x2A0F0F;  c.border    = 0x4A1C1C;
    c.text      = 0xD35C3E;  c.textDim   = 0xAA4A2C;  c.accent    = 0xE2643E;
    // Status colours – distinguishable only by brightness, not by hue
    c.green     = 0xB24C2C;  c.yellow    = 0xC25E2A;  c.red       = 0xE2543A;
    c.orange    = 0xC25E2A;  c.port      = 0xE2543A;  c.starboard = 0xB24C2C;
    c.wind      = 0xC9553A;  c.gridLine  = 0x3A1414;
    // Overlays
    c.navBtnBg  = 0x000000;  c.perfText  = 0xB24C2C;  c.perfBg    = 0x000000;
    c.demoBanner= 0x6A1810;  c.demoText  = 0xE8A08A;
    // Depth display
    c.depthBg   = 0x080202;  c.depthGrad = 0x2A0C0C;  c.depthHdr  = 0x7A2E1E;
    c.depthVal  = 0xE8785A;  c.depthEcho = 0xE2643E;  c.depthGrid = 0x3A1414;
    c.depthScale= 0x8A3A22;  c.depthNow  = 0xFF6A48;
    // Autopilot
    c.apCompassBg  = 0x1A0808; c.apCompassMaj = 0xAA4A2C; c.apCompassMin = 0x4A1C1C;
    c.apTarget     = 0xE2643E; c.apDevBar     = 0x180808;
    c.apModeActive = 0x2E1008; c.apModeManual = 0x220C0C; c.apModeStandby = 0x2A1408;
    // Wind rose
    c.windBezel     = 0x1A0808; c.windInner     = 0x120404; c.windRingBg    = 0x200A0A;
    c.windDepthBg   = 0x1A0808;
    c.windZoneNogo  = 0x3A0C08; c.windZoneClose = 0x2A1008; c.windZoneCloser= 0x321408;
    c.windZoneBeam  = 0x2A1410; c.windZoneBroad = 0x3A1C0A; c.windZoneRun   = 0x421808;
    c.windTickMaj   = 0xA04828; c.windTickMin   = 0x4A1C1C;
    c.windTwa       = 0xE2643E; c.windAwa       = 0xC03A22; c.windVmg       = 0xB24C2C;
    c.windHull      = 0xB05A3C; c.windSail      = 0xD8907A; c.windMast      = 0x4A1C1C;
    c.windFlow      = 0x8A3A22; c.windRigging   = 0x6A2A1A;
    // World map + seabed: without these values the map and water column would
    // have stayed blue-green and would have destroyed the eye's dark
    // adaptation — exactly what night mode is supposed to prevent.
    c.mapSea        = 0x2A0A0A; c.mapLand       = 0x4A1C10;
    c.mapCoast      = 0x7A2E1E; c.mapGrid       = 0x3A1414;
    c.depthBed      = 0x2A1008; c.tideWater     = 0x8A3A22;
    // Horizon: sky brighter than ground so the attitude stays readable even
    // without hue — in night mode only brightness still differentiates.
    c.attSky        = 0x7A2E1E; c.attGround     = 0x3A1208;
    c.onAccent      = 0xF0C0B0;   // light warm red instead of white on accent surfaces
}

bool Config::begin() {
    // Partition in partitions_16MB.csv is named "littlefs" (not "spiffs").
    // Arduino's LittleFS.begin() without label defaults to "spiffs" -> not found.
    // Must pass the explicit label.  formatOnFail=false: never auto-wipe files.
    if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
        Serial.println("[fs] LittleFS mount FAILED");
        Serial.println("[fs] Run: pio run -t uploadfs  then  pio run -t upload");
        return false;
    }
    // List files so the log confirms what the filesystem contains
    Serial.println("[fs] LittleFS mounted OK. Files:");
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    int count = 0;
    while (f && count < 20) {
        Serial.printf("[fs]   %-30s  %u bytes\n", f.name(), f.size());
        f = root.openNextFile();
        count++;
    }
    if (count == 0) Serial.println("[fs]   (empty)");
    fillLightTheme(cfg.themeLight);   // light defaults before load() (file overrides)
    fillNightTheme(cfg.themeNight);   // ditto for the night mode
    // Default engine bottom fields (file overrides in load()).
    {
        auto set = [](GridCell &c, const char *pgn, const char *label, const char *unit, int dec) {
            strlcpy(c.pgn, pgn, 16); strlcpy(c.label, label, 16); strlcpy(c.unit, unit, 8); c.decimals = dec;
        };
        // Label deliberately EMPTY: an empty label means "use the standard,
        // translated field name" (i18nFieldName(pgn), resolved when the screen
        // renders). A non-empty label is the user's own text and is never
        // translated. Hard-coding German here is what made the engine cards
        // stay German in the English UI.
        // (The old comment about avoiding umlauts is obsolete — the panel font
        //  now carries them via the latin_suppl fallback.)
        set(cfg.engineFields[0], "oil",     "", "hPa", 0);
        set(cfg.engineFields[1], "coolant", "", "°C",  1);
        set(cfg.engineFields[2], "hours",   "", "h",   0);
        set(cfg.engineFields[3], "fuel",    "", "L/h", 1);
    }
    bool ok = load();
#if defined(BOARD_PANEL_1024X600) && !defined(SIMULATOR)
    if (wifiBackupRestore(cfg)) {
        Serial.println("[wifi] credentials restored from the NVS backup (uploadfs had erased them)");
        save();   // write them back into config.json on the fresh filesystem
    }
#endif
    // Ensure the hotspot password BEFORE WiFi starts (setup() calls
    // webCfg.begin() after Config::begin()). On the very first start — or when
    // updating an existing device — the field is empty.
    if (!cfg.apPass[0]) {
        Entropy::generateApPassword(cfg.apPass, sizeof(cfg.apPass));
        save();
        Serial.println("[wifi] hotspot password generated (random, not MAC-derived)");
    }
    return ok;
}

bool Config::load() {
    if (!LittleFS.exists(PATH)) {             // use defaults
        cfg.iconsMigrated = true;             // the default set is the new one
        cfg.coordMigrated = true;             // ... and so is the default format
        return true;
    }
    File f = LittleFS.open(PATH, "r");
    if (!f) return false;
    String json = f.readString();
    f.close();
    const bool ok = fromJson(json);
    // OpenBridge became the default icon set in 1.3.0. Until then "classic" was
    // stored on every display whether anyone had picked it or not, so a stored
    // "classic" without the marker is switched once - here, on the file, never
    // on a web patch, which goes through fromJson() too. A set picked after
    // that is saved with the marker and stays.
    bool migrated = false;
    if (ok && !cfg.iconsMigrated) {
        if (cfg.iconSet == ICON_SET_CLASSIC)
            Serial.println("[cfg] icons: classic -> openbridge (new default, switched once)");
        cfg.iconSet = ICON_SET_OPENBRIDGE;
        cfg.iconsMigrated = true;
        migrated = true;
    }
    // The same for the coordinate format: degrees and minutes, what chart
    // plotters show, is the default from 1.3.0 on; the decimal degrees every
    // display had stored until then were nobody's choice either.
    if (ok && !cfg.coordMigrated) {
        if (cfg.coordFormat == 0)
            Serial.println("[cfg] coordinates: decimal -> degrees and minutes (new default, switched once)");
        cfg.coordFormat = 1;
        cfg.coordMigrated = true;
        migrated = true;
    }
    if (migrated) save();
    return ok;
}

bool Config::save() {
    File f = LittleFS.open(PATH, "w");
    if (!f) return false;
    String json = toJson();
    f.print(json);
    f.close();
#if defined(BOARD_PANEL_1024X600) && !defined(SIMULATOR)
    wifiBackupStore(cfg);
#endif
    return true;
}

String Config::toJson() const {
    CFG_JSON_DOC();
    doc["wifi"]["ssid"]     = cfg.wifiSsid;
    doc["wifi"]["password"] = cfg.wifiPassword;
    doc["wifi"]["ap_mode"]  = cfg.apMode;
    doc["wifi"]["enabled"]  = cfg.wifiEnabled;
    doc["wifi"]["country"]  = cfg.wifiCountry;   // "" = default, written as well
    doc["wifi"]["ap_pass"]  = cfg.apPass;
    doc["license_ok"]       = cfg.licenseAccepted;
    doc["ui"]["lang"]       = cfg.lang;
    // OpenBridge icon trial
    doc["ui"]["icons"]      = (cfg.iconSet == ICON_SET_OPENBRIDGE) ? "openbridge" : "classic";
    doc["ui"]["icons_migrated"] = cfg.iconsMigrated;
    doc["boot"]["title"]    = cfg.bootTitle;
    doc["boot"]["name"]     = cfg.bootName;

    doc["display"]["brightness"] = cfg.brightness;
    // Screens: ordered list of {id, enabled}. Replaces the old "screen_order".
    JsonArray screens = doc["display"]["screens"].to<JsonArray>();
    for (int i = 0; i < cfg.screenCount; i++) {
        uint8_t id = cfg.screenOrder[i];
        JsonObject s = screens.add<JsonObject>();
        s["id"] = id;
        s["en"] = (id < MAX_SCREENS) ? cfg.screenEnabled[id] : true;
    }

    doc["nmea2000"]["can_tx"] = cfg.canTxPin;
    doc["nmea2000"]["can_rx"] = cfg.canRxPin;
    doc["nmea2000"]["listen_only"] = cfg.n2kListenOnly;
    doc["nmea2000"]["accept_updates"] = cfg.n2kAcceptUpdates;

    // Own group: this is neither a bus nor a display setting, and grouping it
    // with the gauge data keeps room for a station override later.
    doc["tide"]["bsh_fetch"] = cfg.tideBshFetch;

    doc["engine"]["rpm_idle"]     = cfg.engine.rpmIdle;
    doc["engine"]["rpm_cruise"]   = cfg.engine.rpmCruise;
    doc["engine"]["rpm_max_cont"] = cfg.engine.rpmMaxCont;
    doc["engine"]["rpm_max"]      = cfg.engine.rpmMax;
    doc["engine"]["field_count"]  = cfg.engineFieldCount;
    {
        JsonArray ef = doc["engine"]["fields"].to<JsonArray>();
        int n = max(0, min(cfg.engineFieldCount, 6));
        for (int i = 0; i < n; i++) {
            JsonObject c = ef.add<JsonObject>();
            c["label"]    = cfg.engineFields[i].label;
            c["pgn"]      = cfg.engineFields[i].pgn;
            c["unit"]     = cfg.engineFields[i].unit;
            c["decimals"] = cfg.engineFields[i].decimals;
        }
    }

    doc["depth"]["alarm"] = cfg.depthAlarm;
    doc["depth"]["unit"]  = cfg.depthUnit;
    doc["display"]["rotation"] = cfg.displayRotation;
    doc["display"]["coord_fmt"] = cfg.coordFormat;
    doc["display"]["coord_migrated"] = cfg.coordMigrated;

    doc["ais"]["range"]     = cfg.aisRange;
    doc["ais"]["north_up"]  = cfg.aisNorthUp;
    doc["ais"]["alarm"]     = cfg.aisAlarm;
    doc["ais"]["cpa"]       = cfg.aisCpaAlarm;
    doc["ais"]["tcpa"]      = cfg.aisTcpaAlarm;

    // Sensor calibration + N2K source pinning (see Config.h for semantics).
    doc["cal"]["depth_off"]    = cfg.calDepthOffsetM;
    doc["cal"]["awa_off"]      = cfg.calAwaOffsetDeg;
    doc["cal"]["aws_fac"]      = cfg.calAwsFactorPct;
    doc["cal"]["hdg_off"]      = cfg.calHdgOffsetDeg;
    doc["cal"]["stw_fac"]      = cfg.calStwFactorPct;
    doc["cal"]["roll_zero"]    = cfg.calRollZeroDeg;
    doc["cal"]["pitch_zero"]   = cfg.calPitchZeroDeg;
    doc["cal"]["rudder_zero"]  = cfg.calRudderZeroDeg;
    doc["cal"]["rudder_inv"]   = cfg.calRudderInvert;
    doc["cal"]["wtemp_off"]    = cfg.calWaterTempOffC;
    doc["cal"]["press_off"]    = cfg.calPressureOffHpa;
    // The four damping keys only - the two they replaced (wind_damp_spd/_dir)
    // are read by fromJson but never written again (WindDampCfg.h).
    doc["cal"]["wind_damp_app_spd"]  = cfg.calWindDampAppSpdS;
    doc["cal"]["wind_damp_app_dir"]  = cfg.calWindDampAppDirS;
    doc["cal"]["wind_damp_true_spd"] = cfg.calWindDampTrueSpdS;
    doc["cal"]["wind_damp_true_dir"] = cfg.calWindDampTrueDirS;
    doc["cal"]["twd_cog"]      = cfg.calTwdFromCog;
    doc["sources"]["pos"]   = cfg.srcPos;
    doc["sources"]["hdg"]   = cfg.srcHdg;
    doc["sources"]["wind"]  = cfg.srcWind;
    doc["sources"]["depth"] = cfg.srcDepth;
    doc["sources"]["stw"]   = cfg.srcStw;
    doc["sources"]["env"]   = cfg.srcEnv;
    doc["sources"]["att"]   = cfg.srcAtt;
    doc["sources"]["ap"]    = cfg.srcAp;

    doc["anchor"]["set"]    = cfg.anchorSet;
    doc["anchor"]["lat"]    = cfg.anchorLat;
    doc["anchor"]["lon"]    = cfg.anchorLon;
    doc["anchor"]["radius"] = cfg.anchorRadius;
    doc["anchor"]["alarm"]  = cfg.anchorAlarmOn;
    doc["anchor"]["north_up"] = cfg.anchorNorthUp;
    doc["anchor"]["set_utc"]  = cfg.anchorSetUtc;
    doc["anchor"]["rev_utc"]  = cfg.anchorRevUtc;
    doc["anchor"]["rev_node"] = cfg.anchorRevNode;

    {   // tank user config (only configured entries)
        JsonArray jt = doc["tankcfg"].to<JsonArray>();
        for (int i = 0; i < 6; i++) {
            if (!cfg.tankCfg[i].used) continue;
            const TankCfg &t = cfg.tankCfg[i];
            JsonObject o = jt.add<JsonObject>();
            o["inst"] = t.instance; o["ft"] = t.fluidType; o["name"] = t.name;
            if (!isnan(t.capacity)) o["cap"] = t.capacity;
            if (t.calN >= 2) {
                o["caln"] = t.calN;
                JsonArray pa = o["calp"].to<JsonArray>();
                JsonArray la = o["call"].to<JsonArray>();
                for (int k = 0; k < t.calN; k++) { pa.add(t.calPct[k]); la.add(t.calLit[k]); }
            }
        }
        JsonArray jb = doc["battcfg"].to<JsonArray>();
        for (int i = 0; i < 4; i++) {
            if (!cfg.battCfg[i].used) continue;
            const BatteryCfg &b = cfg.battCfg[i];
            JsonObject o = jb.add<JsonObject>();
            o["inst"] = b.instance; o["name"] = b.name; o["nv"] = b.nominalV;
            if (!isnan(b.capacityAh)) o["ah"] = b.capacityAh;
        }
    }

    doc["polar_file"] = cfg.polarFile;

    doc["demo_mode"]        = cfg.demoMode;
    doc["show_perf_overlay"] = cfg.showPerfOverlay;

    doc["sail"]["lwl_m"]          = cfg.waterlineLengthM;
    doc["sail"]["spi_type"]       = spiTypeName(cfg.spiType);
    // Legacy key, kept for a DOWNGRADE: firmware before 1.3.0 reads only this
    // bool, so after going back it still hides the kite ("none") or shows its
    // one spinnaker. Never consulted while "spi_type" is valid.
    doc["sail"]["spinnaker"]      = cfg.spiType != SpiType::None;
    doc["sail"]["code_zero"]      = cfg.allowCodeZero;
    doc["sail"]["foresail_pct"]   = cfg.foresailPercent;
    doc["sail"]["butterfly"]      = cfg.allowButterfly;
    doc["sail"]["nogo_deg"]       = cfg.noGoAngle;
    doc["sail"]["windlines_app"]  = cfg.windLinesApparent;
    doc["sail"]["wind_trace"]     = cfg.windTraceMode;
    doc["sail"]["compass_north_up"] = cfg.compassNorthUp;
    doc["sail"]["headsail_pct"]   = cfg.headsailSizePct;

    // Data-grid slots: serialise only ACTIVE slots (keeps config.json small),
    // each tagged with its slot index.
    JsonArray jgrids = doc["grids"].to<JsonArray>();
    for (int k = 0; k < MAX_GRIDS; k++) {
        if (!cfg.grids[k].active) continue;
        const GridConfig &g = cfg.grids[k];
        JsonObject jg = jgrids.add<JsonObject>();
        jg["slot"] = k;
        jg["name"] = g.name;
        jg["rows"] = g.rows;
        jg["cols"] = g.cols;
        jg["layout"] = g.layout;
        JsonArray cells = jg["cells"].to<JsonArray>();
        for (int i = 0; i < 9; i++) {
            JsonObject c = cells.add<JsonObject>();
            c["label"]    = g.cells[i].label;
            c["pgn"]      = g.cells[i].pgn;
            c["unit"]     = g.cells[i].unit;
            c["decimals"] = g.cells[i].decimals;
        }
    }

    // 7B right-hand sidebar: count + all cells (always serialized, so configs
    // stay portable between boards; the 4" simply never instantiates it).
    {
        JsonObject js = doc["sidebar"].to<JsonObject>();
        js["count"] = cfg.sidebar.count;
        JsonArray cells = js["cells"].to<JsonArray>();
        for (int i = 0; i < SIDEBAR_MAX_CELLS; i++) {
            JsonObject c = cells.add<JsonObject>();
            c["label"]    = cfg.sidebar.cells[i].label;
            c["pgn"]      = cfg.sidebar.cells[i].pgn;
            c["unit"]     = cfg.sidebar.cells[i].unit;
            c["decimals"] = cfg.sidebar.cells[i].decimals;
        }
    }

    // Theme: active variant + dark/light colour palettes.
    doc["theme"]["active"] = cfg.themeActive;
    doc["theme"]["auto"]   = cfg.themeAuto;
    auto writeColors = [](JsonObject o, const ThemeColors &c) {
        #define X(n,d,l) o[#n]=c.n;
        THEME_COLOR_FIELDS(X)
        #undef X
    };
    writeColors(doc["theme"]["dark"].to<JsonObject>(),  cfg.themeDark);
    writeColors(doc["theme"]["light"].to<JsonObject>(), cfg.themeLight);
    writeColors(doc["theme"]["night"].to<JsonObject>(), cfg.themeNight);
    {
        JsonObject jsz = doc["theme"]["sizes"].to<JsonObject>();
        #define X(n,d) jsz[#n] = cfg.themeSizes.n;
        THEME_SIZE_FIELDS(X)
        #undef X
    }
    {
        JsonObject jf = doc["theme"]["fonts"].to<JsonObject>();
        #define X(r,d) jf[#r] = cfg.themeFonts.r;
        THEME_FONT_FIELDS(X)
        THEME_BIGFONT_FIELDS(X)
        #undef X
    }

    String out;
    // One up-front allocation: >=4 KB goes to PSRAM automatically
    // (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096), and the ~380 realloc-grow
    // steps of serializeJsonPretty - each a chance to fail silently on a
    // tight heap - disappear.
    out.reserve(16384);
    serializeJsonPretty(doc, out);
    return out;
}

// A tank or battery name of nothing but blanks is no name - the same rule as
// the web page, which trims names (saveDisplay() and buildScreenList() in
// index.html). Used where fromJson() merges an entry listed twice.
static bool blankName(const char *s) {
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    return !*s;
}

bool Config::fromJson(const String &json) {
    CFG_JSON_DOC();
    DeserializationError err = deserializeJson(doc, json);
    if (err) { Serial.printf("Config JSON error: %s\n", err.c_str()); return false; }

    if (doc["wifi"]["ssid"].is<const char*>())     strlcpy(cfg.wifiSsid,     doc["wifi"]["ssid"],     sizeof(cfg.wifiSsid));
    if (doc["wifi"]["password"].is<const char*>()) strlcpy(cfg.wifiPassword, doc["wifi"]["password"], sizeof(cfg.wifiPassword));
    cfg.apMode     = doc["wifi"]["ap_mode"]  | cfg.apMode;
    cfg.wifiEnabled= doc["wifi"]["enabled"]  | cfg.wifiEnabled;
    // WiFi country: a code from the list (WifiCountry.h) is taken, anything
    // else becomes "" - the default - so no unknown code ever reaches the WiFi
    // library. A patch without the key (an older page) leaves it alone.
    if (doc["wifi"]["country"].is<const char*>()) {
        const char *cc = doc["wifi"]["country"];
        if (wifiCountryValid(cc)) strlcpy(cfg.wifiCountry, cc, sizeof(cfg.wifiCountry));
        else                      cfg.wifiCountry[0] = 0;
    }
    if (doc["wifi"]["ap_pass"].is<const char*>()) strlcpy(cfg.apPass, doc["wifi"]["ap_pass"], sizeof(cfg.apPass));
    cfg.licenseAccepted = doc["license_ok"] | cfg.licenseAccepted;
    if (doc["ui"]["lang"].is<const char*>()) strlcpy(cfg.lang, doc["ui"]["lang"], sizeof(cfg.lang));
    // Icon set: anything but "classic" is the default, OpenBridge - an unknown value
    // can never leave the panel without symbols.
    if (doc["ui"]["icons"].is<const char*>())
        cfg.iconSet = (strcmp(doc["ui"]["icons"].as<const char*>(), "classic") == 0)
                      ? ICON_SET_CLASSIC : ICON_SET_OPENBRIDGE;
    cfg.iconsMigrated = doc["ui"]["icons_migrated"] | cfg.iconsMigrated;
    if (doc["boot"]["title"].is<const char*>())
        strlcpy(cfg.bootTitle, doc["boot"]["title"], sizeof(cfg.bootTitle));
    if (doc["boot"]["name"].is<const char*>())
        strlcpy(cfg.bootName, doc["boot"]["name"], sizeof(cfg.bootName));
    // Legacy: the web UI used to POST a flat "boot_name" that nothing ever read.
    if (doc["boot_name"].is<const char*>())
        strlcpy(cfg.bootName, doc["boot_name"], sizeof(cfg.bootName));
    // Keep the i18n layer in step with whatever we just loaded. An unknown code
    // resolves to German rather than leaving the UI in a half-set state.
    i18nSetLang(i18nLangFromCode(cfg.lang));
    strlcpy(cfg.lang, i18nLangCode(i18nLang()), sizeof(cfg.lang));

    cfg.brightness = doc["display"]["brightness"] | cfg.brightness;
    if (doc["display"]["screens"].is<JsonArray>()) {
        // New format: ordered list of {id, en}.
        JsonArray arr = doc["display"]["screens"].as<JsonArray>();
        int n = 0;
        for (JsonObject s : arr) {
            if (n >= MAX_SCREENS) break;
            int id = s["id"] | -1;
            if (id < 0 || id >= MAX_SCREENS) continue;
            cfg.screenOrder[n++]   = (uint8_t)id;
            cfg.screenEnabled[id]  = s["en"] | true;
        }
        if (n > 0) cfg.screenCount = n;
    } else if (doc["display"]["screen_order"].is<JsonArray>()) {
        // Legacy format: order only, all screens enabled.
        JsonArray arr = doc["display"]["screen_order"].as<JsonArray>();
        cfg.screenCount = min((int)arr.size(), MAX_SCREENS);
        for (int i = 0; i < cfg.screenCount; i++) cfg.screenOrder[i] = arr[i].as<uint8_t>();
        for (int i = 0; i < MAX_SCREENS; i++) cfg.screenEnabled[i] = true;
    }

    cfg.canTxPin = doc["nmea2000"]["can_tx"] | cfg.canTxPin;
    cfg.canRxPin = doc["nmea2000"]["can_rx"] | cfg.canRxPin;
    // MIGRATION. Until now these two values were stored and served but never
    // read by anything - the pins came only from the compile-time BoardConfig
    // defines. So every config in the field carries one of two historical
    // pairs that nobody chose: 17/18 (the old struct default, correct on no
    // board at all) or 6/0 (the 4-inch pins, which data/config.json shipped to
    // every board). Now that the values ARE honoured, taking them at face
    // value would silence the bus on every existing device the moment this
    // firmware lands. Both pairs therefore mean "not configured".
    // Consequence worth knowing: a modified 4-inch board cannot pin 6/0
    // explicitly - but it does not need to, since that is what BoardConfig
    // resolves to there anyway.
    if ((cfg.canTxPin == 17 && cfg.canRxPin == 18) ||
        (cfg.canTxPin ==  6 && cfg.canRxPin ==  0)) {
        cfg.canTxPin = -1;
        cfg.canRxPin = -1;
    }
    cfg.n2kListenOnly = doc["nmea2000"]["listen_only"] | cfg.n2kListenOnly;
    cfg.n2kAcceptUpdates = doc["nmea2000"]["accept_updates"] | cfg.n2kAcceptUpdates;
    cfg.tideBshFetch  = doc["tide"]["bsh_fetch"]       | cfg.tideBshFetch;

    cfg.engine.rpmIdle    = doc["engine"]["rpm_idle"]     | cfg.engine.rpmIdle;
    cfg.engine.rpmCruise  = doc["engine"]["rpm_cruise"]   | cfg.engine.rpmCruise;
    cfg.engine.rpmMaxCont = doc["engine"]["rpm_max_cont"] | cfg.engine.rpmMaxCont;
    cfg.engine.rpmMax     = doc["engine"]["rpm_max"]      | cfg.engine.rpmMax;
    cfg.engineFieldCount  = doc["engine"]["field_count"]  | cfg.engineFieldCount;
    cfg.engineFieldCount  = max(1, min(cfg.engineFieldCount, 6));
    if (doc["engine"]["fields"].is<JsonArray>()) {
        JsonArrayConst arr = doc["engine"]["fields"].as<JsonArrayConst>();
        for (int i = 0; i < 6 && i < (int)arr.size(); i++) {
            JsonObjectConst c = arr[i].as<JsonObjectConst>();
            strlcpy(cfg.engineFields[i].label, c["label"] | cfg.engineFields[i].label, 16);
            strlcpy(cfg.engineFields[i].pgn,   c["pgn"]   | cfg.engineFields[i].pgn,   16);
            strlcpy(cfg.engineFields[i].unit,  c["unit"]  | cfg.engineFields[i].unit,   8);
            cfg.engineFields[i].decimals = c["decimals"] | cfg.engineFields[i].decimals;
        }
    }

    cfg.depthAlarm   = doc["depth"]["alarm"]   | cfg.depthAlarm;
    if (doc["depth"]["unit"].is<const char*>()) strlcpy(cfg.depthUnit, doc["depth"]["unit"], 4);
    cfg.aisRange     = doc["ais"]["range"]     | cfg.aisRange;
    cfg.aisNorthUp   = doc["ais"]["north_up"]  | cfg.aisNorthUp;
    cfg.aisAlarm     = doc["ais"]["alarm"]     | cfg.aisAlarm;
    cfg.aisCpaAlarm  = doc["ais"]["cpa"]       | cfg.aisCpaAlarm;
    // Only the four right angles are legal; anything else falls back to 0
    // rather than handing LVGL an undefined rotation.
    {
        uint16_t r = doc["display"]["rotation"] | cfg.displayRotation;
        cfg.displayRotation = (r == 90 || r == 180 || r == 270) ? r : 0;
    }
    cfg.coordFormat = doc["display"]["coord_fmt"] | cfg.coordFormat;
    if (cfg.coordFormat > 2) cfg.coordFormat = 1;      // unknown value = the default
    cfg.coordMigrated = doc["display"]["coord_migrated"] | cfg.coordMigrated;
    cfg.aisTcpaAlarm = doc["ais"]["tcpa"]      | cfg.aisTcpaAlarm;

    // Sensor calibration + N2K source pinning (partial patches merge).
    cfg.calDepthOffsetM   = doc["cal"]["depth_off"]   | cfg.calDepthOffsetM;
    cfg.calAwaOffsetDeg   = doc["cal"]["awa_off"]     | cfg.calAwaOffsetDeg;
    cfg.calAwsFactorPct   = doc["cal"]["aws_fac"]     | cfg.calAwsFactorPct;
    cfg.calHdgOffsetDeg   = doc["cal"]["hdg_off"]     | cfg.calHdgOffsetDeg;
    cfg.calStwFactorPct   = doc["cal"]["stw_fac"]     | cfg.calStwFactorPct;
    cfg.calRollZeroDeg    = doc["cal"]["roll_zero"]   | cfg.calRollZeroDeg;
    cfg.calPitchZeroDeg   = doc["cal"]["pitch_zero"]  | cfg.calPitchZeroDeg;
    cfg.calRudderZeroDeg  = doc["cal"]["rudder_zero"] | cfg.calRudderZeroDeg;
    cfg.calRudderInvert   = doc["cal"]["rudder_inv"]  | cfg.calRudderInvert;
    cfg.calWaterTempOffC  = doc["cal"]["wtemp_off"]   | cfg.calWaterTempOffC;
    cfg.calPressureOffHpa = doc["cal"]["press_off"]   | cfg.calPressureOffHpa;
    {
        // Wind damping: the four keys, the two old ones as a stand-in for any
        // of them that is missing (a config saved before the split), clamped
        // to the limits; a key that is missing or not a number leaves the
        // value as it was, so a patch touches only what it names. The rules
        // are WindDampCfg::apply(), which the host tests check.
        WindDampCfg::Keys k;
        k.appSpd  = doc["cal"]["wind_damp_app_spd"]  | (float)NAN;
        k.appDir  = doc["cal"]["wind_damp_app_dir"]  | (float)NAN;
        k.trueSpd = doc["cal"]["wind_damp_true_spd"] | (float)NAN;
        k.trueDir = doc["cal"]["wind_damp_true_dir"] | (float)NAN;
        k.oldSpd  = doc["cal"]["wind_damp_spd"]      | (float)NAN;
        k.oldDir  = doc["cal"]["wind_damp_dir"]      | (float)NAN;
        const WindCalc::WindDamping cur = { cfg.calWindDampAppSpdS, cfg.calWindDampAppDirS,
                                            cfg.calWindDampTrueSpdS, cfg.calWindDampTrueDirS };
        const WindCalc::WindDamping d = WindDampCfg::apply(cur, k);
        cfg.calWindDampAppSpdS  = d.appSpd;
        cfg.calWindDampAppDirS  = d.appDir;
        cfg.calWindDampTrueSpdS = d.trueSpd;
        cfg.calWindDampTrueDirS = d.trueDir;
    }
    cfg.calTwdFromCog     = doc["cal"]["twd_cog"]     | cfg.calTwdFromCog;
    cfg.srcPos   = doc["sources"]["pos"]   | cfg.srcPos;
    cfg.srcHdg   = doc["sources"]["hdg"]   | cfg.srcHdg;
    cfg.srcWind  = doc["sources"]["wind"]  | cfg.srcWind;
    cfg.srcDepth = doc["sources"]["depth"] | cfg.srcDepth;
    cfg.srcStw   = doc["sources"]["stw"]   | cfg.srcStw;
    cfg.srcEnv   = doc["sources"]["env"]   | cfg.srcEnv;
    cfg.srcAtt   = doc["sources"]["att"]   | cfg.srcAtt;
    cfg.srcAp    = doc["sources"]["ap"]    | cfg.srcAp;

    cfg.anchorSet     = doc["anchor"]["set"]    | cfg.anchorSet;
    cfg.anchorLat     = doc["anchor"]["lat"]    | cfg.anchorLat;
    cfg.anchorLon     = doc["anchor"]["lon"]    | cfg.anchorLon;
    cfg.anchorRadius  = doc["anchor"]["radius"] | cfg.anchorRadius;
    cfg.anchorAlarmOn = doc["anchor"]["alarm"]  | cfg.anchorAlarmOn;
    cfg.anchorNorthUp = doc["anchor"]["north_up"] | cfg.anchorNorthUp;
    cfg.anchorSetUtc  = doc["anchor"]["set_utc"]  | cfg.anchorSetUtc;
    cfg.anchorRevUtc  = doc["anchor"]["rev_utc"]  | cfg.anchorRevUtc;
    cfg.anchorRevNode = doc["anchor"]["rev_node"] | cfg.anchorRevNode;

    // A calibration is taken whole or not at all: 2 to 6 points, as many in
    // both lists, the level rising, the litres not falling and ending above
    // where they start. An imported or hand-edited file got here unchecked -
    // a caln of -1 became 255 and liters() read past the arrays, missing
    // points became 0 and a cut-off table lost its 100 % end.
    auto loadTankCal = [](TankCfg &t, JsonObjectConst o) {
        const int cn = o["caln"] | 0;
        t.calN = 0;
        if (cn >= 2 && cn <= 6 && o["calp"].is<JsonArrayConst>() && o["call"].is<JsonArrayConst>()) {
            JsonArrayConst pa = o["calp"].as<JsonArrayConst>();
            JsonArrayConst la = o["call"].as<JsonArrayConst>();
            bool ok = (int)pa.size() >= cn && (int)la.size() >= cn;
            for (int k = 0; ok && k < cn; k++) {
                const float pc = pa[k] | (float)NAN, li = la[k] | (float)NAN;
                ok = !isnan(pc) && !isnan(li) && li >= 0.f
                     && (k == 0 || (pc > t.calPct[k - 1] && li >= t.calLit[k - 1]));
                t.calPct[k] = pc; t.calLit[k] = li;
            }
            if (ok && t.calLit[cn - 1] > t.calLit[0]) t.calN = (uint8_t)cn;
        }
    };
    // One entry per tank - (instance, fluid) - and per battery bank - instance -,
    // the keys the panel (TankScreen, BatteryScreen) and the web page match on.
    // A file edited by hand can list one twice. Index i used to go into slot i,
    // so the page showed one row for both and its next save dropped the other's
    // name for good, and a duplicate pushed a real tank or bank past the cut-off.
    // Now the first entry wins, and what it left empty is filled from the later
    // one: a name of only blanks (blankName()), no capacity, a bank at 12 V, a
    // tank without a calibration (the group rule below). The page applies the
    // same rules to a config that still holds duplicates (buildScreenList()).
    // ORDER MATTERS: the duplicate search comes BEFORE the 6 / 4 cut-off, so a
    // duplicate listed after the 6th tank or the 4th bank - e.g. a bank's name
    // in its 5th entry - still reaches its first entry.
    // The merges are logged as ONE line per list, after the loop: the loop now
    // reads every element, and an import of up to 32 KB (POST_BODY_MAX) of
    // repeated entries, logged one by one, held the web server's task on the
    // 7B's 115200-baud console for up to a minute. The line counts the repeats
    // (entries merged into an earlier one) and names the first repeat in the
    // list - not necessarily the first entry that has one: [2, 1, 1, 2] names 1.
    if (doc["tankcfg"].is<JsonArrayConst>()) {
        for (int i = 0; i < 6; i++) cfg.tankCfg[i] = TankCfg{};
        int n = 0, merged = 0;
        unsigned firstInst = 0, firstFt = 0;
        for (JsonObjectConst o : doc["tankcfg"].as<JsonArrayConst>()) {
            const uint8_t inst = o["inst"] | 0, ft = o["ft"] | 0xFF;
            TankCfg *t = nullptr;
            for (int k = 0; k < n && !t; k++)
                if (cfg.tankCfg[k].instance == inst && cfg.tankCfg[k].fluidType == ft) t = &cfg.tankCfg[k];
            if (t) {
                const char *nm = o["name"] | "";
                if (blankName(t->name) && !blankName(nm)) strlcpy(t->name, nm, sizeof(t->name));
                // Capacity and calibration as ONE group: each entry's pair agrees
                // with itself (the page takes a table's 100 % point from the
                // capacity), halves from two entries need not - 150 L with a
                // table ending at 200 L reads "200/150 L" on the panel, and the
                // page's next save refuses the table. So a half is taken only
                // where it matches the half the first entry has.
                const bool hasCap = !isnan(t->capacity), hasCal = t->calN >= 2;
                const float cap = o["cap"] | (float)NAN;
                if (!hasCap && !hasCal) {
                    t->capacity = cap;
                    loadTankCal(*t, o);
                } else if (!hasCap) {
                    if (!isnan(cap) && cap == t->calLit[t->calN - 1]) t->capacity = cap;
                } else if (!hasCal) {
                    loadTankCal(*t, o);
                    if (t->calN >= 2 && t->calLit[t->calN - 1] != t->capacity) t->calN = 0;
                }
                if (!merged++) { firstInst = inst; firstFt = ft; }
                continue;
            }
            if (n >= 6) continue;   // as before: what does not fit is dropped
            t = &cfg.tankCfg[n++];
            t->used = true;
            t->instance  = inst;
            t->fluidType = ft;
            strlcpy(t->name, o["name"] | "", sizeof(t->name));
            t->capacity  = o["cap"]  | (float)NAN;
            loadTankCal(*t, o);
        }
        if (merged)
            Serial.printf("[cfg] tankcfg: %d duplicate %s merged (first: tank %u/%u)\n",
                          merged, merged == 1 ? "entry" : "entries", firstInst, firstFt);
    }
    if (doc["battcfg"].is<JsonArrayConst>()) {
        for (int i = 0; i < 4; i++) cfg.battCfg[i] = BatteryCfg{};
        int n = 0, merged = 0;
        unsigned firstInst = 0;
        for (JsonObjectConst o : doc["battcfg"].as<JsonArrayConst>()) {
            const uint8_t inst = o["inst"] | 0;
            BatteryCfg *b = nullptr;
            for (int k = 0; k < n && !b; k++)
                if (cfg.battCfg[k].instance == inst) b = &cfg.battCfg[k];
            if (b) {
                const char *nm = o["name"] | "";
                if (blankName(b->name) && !blankName(nm)) strlcpy(b->name, nm, sizeof(b->name));
                if (isnan(b->capacityAh)) b->capacityAh = o["ah"] | (float)NAN;
                if (b->nominalV == 12)    b->nominalV   = o["nv"] | 12;
                if (!merged++) firstInst = inst;
                continue;
            }
            if (n >= 4) continue;   // as before: what does not fit is dropped
            b = &cfg.battCfg[n++];
            b->used = true;
            b->instance   = inst;
            strlcpy(b->name, o["name"] | "", sizeof(b->name));
            b->capacityAh = o["ah"]   | (float)NAN;
            b->nominalV   = o["nv"]   | 12;
        }
        if (merged)
            Serial.printf("[cfg] battcfg: %d duplicate %s merged (first: bank %u)\n",
                          merged, merged == 1 ? "entry" : "entries", firstInst);
    }

    if (doc["polar_file"].is<const char*>()) strlcpy(cfg.polarFile, doc["polar_file"], sizeof(cfg.polarFile));

    cfg.demoMode          = doc["demo_mode"]              | cfg.demoMode;
    cfg.showPerfOverlay   = doc["show_perf_overlay"]      | cfg.showPerfOverlay;

    cfg.waterlineLengthM  = doc["sail"]["lwl_m"]        | cfg.waterlineLengthM;
    // Downwind sail: a valid "spi_type" wins. Without one (a config, export
    // or web page from before 1.3.0, or a type this firmware does not know)
    // the legacy bool decides: false = none, true = the balloon, i.e. the one
    // spinnaker there used to be. A "spi_type" this firmware does not know
    // and no legacy bool (an export from a newer firmware that has dropped
    // it) is the balloon as well - the same answer the web page gives for
    // that document (spiTypeOf() in index.html). Neither key at all keeps
    // the current value, like every other key here: on a load from flash
    // that is the Balloon default, and a partial POST that does not touch
    // the sail must not reset what the user picked.
    {
        SpiType t;
        if (spiTypeFromName(doc["sail"]["spi_type"].as<const char*>(), t))
            cfg.spiType = t;
        else if (doc["sail"]["spinnaker"].is<bool>())
            cfg.spiType = doc["sail"]["spinnaker"].as<bool>() ? SpiType::Balloon : SpiType::None;
        else if (!doc["sail"]["spi_type"].isNull())
            cfg.spiType = SpiType::Balloon;
    }
    cfg.allowCodeZero     = doc["sail"]["code_zero"]     | cfg.allowCodeZero;
    cfg.foresailPercent   = doc["sail"]["foresail_pct"]  | cfg.foresailPercent;
    cfg.allowButterfly    = doc["sail"]["butterfly"]     | cfg.allowButterfly;
    cfg.noGoAngle         = doc["sail"]["nogo_deg"]      | cfg.noGoAngle;
    cfg.windLinesApparent = doc["sail"]["windlines_app"] | cfg.windLinesApparent;
    cfg.windTraceMode     = doc["sail"]["wind_trace"]    | cfg.windTraceMode;
    if (cfg.windTraceMode > 3) cfg.windTraceMode = 0;   // unknown value = off
    // Missing key keeps the current value, so the WebUI's sail editor - which
    // posts only the keys it knows - cannot reset the card orientation the user
    // last picked on the panel.
    cfg.compassNorthUp    = doc["sail"]["compass_north_up"] | cfg.compassNorthUp;
    cfg.headsailSizePct   = doc["sail"]["headsail_pct"]  | cfg.headsailSizePct;

    // Helper to fill one GridConfig from a JSON object {name,rows,cols,cells[]}.
    auto loadGrid = [](GridConfig &g, JsonObjectConst jg, const char *defName) {
        g.active = true;
        strlcpy(g.name, jg["name"] | defName, sizeof(g.name));
        g.rows = jg["rows"] | 3;
        g.cols = jg["cols"] | 3;
        strlcpy(g.layout, jg["layout"] | "", sizeof(g.layout));
        JsonArrayConst arr = jg["cells"].as<JsonArrayConst>();
        for (int i = 0; i < 9; i++) {
            JsonObjectConst c = (i < (int)arr.size()) ? arr[i].as<JsonObjectConst>() : JsonObjectConst();
            strlcpy(g.cells[i].label, c["label"] | "", 16);
            strlcpy(g.cells[i].pgn,   c["pgn"]   | "", 16);
            strlcpy(g.cells[i].unit,  c["unit"]  | "",  8);
            g.cells[i].decimals = c["decimals"] | 1;
        }
    };

    if (doc["grids"].is<JsonArray>()) {
        // New format: list of active slots, each with a "slot" index.
        for (int k = 0; k < MAX_GRIDS; k++) cfg.grids[k].active = false;
        for (JsonObjectConst jg : doc["grids"].as<JsonArrayConst>()) {
            int k = jg["slot"] | -1;
            if (k < 0 || k >= MAX_GRIDS) continue;
            loadGrid(cfg.grids[k], jg, "Datenraster");
        }
    } else if (doc["grid"].is<JsonObject>()) {
        // Legacy migration: single "grid" object -> slot 0.
        loadGrid(cfg.grids[0], doc["grid"].as<JsonObjectConst>(), "Datenraster 1");
    }

    // 7B sidebar. When the block is present, the posted cells REPLACE the
    // stored ones up to the array length and the remainder is cleared -
    // otherwise shrinking the count and re-growing it would resurrect stale
    // cell contents (the engine-fields loader has exactly that quirk).
    if (doc["sidebar"].is<JsonObject>()) {
        JsonObjectConst js = doc["sidebar"].as<JsonObjectConst>();
        int cnt = js["count"] | (int)cfg.sidebar.count;
        if (cnt < 1) cnt = 1;
        if (cnt > SIDEBAR_MAX_CELLS) cnt = SIDEBAR_MAX_CELLS;
        cfg.sidebar.count = (uint8_t)cnt;
        if (js["cells"].is<JsonArrayConst>()) {
            int i = 0;
            for (JsonObjectConst c : js["cells"].as<JsonArrayConst>()) {
                if (i >= SIDEBAR_MAX_CELLS) break;
                GridCell &g = cfg.sidebar.cells[i];
                strlcpy(g.label, c["label"] | "", sizeof(g.label));
                strlcpy(g.pgn,   c["pgn"]   | "", sizeof(g.pgn));
                strlcpy(g.unit,  c["unit"]  | "", sizeof(g.unit));
                g.decimals = c["decimals"] | 1;
                i++;
            }
            for (; i < SIDEBAR_MAX_CELLS; i++) cfg.sidebar.cells[i] = GridCell{};
        }
    }

    // Theme
    if (doc["theme"].is<JsonObject>()) {
        if (doc["theme"]["active"].is<const char*>())
            strlcpy(cfg.themeActive, doc["theme"]["active"], sizeof(cfg.themeActive));
        cfg.themeAuto = doc["theme"]["auto"] | cfg.themeAuto;
        auto readColors = [](JsonObjectConst o, ThemeColors &c) {
            if (o.isNull()) return;
            #define X(n,d,l) c.n = o[#n] | c.n;
            THEME_COLOR_FIELDS(X)
            #undef X
        };
        readColors(doc["theme"]["dark"].as<JsonObjectConst>(),  cfg.themeDark);
        readColors(doc["theme"]["light"].as<JsonObjectConst>(), cfg.themeLight);
        readColors(doc["theme"]["night"].as<JsonObjectConst>(), cfg.themeNight);
        JsonObjectConst jsz = doc["theme"]["sizes"].as<JsonObjectConst>();
        if (!jsz.isNull()) {
            #define X(n,d) cfg.themeSizes.n = jsz[#n] | cfg.themeSizes.n;
            THEME_SIZE_FIELDS(X)
            #undef X
        }
        JsonObjectConst jf = doc["theme"]["fonts"].as<JsonObjectConst>();
        if (!jf.isNull()) {
            #define X(r,d) cfg.themeFonts.r = jf[#r] | cfg.themeFonts.r;
            THEME_FONT_FIELDS(X)
            THEME_BIGFONT_FIELDS(X)
            #undef X
        }
    }
    return true;
}

// Stub - an unrelated idea for pushing the whole config over the bus; never
// built. The example PGN it used to name was 130900, which now reads like the
// anchor sync and is not: that one lives in AnchorSync.h and has since moved.
void Config::sendViaN2k()                                   {}
bool Config::receiveViaN2k(const uint8_t *d, size_t len)   { return false; }
