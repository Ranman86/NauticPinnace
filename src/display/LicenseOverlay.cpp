#include "LicenseOverlay.h"
#if defined(BOARD_PANEL_1024X600)
#include "../BoardConfig.h"   // board macros (the geometry comes from Theme.h)
#endif
#include "LicenseText.h"
#include "Theme.h"
#include "DisplayManager.h"   // dispMgr: rebuild screens after the language choice
#include "../i18n/I18n.h"
#include "../config/Config.h"
#include "../Entropy.h"       // touch-mixed hotspot password
#ifndef SIMULATOR
#include <WiFi.h>
#endif

LicenseOverlay licenseOverlay;

void LicenseOverlay::update() {
    if (_pendingFirstRun) { _pendingFirstRun = false; openFirstRun(); }
}

#if defined(PERF_LICENSE_SCROLL)
// ── Per-object draw stopwatch (measurement build only) ───────────────────────
// Three hypotheses about where a 1,000 ms scroll frame goes have now each been
// worth under ten per cent, so this stops guessing and asks LVGL directly.
//
// lv_obj_redraw() brackets each object's own painting with DRAW_MAIN_BEGIN /
// DRAW_MAIN_END and its chrome (borders, scrollbars) with DRAW_POST_BEGIN /
// DRAW_POST_END, and it draws the CHILDREN between the two pairs - so MAIN
// measures the object alone and POST measures its trim, neither containing a
// child's time. The events fire once per draw-buffer strip, which is exactly
// the granularity in question, so the numbers are accumulated and reported per
// second together with the call count.
//
// Slots: 0 = overlay root, 1 = the scrolling box, 2 = the text label.
static int64_t  s_probeT0[3]    = { 0, 0, 0 };
static uint32_t s_probeMainUs[3] = { 0, 0, 0 };
static uint32_t s_probePostUs[3] = { 0, 0, 0 };
static uint32_t s_probeCalls[3]  = { 0, 0, 0 };

static void perfProbeCb(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 2) return;
    switch (lv_event_get_code(e)) {
        case LV_EVENT_DRAW_MAIN_BEGIN:
        case LV_EVENT_DRAW_POST_BEGIN:
            s_probeT0[idx] = esp_timer_get_time();
            break;
        case LV_EVENT_DRAW_MAIN_END:
            s_probeMainUs[idx] += (uint32_t)(esp_timer_get_time() - s_probeT0[idx]);
            s_probeCalls[idx]++;
            break;
        case LV_EVENT_DRAW_POST_END:
            s_probePostUs[idx] += (uint32_t)(esp_timer_get_time() - s_probeT0[idx]);
            break;
        default: break;
    }
}

void LicenseOverlay::perfProbe(lv_obj_t *o, int slot) {
    if (!o) return;
    void *ud = (void *)(intptr_t)slot;
    lv_obj_add_event_cb(o, perfProbeCb, LV_EVENT_DRAW_MAIN_BEGIN, ud);
    lv_obj_add_event_cb(o, perfProbeCb, LV_EVENT_DRAW_MAIN_END,   ud);
    lv_obj_add_event_cb(o, perfProbeCb, LV_EVENT_DRAW_POST_BEGIN, ud);
    lv_obj_add_event_cb(o, perfProbeCb, LV_EVENT_DRAW_POST_END,   ud);
}

// See the declaration. Waits for the device to finish booting, opens itself,
// then walks the text box down and back up in 24 px steps - about the distance
// a finger drag covers between two frames at this frame rate. LV_ANIM_OFF so
// each call produces exactly one scroll, not an animation that would keep
// generating frames on its own and muddy the measurement.
void LicenseOverlay::perfScrollTick() {
    static uint32_t nextAt = 0;
    static bool     opened = false;
    static int      dir    = 1;

    const uint32_t now = millis();
    if (!opened) {
        if (now < 20000) return;    // let WiFi, the bus task and the first
        opened = true;              // screens settle before measuring
        open();
        Serial.println("[perf] licence overlay opened for scroll measurement");
        return;
    }
    if (!_open || !_box) return;
    if ((int32_t)(now - nextAt) < 0) return;
    nextAt = now + 150;

    const lv_coord_t below = lv_obj_get_scroll_bottom(_box);
    const lv_coord_t above = lv_obj_get_scroll_top(_box);
    if (dir > 0 && below <= 0) dir = -1;
    if (dir < 0 && above <= 0) dir =  1;
    lv_obj_scroll_by(_box, 0, -24 * dir, LV_ANIM_OFF);

    // Report the stopwatch every 5 s and start a fresh window, so the figures
    // line up with one heartbeat's worth of frames.
    static uint32_t reportAt = 0;
    if ((int32_t)(now - reportAt) >= 0) {
        reportAt = now + 5000;
        Serial.printf("[probe] root main=%u post=%u n=%u | box main=%u post=%u n=%u"
                      " | label main=%u post=%u n=%u  (us, 5 s)\n",
                      s_probeMainUs[0], s_probePostUs[0], s_probeCalls[0],
                      s_probeMainUs[1], s_probePostUs[1], s_probeCalls[1],
                      s_probeMainUs[2], s_probePostUs[2], s_probeCalls[2]);
        Serial.flush();
        for (int i = 0; i < 3; i++) {
            s_probeMainUs[i] = 0; s_probePostUs[i] = 0; s_probeCalls[i] = 0;
        }
    }
}
#endif

