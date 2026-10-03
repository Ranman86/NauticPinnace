#include "DisplayManager.h"
#include "Icons.h"   // OpenBridge icon trial
#include "../nmea/AnchorSyncNode.h"
#include "SideBar.h"
#include "Theme.h"
#include "../i18n/I18n.h"
#include "../config/Config.h"
#include "../PolarTable.h"
#include "../DisplaySetup.h"
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include "screens/WindScreen.h"
#include "screens/SpeedScreen.h"
#include "screens/DepthScreen.h"
#include "screens/EngineScreen.h"
#include "screens/RudderScreen.h"
#include "screens/AisScreen.h"
#include "screens/WindPlotScreen.h"
#include "screens/AutopilotScreen.h"
#include "screens/GridScreen.h"
#include "screens/MediaScreen.h"
#include "screens/AnchorScreen.h"
#include "screens/TankScreen.h"
#include "screens/BatteryScreen.h"
#include "screens/WeatherScreen.h"
#include "screens/ClockScreen.h"
#include "screens/VmgScreen.h"
#include "screens/RouteScreen.h"
#include "../SunCalc.h"
#include "ConfigOverlay.h"
#include "HomeOverlay.h"
#include "LicenseOverlay.h"
#include "LanguageOverlay.h"
#include <string.h>

DisplayManager dispMgr;

#if defined(BOARD_PANEL_1024X600)
// Where the 600x600 instrument block sits, per orientation. Landscape puts the
// three blocks side by side (rail | instrument | sidebar), portrait stacks them
// (rail on top, instrument, sidebar at the bottom) - the sizes are identical,
// only the axis changes, which is why no screen needs to know about this.
static inline lv_coord_t contentX() { return uiPortrait() ? 0 : UI7_CONTENT_X; }
static inline lv_coord_t contentY() { return uiPortrait() ? UI7_RAIL_W : UI7_CONTENT_Y; }

// ---- Paint split (1024x600 boards only) -------------------------------------
// main.cpp times the whole of dispMgr.update() as one number ("paint"). That
// lump is two very different jobs: the current screen painting its instrument
// into a PSRAM canvas, and everything else this manager does around it -
// sidebar values, demo and alarm banners, the perf overlay, the anchor watch,
// the deferred overlay/theme/config work. Knowing which of the two carries the
// 250 ms decides where any optimisation goes, so both are measured here, per
// call, and the two worst cases are handed to the heartbeat.
//
// Nothing prints from here; dispPaintTake() is reset-on-read like
// dispPerfTake() in DisplaySetup_7B.cpp, so every heartbeat describes its own
// 5 s window. These two counters have exactly ONE consumer (the heartbeat),
// which is what makes reset-on-read safe - see the two-consumer trap spelled
// out around s_refrTotal in DisplaySetup_7B.cpp. _perfBusyUs below is the
// on-screen overlay's own, separate accumulator and is left untouched.
static uint32_t s_scrPaintMaxUs = 0;   // worst _screens[_cur]->update()
static uint32_t s_chromePaintMaxUs = 0;   // worst rest-of-update() in the SAME call

// The same worst case, but kept PER SCREEN and never reset. s_scrPaintMaxUs
// above only ever describes whatever screen happened to be up, which is why
// the wind instrument was the only one anybody had numbers for. This table
// costs 4 bytes per screen and fills itself in as the user navigates, so one
// walk through the carousel produces the whole picture. Printed by
// dispScreenPaintReport(), never reset - a max is only meaningful cumulative.
static uint32_t s_scrPaintMaxUsBy[MAX_SCREENS] = { 0 };

// Three properties that cost real time on EVERY draw-buffer pass and buy this
// UI nothing. Applied to the full-bleed containers - the screen root, the
// instrument block, and each screen's own container - which are by design
// plain coloured rectangles: no scrolling, no border, no rounded corners.
//
// Measured context: a full-screen refresh of a nearly EMPTY screen costs 226 ms
// of which 186 ms is LVGL drawing, and the per-strip cost is the same (~3 ms)
// whether the pass paints 10,200 pixels or 3,300. So almost all of it is paid
// per pass, per object - not per pixel. These three are exactly that shape:
//
//   SCROLLABLE  - lv_obj_create() sets it by default. A scrollable object
//                 misses the early return in lv_obj_scroll.c and runs
//                 lv_obj_get_scrollbar_area (~1.9 KB of code) twice per strip,
//                 walking every child to conclude that nothing scrolls. Every
//                 other container in this UI already clears it; these did not.
//   border_post - the default theme's card style sets it, so lv_obj runs a
//                 COMPLETE second descriptor pass per strip to draw a border
//                 that these containers give width 0.
//   radius      - inherited as ~10 from the theme. Nothing here is rounded, but
//                 a non-zero radius makes every object draw allocate,
//                 initialise and free a radius mask - out of the LVGL pool
//                 whose exhaustion has rebooted this device before.
//
// Deliberately NOT applied to the tiles, cards or the sidebar cells: those are
// rounded on purpose and that is the design.
static void stripChrome(lv_obj_t *o) {
    if (!o) return;
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_post(o, false, 0);
    lv_obj_set_style_radius(o, 0, 0);
}

// Declared extern at the call site (main.cpp) rather than in DisplayManager.h,
// because that header is shared with the 4" board, which has no such counters.
void dispPaintTake(uint32_t *screenMaxUs, uint32_t *chromeMaxUs) {
    if (screenMaxUs) *screenMaxUs = s_scrPaintMaxUs;
    if (chromeMaxUs) *chromeMaxUs = s_chromePaintMaxUs;
    s_scrPaintMaxUs    = 0;
    s_chromePaintMaxUs = 0;
}
#endif


static WindScreen      s_wind;
static SpeedScreen     s_speed;
static DepthScreen     s_depth;
static EngineScreen    s_engine;
static RudderScreen    s_rudder;
static AisScreen       s_ais;
static WindPlotScreen  s_windplot;
static AutopilotScreen s_autopilot;
static MediaScreen    s_media;
static WindScreen      s_attitude;          // 2nd WindScreen instance, ATTITUDE mode
static AnchorScreen    s_anchor;
static TankScreen      s_tank;
static BatteryScreen   s_battery;
static WeatherScreen   s_weather;
static ClockScreen     s_clock;
static VmgScreen       s_vmg;
static RouteScreen     s_route;
static GridScreen      s_grid[MAX_GRIDS];   // one per data-grid slot

// ---- begin ------------------------------------------------------------------
// Called during setup() while the boot screen is still active.
// IMPORTANT: NO lv_* calls here.  Any LVGL heap allocation while the boot
// screen labels (_statusLbl, _bar, _pctLbl) are alive can corrupt their
// styles pointer – manifests as EXCVADDR=0x6 in get_prop_core.
// All LVGL object construction is deferred to activate().

void DisplayManager::begin() {
    _screens[SCR_WIND]      = &s_wind;
    _screens[SCR_SPEED]     = &s_speed;
    _screens[SCR_DEPTH]     = &s_depth;
    _screens[SCR_ENGINE]    = &s_engine;
    _screens[SCR_RUDDER]    = &s_rudder;
    _screens[SCR_AIS]       = &s_ais;
    _screens[SCR_WINDPLOT]  = &s_windplot;
    _screens[SCR_AUTOPILOT] = &s_autopilot;
    _screens[SCR_MEDIA]    = &s_media;
    s_attitude.setCenterMode(WindScreen::CenterMode::ATTITUDE);   // before activate()/create()
    _screens[SCR_ATTITUDE]  = &s_attitude;
    _screens[SCR_ANCHOR]    = &s_anchor;
    _screens[SCR_TANK]      = &s_tank;
    _screens[SCR_BATTERY]   = &s_battery;
    _screens[SCR_WEATHER]   = &s_weather;
    _screens[SCR_CLOCK]     = &s_clock;
    _screens[SCR_VMG]       = &s_vmg;
    _screens[SCR_ROUTE]     = &s_route;
    // Data-grid slots: bind each instance to its config slot; only ACTIVE slots
    // become real screens (created in activate()). Inactive slots stay nullptr.
    for (int k = 0; k < MAX_GRIDS; k++) {
        s_grid[k].setSlot(k);
        _screens[GRID_ID_BASE + k] = appConfig.cfg.grids[k].active ? &s_grid[k] : nullptr;
    }

    // Hardware button pins – safe to configure any time.
    if (BTN_PREV_PIN >= 0) { pinMode(BTN_PREV_PIN, INPUT_PULLUP); }
    if (BTN_NEXT_PIN >= 0) { pinMode(BTN_NEXT_PIN, INPUT_PULLUP); }
}

