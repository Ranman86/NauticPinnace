// ============================================================================
// SideBar.cpp - see SideBar.h. Entire implementation is 7B-only.
// ============================================================================
#include "SideBar.h"

SideBar sideBar;   // single global, like dispMgr

#if defined(BOARD_PANEL_1024X600)

#include "DisplayManager.h"
#include "Theme.h"
#include "../config/Config.h"
#include "../nmea/DataModel.h"     // dmFieldByKey()
#include "../i18n/I18n.h"          // i18nFieldName()
#include <math.h>
#include <string.h>
#include <stdio.h>

// NauticPi brand mark (tools/gen_logo_mark.py): two recolorable alpha layers,
// so the pi follows the theme text color and the wave the accent. The rail
// uses the small 54x35 render (75%); the home overlay header the full 72x46.
LV_IMG_DECLARE(logo_mark_pi_sm);
LV_IMG_DECLARE(logo_mark_wave_sm);

// ---- rail button callbacks ---------------------------------------------------
// All four route through DisplayManager's public API. Home and Config go
// through deferred request*() calls (opened in update(), outside the event).
static void cbHome(lv_event_t *e)   { dispMgr.requestOpenHome(); }
static void cbPrev7(lv_event_t *e)  { dispMgr.prevScreen(); }
static void cbNext7(lv_event_t *e)  { dispMgr.nextScreen(); }
static void cbConfig(lv_event_t *e) { dispMgr.requestOpenConfig(); }

// Bounds-guarded access to the configured cells.
static const GridCell &cellCfg(const SidebarConfig &sc, int i) {
    // const, so the 44-byte "nothing here" placeholder is constant-initialized
    // into flash (.rodata) instead of taking up .bss for the whole app lifetime.
    // GridCell's default member initializers are all compile-time constants, so
    // there is nothing left to run at start-up either (no guard variable). The
    // braces are not decoration: plain "const GridCell empty;" leans on the
    // const-default-constructible rule, which older GCC front ends reject
    // outright - value-initialization means the same thing on every compiler.
    // We only ever hand this out through the const reference below, so nobody
    // can write to it; an attempt would now be a compile error instead of
    // silently corrupting the shared placeholder for every caller.
    static const GridCell empty{};
    if (i < 0 || i >= SIDEBAR_MAX_CELLS) return empty;
    return sc.cells[i];
}

// Defaults are the LANDSCAPE geometry (72 px wide = 11 mm at 170 ppi); the
// portrait top bar passes both dimensions explicitly.
// Turn off the theme's border_post on the chrome that every strip crosses.
//
// WHY IT COSTS: lv_obj_create() picks up the default theme's card style, which
// sets border_post. That makes lv_obj run a COMPLETE SECOND descriptor rebuild
// per strip - ~15 style properties re-read - purely to draw the border after
// the children instead of before them. The rail and the sidebar slab have
// border width 0, so their second pass draws nothing at all. The cells and the
// rail buttons do have a border, but drawing it in the first pass is just as
// good here: nothing inside them overlaps the edge except the unit label, which
// sits 2 px clear of it.
//
// WHY IT MATTERS ON EVERY SCREEN: in landscape a draw-buffer strip spans the
// full 1024 px, so it crosses the rail, the instrument block AND the sidebar
// every single time - about 30 chrome objects visited on each of the 60 strips
// of a full refresh, whatever screen is showing. Measured: the same treatment
// on four containers in DisplayManager bought 17 ms of 214.
//
// Radius is deliberately NOT touched here: the cells and buttons are rounded by
// design. Only border_post goes, and only where it changes nothing visible.
static inline void noBorderPost(lv_obj_t *o) {
    if (o) lv_obj_set_style_border_post(o, false, 0);
}

