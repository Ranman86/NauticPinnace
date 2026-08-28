#include "LanguageOverlay.h"
#if defined(BOARD_PANEL_1024X600)
#include "../BoardConfig.h"   // board macros (the geometry comes from Theme.h)
#endif
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

#if defined(BOARD_PANEL_1024X600)
    // Fill the LOGICAL screen; the sparse layout just gets more air. Not
    // LCD_WIDTH/LCD_HEIGHT: those are the PHYSICAL panel and stay 1024x600
    // even with display.rotation 90/270, where LVGL lays out in 600x1024 and
    // clips the root to it - which would cut the centred buttons in half. In
    // landscape uiScreenW()/uiScreenH() are exactly LCD_WIDTH/LCD_HEIGHT.
    const int W = uiScreenW(), H = uiScreenH();
    const int BTN_W = 480, BTN_H = 70;   // 480 of 600 still fits in portrait
    // The Y positions are literals tuned to the 600 px tall landscape panel.
    // Rotated there are 1024 px of height, so derive them from H instead -
    // otherwise the whole picker huddles in the top 410 px with ~600 px of
    // empty background below it. Landscape keeps the literals verbatim.
    const bool port   = uiPortrait();
    const int TITLE_Y = port ? H / 6         : 100;
    const int SUB_Y   = port ? TITLE_Y + 50  : 150;
    const int BTN_Y1  = port ? H / 2 - 90    : 250;
    const int BTN_Y2  = port ? BTN_Y1 + 90   : 340;
#else
    const int W = SCREEN_W, H = SCREEN_H;
    const int TITLE_Y = 70, SUB_Y = 118, BTN_W = SCREEN_W - 120, BTN_Y1 = 200, BTN_Y2 = 274, BTN_H = 62;
#endif

    _root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_root, W, H);
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, CLR_BG, 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);   // see the note in LicenseOverlay::build()
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    lv_obj_t *sub = lv_label_create(_root);
    lv_label_set_text(sub, "Bitte Sprache wählen\nPlease choose your language");
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(sub, FONT_SMALL, 0);
    lv_obj_set_style_text_color(sub, CLR_TEXT_DIM, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, SUB_Y);

    // ── Two large buttons ────────────────────────────────────────────────────
    // Centered via align: on the 4" this equals the old x=60 fixed position.
    struct Choice { const char *label; lv_event_cb_t cb; int y; };
    const Choice choices[] = {
        { "Deutsch", cbPickDe, BTN_Y1 },
        { "English", cbPickEn, BTN_Y2 },
    };
    for (const Choice &c : choices) {
        lv_obj_t *btn = lv_btn_create(_root);
        lv_obj_set_size(btn, BTN_W, BTN_H);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, c.y);
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

    uiDisableLabelScroll(_root);   // see the note in Theme.h
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
