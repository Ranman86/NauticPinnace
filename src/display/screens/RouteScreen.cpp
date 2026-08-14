#include "RouteScreen.h"
#include "../Theme.h"
#include "../../i18n/I18n.h"
#include <math.h>
#include <stdio.h>

void RouteScreen::mkTile(lv_obj_t *parent, int x, int y, const char *label, lv_obj_t *&valOut) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 228, 104);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, CLR_BORDER, 0);
    lv_obj_set_style_bg_color(c, CLR_SURFACE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, CLR_TEXT_DIM, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 10);
    valOut = lv_label_create(c);
    lv_label_set_text(valOut, "--");
    lv_obj_set_style_text_font(valOut, FONT_LARGE, 0);
    lv_obj_set_style_text_color(valOut, CLR_TEXT, 0);
    lv_obj_align(valOut, LV_ALIGN_CENTER, 0, 10);
}

void RouteScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    _wp = lv_label_create(container);
    lv_label_set_text(_wp, T(STR_ROUTE_NO_WP));
    lv_obj_set_style_text_font(_wp, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_wp, CLR_ACCENT, 0);
    lv_obj_align(_wp, LV_ALIGN_TOP_MID, 0, 14);

    _dtw = lv_label_create(container);
    lv_label_set_text(_dtw, "--");
    lv_obj_set_style_text_font(_dtw, FONT_XL, 0);
    lv_obj_set_style_text_color(_dtw, CLR_TEXT, 0);
    lv_obj_align(_dtw, LV_ALIGN_TOP_MID, 0, 34);

    // CDI: cross-track error, filled from the centre toward the boat's side.
    _bar = lv_bar_create(container);
    lv_bar_set_mode(_bar, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_range(_bar, -100, 100);
    lv_obj_set_size(_bar, 400, 18);
    lv_obj_align(_bar, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_bg_color(_bar, CLR_SURFACE, 0);
    lv_obj_set_style_radius(_bar, 6, 0);
    lv_obj_set_style_radius(_bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_bar, CLR_ACCENT, LV_PART_INDICATOR);

    lv_obj_t *bbL = lv_label_create(container);
    lv_label_set_text(bbL, T(STR_ROUTE_PORT));
    lv_obj_set_style_text_font(bbL, FONT_SMALL, 0);
    lv_obj_set_style_text_color(bbL, CLR_TEXT_DIM, 0);
    lv_obj_set_pos(bbL, 40, 142);
    lv_obj_t *stbL = lv_label_create(container);
    lv_label_set_text(stbL, T(STR_ROUTE_STBD));
    lv_obj_set_style_text_font(stbL, FONT_SMALL, 0);
    lv_obj_set_style_text_color(stbL, CLR_TEXT_DIM, 0);
    lv_obj_align(stbL, LV_ALIGN_TOP_RIGHT, -40, 142);

    _xteL = lv_label_create(container);
    lv_label_set_text(_xteL, "");
    lv_obj_set_style_text_font(_xteL, FONT_MED, 0);
    lv_obj_align(_xteL, LV_ALIGN_TOP_MID, 0, 160);

    mkTile(container, 11,  210, T(STR_ROUTE_BEARING), _t[0]);
    mkTile(container, 241, 210, T(STR_ROUTE_TTG),     _t[1]);
    mkTile(container, 11,  322, "VMC",                _t[2]);   // abbreviation, both languages
    mkTile(container, 241, 322, T(STR_ROUTE_XTE),     _t[3]);
}

void RouteScreen::update() {
    bool active; float dtw, btw, xte, vmc; uint32_t wp;
    {
        auto lk = data.lock();
        active = data.navActive; dtw = data.navDtw; btw = data.navBtw;
        xte = data.navXte; vmc = data.navVmc; wp = data.navWpNum;
    }
    char b[28];

    if (!active) {
        lv_label_set_text(_wp, T(STR_ROUTE_NO_WP));
        lv_label_set_text(_dtw, "--");
        lv_label_set_text(_xteL, "");
        lv_bar_set_value(_bar, 0, LV_ANIM_OFF);
        for (int i = 0; i < 4; i++) lv_label_set_text(_t[i], "--");
        return;
    }

    snprintf(b, sizeof(b), "%s %u", T(STR_ROUTE_WP), (unsigned)wp);
    lv_label_set_text(_wp, b);

    // Distance to waypoint.
    if (isnan(dtw))      snprintf(b, sizeof(b), "--");
    else if (dtw < 1.0f) snprintf(b, sizeof(b), "%d m", (int)(dtw * 1852.f + 0.5f));
    else                 snprintf(b, sizeof(b), "%.2f %s", dtw, T(STR_UNIT_NM));
    lv_label_set_text(_dtw, b);

    // CDI: ±50 m full scale.
    if (isnan(xte)) { lv_bar_set_value(_bar, 0, LV_ANIM_OFF); lv_label_set_text(_xteL, ""); }
    else {
        int v = (int)(xte / 50.f * 100.f);
        if (v > 100) v = 100; if (v < -100) v = -100;
        lv_bar_set_value(_bar, v, LV_ANIM_OFF);
        lv_color_t c = (fabsf(xte) > 30.f) ? CLR_RED : (fabsf(xte) > 12.f ? CLR_ORANGE : CLR_GREEN);
        lv_obj_set_style_bg_color(_bar, c, LV_PART_INDICATOR);
        snprintf(b, sizeof(b), "%d m %s", (int)(fabsf(xte) + 0.5f),
                 xte >= 0 ? T(STR_ROUTE_STBD) : T(STR_ROUTE_PORT));
        lv_label_set_text(_xteL, b);
        lv_obj_set_style_text_color(_xteL, c, 0);
    }

    // Tiles.
    if (isnan(btw)) snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%03d\xc2\xb0", ((int)(btw + 0.5f)) % 360);
    lv_label_set_text(_t[0], b);

    if (!isnan(dtw) && !isnan(vmc) && vmc > 0.1f) {
        int mins = (int)(dtw / vmc * 60.f + 0.5f);
        snprintf(b, sizeof(b), "%d:%02d h", mins / 60, mins % 60);
    } else snprintf(b, sizeof(b), "--");
    lv_label_set_text(_t[1], b);

    if (isnan(vmc)) snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%.1f kn", vmc);
    lv_label_set_text(_t[2], b);

    if (isnan(xte)) snprintf(b, sizeof(b), "--");
    else            snprintf(b, sizeof(b), "%d m %s", (int)(fabsf(xte) + 0.5f),
                             xte >= 0 ? T(STR_ROUTE_STBD) : T(STR_ROUTE_PORT));
    lv_label_set_text(_t[3], b);
}

void RouteScreen::resetForRebuild() {
    _wp = _dtw = _bar = _xteL = nullptr;
    for (int i = 0; i < 4; i++) _t[i] = nullptr;
}