void DisplayManager::showScreen(int idx) {
    if (idx < 0 || idx >= MAX_SCREENS || !_screens[idx]) return;
    if (_screens[_cur] && _screens[_cur]->container)
        lv_obj_add_flag(_screens[_cur]->container, LV_OBJ_FLAG_HIDDEN);
    if (_screens[_cur]) _screens[_cur]->onHide();
    _cur = idx;
    if (_screens[_cur]->container)
        lv_obj_clear_flag(_screens[_cur]->container, LV_OBJ_FLAG_HIDDEN);
    _screens[_cur]->onShow();
    // Every time, over the whole tree rather than just the incoming screen:
    // it also catches the sidebar, the banners and anything a screen created
    // lazily in onShow(). See uiDisableLabelScroll() in Theme.h for what this
    // costs when it is NOT done - it was the single biggest item in the frame.
    uiDisableLabelScroll(_mainScreen);
    // Note it, do not write it - the flash write happens later, from update().
    // Overlays never reach this function: the settings, licence, language and
    // home pages live on lv_layer_top() and leave _cur alone, so a restart
    // while one of them is open comes back to the INSTRUMENT behind it, never
    // into a menu.
    _lastScreenPending = _cur;
    _lastScreenAtMs    = millis();
    _forceUpdate = true;
    // Bring overlay arrows to top so they stay visible over screen content
    if (_btnPrev) lv_obj_move_foreground(_btnPrev);
    if (_btnNext) lv_obj_move_foreground(_btnNext);
    if (_btnSettings) lv_obj_move_foreground(_btnSettings);
    if (_demoBanner) lv_obj_move_foreground(_demoBanner);
    if (_perfOverlay) lv_obj_move_foreground(_perfOverlay);
}

// ---- Screen catalog ---------------------------------------------------------
// Translated labels + type keys for the fixed instrument screens.
// The "type" tells the WebUI which inline config editor to render.
// Only the NAME is translated — SCREEN_TYPES are protocol keys the WebUI
// switches on, so they must stay exactly as they are.
static const StrId SCREEN_NAME_IDS[N_FIXED_SCREENS] = {
    STR_SCREEN_WIND,      STR_SCREEN_SPEED,     STR_SCREEN_DEPTH,
    STR_SCREEN_ENGINE,    STR_SCREEN_RUDDER,    STR_SCREEN_AIS,
    STR_SCREEN_WINDPLOT,  STR_SCREEN_AUTOPILOT, STR_SCREEN_MEDIA,
    STR_SCREEN_ATTITUDE,  STR_SCREEN_ANCHOR,    STR_SCREEN_TANK,
    STR_SCREEN_BATTERY,   STR_SCREEN_WEATHER,   STR_SCREEN_CLOCK,
    STR_SCREEN_VMG,       STR_SCREEN_ROUTE,
};
static const char *const SCREEN_TYPES[N_FIXED_SCREENS] = {
    "wind", "speed", "depth", "engine", "rudder", "ais", "windplot", "autopilot", "media", "attitude", "anchor", "tank", "battery", "weather", "clock", "vmg", "route",
};

// A screen id is "present" if it's a fixed instrument or an active grid slot.
bool DisplayManager::screenPresent(int id) const {
    if (id < 0 || id >= MAX_SCREENS) return false;
    if (id < N_FIXED_SCREENS) return true;
    return appConfig.cfg.grids[id - GRID_ID_BASE].active;
}

const char *DisplayManager::screenName(int id) const {
    if (id < 0 || id >= MAX_SCREENS) return "?";
    if (id < N_FIXED_SCREENS) return T(SCREEN_NAME_IDS[id]);
    // Grid names are user data from config.json — never translated, only the
    // fallback for an unnamed grid is.
    const char *n = appConfig.cfg.grids[id - GRID_ID_BASE].name;
    return (n && n[0]) ? n : T(STR_SCREEN_GRID_DEFAULT);
}

const char *DisplayManager::screenType(int id) const {
    if (id < 0 || id >= MAX_SCREENS) return "?";
    return (id < N_FIXED_SCREENS) ? SCREEN_TYPES[id] : "grid";
}

int DisplayManager::navPosOf(int id) const {
    for (int i = 0; i < _navLen; i++) if (_navOrder[i] == id) return i;
    return -1;
}

// ---- Build navigation order/visibility from appConfig -----------------------
// Must run in an LVGL-safe context (activate() or update()), never the web task.
void DisplayManager::applyScreenConfig() {
    // 1. Canonical order: take valid, unique IDs from cfg.screenOrder, then
    //    append any known screen IDs that are missing (e.g. added by a firmware
    //    update). This keeps a stable full ordering of ALL screens.
    bool    seen[MAX_SCREENS] = { false };
    uint8_t ordered[MAX_SCREENS];
    int     n = 0;
    for (int i = 0; i < appConfig.cfg.screenCount && n < MAX_SCREENS; i++) {
        int id = appConfig.cfg.screenOrder[i];
        if (id >= 0 && id < MAX_SCREENS && screenPresent(id) && !seen[id]) { seen[id] = true; ordered[n++] = (uint8_t)id; }
    }
    // Append any present screen (fixed instrument or newly-activated grid) missing
    // from the saved order. Inactive grid slots are excluded entirely.
    for (int id = 0; id < MAX_SCREENS; id++)
        if (screenPresent(id) && !seen[id]) ordered[n++] = (uint8_t)id;

    // Write the normalised order back so WebUI / next save stays consistent.
    for (int i = 0; i < n; i++) appConfig.cfg.screenOrder[i] = ordered[i];
    appConfig.cfg.screenCount = (uint8_t)n;

    // 2. Navigation list = enabled screens only, in order.
    _navLen = 0;
    for (int i = 0; i < n; i++) {
        int id = ordered[i];
        if (appConfig.cfg.screenEnabled[id]) _navOrder[_navLen++] = (uint8_t)id;
    }
    // Safety: never leave the device with zero navigable screens.
    if (_navLen == 0) {
        for (int i = 0; i < n; i++) _navOrder[i] = ordered[i];
        _navLen = n;
    }

    // 3. If the current screen was just disabled, jump to the first nav screen.
    if (navPosOf(_cur) < 0 && _navLen > 0) showScreen(_navOrder[0]);
}