void LicenseOverlay::openFirstRun() { if (!_open) build(true); }
void LicenseOverlay::open()         { if (!_open) build(false); }

void LicenseOverlay::close() {
    if (!_open) return;
    _open = false;
    if (_root) lv_obj_del_async(_root);
    _root = nullptr;
}

void LicenseOverlay::build(bool firstRun) {
    _open = true;
    // (lv_mem_buf_free_all() removed - the boot primes the caches instead,
    //  see main.cpp.)

#if defined(BOARD_PANEL_1024X600)
    // Fill the LOGICAL screen, not the physical panel. LCD_WIDTH/LCD_HEIGHT
    // stay 1024x600 however the picture is turned, so with display.rotation
    // 90/270 a root built from them is 1024 px wide on a 600 px screen and
    // LVGL clips away everything past the edge - text, and the accept button
    // that the first run cannot get past. Everything else in build() is
    // derived from W/H and follows along. In landscape uiScreenW()/uiScreenH()
    // ARE LCD_WIDTH/LCD_HEIGHT, so nothing changes there.
    const int W = uiScreenW(), H = uiScreenH();
#else
    const int W = SCREEN_W, H = SCREEN_H;
#endif

    // Full-screen on the topmost layer -> sits above screens and nav arrows.
    _root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_root, W, H);
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, CLR_BG, 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    // Explicitly square, like the config and home overlays. The default theme
    // rounds lv_obj, and on a full-screen modal that has two consequences: the
    // four corners let the screen behind show through (which the manager now
    // hides while an overlay is open), and the largest object on the display
    // allocates and evaluates a radius mask once per draw-buffer strip.
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ───────────────────────────────────────────────────────────────
    lv_obj_t *title = lv_label_create(_root);
    lv_label_set_text(title, T(firstRun ? STR_LIC_TITLE : STR_CFG_LICENSES_BTN));
    lv_obj_set_style_text_font(title, FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, CLR_ACCENT, 0);
    lv_obj_set_pos(title, 14, 10);

    lv_obj_t *sub = lv_label_create(_root);
    lv_label_set_text(sub, firstRun
        ? T(STR_LIC_SUBTITLE)
        : T(STR_LIC_FULL_IN_NOTICES));
    lv_obj_set_style_text_font(sub, FONT_SMALL, 0);
    lv_obj_set_style_text_color(sub, CLR_TEXT_DIM, 0);
    lv_obj_set_pos(sub, 14, 40);

    // ── Scrollable text area ─────────────────────────────────────────────────
    const int listY = 64, btnH = 46, listH = H - listY - btnH - 22;
    lv_obj_t *box = lv_obj_create(_root);
    lv_obj_set_size(box, W - 20, listH);
    lv_obj_set_pos(box, 10, listY);
    lv_obj_set_style_bg_color(box, CLR_SURFACE, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, CLR_BORDER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_ON);
#if defined(PERF_LICENSE_SCROLL)
    _box = box;   // measurement build only, see perfScrollTick()
    perfProbe(_root, 0);
    perfProbe(box,   1);
