#include "Config.h"
#include "../i18n/I18n.h"
#include "../Entropy.h"

Config appConfig;

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
    // Ensure the hotspot password BEFORE WiFi starts (setup() calls
    // webCfg.begin() after Config::begin()). On the very first start — or when
    // updating an existing device — the field is empty.
    if (!cfg.apPass[0]) {
        Entropy::generateApPassword(cfg.apPass, sizeof(cfg.apPass));
        save();
        Serial.println("[wifi] Hotspot-Passwort erzeugt (Zufall, nicht MAC-abgeleitet)");
    }
    return ok;
}

bool Config::load() {
    if (!LittleFS.exists(PATH)) return true;  // use defaults
    File f = LittleFS.open(PATH, "r");
    if (!f) return false;
    String json = f.readString();
    f.close();
    return fromJson(json);
}

bool Config::save() {
    File f = LittleFS.open(PATH, "w");
    if (!f) return false;
    String json = toJson();
    f.print(json);
    f.close();
    return true;
}

String Config::toJson() const {
    JsonDocument doc;
    doc["wifi"]["ssid"]     = cfg.wifiSsid;
    doc["wifi"]["password"] = cfg.wifiPassword;
    doc["wifi"]["ap_mode"]  = cfg.apMode;
    doc["wifi"]["enabled"]  = cfg.wifiEnabled;
    doc["wifi"]["ap_pass"]  = cfg.apPass;
    doc["license_ok"]       = cfg.licenseAccepted;
    doc["ui"]["lang"]       = cfg.lang;
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

    doc["ais"]["range"]     = cfg.aisRange;
    doc["ais"]["alarm"]     = cfg.aisAlarm;
    doc["ais"]["cpa"]       = cfg.aisCpaAlarm;
    doc["ais"]["tcpa"]      = cfg.aisTcpaAlarm;

    doc["anchor"]["set"]    = cfg.anchorSet;
    doc["anchor"]["lat"]    = cfg.anchorLat;
    doc["anchor"]["lon"]    = cfg.anchorLon;
    doc["anchor"]["radius"] = cfg.anchorRadius;
    doc["anchor"]["alarm"]  = cfg.anchorAlarmOn;
    doc["anchor"]["north_up"] = cfg.anchorNorthUp;

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
    doc["sail"]["spinnaker"]      = cfg.allowSpinnaker;
    doc["sail"]["code_zero"]      = cfg.allowCodeZero;
    doc["sail"]["foresail_pct"]   = cfg.foresailPercent;
    doc["sail"]["butterfly"]      = cfg.allowButterfly;
    doc["sail"]["nogo_deg"]       = cfg.noGoAngle;
    doc["sail"]["windlines_app"]  = cfg.windLinesApparent;
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
    serializeJsonPretty(doc, out);
    return out;
}