// ---- Live theme reload (no reboot) ------------------------------------------
// Re-fills uiTheme/uiSz/uiFont from config, then rebuilds every screen's LVGL
// objects in place. Canvas pixel buffers are REUSED (the screens guard their
// PsramArena alloc with `if(!_cbuf)`), so the bump-arena is not re-consumed.
// Runs in update() (LVGL-safe, Core 1). ~200-400 ms one-time pause.
void DisplayManager::reloadThemeLive() {
    if (!_content) return;
    applyThemeFromConfig();

    for (int i = 0; i < MAX_SCREENS; i++) {
        if (!_screens[i]) continue;
        if (_screens[i]->container) {
            lv_obj_del(_screens[i]->container);     // frees LVGL objects (not the arena _cbuf)
            _screens[i]->container = nullptr;
        }
        _screens[i]->resetForRebuild();             // null now-dangling child ptrs
        // esp_task_wdt_reset() removed: this task was never subscribed to the
        // TWDT, so the call was always a no-op - but on Arduino 3.x / IDF 5.x
        // every call logs "task_wdt: esp_task_wdt_reset(): task not found",
        // ~100 lines/s of serial flood on the 7B.
        vTaskDelay(pdMS_TO_TICKS(2));               // yield SPI0 to WiFi beacon ISR
        _screens[i]->create(_content);              // rebuilds with new theme; reuses _cbuf
        if (_screens[i]->container && i != _cur)
            lv_obj_add_flag(_screens[i]->container, LV_OBJ_FLAG_HIDDEN);
    }

    // Persistent chrome (not recreated) — re-apply its theme colours.
    if (_mainScreen) lv_obj_set_style_bg_color(_mainScreen, CLR_BG, 0);
    lv_obj_set_style_bg_color(_content, CLR_BG, 0);
    if (_btnPrev) lv_obj_set_style_bg_color(_btnPrev, uiTheme.navBtnBg, 0);
    if (_btnNext) lv_obj_set_style_bg_color(_btnNext, uiTheme.navBtnBg, 0);
    if (_demoBanner) lv_obj_set_style_bg_color(_demoBanner, uiTheme.demoBanner, 0);
    if (_perfOverlay) {
        lv_obj_set_style_text_color(_perfOverlay, uiTheme.perfText, 0);
        lv_obj_set_style_bg_color(_perfOverlay, uiTheme.perfBg, 0);
    }

    refreshTrialChrome();   // OpenBridge icon trial
    showScreen(_cur);   // unhide current, onShow(), bring chrome to foreground
    Serial.println("[theme] live reload done"); Serial.flush();
}

// OpenBridge icon trial: the floating buttons and the demo banner are built once and
// survive a live reload, so a change of ui.icons would leave their old symbols
// standing. Relabel them here - the banner only when its symbol actually changed,
// so a live LANGUAGE change leaves it in the boot language exactly as before the
// trial (the banner has never followed one; changing that is not this trial's job).
void DisplayManager::refreshTrialChrome() {
    struct { lv_obj_t *btn; NpIcon icon; } btns[] = {
        { _btnPrev, NP_ICON_PREV }, { _btnNext, NP_ICON_NEXT }, { _btnSettings, NP_ICON_SETTINGS },
    };
    for (auto &b : btns) {
        lv_obj_t *lbl = b.btn ? lv_obj_get_child(b.btn, 0) : nullptr;
        if (lbl) lv_label_set_text(lbl, npSym(b.icon));
    }
    lv_obj_t *bannerLbl = _demoBanner ? lv_obj_get_child(_demoBanner, 0) : nullptr;
    const char *sym = npSym(NP_ICON_DEMO);
    if (bannerLbl && strncmp(lv_label_get_text(bannerLbl), sym, strlen(sym)) != 0) {
        char banner[96];
        snprintf(banner, sizeof(banner), "%s  %s  %s", npSym(NP_ICON_DEMO),
                 T(STR_BANNER_DEMO_MODE), npSym(NP_ICON_DEMO));
        lv_label_set_text(bannerLbl, banner);
    }
}

void DisplayManager::nextScreen() {
    if (_navLen == 0) return;
    int pos = navPosOf(_cur);
    int next = (pos < 0) ? 0 : (pos + 1) % _navLen;
    showScreen(_navOrder[next]);
}
void DisplayManager::prevScreen() {
    if (_navLen == 0) return;
    int pos = navPosOf(_cur);
    int prev = (pos < 0) ? 0 : (pos - 1 + _navLen) % _navLen;
    showScreen(_navOrder[prev]);
}