static lv_obj_t *makeRailBtn(lv_obj_t *parent, const char *symbol,
                             lv_event_cb_t cb, int h = 80,
                             int w = UI7_RAIL_W - 16) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, (uiTheme.surface), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, (uiTheme.border), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, uiSz.cardRadius, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    noBorderPost(btn);   // keeps the 1 px border, drops the second pass
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, (uiTheme.text), 0);
    lv_obj_set_style_text_font(lbl, FONT_LARGE, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

// The two brand-mark layers, stacked at the same spot: the pi follows the theme
// text colour, the wave the accent. Alignment differs per orientation, so the
// caller does that; everything else is identical.
static void makeRailLogo(lv_obj_t *rail, lv_obj_t **pi, lv_obj_t **wave) {
    *pi = lv_img_create(rail);
    lv_img_set_src(*pi, &logo_mark_pi_sm);
    lv_obj_set_style_img_recolor(*pi, (uiTheme.text), 0);
    lv_obj_set_style_img_recolor_opa(*pi, LV_OPA_COVER, 0);
    *wave = lv_img_create(rail);
    lv_img_set_src(*wave, &logo_mark_wave_sm);
    lv_obj_set_style_img_recolor(*wave, (uiTheme.accent), 0);
    lv_obj_set_style_img_recolor_opa(*wave, LV_OPA_COVER, 0);
}

void SideBar::buildRail() {
    // The rail keeps its 88 px thickness in both orientations - only the axis
    // it runs along changes, so the 88 + 600 + 336 = 1024 split still adds up.
    // Rotation is a runtime config value, so this must branch at runtime, not
    // on a macro: LCD_WIDTH/LCD_HEIGHT are the PHYSICAL panel and stay
    // 1024x600 even when the picture is turned.
    const bool portrait = uiPortrait();

    _rail = lv_obj_create(_root);
    if (portrait) lv_obj_set_size(_rail, uiScreenW(), UI7_RAIL_W);  // top bar
    else          lv_obj_set_size(_rail, UI7_RAIL_W, uiScreenH());  // left column
    lv_obj_set_pos(_rail, 0, 0);
    lv_obj_set_style_bg_color(_rail, (uiTheme.bg), 0);
    lv_obj_set_style_bg_opa(_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_rail, 0, 0);
    lv_obj_set_style_pad_all(_rail, 8, 0);
    lv_obj_clear_flag(_rail, LV_OBJ_FLAG_SCROLLABLE);
    noBorderPost(_rail);
    lv_obj_set_style_radius(_rail, 0, 0);      // border width is 0; nothing to round

    lv_obj_t *logoPi = nullptr, *logoWave = nullptr;
    makeRailLogo(_rail, &logoPi, &logoWave);

    if (portrait) {
        // ---- PORTRAIT: 600 x 88 bar across the top, children left->right ----
        // Height is now the scarce axis (72 px of content after the 8 px pad),
        // so Prev/Next cannot be made taller than the rest the way they are in
        // landscape. They get the WIDER footprint instead - 1.5x Home/Config -
        // so the most-used targets still are the biggest ones under a wet
        // glove. Widths are derived, not literals, so the row always fills the
        // bar exactly (at 600 px: logo 54, Home/Config 98, Prev/Next 147,
        // gaps 10 -> 54+98+147+147+98 + 4*10 = 584 = the content width).
        const int pad    = 8;                            // == pad_all above
        const int availW = uiScreenW() - 2 * pad;
        const int btnH   = UI7_RAIL_W - 2 * pad;         // 72 px, capped by the bar
        const int logoW  = 54;                           // logo_mark_*_sm is 54x35
        const int gapMin = 10;
        const int budget = availW - logoW - 4 * gapMin;  // width left for 4 buttons
        const int wNav   = budget / 5;                   // Home/Config = 1 unit
        const int wBig   = (budget - 2 * wNav) / 2;      // Prev/Next   = 1.5 units
        // Re-derive the gap from what is actually left so the integer division
        // above cannot leave a ragged right edge.
        const int gap    = (availW - logoW - 2 * wNav - 2 * wBig) / 4;
        int x = 0;

        lv_obj_align(logoPi,   LV_ALIGN_LEFT_MID, x, 0);
        lv_obj_align(logoWave, LV_ALIGN_LEFT_MID, x, 0);
        x += logoW + gap;

        _btnHome = makeRailBtn(_rail, LV_SYMBOL_HOME,     cbHome,   btnH, wNav);
        lv_obj_align(_btnHome, LV_ALIGN_LEFT_MID, x, 0);
        x += wNav + gap;

        _btnPrev = makeRailBtn(_rail, LV_SYMBOL_LEFT,     cbPrev7,  btnH, wBig);
        lv_obj_align(_btnPrev, LV_ALIGN_LEFT_MID, x, 0);
        x += wBig + gap;

        _btnNext = makeRailBtn(_rail, LV_SYMBOL_RIGHT,    cbNext7,  btnH, wBig);
        lv_obj_align(_btnNext, LV_ALIGN_LEFT_MID, x, 0);
        x += wBig + gap;

        _btnCfg  = makeRailBtn(_rail, LV_SYMBOL_SETTINGS, cbConfig, btnH, wNav);
        lv_obj_align(_btnCfg, LV_ALIGN_LEFT_MID, x, 0);
        return;
    }

    // ---- LANDSCAPE: 88 x 600 column, children top->bottom (unchanged) ------
    // Brand mark on top; Home below it; Prev/Next as a taller middle pair
    // (the most-used buttons get the biggest targets: 72x120 px = 11x18 mm);
    // Config bottom. Each element is an independent object, so moving one
    // later is a two-line change.
    lv_obj_align(logoPi,   LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_align(logoWave, LV_ALIGN_TOP_MID, 0, 2);

    _btnHome = makeRailBtn(_rail, LV_SYMBOL_HOME,     cbHome);
    lv_obj_align(_btnHome, LV_ALIGN_TOP_MID, 0, 45);

    _btnPrev = makeRailBtn(_rail, LV_SYMBOL_LEFT,     cbPrev7, 120);
    lv_obj_align(_btnPrev, LV_ALIGN_CENTER, 0, -66);

    _btnNext = makeRailBtn(_rail, LV_SYMBOL_RIGHT,    cbNext7, 120);
    lv_obj_align(_btnNext, LV_ALIGN_CENTER, 0, 66);

    _btnCfg  = makeRailBtn(_rail, LV_SYMBOL_SETTINGS, cbConfig);
    lv_obj_align(_btnCfg, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// Value font for a cell. The HEIGHT ladder is the original one (same rungs as
// GridScreen). The WIDTH term only ever bites in portrait: the landscape column
// is always UI7_SIDEBAR_W - 12 = 324 px wide, which is the top rung, and max()
// with the top rung is a no-op - so the landscape column keeps picking exactly
// the font it always did. The portrait grid however also narrows the cells, and
// a 48 px value in a 142 px cell would be clipped away.
// Widths are ~6 digits at Montserrat's ~0.6 em advance plus 2x cardPad.
// Rung 0 = biggest.
static const lv_font_t *cellValueFont(int w, int h) {
    const int byH = (h > 100) ? 0 : (h > 75)  ? 1 : (h > 55)  ? 2 : 3;
    const int byW = (w >= 200) ? 0 : (w >= 170) ? 1 : (w >= 140) ? 2 : 3;
    const int rung = (byH > byW) ? byH : byW;
    return (rung == 0) ? FONT_HUGE :
           (rung == 1) ? FONT_XXL  :
           (rung == 2) ? FONT_XL   : FONT_LARGE;
}

// One value cell at an absolute rect inside _sidebar. Both orientation
// branches of buildCells() only compute rects and call this, so the cell
// content and styling physically cannot drift apart between the two layouts.
void SideBar::makeCell(int i, int x, int y, int w, int h) {
    const GridCell &cfg = cellCfg(appConfig.cfg.sidebar, i);
    Cell &c = _cells[i];
    c.container = lv_obj_create(_sidebar);
    lv_obj_set_size(c.container, w, h);
    lv_obj_set_pos(c.container, x, y);
    styleCard(c.container);
    lv_obj_clear_flag(c.container, LV_OBJ_FLAG_SCROLLABLE);
    noBorderPost(c.container);

    c.lblLabel = lv_label_create(c.container);
    const char *labelText = strlen(cfg.label) > 0 ? cfg.label
                                                  : i18nFieldName(cfg.pgn);
    lv_label_set_text(c.lblLabel, labelText);
    styleLabel(c.lblLabel, FONT_SMALL, CLR_TEXT_DIM);
    lv_obj_align(c.lblLabel, LV_ALIGN_TOP_MID, 0, 0);

    c.lblValue = lv_label_create(c.container);
    lv_label_set_text(c.lblValue, "--");
    styleLabel(c.lblValue, cellValueFont(w, h), CLR_TEXT);
    lv_obj_align(c.lblValue, LV_ALIGN_CENTER, 0, 4);

    c.lblUnit = lv_label_create(c.container);
    lv_label_set_text(c.lblUnit, cfg.unit);
    styleLabel(c.lblUnit, FONT_TINY, CLR_TEXT_DIM);
    lv_obj_align(c.lblUnit, LV_ALIGN_BOTTOM_RIGHT, -2, 0);
}

// Portrait column count per configured cell count (index = count - 1). Rows
// follow from it. Picked for the squarest and largest cells the 600x336 band
// allows (m = g = 6 -> 588 x 324 usable):
//   1 -> 1 row  x 1 col : 588 x 324     5 -> 2 rows x 3 cols : 192 x 159 *
//   2 -> 1 row  x 2 cols: 291 x 324     6 -> 2 rows x 3 cols : 192 x 159
//   3 -> 1 row  x 3 cols: 192 x 324     7 -> 2 rows x 4 cols : 142 x 159 *
//   4 -> 2 rows x 2 cols: 291 x 159     8 -> 2 rows x 4 cols : 142 x 159
// (*) 5 and 7 keep the 6- resp. 8-cell grid and leave the last slot empty.
// Squeezing 5 or 7 columns into 588 px would give 112 / 78 px cells, too
// narrow to render a value at all - an empty slot is the better trade.
static const uint8_t kPortraitCols[SIDEBAR_MAX_CELLS] = { 1, 2, 3, 2, 3, 3, 4, 4 };

void SideBar::buildCells() {
    const SidebarConfig &sc = appConfig.cfg.sidebar;
    _count = sc.count;
    if (_count < 1) _count = 1;
    if (_count > MAX_CELLS) _count = MAX_CELLS;

    const int m = 6, g = 6;

    if (uiPortrait()) {
        // ---- PORTRAIT: the sidebar is a 600 x 336 band -> a real grid ------
        // UI7_SIDEBAR_W is the slab's 336 px thickness, which is its HEIGHT
        // here; the width comes from the (rotated) screen.
        const int cols = kPortraitCols[_count - 1];
        const int rows = (_count + cols - 1) / cols;
        const int w = (uiScreenW()   - 2 * m - (cols - 1) * g) / cols;
        const int h = (UI7_SIDEBAR_W - 2 * m - (rows - 1) * g) / rows;
        for (int i = 0; i < _count; i++)      // row-major: left->right, top->bottom
            makeCell(i, m + (i % cols) * (w + g), m + (i / cols) * (h + g), w, h);
        return;
    }

    // ---- LANDSCAPE: 336 x 600 column -> the original 1xN stack -------------
    const int w = UI7_SIDEBAR_W - 2 * m;
    const int h = (uiScreenH() - 2 * m - (_count - 1) * g) / _count;
    for (int i = 0; i < _count; i++)
        makeCell(i, m, m + i * (h + g), w, h);
}

void SideBar::build(lv_obj_t *root) {
    _root = root;
    buildRail();

    _sidebar = lv_obj_create(_root);
    // Same 336 px slab in both orientations, only the axis changes: the right
    // column in landscape, a bottom band in portrait sitting directly below
    // the 600 px instrument square (88 + 600 + 336 = 1024 either way, so the
    // instrument keeps its size and no screen has to know about rotation).
    if (uiPortrait()) {
        lv_obj_set_size(_sidebar, uiScreenW(), UI7_SIDEBAR_W);
        // Flush to the BOTTOM edge, stated as such instead of as the sum
        // UI7_RAIL_W + UI7_CENTER_W. Same 688 px today (1024 - 336), but it now
        // follows the logical screen rather than silently depending on
        // 88 + 600 + 336 still adding up to the screen height.
        lv_obj_set_pos(_sidebar, 0, uiScreenH() - UI7_SIDEBAR_W);
    } else {
        lv_obj_set_size(_sidebar, UI7_SIDEBAR_W, uiScreenH());
        lv_obj_set_pos(_sidebar, UI7_SIDEBAR_X, 0);
    }
    lv_obj_set_style_bg_color(_sidebar, (uiTheme.bg), 0);
    lv_obj_set_style_bg_opa(_sidebar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_sidebar, 0, 0);
    lv_obj_set_style_pad_all(_sidebar, 0, 0);
    lv_obj_clear_flag(_sidebar, LV_OBJ_FLAG_SCROLLABLE);
    noBorderPost(_sidebar);
    lv_obj_set_style_radius(_sidebar, 0, 0);   // border width is 0; nothing to round

    buildCells();
}

void SideBar::destroyCells() {
    for (int i = 0; i < MAX_CELLS; i++) {
        if (_cells[i].container) lv_obj_del(_cells[i].container);
        _cells[i] = Cell{};
    }
    _count = 0;
}

void SideBar::applyConfig() {
    if (!_sidebar) return;
    destroyCells();
    buildCells();
}

void SideBar::updateValues() {
    const SidebarConfig &sc = appConfig.cfg.sidebar;
    for (int i = 0; i < _count; i++) {
        if (!_cells[i].lblValue) continue;
        const GridCell &cfg = cellCfg(sc, i);
        if (!cfg.pgn[0]) { lv_label_set_text(_cells[i].lblValue, ""); continue; }
        // dmFieldByKey takes the DataModel lock itself (non-recursive mutex -
        // this must NOT run while already holding data.lock()).
        const float v = dmFieldByKey(cfg.pgn);
        char buf[24];
        if (isnan(v)) snprintf(buf, sizeof(buf), "--");
        else {
            char fmt[12];
            snprintf(fmt, sizeof(fmt), "%%.%df", cfg.decimals);
            snprintf(buf, sizeof(buf), fmt, v);
        }
        lv_label_set_text(_cells[i].lblValue, buf);
    }
}

void SideBar::restyle() {
    // Colors/sizes/fonts changed: cheapest correct path is a full rebuild.
    // No orientation state is cached in this class - build() re-asks
    // uiPortrait() - but do NOT conclude that a rotation change could be
    // applied through here. It cannot: DisplayManager::reloadThemeLive()
    // rebuilds the screens but never MOVES _content, so rail and sidebar would
    // take the new orientation while the instrument square stayed at the old
    // contentX()/contentY() - a half-rotated UI. Rotation is boot-only by
    // design (Config.h) and the WebUI restarts the device after saving it.
    if (!_root) return;
    destroyCells();
    if (_rail)    { lv_obj_del(_rail);    _rail = nullptr; }
    if (_sidebar) { lv_obj_del(_sidebar); _sidebar = nullptr; }
    build(_root);
}

#endif  // BOARD_PANEL_1024X600
