#include "LicenseOverlay.h"
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

    // Full-screen on the topmost layer -> sits above screens and nav arrows.
    _root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_root, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, CLR_BG, 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
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
    const int listY = 64, btnH = 46, listH = SCREEN_H - listY - btnH - 22;
    lv_obj_t *box = lv_obj_create(_root);
    lv_obj_set_size(box, SCREEN_W - 20, listH);
    lv_obj_set_pos(box, 10, listY);
    lv_obj_set_style_bg_color(box, CLR_SURFACE, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, CLR_BORDER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_ON);

    lv_obj_t *txt = lv_label_create(box);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(txt, SCREEN_W - 20 - 26);
    lv_label_set_text_static(txt, licenseText());  // static: no RAM duplicate
    lv_obj_set_style_text_font(txt, FONT_SMALL, 0);
    lv_obj_set_style_text_color(txt, CLR_TEXT, 0);
    lv_obj_set_pos(txt, 0, 0);

    // ── Button ───────────────────────────────────────────────────────────────
    lv_obj_t *btn = lv_btn_create(_root);
    lv_obj_set_size(btn, SCREEN_W - 20, btnH);
    lv_obj_set_pos(btn, 10, SCREEN_H - btnH - 10);
    lv_obj_set_style_bg_color(btn, CLR_ACCENT, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_event_cb(btn, firstRun ? cbAccept : cbClose, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, T(firstRun ? STR_LIC_ACCEPT : STR_LIC_CLOSE));
    lv_obj_set_style_text_font(bl, FONT_MED, 0);
    lv_obj_set_style_text_color(bl, CLR_ON_ACCENT, 0);
    lv_obj_center(bl);
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