void DisplayManager::update() {
#if defined(BOARD_PANEL_1024X600)
    // Whole-function stamp. The screen's own paint is subtracted from it at the
    // bottom, so "chrome" is measured on the SAME call as the instrument and the
    // two never come from different frames.
    const int64_t tUpdate0 = esp_timer_get_time();
#endif
    _forceUpdate = false;
    languageOverlay.update();  // deferred open in the LVGL context
    licenseOverlay.update();   // deferred open in the LVGL context
    // Apply a pending screen-config change (set by the web handler) here, in the
    // LVGL-safe loop context rather than the async TCP task.
    if (_screenCfgPending) { _screenCfgPending = false; applyScreenConfig();
                             sideBar.applyConfig();
                             // A data grid rebuilds its cells only in onShow(),
                             // so a saved change to the grid ON DISPLAY - a cell's
                             // field, its decimals, the coordinate format - stayed
                             // invisible until the next page flip, while update()
                             // already wrote the new text into the old label: a
                             // coordinate in the digits-only hero font came out as
                             // placeholder boxes. The sidebar has always rebuilt
                             // on every save; the visible grid now does the same.
                             if (_cur >= GRID_ID_BASE && _screens[_cur] &&
                                 _screens[_cur]->container) {
                                 _screens[_cur]->onShow();
                                 uiDisableLabelScroll(_mainScreen);   // fresh labels
                             }
                             // Rebuilt sidebar cells are the newest siblings and
                             // would otherwise render over the perf overlay.
                             if (_perfOverlay) lv_obj_move_foreground(_perfOverlay); }
    // An anchor state adopted from the bus is applied HERE, not on the bus
    // task: it writes the config to LittleFS and touches the alarm, both of
    // which belong to this loop. Re-evaluate the alarm at once - an anchor
    // armed on the other device must not lie here unwatched until the next
    // cycle.
    if (AnchorSyncNode::applyPending()) {
        evaluateAnchorAlarm();
        if (_cur == SCR_ANCHOR && _screens[_cur]) _screens[_cur]->update();
    }
    // Reload polar table if the web handler just saved new data.
    if (_polarReloadPending) { _polarReloadPending = false; gPolar().load(appConfig.cfg.polarFile); }
    // Live theme re-apply (colours/sizes/fonts) — rebuilds the UI without a reboot.
    if (_themeReloadPending) { _themeReloadPending = false; reloadThemeLive();
                               sideBar.restyle(); }
    // Open the on-screen config overlay (requested by the top-edge swipe gesture).
    if (_openConfigPending) { _openConfigPending = false; configOverlay.open(); }
    // Open the 7B home/launcher overlay (requested by the rail's home button).
    if (_openHomePending) { _openHomePending = false; homeOverlay.open(); }
    // Deferred reboot: the message was painted by earlier displayTick()s; restart
    // once the grace period elapses (config changes that need a clean WiFi re-init).
    if (_rebootPending && (int32_t)(millis() - _rebootAtMs) >= 0) {
#ifndef SIMULATOR
        Serial.println("[cfg] rebooting to apply settings"); Serial.flush();
        ESP.restart();
#else
        _rebootPending = false;
#endif
    }
    // A full-screen overlay covers the instrument completely. Continuing to
    // draw it anyway is pure waste — the wind page paints an entire
    // 480x480 PSRAM canvas in the process — and exactly this compute time is
    // missing from the overlay, which is why scrolling in the license text
    // stuttered badly.
    //
    // The home launcher belongs in this list as well. It was built after the
    // other three and never added here, so the instrument kept rendering at
    // full cost behind its tiles.
    const bool modalOpen = configOverlay.isOpen() || licenseOverlay.isOpen() ||
                           languageOverlay.isOpen() || homeOverlay.isOpen();
    // Suppressing the PAINT above is only half of it. LVGL still DRAWS the whole
    // screen behind the overlay and the overlay then paints over the result:
    // lv_refr.c searches for an object that covers the redraw area only inside
    // the ACTIVE SCREEN (lv_refr_area_part, line 730), while an overlay parented
    // to lv_layer_top() is refreshed unconditionally afterwards (line 793). So an
    // opaque full-screen overlay can never suppress anything below it, however
    // opaque it is.
    //
    // What that costs, measured on the 5B while scrolling the licence text: one
    // frame took 1,190 ms, of which 1,160 ms was LVGL drawing and 31 ms the
    // flush. That is 2.4 us per pixel against 0.29 us on an ordinary screen -
    // eight times the cost, all of it spent rendering an instrument that nobody
    // can see, including a 600x600 canvas image blitted out of PSRAM once per
    // strip.
    //
    // Hiding the screen makes refr_obj() return immediately (lv_refr.c line 967)
    // for the entire subtree. All four overlays are full-screen and LV_OPA_COVER
    // (see their build() functions), so there is nothing to show through; the
    // display background is set to CLR_BG in activate() for the same reason.
    // Hidden objects are also skipped by hit-testing, so touches stop leaking
    // through to the instrument behind a modal.
    if (modalOpen != _modalWasOpen && _mainScreen) {
        if (modalOpen) lv_obj_add_flag(_mainScreen, LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_clear_flag(_mainScreen, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(_mainScreen);   // nothing below was drawn while hidden
        }
    }
    // Closing edge: bring the whole view up to date in the very next frame, so
    // the instrument does not reappear showing values from before the menu.
    if (_modalWasOpen && !modalOpen) _forceUpdate = true;
    _modalWasOpen = modalOpen;

    // 7B sidebar values, twice a second (no-op stub on other boards). Since the
    // overlays went full-screen the sidebar is covered too, so this is just as
    // wasteful as the instrument itself.
    if (!modalOpen) {
        const uint32_t nowSb = millis();
        if (nowSb - _lastSideBarMs >= 500) { _lastSideBarMs = nowSb; sideBar.updateValues(); }
    }
    // Apply any pending nav-arrow show request first (safe: called outside lv_timer_handler)
    applyNavArrowsPending();

    // Wind trace sampler. Deliberately OUTSIDE the modalOpen guard and not tied
    // to the wind screen being visible: the curve is a history, and it would be
    // a poor one if it grew a minute-long hole every time somebody opened the
    // settings or looked at the depth page. Costs one comparison per tick and
    // does nothing until WIND_TRACE_MS is up.
    {
        auto lk = data.lock();
        data.pushWindTraceDue(millis());
    }

    // This brace measures the CANVAS PAINT of the current screen and nothing
    // else - it is the "CPU%" of the perf overlay. It is a different event from
    // the overlay's "fps", which counts finished LVGL display refreshes; the
    // paint writes pixels into a PSRAM canvas, and LVGL only puts them on the
    // panel later, from lv_timer_handler(). _perfFrames counts paint passes and
    // is currently kept for the reset alone - no readout shows it.
    uint32_t t0 = (uint32_t)(esp_timer_get_time());  // µs
    if (!modalOpen && _screens[_cur]) _screens[_cur]->update();
#if defined(BOARD_PANEL_1024X600)
    // Same stamp, two readers: the overlay's running total (unchanged) and the
    // heartbeat's worst case. No extra timer read, and the 4" branch below is
    // the original statement token for token.
    const uint32_t scrUs = (uint32_t)(esp_timer_get_time()) - t0;
    _perfBusyUs += scrUs;
    if (scrUs > s_scrPaintMaxUs) s_scrPaintMaxUs = scrUs;
    // Only when a screen was actually painted: with a modal open scrUs is the
    // cost of not painting, and folding that into the screen's own maximum
    // would make an untouched screen look fast for the wrong reason.
    if (!modalOpen && _cur >= 0 && _cur < MAX_SCREENS &&
        scrUs > s_scrPaintMaxUsBy[_cur]) s_scrPaintMaxUsBy[_cur] = scrUs;
#else
    _perfBusyUs += (uint32_t)(esp_timer_get_time()) - t0;
#endif
    _perfFrames++;
    // Both of these write labels that sit UNDER the overlay. A label write
    // invalidates its area, and LVGL then redraws that strip including the
    // overlay stacked above it - so skipping them is a real saving, not just a
    // skipped assignment. The perf figures stay honest: the counters keep
    // running and the next reading simply spans the time the menu was open.
    if (!modalOpen) {
        updateDemoBanner();
        updatePerfOverlay();
    }
    // Deliberately NOT suspended: the anchor watch is a safety function and has
    // to keep running while the user is standing in a menu.
    evaluateAnchorAlarm();
    evaluateAutoTheme();
    // Deferred flash write for "remember the screen". Runs on every tick but
    // does nothing until the delay is up, and is deliberately outside the
    // modalOpen guard: switching screens from the home launcher has to be
    // remembered too, and the launcher is a modal.
    persistLastScreenDue();
#if defined(BOARD_PANEL_1024X600)
    // Everything this function did EXCEPT the instrument paint. Unsigned
    // subtraction of two values taken in this same call, so it cannot go
    // negative even when the paint was skipped (scrUs is then ~0, not stale:
    // t0 is stamped every call whether or not a screen is painted).
    const uint32_t chromeUs = (uint32_t)(esp_timer_get_time() - tUpdate0) - scrUs;
    if (chromeUs > s_chromePaintMaxUs) s_chromePaintMaxUs = chromeUs;
#endif
}

// Solar auto theme: when cfg.themeAuto is on, switch the active palette to light
// by day / dark by night (from time + GPS via SunCalc). Checked ~once a minute;
// applies the change with the existing live theme reload.
void DisplayManager::evaluateAutoTheme() {
    if (!appConfig.cfg.themeAuto) return;
    static uint32_t lastCheck = 0;
    uint32_t now = millis();
    if (lastCheck != 0 && (now - lastCheck) < 60000) return;
    lastCheck = now;

    uint16_t days; double secOfDay; int16_t offMin; uint32_t lastUpd; bool valid; float lat, lon;
    {
        auto lk = data.lock();
        days = data.sysDays; secOfDay = data.sysSecOfDay; offMin = data.localOffsetMin;
        lastUpd = data.lastTimeUpdate; valid = data.timeValid; lat = data.lat; lon = data.lon;
    }
    if (!valid || isnan(lat) || isnan(lon)) return;

    double utcSec = secOfDay + (double)(now - lastUpd) / 1000.0;
    long   utcDays = days;
    while (utcSec >= 86400.0) { utcSec -= 86400.0; utcDays++; }
    double localSec = utcSec + (double)offMin * 60.0;
    long   localDays = utcDays;
    while (localSec >= 86400.0) { localSec -= 86400.0; localDays++; }
    while (localSec < 0.0)      { localSec += 86400.0; localDays--; }
    int yy; unsigned mo, dd; scCivilFromDays(localDays, yy, mo, dd);

    float riseUTC, setUTC; int state;
    scSunTimesUTC(yy, mo, dd, lat, lon, riseUTC, setUTC, state);
    bool isDay;
    if      (state ==  1) isDay = true;
    else if (state == -1) isDay = false;
    else {
        float nowH  = (float)(localSec / 3600.0);
        float riseL = fmodf(riseUTC + offMin / 60.f + 24.f, 24.f);
        float setL  = fmodf(setUTC  + offMin / 60.f + 24.f, 24.f);
        isDay = (riseL <= setL) ? (nowH >= riseL && nowH < setL)
                                : (nowH >= riseL || nowH < setL);
    }
    const char *desired = isDay ? "light" : "dark";
    if (strcmp(appConfig.cfg.themeActive, desired) != 0) {
        strncpy(appConfig.cfg.themeActive, desired, sizeof(appConfig.cfg.themeActive) - 1);
        appConfig.cfg.themeActive[sizeof(appConfig.cfg.themeActive) - 1] = 0;
        requestThemeReload();   // rebuild UI live next tick
    }
}

// Global anchor-drag alarm: compares the live GPS fix against the persisted
// anchor position/radius and, if exceeded, shows a blinking red banner (on any
// screen) plus a buzzer pattern. Runs every tick regardless of current screen.
void DisplayManager::evaluateAnchorAlarm() {
    if (!_alarmBanner) return;
    bool  alarming = false;
    float distM = 0.f;
    if (appConfig.cfg.anchorSet && appConfig.cfg.anchorAlarmOn &&
        !isnan(appConfig.cfg.anchorLat)) {
        float lat, lon; uint32_t age;
        { auto lk = data.lock(); lat = data.lat; lon = data.lon; age = millis() - data.lastGpsUpdate; }
        if (!isnan(lat) && !isnan(lon) && age < 10000) {
            float n = (lat - appConfig.cfg.anchorLat) * 60.f * 1852.f;
            float e = (lon - appConfig.cfg.anchorLon) * 60.f * 1852.f *
                      cosf(appConfig.cfg.anchorLat * 0.017453292f);
            distM = sqrtf(n*n + e*e);
            alarming = distM > appConfig.cfg.anchorRadius;
        }
    }

    if (alarming) {
        lv_obj_clear_flag(_alarmBanner, LV_OBJ_FLAG_HIDDEN);
        if (!_anchorAlarmActive) lv_obj_move_foreground(_alarmBanner);   // on trigger
        char b[40];
        // LV_SYMBOL_* glyphs exist in the Montserrat build; U+2693 (⚓) does NOT
        // and rendered as an empty box. See the font-coverage note below.
        snprintf(b, sizeof(b), "%s %s  %d m", npSym(NP_ICON_WARNING),   // OpenBridge icon trial
                 T(STR_ALARM_ANCHOR_DRAG), (int)(distM + 0.5f));
        lv_label_set_text(_alarmLbl, b);
        _alarmBlink++;
        lv_obj_set_style_bg_opa(_alarmBanner, (_alarmBlink & 1) ? LV_OPA_COVER : LV_OPA_50, 0);
#ifndef SIMULATOR
        bool wantOn = ((millis() / 250) % 4) == 0;   // 250 ms on / 750 ms off
        if (wantOn != _buzzOn) { _buzzOn = wantOn; boardBuzzer(wantOn); }
#endif
    } else if (_anchorAlarmActive) {                 // just cleared
        lv_obj_add_flag(_alarmBanner, LV_OBJ_FLAG_HIDDEN);
#ifndef SIMULATOR
        if (_buzzOn) { _buzzOn = false; boardBuzzer(false); }
#endif
    }
    _anchorAlarmActive = alarming;
}

void DisplayManager::updateDemoBanner() {
    if (!_demoBanner) return;
    if (appConfig.cfg.demoMode) {
        lv_obj_clear_flag(_demoBanner, LV_OBJ_FLAG_HIDDEN);
        // Blink: alternate between full and half opacity every second
        _demoBlink++;
        lv_obj_set_style_bg_opa(_demoBanner,
            (_demoBlink & 1) ? LV_OPA_COVER : LV_OPA_70, 0);
    } else {
        lv_obj_add_flag(_demoBanner, LV_OBJ_FLAG_HIDDEN);
        _demoBlink = 0;
    }
}

void DisplayManager::handleButtons() {
    if (BTN_PREV_PIN >= 0) {
        bool pressed = (digitalRead(BTN_PREV_PIN) == LOW);
        if (pressed && !_prevPressed) prevScreen();
        _prevPressed = pressed;
    }
    if (BTN_NEXT_PIN >= 0) {
        bool pressed = (digitalRead(BTN_NEXT_PIN) == LOW);
        if (pressed && !_nextPressed) nextScreen();
        _nextPressed = pressed;
    }
}

const char *DisplayManager::currentTitle() const {
    return _screens[_cur] ? _screens[_cur]->title() : "";
}

// ── Remember the screen across a restart ─────────────────────────────────────
// See the declaration for why this is NVS and why it is delayed.
#if !defined(SIMULATOR)
#include <Preferences.h>
static const char *LASTSCR_NS  = "ui";
static const char *LASTSCR_KEY = "lastscr";
// Long enough that swiping from one end of the carousel to the other is one
// write rather than twenty, short enough that pulling the breaker a few
// seconds after settling on a screen still remembers it.
static const uint32_t LASTSCR_DELAY_MS = 5000;
#endif

void DisplayManager::persistLastScreenDue() {
#if !defined(SIMULATOR)
    if (_lastScreenPending < 0) return;
    if (millis() - _lastScreenAtMs < LASTSCR_DELAY_MS) return;
    const int id = _lastScreenPending;
    _lastScreenPending = -1;

    Preferences p;
    if (!p.begin(LASTSCR_NS, false)) return;
    // Read before writing: NVS skips an identical value itself, but the check
    // is free here and keeps the log honest about what actually moved.
    if (p.getInt(LASTSCR_KEY, -1) != id) p.putInt(LASTSCR_KEY, id);
    p.end();
#endif
}

void DisplayManager::restoreLastScreen() {
    int id = -1;
#if !defined(SIMULATOR)
    Preferences p;
    if (p.begin(LASTSCR_NS, true)) {         // read-only; absent on first boot
        id = p.getInt(LASTSCR_KEY, -1);
        p.end();
    }
#endif
    // Validated against the CURRENT navigation order, not just the array
    // bounds: a screen that was on display can since have been switched off in
    // the web interface, and a data grid can have been deleted outright. Then
    // the stored id names a screen that is no longer reachable, and falling
    // back to the first one is the only sensible answer.
    if (id >= 0 && navPosOf(id) >= 0) {
        Serial.printf("[ui] restoring last screen %d\n", id);
        showScreen(id);
        return;
    }
    if (id >= 0) Serial.printf("[ui] last screen %d is no longer enabled\n", id);
    showScreen(_navLen > 0 ? _navOrder[0] : 0);
}

#if defined(BOARD_PANEL_1024X600)
// One line naming every screen that has been on display since boot and what
// its worst canvas paint cost, sorted by cost. Screens never visited are left
// out rather than printed as 0, so the line says what was measured and nothing
// more. Called from the heartbeat about once a minute.
void DisplayManager::reportScreenPaint() const {
    int idx[MAX_SCREENS], n = 0;
    for (int i = 0; i < MAX_SCREENS; i++)
        if (s_scrPaintMaxUsBy[i] && _screens[i]) idx[n++] = i;
    if (!n) return;
    for (int i = 1; i < n; i++) {              // insertion sort, descending
        const int v = idx[i];
        int k = i - 1;
        while (k >= 0 && s_scrPaintMaxUsBy[idx[k]] < s_scrPaintMaxUsBy[v]) { idx[k+1] = idx[k]; k--; }
        idx[k+1] = v;
    }
    Serial.print("[paint/screen]");
    for (int i = 0; i < n; i++)
        Serial.printf(" %s=%.0f", _screens[idx[i]]->title(),
                      s_scrPaintMaxUsBy[idx[i]] / 1000.0f);
    Serial.println();
    Serial.flush();
}
#endif

// ── Opacity animation helper ──────────────────────────────────────────────────
static void anim_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void nav_fade_anim(lv_obj_t *obj, lv_opa_t from, lv_opa_t to, uint32_t ms) {
    if (!obj) return;
    lv_anim_del(obj, anim_opa_cb);          // cancel any running fade
    if (from == to) { lv_obj_set_style_opa(obj, to, 0); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, (int32_t)from, (int32_t)to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// ── Nav arrow fade timer callback ─────────────────────────────────────────────
void DisplayManager::onNavFadeTimer() {
    if (!_btnPrev || !_btnNext) return;
    // Pause timer so it doesn't keep firing (re-armed by applyNavArrowsPending)
    if (_navFadeTimer) lv_timer_pause(_navFadeTimer);
    _navArrowsShown = false;
    lv_opa_t curOpa = lv_obj_get_style_opa(_btnPrev, 0);
    nav_fade_anim(_btnPrev,     curOpa, LV_OPA_TRANSP, UI_NAV_FADE_OUT_MS);
    nav_fade_anim(_btnNext,     curOpa, LV_OPA_TRANSP, UI_NAV_FADE_OUT_MS);
    nav_fade_anim(_btnSettings, curOpa, LV_OPA_TRANSP, UI_NAV_FADE_OUT_MS);
}

// ── Apply pending nav-arrow show request ──────────────────────────────────────
// Called from update() which runs OUTSIDE lv_timer_handler(), safe for LVGL.
// The touch callback only sets _navShowPending = true (no LVGL calls there).
void DisplayManager::applyNavArrowsPending() {
    if (!_navShowPending) return;
    _navShowPending = false;

    if (!_btnPrev || !_btnNext) return;

    // Re-arm the idle countdown: reset elapsed time and resume the paused timer
    if (_navFadeTimer) {
        lv_timer_reset(_navFadeTimer);   // restart period from now
        lv_timer_resume(_navFadeTimer);  // re-activate after being paused by onNavFadeTimer
    }

    // Animate in only if currently faded out (avoids redundant animations)
    if (!_navArrowsShown) {
        _navArrowsShown = true;
        lv_opa_t curOpa = lv_obj_get_style_opa(_btnPrev, 0);
        nav_fade_anim(_btnPrev,     curOpa, LV_OPA_COVER, UI_NAV_FADE_IN_MS);
        nav_fade_anim(_btnNext,     curOpa, LV_OPA_COVER, UI_NAV_FADE_IN_MS);
        nav_fade_anim(_btnSettings, curOpa, LV_OPA_COVER, UI_NAV_FADE_IN_MS);
    }
}

// A faded-out arrow is invisible but STILL clickable, so a tap in its 80×120
// corner silently changed screens — and swallowed taps meant for whatever sits
// underneath. Mirror the gear's guard (cbSettings): sample the visibility when
// the press begins; a tap that starts while faded out only reveals the arrows.
void DisplayManager::cbPrev(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED)       dispMgr._navVisibleAtPress = dispMgr._navArrowsShown;
    else if (code == LV_EVENT_CLICKED && dispMgr._navVisibleAtPress) dispMgr.prevScreen();
}
void DisplayManager::cbNext(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED)       dispMgr._navVisibleAtPress = dispMgr._navArrowsShown;
    else if (code == LV_EVENT_CLICKED && dispMgr._navVisibleAtPress) dispMgr.nextScreen();
}
void DisplayManager::cbGesture(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT)  dispMgr.nextScreen();
    if (dir == LV_DIR_RIGHT) dispMgr.prevScreen();
}

