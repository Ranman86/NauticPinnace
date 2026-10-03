#pragma once
#include <lvgl.h>
#include "BaseScreen.h"
#include "../BoardConfig.h"
#include "../config/Config.h"   // MAX_SCREENS, N_FIXED_SCREENS, MAX_GRIDS, GRID_ID_BASE

// Fixed instrument screen ids (0..7). Data-grid slots use ids
// GRID_ID_BASE..GRID_ID_BASE+MAX_GRIDS-1 (see Config.h). Total = MAX_SCREENS.
enum ScreenID {
    SCR_WIND      = 0,
    SCR_SPEED     = 1,
    SCR_DEPTH     = 2,
    SCR_ENGINE    = 3,
    SCR_RUDDER    = 4,
    SCR_AIS       = 5,
    SCR_WINDPLOT  = 6,
    SCR_AUTOPILOT = 7,
    SCR_MEDIA    = 8,
    SCR_ATTITUDE  = 9,   // "Schiffslage" – attitude variant of the wind screen
    SCR_ANCHOR    = 10,  // "Ankerwache" – anchor drift watch
    SCR_TANK      = 11,  // "Tanks" – fluid levels (PGN 127505)
    SCR_BATTERY   = 12,  // "Batterie" – DC energy monitor (PGN 127508/127506)
    SCR_WEATHER   = 13,  // "Wetter" – barometer + environment (PGN 130310/11/14)
    SCR_CLOCK     = 14,  // "Uhr" – clock + sunrise/sunset (PGN 126992/129033)
    SCR_VMG       = 15,  // "VMG" – performance optimisation (polar)
    SCR_ROUTE     = 16,  // "Route" – waypoint navigation (PGN 129283/129284)
};

class DisplayManager {
public:
    void begin();
    void activate();
    void showScreen(int idx);
    void nextScreen();
    void prevScreen();
    void update();
    bool pendingUpdate() const { return _forceUpdate; }
    void handleButtons();        // physical buttons (BTN_PREV/NEXT pins)
    // Called from the touch callback (safe: only sets a flag, no LVGL calls)
    void requestShowNavArrows()  { _navShowPending = true; }
    // Called by internal LVGL timer – fades out arrows
    void onNavFadeTimer();
    // Called from update() to apply pending show-request outside lv_timer_handler
    void applyNavArrowsPending();

    int  currentIndex() const { return _cur; }
    const char *currentTitle() const;

#if defined(BOARD_PANEL_1024X600)
    // Worst canvas paint per screen since boot, one line, sorted. See the
    // definition; the 4" board has no such counters and no such method.
    void reportScreenPaint() const;
#endif

    // ---- Screen catalog / navigation config -------------------------------
    int  screenTotal() const { return MAX_SCREENS; }   // iteration bound for the catalog
    // Active navigation order, read-only - the 7B home overlay renders one
    // launcher tile per entry.
    int  navCount() const { return _navLen; }
    int  navScreenId(int pos) const { return (pos >= 0 && pos < _navLen) ? _navOrder[pos] : -1; }
    bool screenPresent(int id) const;                  // fixed (0..7) or active grid slot
    const char *screenName(int id) const;              // German label for WebUI
    const char *screenType(int id) const;              // "wind".."grid" – picks WebUI editor
    // Set a flag (safe from any task, e.g. the async web handler) to rebuild the
    // navigation order/visibility from appConfig on the next update() tick.
    void requestApplyScreenConfig() { _screenCfgPending = true; }
    // Reload the polar table from LittleFS on the next update() tick (safe from
    // the async web handler; the actual file read happens in the display loop).
    void requestPolarReload() { _polarReloadPending = true; }
    // Re-apply the theme (colours/sizes/fonts) and rebuild the UI live — no
    // reboot. Set from the async web handler; applied on the next update() tick.
    void requestThemeReload() { _themeReloadPending = true; }
    // Open the on-screen config overlay on the next update() tick (set from the
    // top hot-zone gesture cb; deferred so we open outside lv_timer_handler).
    void requestOpenConfig() { _openConfigPending = true; _forceUpdate = true; }
    // Open the 7B home/launcher overlay on the next update() tick (set by the
    // rail's home button; deferred like the config overlay above).
    void requestOpenHome() { _openHomePending = true; _forceUpdate = true; }
    // Show a full-screen message and reboot a few ticks later (so the message is
    // visible first). Avoids reentrant lv_refr_now()/blocking delay in an event cb.
    void requestReboot(const char *msg);

private:
    BaseScreen *_screens[MAX_SCREENS] = { nullptr };
    lv_obj_t   *_content     = nullptr;   // parent for screen containers (from activate)
    int         _cur         = 0;