#endif

    lv_obj_t *txt = lv_label_create(box);
    // NO explicit width, on purpose - this one line was the whole reason
    // scrolling here crawled.
    //
    // lv_draw_label() has no per-strip memory of where it was: for EVERY
    // draw-buffer strip it starts at byte 0 and walks the text line by line
    // until it reaches the strip (lv_draw_label.c:157, "Go the first visible
    // line"). LVGL's hint is meant to shortcut that, but it is anchored at
    // line 1 here because LV_LABEL_HINT_UPDATE_TH is 1024 px while this
    // label's entire scroll range is only ~960 px - so it saves one line out
    // of 67.
    //
    // What that walk costs depends entirely on LV_TEXT_FLAG_FIT. The label
    // sets that flag when, and only when, its width is LV_SIZE_CONTENT
    // (lv_label.c:801). With the flag, _lv_txt_get_next_line() is a plain
    // byte scan for the next '\n' (lv_txt.c:292-299) - no font access at all.
    // WITHOUT it, LVGL word-wraps: lv_font_get_glyph_width() per character,
    // and each of those walks the Montserrat -> latin-supplement fallback
    // chain. Setting an explicit width therefore bought word wrapping this
    // text never needs - it is hand-broken with '\n' at <= 49 characters,
    // about 490 px, against 558 px of content width even in portrait - and
    // paid for it with ~2,000 font lookups per strip, ~96,000 per frame.
    //
    // Measured on the 5B, one scroll frame: 1,050 ms before, see below for
    // after. LV_SIZE_CONTENT is the label class default (lv_label.c:59), so
    // deleting the line is all it takes.
    //
    // TRADE-OFF: with FIT there is no wrapping. A line longer than the box
    // gets clipped instead of wrapped, so keep LicenseText.h hand-broken.
    // THE one that mattered. A label is scrollable: lv_obj_constructor sets
    // LV_OBJ_FLAG_SCROLLABLE (lv_obj.c:441) and lv_label_constructor clears
    // only CLICKABLE, never this. So every strip runs the label's DRAW_POST ->
    // draw_scrollbar() -> lv_obj_get_scrollbar_area(), which does NOT take its
    // early return (lv_obj_scroll.c:105 needs the flag gone, or the scrollbar
    // mode to be OFF - the default is AUTO) and goes on to ask for scroll_top,
    // scroll_bottom, scroll_left and scroll_right. Each of those calls
    // lv_obj_get_self_height/width (lv_obj_scroll.c:155/197/234), which for a
    // label raises LV_EVENT_GET_SELF_SIZE, and lv_label.c:764 answers it by
    // measuring THE ENTIRE TEXT with lv_txt_get_size().
    //
    // That measurement is the expensive kind: lv_label.c:770 hard-codes
    // flag = LV_TEXT_FLAG_NONE, so it word-wraps character by character through
    // lv_font_get_glyph_width() no matter what the label's own width is - which
    // is why dropping the explicit width above helped the drawing but barely
    // touched this. Four full 2 KB text measurements per strip, 48 strips per
    // scroll frame.
    //
    // Measured with a per-object stopwatch on LV_EVENT_DRAW_MAIN/POST: the
    // label's POST pass was 5.0 seconds out of every 5 seconds of wall clock,
    // against 0.3 s for actually drawing the text. Nothing else on the overlay
    // came to 3 %.
    //
    // A label inside a scrolling box has no business scrolling itself, so the
    // flag is simply wrong here - clearing it also stops the label from
    // swallowing the drag gesture meant for the box.
    lv_obj_clear_flag(txt, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_label_set_text_static(txt, licenseText());  // static: no RAM duplicate
#if defined(PERF_LICENSE_SCROLL)
    perfProbe(txt, 2);
#endif
    lv_obj_set_style_text_font(txt, FONT_SMALL, 0);
    lv_obj_set_style_text_color(txt, CLR_TEXT, 0);
    lv_obj_set_pos(txt, 0, 0);

    // ── Button ───────────────────────────────────────────────────────────────
    lv_obj_t *btn = lv_btn_create(_root);
    lv_obj_set_size(btn, W - 20, btnH);
    lv_obj_set_pos(btn, 10, H - btnH - 10);
    lv_obj_set_style_bg_color(btn, CLR_ACCENT, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_event_cb(btn, firstRun ? cbAccept : cbClose, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, T(firstRun ? STR_LIC_ACCEPT : STR_LIC_CLOSE));
    lv_obj_set_style_text_font(bl, FONT_MED, 0);
    lv_obj_set_style_text_color(bl, CLR_ON_ACCENT, 0);
    lv_obj_center(bl);

    uiDisableLabelScroll(_root);   // see Theme.h - this is why scrolling here works
}

void LicenseOverlay::cbAccept(lv_event_t *e) {
    appConfig.cfg.licenseAccepted = true;
#ifndef SIMULATOR
    // Re-roll the hotspot password ONCE — by now the entropy pool is filled
    // with the touches of the initial setup (language choice, scrolling in the
    // licence text). Only while the radio is off: an already running hotspot
    // would otherwise have a password that was no longer shown to anyone.
    if (WiFi.getMode() == WIFI_MODE_NULL) {
        Entropy::generateApPassword(appConfig.cfg.apPass,
                                    sizeof(appConfig.cfg.apPass));
        Serial.println("[wifi] Hotspot-Passwort mit Touch-Entropie erneuert");
    }
#endif
    appConfig.save();                       // no longer appears afterwards
    licenseOverlay.close();
    // Final step of the initial setup: the screens were built at boot in the
    // startup language (English); their labels are created once during that.
    // Now - with the modal closed - rebuild in the chosen language. If the
    // choice stayed English, this only costs the rebuild.
    dispMgr.requestThemeReload();
}

void LicenseOverlay::cbClose(lv_event_t *e) {
    licenseOverlay.close();
}