// Settings gear: opens the on-screen config overlay. Like the nav arrows it
// auto-fades when idle. A tap that STARTS while the gear is faded out only
// reveals it (the touch already triggers requestShowNavArrows); a second tap on
// the now-visible gear opens the modal. Visibility is sampled at PRESS time
// (not CLICK/release) because the gear may fade in between press and release.
void DisplayManager::cbSettings(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        dispMgr._settingsVisibleAtPress = dispMgr._navArrowsShown;
    } else if (code == LV_EVENT_CLICKED) {
        if (dispMgr._settingsVisibleAtPress) dispMgr.requestOpenConfig();
    }
}

// Paint a full-screen "rebooting" message on the top layer, then restart a few
// ticks later (handled in update()). Safe to call from an LVGL event callback:
// it only CREATES objects (no reentrant refresh, no blocking delay).
void DisplayManager::requestReboot(const char *msg) {
    lv_obj_t *p = lv_obj_create(lv_layer_top());
#if defined(BOARD_PANEL_1024X600)
    // Cover the whole LOGICAL screen (portrait swaps w/h). This backdrop is a
    // child of lv_layer_top(), and applying the rotation resizes that layer to
    // the logical resolution - sizing it from the PHYSICAL panel would leave
    // the lower 424 px live underneath a supposedly modal message and push the
    // centred label off the right edge. Landscape is unaffected: there
    // uiScreenW()/uiScreenH() are exactly LCD_WIDTH/LCD_HEIGHT.
    lv_obj_set_size(p, uiScreenW(), uiScreenH());
#else
    lv_obj_set_size(p, SCREEN_W, SCREEN_H);
#endif
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, CLR_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_font(l, FONT_LARGE, 0);
    lv_obj_set_style_text_color(l, CLR_TEXT, 0);
    lv_obj_center(l);
    _rebootPending = true;
    _rebootAtMs    = millis() + 600;
    _forceUpdate   = true;
}