    // Active navigation order: enabled screen IDs in display sequence.
    uint8_t     _navOrder[MAX_SCREENS];
    int         _navLen          = 0;
    bool        _screenCfgPending = false;
    bool        _polarReloadPending = false;
    bool        _themeReloadPending = false;
    bool        _openConfigPending  = false;
    bool        _openHomePending    = false;
    bool        _rebootPending = false;
    uint32_t    _rebootAtMs    = 0;
    void applyScreenConfig();          // rebuild _navOrder (call from LVGL-safe ctx)
    void reloadThemeLive();
    void refreshTrialChrome();      // OpenBridge icon trial: relabel persistent chrome            // re-theme + rebuild screens in place (no reboot)
    int  navPosOf(int id) const;       // position of screen id in _navOrder, or -1

    // ---- Remember the screen across a restart -------------------------------
    // A boat display should come back to what you were watching. Power on a
    // boat is not a controlled shutdown - the breaker goes and that is that -
    // so this cannot be written "on exit"; it has to be stored as it changes.
    //
    // NVS, not config.json: config.json means rewriting the whole file on the
    // LittleFS partition, and doing that on every swipe would be both slow and
    // hard on the flash. NVS is built for small values, wear-levels them, and
    // survives a "pio run -t uploadfs" - which replaces the whole filesystem.
    //
    // Written with a delay, never straight from the swipe: paging through the
    // carousel would otherwise put one flash write behind every screen. The
    // write lands once the user has settled, and the flash hit does not land in
    // the middle of a gesture.
    void restoreLastScreen();          // pick the boot screen; call after applyScreenConfig()
    void persistLastScreenDue();       // called every update(); writes when due
    int         _lastScreenPending = -1;   // screen id waiting to be stored, -1 = nothing
    uint32_t    _lastScreenAtMs    = 0;    // when it became pending
    lv_obj_t   *_mainScreen  = nullptr;
    lv_obj_t   *_demoBanner  = nullptr;
    uint8_t     _demoBlink   = 0;
    bool        _forceUpdate = false;

    // Everything the instrument view costs is suspended while a full-screen
    // overlay covers it. _modalWasOpen spots the closing edge so the view can
    // be brought up to date in one go instead of reappearing stale.
    bool        _modalWasOpen  = false;
    uint32_t    _lastSideBarMs = 0;

    // Global anchor-drag alarm (evaluated every tick so it fires on ANY screen).
    lv_obj_t   *_alarmBanner = nullptr;
    lv_obj_t   *_alarmLbl    = nullptr;
    bool        _anchorAlarmActive = false;
    uint8_t     _alarmBlink  = 0;
    uint32_t    _buzzToggleMs = 0;
    bool        _buzzOn       = false;
    void evaluateAnchorAlarm();
    void evaluateAutoTheme();   // solar light/dark switch (cfg.themeAuto)

    // Side-overlay navigation arrows + top-centre settings gear (transparent,
    // floating, auto-fade together).
    lv_obj_t   *_btnPrev        = nullptr;
    lv_obj_t   *_btnNext        = nullptr;
    lv_obj_t   *_btnSettings    = nullptr;   // opens the on-screen config overlay
    lv_timer_t *_navFadeTimer   = nullptr;  // fires after idle → fades out arrows
    bool        _navArrowsShown = true;
    bool        _navShowPending = false;    // set by touch-cb; consumed by update()
    bool        _settingsVisibleAtPress = false;  // gear visible when the tap began?
    bool        _navVisibleAtPress      = false;  // prev/next arrows visible when the tap began?

    // Performance overlay (FPS + CPU%)
    lv_obj_t   *_perfOverlay = nullptr;
    uint32_t    _perfFrames  = 0;
    uint32_t    _perfLastMs  = 0;
    uint32_t    _perfBusyUs  = 0;

    // Physical button state
    bool _prevPressed = false;
    bool _nextPressed = false;

    void buildOverlayNav(lv_obj_t *parent);
    void buildPerfOverlay(lv_obj_t *parent);
    void updatePerfOverlay();
    void updateDemoBanner();

    static void cbPrev(lv_event_t *e);
    static void cbNext(lv_event_t *e);
    static void cbGesture(lv_event_t *e);    // swipe gesture handler (screen content)
    static void cbSettings(lv_event_t *e);   // settings gear tap: opens config overlay
};

extern DisplayManager dispMgr;
