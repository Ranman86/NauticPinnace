#pragma once
// ============================================================================
// SideBar - the 7B's three-part chrome: nav rail + data sidebar.
//
//   Rail (88 px):    Home, Prev/Next, Config.
//   Sidebar (336px): N configurable value cells (appConfig.cfg.sidebar),
//                    same GridCell config struct and dmFieldByKey() value
//                    pipeline as the data-grid screens.
//
// Both keep their pixel sizes in either orientation - only the axis changes,
// because 88 + 600 + 336 = 1024 works out the same stacked as side by side:
//   landscape (1024x600): rail column left  | instrument | sidebar column right
//   portrait  (600x1024): rail bar on top   / instrument / sidebar band bottom
// Rotation is a RUNTIME config value, so the same binary must render both:
// build() branches on uiPortrait(), never on a macro.
//
// 7B-only: on the 4" and the simulator this header still compiles, but the
// class collapses to no-op inlines, so DisplayManager can call it without
// per-call guards.
// ============================================================================
#include <lvgl.h>

#if defined(BOARD_PANEL_1024X600)

class SideBar {
public:
    // Build rail + sidebar as children of the main screen root. Call once from
    // DisplayManager::activate(), AFTER _content exists (z-order: chrome above).
    void build(lv_obj_t *root);

    // Re-read appConfig.cfg.sidebar and rebuild the value cells (count or
    // field changes from the WebUI - live, no reboot).
    void applyConfig();

    // Refresh the displayed values (throttled by the caller).
    void updateValues();

    // Theme changed: restyle rail buttons and rebuild cells with new colors.
    void restyle();

private:
    void buildRail();
    void buildCells();
    // One value cell at an absolute rect inside _sidebar. Split out so the
    // landscape column and the portrait grid only differ in the rects they
    // compute, never in the cell content.
    void makeCell(int i, int x, int y, int w, int h);
    void destroyCells();

    lv_obj_t *_root    = nullptr;
    lv_obj_t *_rail    = nullptr;
    lv_obj_t *_sidebar = nullptr;
    lv_obj_t *_btnHome = nullptr, *_btnPrev = nullptr,
             *_btnNext = nullptr, *_btnCfg  = nullptr;

    struct Cell {
        lv_obj_t *container = nullptr;
        lv_obj_t *lblLabel  = nullptr;
        lv_obj_t *lblValue  = nullptr;
        lv_obj_t *lblUnit   = nullptr;
    };
    static constexpr int MAX_CELLS = 8;
    Cell _cells[MAX_CELLS];
    int  _count = 0;
};

#else   // 4" board / simulator: inert stub

class SideBar {
public:
    void build(lv_obj_t *) {}
    void applyConfig() {}
    void updateValues() {}
    void restyle() {}
};

#endif

extern SideBar sideBar;
