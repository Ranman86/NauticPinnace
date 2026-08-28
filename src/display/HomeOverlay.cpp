// ============================================================================
// HomeOverlay.cpp - see HomeOverlay.h. Entire implementation is 7B-only.
// ============================================================================
#include "HomeOverlay.h"

HomeOverlay homeOverlay;

#if defined(BOARD_PANEL_1024X600)

#include "../BoardConfig.h"   // board macros (the geometry comes from Theme.h)
#include "DisplayManager.h"
#include "Theme.h"
#include "../i18n/I18n.h"

#include <string.h>   // strchr/strlen for the tile caption measurement

// NauticPi brand mark (tools/gen_logo_mark.py) - same asset as the rail.
LV_IMG_DECLARE(logo_mark_pi);
LV_IMG_DECLARE(logo_mark_wave);

// Largest font whose LONGEST SINGLE WORD still fits the tile.
//
// LV_LABEL_LONG_WRAP breaks between words where it can, but when one word is
// wider than the label it breaks INSIDE the word, without a hyphen. In the
// narrow portrait tiles that turned "Geschwindigkeit & Polar" into
// "Geschwindigke / it & Polar". German compound nouns make that the normal
// case rather than an edge case, so the caption is measured instead of
// assumed - and it costs nothing on captions that already fit.
static const lv_font_t *tileLabelFont(const char *txt, lv_coord_t avail) {
    const lv_font_t *candidates[] = { FONT_MED, FONT_SMALL, FONT_TINY };
    for (const lv_font_t *f : candidates) {
        lv_coord_t widest = 0;
        for (const char *w = txt; *w; ) {
            const char  *end = strchr(w, ' ');
            const size_t len = end ? (size_t)(end - w) : strlen(w);
            const lv_coord_t ww =
                lv_txt_get_width(w, (uint32_t)len, f, 0, LV_TEXT_FLAG_NONE);
            if (ww > widest) widest = ww;
            if (!end) break;
            w = end + 1;
        }
        if (widest <= avail) return f;
    }
    return FONT_TINY;   // nothing fits: the smallest is still the least bad
}

// One built-in LVGL symbol per screen - the closest match the FontAwesome
// subset offers. Rendered big and accent-colored, like the line icons on the
// NauticPi MFD start page.
static const char *screenSymbol(int id) {
    switch (id) {
        case SCR_WIND:      return LV_SYMBOL_REFRESH;       // swirling air
        case SCR_SPEED:     return LV_SYMBOL_CHARGE;
        case SCR_DEPTH:     return LV_SYMBOL_DOWN;
        case SCR_ENGINE:    return LV_SYMBOL_POWER;
        case SCR_RUDDER:    return LV_SYMBOL_SHUFFLE;       // crossing courses
        case SCR_AIS:       return LV_SYMBOL_GPS;
        case SCR_WINDPLOT:  return LV_SYMBOL_LIST;          // history rows
        case SCR_AUTOPILOT: return LV_SYMBOL_PLAY;          // engaged
        case SCR_MEDIA:     return LV_SYMBOL_AUDIO;
        case SCR_ATTITUDE:  return LV_SYMBOL_LOOP;          // heel/roll
        case SCR_ANCHOR:    return LV_SYMBOL_DOWNLOAD;      // drop the hook
        case SCR_TANK:      return LV_SYMBOL_TINT;
        case SCR_BATTERY:   return LV_SYMBOL_BATTERY_FULL;
        case SCR_WEATHER:   return LV_SYMBOL_EYE_OPEN;      // watching conditions
        case SCR_CLOCK:     return LV_SYMBOL_BELL;          // alarm clock
        case SCR_VMG:       return LV_SYMBOL_UP;            // performance
        case SCR_ROUTE:     return LV_SYMBOL_NEXT;          // next waypoint
        default:            return LV_SYMBOL_KEYBOARD;      // data grids
    }
}

void HomeOverlay::cbClose(lv_event_t *e) { homeOverlay.close(); }

void HomeOverlay::cbTile(lv_event_t *e) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    homeOverlay.close();          // async delete - safe inside the event
    dispMgr.showScreen(id);
}

void HomeOverlay::cbConfigTile(lv_event_t *e) {
    homeOverlay.close();
    dispMgr.requestOpenConfig();  // deferred: opens on the next update() tick
}

void HomeOverlay::cbRootGesture(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) homeOverlay.close();
}