bool Config::fromJson(const String &json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) { Serial.printf("Config JSON error: %s\n", err.c_str()); return false; }

    if (doc["wifi"]["ssid"].is<const char*>())     strlcpy(cfg.wifiSsid,     doc["wifi"]["ssid"],     sizeof(cfg.wifiSsid));
    if (doc["wifi"]["password"].is<const char*>()) strlcpy(cfg.wifiPassword, doc["wifi"]["password"], sizeof(cfg.wifiPassword));
    cfg.apMode     = doc["wifi"]["ap_mode"]  | cfg.apMode;
    cfg.wifiEnabled= doc["wifi"]["enabled"]  | cfg.wifiEnabled;
    if (doc["wifi"]["ap_pass"].is<const char*>()) strlcpy(cfg.apPass, doc["wifi"]["ap_pass"], sizeof(cfg.apPass));
    cfg.licenseAccepted = doc["license_ok"] | cfg.licenseAccepted;
    if (doc["ui"]["lang"].is<const char*>()) strlcpy(cfg.lang, doc["ui"]["lang"], sizeof(cfg.lang));
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
    cfg.n2kListenOnly = doc["nmea2000"]["listen_only"] | cfg.n2kListenOnly;

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
    cfg.aisAlarm     = doc["ais"]["alarm"]     | cfg.aisAlarm;
    cfg.aisCpaAlarm  = doc["ais"]["cpa"]       | cfg.aisCpaAlarm;
    cfg.aisTcpaAlarm = doc["ais"]["tcpa"]      | cfg.aisTcpaAlarm;

    cfg.anchorSet     = doc["anchor"]["set"]    | cfg.anchorSet;
    cfg.anchorLat     = doc["anchor"]["lat"]    | cfg.anchorLat;
    cfg.anchorLon     = doc["anchor"]["lon"]    | cfg.anchorLon;
    cfg.anchorRadius  = doc["anchor"]["radius"] | cfg.anchorRadius;
    cfg.anchorAlarmOn = doc["anchor"]["alarm"]  | cfg.anchorAlarmOn;
    cfg.anchorNorthUp = doc["anchor"]["north_up"] | cfg.anchorNorthUp;

    if (doc["tankcfg"].is<JsonArrayConst>()) {
        for (int i = 0; i < 6; i++) cfg.tankCfg[i] = TankCfg{};
        JsonArrayConst arr = doc["tankcfg"].as<JsonArrayConst>();
        for (int i = 0; i < 6 && i < (int)arr.size(); i++) {
            JsonObjectConst o = arr[i].as<JsonObjectConst>();
            TankCfg &t = cfg.tankCfg[i];
            t.used = true;
            t.instance  = o["inst"] | 0;
            t.fluidType = o["ft"]   | 0xFF;
            strlcpy(t.name, o["name"] | "", sizeof(t.name));
            t.capacity  = o["cap"]  | (float)NAN;
            int cn = o["caln"] | 0; if (cn > 6) cn = 6; t.calN = (uint8_t)cn;
            if (o["calp"].is<JsonArrayConst>() && o["call"].is<JsonArrayConst>()) {
                JsonArrayConst pa = o["calp"].as<JsonArrayConst>();
                JsonArrayConst la = o["call"].as<JsonArrayConst>();
                for (int k = 0; k < cn; k++) { t.calPct[k] = pa[k] | 0.f; t.calLit[k] = la[k] | 0.f; }
            }
        }
    }
    if (doc["battcfg"].is<JsonArrayConst>()) {
        for (int i = 0; i < 4; i++) cfg.battCfg[i] = BatteryCfg{};
        JsonArrayConst arr = doc["battcfg"].as<JsonArrayConst>();
        for (int i = 0; i < 4 && i < (int)arr.size(); i++) {
            JsonObjectConst o = arr[i].as<JsonObjectConst>();
            BatteryCfg &b = cfg.battCfg[i];
            b.used = true;
            b.instance   = o["inst"] | 0;
            strlcpy(b.name, o["name"] | "", sizeof(b.name));
            b.capacityAh = o["ah"]   | (float)NAN;
            b.nominalV   = o["nv"]   | 12;
        }
    }

    if (doc["polar_file"].is<const char*>()) strlcpy(cfg.polarFile, doc["polar_file"], sizeof(cfg.polarFile));

    cfg.demoMode          = doc["demo_mode"]              | cfg.demoMode;
    cfg.showPerfOverlay   = doc["show_perf_overlay"]      | cfg.showPerfOverlay;

    cfg.waterlineLengthM  = doc["sail"]["lwl_m"]        | cfg.waterlineLengthM;
    cfg.allowSpinnaker    = doc["sail"]["spinnaker"]     | cfg.allowSpinnaker;
    cfg.allowCodeZero     = doc["sail"]["code_zero"]     | cfg.allowCodeZero;
    cfg.foresailPercent   = doc["sail"]["foresail_pct"]  | cfg.foresailPercent;
    cfg.allowButterfly    = doc["sail"]["butterfly"]     | cfg.allowButterfly;
    cfg.noGoAngle         = doc["sail"]["nogo_deg"]      | cfg.noGoAngle;
    cfg.windLinesApparent = doc["sail"]["windlines_app"] | cfg.windLinesApparent;
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

// Stub – implement with a proprietary N2K PGN (e.g. 130900)
void Config::sendViaN2k()                                   {}
bool Config::receiveViaN2k(const uint8_t *d, size_t len)   { return false; }
