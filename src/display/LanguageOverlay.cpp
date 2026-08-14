#include "LanguageOverlay.h"
#include "LicenseOverlay.h"
#include "Theme.h"
#include "../i18n/I18n.h"
#include "../config/Config.h"
#include <string.h>

LanguageOverlay languageOverlay;

void LanguageOverlay::update() {
    if (_pendingOpen) { _pendingOpen = false; open(); }
}

void LanguageOverlay::open() { if (!_open) build(); }

void LanguageOverlay::close() {
    if (!_open) return;
    _open = false;
    if (_root) lv_obj_del_async(_root);
    _root = nullptr;
}

void LanguageOverlay::build() {
    _open = true;

    _root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_root, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, CLR_BG, 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_root, LV_OBJ_FLAG_CLICKABLE);   // modal: absorb touches

    // ── Header ───────────────────────────────────────────────────────────────
    // Deliberately labelled BILINGUALLY instead of translated: at this point
    // the device does not yet know which language the user can read.
    lv_obj_t *title = lv_label_create(_root);
    lv_label_set_text(title, "Sprache / Language");
    lv_obj_set_style_text_font(title, FONT_XL, 0);
    lv_obj_set_style_text_color(title, CLR_ACCENT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t *sub = lv_label_create(_root);
    lv_label_set_text(sub, "Bitte Sprache wählen\nPlease choose your language");
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(sub, FONT_SMALL, 0);
    lv_obj_set_style_text_color(sub, CLR_TEXT_DIM, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 118);

    // ── Two large buttons ────────────────────────────────────────────────────
    struct Choice { const char *label; lv_event_cb_t cb; int y; };
    const Choice choices[] = {
        { "Deutsch", cbPickDe, 200 },
        { "English", cbPickEn, 274 },
    };
    for (const Choice &c : choices) {
        lv_obj_t *btn = lv_btn_create(_root);
        lv_obj_set_size(btn, SCREEN_W - 120, 62);
        lv_obj_set_pos(btn, 60, c.y);
        lv_obj_set_style_bg_color(btn, CLR_SURFACE, 0);
        lv_obj_set_style_border_color(btn, CLR_BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, c.cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, c.label);
        lv_obj_set_style_text_font(l, FONT_LARGE, 0);
        lv_obj_set_style_text_color(l, CLR_TEXT, 0);
        lv_obj_center(l);
    }

    lv_obj_t *hint = lv_label_create(_root);
    lv_label_set_text(hint, "Änderbar in den Einstellungen\nChangeable later in the settings");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, FONT_TINY, 0);
    lv_obj_set_style_text_color(hint, CLR_TEXT_DIM, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
}

void LanguageOverlay::pick(bool english) {
    const Lang l = english ? Lang::EN : Lang::DE;
    i18nSetLang(l);
    strlcpy(appConfig.cfg.lang, i18nLangCode(l), sizeof(appConfig.cfg.lang));
    appConfig.save();
    languageOverlay.close();
    // Only now the licences - that way they already appear in the chosen
    // language. Request deferred instead of opening directly: we are inside an
    // LVGL event here, and close() deletes _root asynchronously.
    licenseOverlay.requestOpenFirstRun();
    // The screens were built in the startup language; their labels are created
    // during the build. Do not rebuild here - that happens after the licences
    // have been accepted (LicenseOverlay::cbAccept), so the rebuild does not
    // collide with the open modal.
}

void LanguageOverlay::cbPickDe(lv_event_t *e) { pick(false); }
void LanguageOverlay::cbPickEn(lv_event_t *e) { pick(true); }