void HomeOverlay::open() {
    if (_open) return;
    _open = true;
    // Deliberately NO lv_mem_buf_free_all() (earlier fix attempt): the boot
    // PRIMES the big draw buffers into LVGL's cache while the pool is still
    // unfragmented (main.cpp), and freeing them would re-expose every render
    // to pool fragmentation - the actual killer (measured: a 4808-byte
    // lv_mem_buf_get failed at 11.9K free / 25% frag after a settings visit).

    _root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(_root);   // theme-free: every style below is explicit
    // The LOGICAL screen, not LCD_WIDTH/LCD_HEIGHT: those are the PHYSICAL
    // panel and stay 1024x600 however the picture is turned. lv_layer_top() is
    // sized to the logical resolution and clips its children, so at
    // display.rotation 90/270 a 1024-wide root loses its right third AND
    // leaves the bottom of the screen showing - and touchable - defeating the
    // modal. In landscape these helpers return exactly LCD_WIDTH/LCD_HEIGHT.
    lv_obj_set_size(_root, uiScreenW(), uiScreenH());
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, CLR_BG, 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_root, LV_OBJ_FLAG_CLICKABLE);   // modal: absorb touches
    lv_obj_add_event_cb(_root, cbRootGesture, LV_EVENT_GESTURE, nullptr);

    // ---- header: brand mark + name, like the MFD start page ----
    lv_obj_t *logoPi = lv_img_create(_root);
    lv_img_set_src(logoPi, &logo_mark_pi);
    lv_obj_set_pos(logoPi, 16, 4);
    lv_obj_set_style_img_recolor(logoPi, CLR_TEXT, 0);
    lv_obj_set_style_img_recolor_opa(logoPi, LV_OPA_COVER, 0);
    lv_obj_t *logoWave = lv_img_create(_root);
    lv_img_set_src(logoWave, &logo_mark_wave);
    lv_obj_set_pos(logoWave, 16, 4);
    lv_obj_set_style_img_recolor(logoWave, CLR_ACCENT, 0);
    lv_obj_set_style_img_recolor_opa(logoWave, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(_root);
    lv_label_set_text(title, "NauticPinnace");   // brand - never translated
    lv_obj_set_style_text_font(title, FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, CLR_TEXT, 0);
    lv_obj_set_pos(title, 100, 14);

    lv_obj_t *closeBtn = lv_btn_create(_root);
    lv_obj_set_size(closeBtn, 44, 36);
    // Off the logical width, so the button stays in the top right corner when
    // rotated instead of sitting 400 px past the edge - the only other way out
    // of the launcher is the undiscoverable swipe-up on cbRootGesture.
    lv_obj_set_pos(closeBtn, uiScreenW() - 12 - 44, 8);
    lv_obj_set_style_bg_color(closeBtn, CLR_SURFACE, 0);
    lv_obj_set_style_bg_color(closeBtn, CLR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(closeBtn, 8, 0);
    lv_obj_set_style_border_width(closeBtn, 0, 0);
    lv_obj_set_style_shadow_width(closeBtn, 0, 0);
    lv_obj_t *cl = lv_label_create(closeBtn);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(cl, CLR_TEXT, 0);
    lv_obj_set_style_text_font(cl, FONT_MED, 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(closeBtn, cbClose, LV_EVENT_CLICKED, nullptr);

    // ---- tile grid: active screens in nav order + one settings tile --------
    // Column count adapts to the tile count so a handful of screens gets big
    // comfortable targets while the worst case (17 fixed + 6 grids + settings
    // = 24 tiles) still fits without scrolling - as 6x4 in landscape and as
    // 3x8 in portrait, see the ladder below.
    const int total = dispMgr.navCount() + 1;
    // Rotation is a runtime config value, so the reflow has to branch at
    // runtime the way SideBar::buildCells() does. The ladder in the else-arm
    // is the old one, tuned for the 1024 px wide landscape canvas; on a 600 px
    // portrait canvas even 4 columns overflow. 3 columns give 178 px tiles
    // there, and for the 24-tile worst case 8 rows of 108 px - finger-sized
    // and still scroll-free. 2 columns would need 12 rows and collapse the
    // tiles to 68 px, so the portrait ladder falls back to 3 above 6 tiles.
    const bool port = uiPortrait();
    const int COLS  = port ? ((total <= 6) ? 2 : 3)
                           : ((total <= 8) ? 4 : (total <= 15) ? 5 : 6);
    const int M = 20, G = 12;
    // Both spans come from the LOGICAL screen; in landscape they are the
    // physical 1024x600 and every number below is unchanged.
    const int tw   = (uiScreenW() - 2 * M - (COLS - 1) * G) / COLS;
    const int rows = (total + COLS - 1) / COLS;
    int TOP = 56;                 // clears the logo/title header row
    int th = (uiScreenH() - TOP - 16 - (rows - 1) * G) / rows;
    if (th > 150) th = 150;       // keeps a handful of tiles from ballooning
    if (port) {
        // The cap bites far harder in portrait - four tiles in two columns
        // would compute 470 and clamp to 150, leaving ~650 px of empty
        // background under the grid. Push half the slack above it so the
        // block sits centred instead of hanging off the header. Landscape
        // never enters here, so its TOP stays 56 exactly.
        const int used  = rows * (th + G) - G;
        const int slack = uiScreenH() - TOP - 16 - used;
        if (slack > 0) TOP += slack / 2;
    }

    for (int slot = 0; slot < total; slot++) {
        const bool isCfg = (slot == total - 1);
        const int  id    = isCfg ? -1 : dispMgr.navScreenId(slot);
        const int  c     = slot % COLS, r = slot / COLS;

        // MFD-style module card: uniform light surface, big accent line icon
        // on top, dark name underneath.
        // Bare lv_obj instead of lv_btn, stripped of ALL theme styles: with
        // up to 24 tiles the theme's per-button style/transition baggage was
        // the difference between the render fitting the LVGL pool or the
        // device rebooting (lv_mem_buf_get OOM - the menu-crash hunt).
        lv_obj_t *tile = lv_obj_create(_root);
        lv_obj_remove_style_all(tile);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_size(tile, tw, th);
        lv_obj_set_pos(tile, M + c * (tw + G), TOP + r * (th + G));
        lv_obj_set_style_bg_color(tile, CLR_SURFACE, 0);
        lv_obj_set_style_bg_color(tile, CLR_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(tile, CLR_BORDER, 0);
        // Measured 2026-08-22: squaring these off (radius 0, no border) made no
        // perceptible difference to the rebuild, and neither did dropping the
        // 24 icon glyphs below. Together those are ~143,000 of the ~612,000
        // pixels in a full-screen refresh - nearly a quarter of the image - so
        // the ~400-500 ms a rebuild costs is NOT the tile decoration and not
        // the glyphs. It is a fixed cost every full-screen refresh pays, which
        // is also why screens with no tiles at all (Depth, Rudder, Autopilot)
        // measure the same. Do not spend effort here again without new
        // evidence; the open lead is the per-refresh cost itself.
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_radius(tile, 14, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        // The icons were removed once as an experiment - 24 glyphs at FONT_HUGE
        // or FONT_XXL, roughly 110,000 alpha-blended pixels - on the theory
        // that they dominated the rebuild. They do not: with them gone the
        // rebuild was not perceptibly faster. Kept, and worth knowing before
        // anyone proposes pre-rendering them into images: it would buy nothing.
        lv_obj_t *ico = lv_label_create(tile);
        lv_label_set_text(ico, isCfg ? LV_SYMBOL_SETTINGS : screenSymbol(id));
        lv_obj_set_style_text_font(ico, (th >= 130) ? FONT_HUGE : FONT_XXL, 0);
        lv_obj_set_style_text_color(ico, CLR_ACCENT, 0);
        lv_obj_align(ico, LV_ALIGN_TOP_MID, 0, th / 6);

        const char *caption = isCfg ? T(STR_CFG_TITLE) : dispMgr.screenName(id);
        lv_obj_t *l = lv_label_create(tile);
        lv_label_set_text(l, caption);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, tw - 16);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, tileLabelFont(caption, tw - 16), 0);
        lv_obj_set_style_text_color(l, CLR_TEXT, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);

        lv_obj_add_event_cb(tile, isCfg ? cbConfigTile : cbTile,
                            LV_EVENT_CLICKED, (void *)(intptr_t)id);
    }

    uiDisableLabelScroll(_root);   // see the note in Theme.h
}

void HomeOverlay::close() {
    if (!_open) return;
    _open = false;
    if (_root) lv_obj_del_async(_root);
    _root = nullptr;
}

#endif  // BOARD_PANEL_1024X600