// ---- Side-overlay navigation arrows -----------------------------------------

void DisplayManager::buildOverlayNav(lv_obj_t *parent) {
    // Each button: 1/6 screen width × 1/4 screen height, vertically centred.
    // Semi-transparent dark background, white arrow symbol.
    // They float on top of all screen content (moved foreground in showScreen).

    auto makeFloatBtn = [&](lv_align_t align, const char *sym, lv_event_cb_t cb,
                            int w, int h, int yofs) -> lv_obj_t* {
        lv_obj_t *btn = lv_btn_create(parent);
        lv_obj_set_size(btn, w, h);
        lv_obj_align(btn, align, 0, yofs);

        // Background: dark, semi-transparent
        lv_obj_set_style_bg_color(btn, (uiTheme.navBtnBg), 0);
        lv_obj_set_style_bg_opa(btn,  UI_NAV_BTN_BG_OPA, 0);
        lv_obj_set_style_bg_opa(btn,  UI_NAV_BTN_BG_OPA_PRESS, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, 0);
        // radius=0 avoids lv_draw_mask_radius_init AA-buffer allocation (prevents OOM)
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        // Arrow label
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, sym);
        lv_obj_set_style_text_font(lbl, FONT_XL, 0);
        lv_obj_set_style_text_color(lbl, CLR_ON_ACCENT, 0);
        lv_obj_set_style_text_opa(lbl, UI_NAV_BTN_ARROW_OPA, 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        return btn;
    };

    // OpenBridge icon trial: symbols via npSym(); relabelled in refreshTrialChrome().
    _btnPrev = makeFloatBtn(LV_ALIGN_LEFT_MID,  npSym(NP_ICON_PREV), cbPrev, UI_NAV_BTN_W, UI_NAV_BTN_H, 0);
    _btnNext = makeFloatBtn(LV_ALIGN_RIGHT_MID, npSym(NP_ICON_NEXT), cbNext, UI_NAV_BTN_W, UI_NAV_BTN_H, 0);
    // PRESSED too, so cbPrev/cbNext can sample whether the arrow was actually
    // visible when the tap began (see cbPrev).
    lv_obj_add_event_cb(_btnPrev, cbPrev, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(_btnNext, cbNext, LV_EVENT_PRESSED, nullptr);
    // Settings gear: top-centre, smaller, offset below the demo banner.
    // (makeFloatBtn wires CLICKED; add PRESSED too so cbSettings can sample the
    // gear's visibility at the moment the tap begins — see cbSettings.)
    _btnSettings = makeFloatBtn(LV_ALIGN_TOP_MID, npSym(NP_ICON_SETTINGS), cbSettings, 64, 40, 22);   // OpenBridge icon trial
    lv_obj_add_event_cb(_btnSettings, cbSettings, LV_EVENT_PRESSED, nullptr);

    // Swipe gestures on the full screen (register GESTURE event on parent)
    lv_obj_add_event_cb(parent, cbGesture, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Auto-fade timer: repeating, but manually paused after each fire.
    // NOTE: lv_timer_set_repeat_count(t, 1) deletes the timer after it fires,
    // which would leave _navFadeTimer as a dangling pointer → heap corruption.
    // Instead we use repeat_count=-1 (infinite) and call lv_timer_pause() after
    // each fire so it only re-activates when reset by showNavArrows().
    _navFadeTimer = lv_timer_create(
        [](lv_timer_t *t) { dispMgr.onNavFadeTimer(); },
        UI_NAV_FADE_DELAY_MS, nullptr);
    // Infinite repeats – paused after each fire, re-armed on touch
    lv_timer_set_repeat_count(_navFadeTimer, -1);
}

// ---- Performance overlay ----------------------------------------------------

void DisplayManager::buildPerfOverlay(lv_obj_t *parent) {
    _perfOverlay = lv_label_create(parent);
    lv_label_set_text(_perfOverlay, "");
    lv_obj_set_style_text_font(_perfOverlay, FONT_TINY, 0);
    lv_obj_set_style_text_color(_perfOverlay, (uiTheme.perfText), 0);
    lv_obj_set_style_bg_color(_perfOverlay, (uiTheme.perfBg), 0);
    lv_obj_set_style_bg_opa(_perfOverlay, (lv_opa_t)uiSz.perfBgOpa, 0);
    lv_obj_set_style_pad_all(_perfOverlay, uiSz.perfPad, 0);
#if defined(BOARD_PANEL_1024X600)
    // Top-right corner of the INSTRUMENT area, never of the screen: the screen
    // corner belongs to the rail or the data sidebar, and every WebUI save
    // rebuilds the sidebar cells - the fresh objects then covered this overlay,
    // which looked exactly like "the perf overlay switch stopped working".
    // Landscape: pull left past the sidebar column. Portrait: the sidebar is at
    // the BOTTOM, so the right edge is free and we only drop below the rail.
    if (uiPortrait()) lv_obj_align(_perfOverlay, LV_ALIGN_TOP_RIGHT, -2, UI7_RAIL_W + 2);
    else              lv_obj_align(_perfOverlay, LV_ALIGN_TOP_RIGHT, -(UI7_SIDEBAR_W + 2), 2);
#else
    lv_obj_align(_perfOverlay, LV_ALIGN_TOP_RIGHT, -2, 2);
#endif
    // Visibility follows config
    if (!appConfig.cfg.showPerfOverlay)
        lv_obj_add_flag(_perfOverlay, LV_OBJ_FLAG_HIDDEN);
}

void DisplayManager::updatePerfOverlay() {
    if (!_perfOverlay) return;

    // Show/hide based on current config (can be toggled live via WebUI)
    if (!appConfig.cfg.showPerfOverlay) {
        lv_obj_add_flag(_perfOverlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(_perfOverlay, LV_OBJ_FLAG_HIDDEN);

    // Update at most once per second
    uint32_t now = millis();
    uint32_t elapsed = now - _perfLastMs;
    if (elapsed < 1000) return;

    // fps = COMPLETED LVGL REFRESHES per second, not displayTick() calls as
    // this comment used to claim: displayTick() runs on every loop() pass and
    // usually finds nothing invalidated, so counting those would have reported
    // a couple of hundred "frames" a second on a screen that never changed.
    // What the counter behind getTickFps() means per build:
    //   1024x600 - LVGL's monitor_cb, one call per finished refresh in every
    //              orientation (DisplaySetup_7B.cpp)
    //   simulator- presented SDL frames (sim_main.cpp)
    //   4"       - unchanged, a flush strip that reached the last screen row
    // On 1024x600 the heartbeat in main.cpp reads that same counter, so
    // getTickFps(true) there resets only THIS reader's baseline - otherwise the
    // two consumers would each see a fraction of the real frame count. The 4"
    // board and the simulator have only this one reader and keep the plain
    // reset-on-read they always had.
    float fps = (getTickFps(true) * 1000.0f) / elapsed;

    // CPU% = share of wall time spent painting the CURRENT SCREEN's canvas.
    // Narrower than the "dispMgr.update()" this used to name: _perfBusyUs is
    // accumulated around _screens[_cur]->update() alone (see update()), so the
    // overlay handling, sidebar, alarm and theme work in the same function are
    // not in it. While a modal is open nothing is painted and this reads ~0,
    // which is correct rather than broken.
    float cpu = (_perfBusyUs / 1000.0f) * 100.0f / elapsed;

    // Free heap
    uint32_t freeDram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;

    char buf[48];
    snprintf(buf, sizeof(buf), "%.0ffps  CPU%.0f%%  %uk", fps, cpu, freeDram);
    lv_label_set_text(_perfOverlay, buf);

    // Reset counters
    _perfFrames  = 0;
    _perfBusyUs  = 0;
    _perfLastMs  = now;
}

// ---- activate ---------------------------------------------------------------
// Call this once, after bootScreen.dismiss().
//
// Boot screen is fully deleted by now – no LVGL objects from it remain.
// Safe to:
//   1. Apply the dark theme (fires LV_EVENT_STYLE_CHANGED on all objects,
//      but there are no boot-screen labels left to corrupt).
//   2. Build the entire main-UI object tree.
//   3. Load the main screen and show the first data screen.

void DisplayManager::activate() {
    Serial.println("[ACT] 1: theme (reuse)"); Serial.flush();
    // lv_disp_drv_register() already called lv_theme_default_init() once when the
    // display was first registered (see lv_disp.c).  Calling it a SECOND time here
    // triggers style_init_reset → lv_style_reset → lv_mem_free on theme style
    // values_and_props that were allocated during the first init.  By this point
    // that PSRAM block's header has been corrupted (→ "Bad head" assert).
    //
    // Fix: reuse the theme that lv_disp_drv_register() already installed.
    // Individual screens use explicit lv_obj_set_style_* calls for their exact
    // colours, so the theme's default palette doesn't matter for our UI.
    lv_theme_t *th = lv_disp_get_theme(lv_disp_get_default());
    lv_disp_set_theme(lv_disp_get_default(), th);   // no-op, keeps existing theme
    Serial.println("[ACT] 2: theme set"); Serial.flush();

    // 2. Build main screen.
    _mainScreen = lv_obj_create(nullptr);
    lv_obj_t *root = _mainScreen;
    lv_obj_set_style_bg_color(root, CLR_BG, 0);
    lv_obj_set_style_bg_opa(root, OPA_FULL, 0);
    // While a full-screen overlay is open the main screen is hidden (see
    // update()), and LVGL then paints the DISPLAY background instead - which
    // defaults to white. Nothing of it is visible behind an LV_OPA_COVER
    // overlay, but a white flash on the way in or out would be, so it matches
    // the UI background.
    lv_disp_set_bg_color(lv_disp_get_default(), CLR_BG);
#if defined(BOARD_PANEL_1024X600)
    stripChrome(root);   // see the note at stripChrome()
#endif
    Serial.println("[ACT] 3: root created"); Serial.flush();

    // Content area (above nav bar)
    lv_obj_t *content = lv_obj_create(root);
    lv_obj_set_size(content, SCREEN_W, SCREEN_H - NAV_BAR_H);
#if defined(BOARD_PANEL_1024X600)
    stripChrome(content);   // see the note at stripChrome()
#endif
#if defined(BOARD_PANEL_1024X600)
#if UI7_CONTENT_ZOOM != 256
    // Stage 2 (DEAD since the stage-3 port - UI7_CONTENT_ZOOM is 256 on both
    // 1024x600 boards, and SCREEN_W/H are already 600, so line 718 makes this
    // container 600x600 and a 1.25x zoom would produce 750x750, not 600x600).
    // The original intent was to zoom the 480x480 grid up to the 600 px middle
    // column. Pivot top-left, so the layout position stays the column origin;
    // LVGL extends invalidation and hit-testing to the transformed area itself.
    // Position comes from the same orientation-aware helpers as the live stage-1
    // branch below - the hard-coded (UI7_CENTER_X, 0) that used to stand here
    // was the landscape three-column shape and would have dropped the block on
    // top of the rail in portrait. Identical in landscape, where contentX() is
    // UI7_CONTENT_X == UI7_CENTER_X and contentY() is UI7_CONTENT_Y == 0.
    lv_obj_set_pos(content, contentX(), contentY());
    lv_obj_set_style_transform_pivot_x(content, 0, 0);
    lv_obj_set_style_transform_pivot_y(content, 0, 0);
    lv_obj_set_style_transform_zoom(content, UI7_CONTENT_ZOOM, 0);
#else
    // Stage 1: 7B three-column layout, the 480x480 screens render 1:1 and
    // sharp, centered in the 600px middle column between rail and sidebar.
    lv_obj_set_pos(content, contentX(), contentY());
#endif
#else
    lv_obj_set_pos(content, 0, 0);
#endif
    lv_obj_set_style_bg_color(content, CLR_BG, 0);
    lv_obj_set_style_bg_opa(content, OPA_FULL, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    _content = content;   // retained for reference (screens are created below)
    Serial.println("[ACT] 4: content created"); Serial.flush();

    // Create all screen containers (initially hidden).
    // esp_task_wdt_reset() per screen: canvas memset (up to 412 KB of PSRAM) +
    // LVGL object creation together take ~200-400 ms per screen.  Resetting the
    // WDT here ensures Core-0-IDLE's 3 s subscription window is never exceeded
    // even if disableCore0WDT() was called too early (before WiFi subscribes it).
    //
    // vTaskDelay(2) before each screen: the WiFi beacon ISR (priority 3, Core 0)
    // fires every 100 ms and needs SPI0 for its PSRAM packet buffers.  ICache
    // fills for Flash-resident LVGL code intermittently hold SPI0 for short
    // bursts.  On Arduino-ESP32 the IWDT threshold is 300 ms; without a yield
    // the cumulative stall across 8-9 cells can exceed it.  2 ms of sleep
    // provides a guaranteed SPI0-free window every screen so the beacon ISR
    // can finish its DMA and the FreeRTOS tick ISR can run.
    // Create only PRESENT screens: the 8 fixed instruments + active grid slots.
    for (int i = 0; i < MAX_SCREENS; i++) {
        if (!_screens[i]) continue;   // inactive grid slot
        // esp_task_wdt_reset() removed - never-subscribed no-op, floods the
        // serial log on Arduino 3.x (see the note at the other site above)
        vTaskDelay(pdMS_TO_TICKS(2));   // yield SPI0 to WiFi beacon ISR
        Serial.printf("[ACT] 5.%d: create \"%s\"  free=%u\n",
            i, _screens[i]->title(),
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        Serial.flush();
        _screens[i]->create(content);
        if (_screens[i]->container)
            lv_obj_add_flag(_screens[i]->container, LV_OBJ_FLAG_HIDDEN);
#if defined(BOARD_PANEL_1024X600)
        // Centrally, not inside each screen's create(): the screen files are
        // shared with the released 4-inch board and must not change there.
        stripChrome(_screens[i]->container);
#endif
        {
            lv_mem_monitor_t _m; lv_mem_monitor(&_m);
            Serial.printf("[ACT] 5.%d: OK  lv pool used=%u free=%u\n",
                i, _m.total_size - _m.free_size, _m.free_size);
        }
        Serial.flush();
    }
    Serial.println("[ACT] 6: screens created"); Serial.flush();

#if defined(BOARD_PANEL_1024X600)
    // 7B: navigation lives in the left rail (SideBar) - no floating overlay
    // arrows, no fade timer, no top gear. Everything downstream is null-safe
    // for the untouched _btnPrev/_btnNext/_btnSettings pointers.
    sideBar.build(root);
    Serial.println("[ACT] 7: rail + sidebar created"); Serial.flush();
#else
    // Side-overlay navigation arrows (no bottom bar)
    buildOverlayNav(root);
    Serial.println("[ACT] 7: overlay nav created"); Serial.flush();
#endif

    // Demo mode warning banner – last child so it renders on top.
    _demoBanner = lv_obj_create(root);
#if defined(BOARD_PANEL_1024X600)
    // Banner spans the instrument column only (not rail/sidebar).
    lv_obj_set_size(_demoBanner, UI7_CENTER_W, uiSz.demoBannerH);
    lv_obj_set_pos(_demoBanner, contentX(), contentY());
#else
    lv_obj_set_size(_demoBanner, SCREEN_W, uiSz.demoBannerH);
    lv_obj_set_pos(_demoBanner, 0, 0);
#endif
    lv_obj_set_style_bg_color(_demoBanner, (uiTheme.demoBanner), 0);
    lv_obj_set_style_bg_opa(_demoBanner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_demoBanner, 0, 0);
    lv_obj_set_style_pad_all(_demoBanner, 2, 0);
    lv_obj_clear_flag(_demoBanner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_demoBanner, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bannerLbl = lv_label_create(_demoBanner);
    // U+26A0 (warning sign) is not in the font — use LV_SYMBOL_WARNING. The en
    // dash IS available now via the latin_suppl fallback, but the banner text
    // itself lives in the string table.
    char banner[96];
    snprintf(banner, sizeof(banner), "%s  %s  %s", npSym(NP_ICON_DEMO),   // OpenBridge icon trial
             T(STR_BANNER_DEMO_MODE), npSym(NP_ICON_DEMO));
    lv_label_set_text(bannerLbl, banner);
    lv_obj_set_style_text_color(bannerLbl, (uiTheme.demoText), 0);
    lv_obj_set_style_text_font(bannerLbl, FONT_TINY, 0);
    lv_obj_align(bannerLbl, LV_ALIGN_CENTER, 0, 0);
    Serial.println("[ACT] 8: demoBanner created"); Serial.flush();

    // Global anchor-drag alarm banner – red, blinking, top of screen, floats above
    // all content (created last + moved to foreground when active). Fires on ANY
    // screen so the watch is effective even when another instrument is shown.
    _alarmBanner = lv_obj_create(root);
#if defined(BOARD_PANEL_1024X600)
    lv_obj_set_size(_alarmBanner, UI7_CENTER_W, 30);
    lv_obj_set_pos(_alarmBanner, contentX(), contentY());
#else
    lv_obj_set_size(_alarmBanner, SCREEN_W, 30);
    lv_obj_set_pos(_alarmBanner, 0, 0);
#endif
    lv_obj_set_style_bg_color(_alarmBanner, CLR_RED, 0);
    lv_obj_set_style_bg_opa(_alarmBanner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_alarmBanner, 0, 0);
    lv_obj_set_style_radius(_alarmBanner, 0, 0);
    lv_obj_set_style_pad_all(_alarmBanner, 2, 0);
    lv_obj_clear_flag(_alarmBanner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_alarmBanner, LV_OBJ_FLAG_HIDDEN);
    _alarmLbl = lv_label_create(_alarmBanner);
    lv_label_set_text(_alarmLbl, T(STR_ALARM_ANCHOR_DRAG));
    lv_obj_set_style_text_color(_alarmLbl, CLR_ON_ACCENT, 0);
    lv_obj_set_style_text_font(_alarmLbl, FONT_SMALL, 0);
    lv_obj_align(_alarmLbl, LV_ALIGN_CENTER, 0, 0);

    // Performance overlay – top-right corner, above all other content.
    buildPerfOverlay(root);
    _perfLastMs = millis();
    Serial.println("[ACT] 9a: perfOverlay created"); Serial.flush();

    // 3. Make main screen active and show the first ENABLED screen per config.
    lv_scr_load(_mainScreen);
    Serial.println("[ACT] 10: scr_load done"); Serial.flush();
    applyScreenConfig();                          // builds _navOrder from appConfig
    restoreLastScreen();                          // last one used, or the first
    Serial.println("[ACT] 11: showScreen done"); Serial.flush();
}
