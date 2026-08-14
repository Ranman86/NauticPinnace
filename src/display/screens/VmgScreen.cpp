#include "VmgScreen.h"
#include "../Theme.h"
#include "../../PolarTable.h"
#include "../../i18n/I18n.h"
#include <math.h>
#include <stdio.h>

static constexpr float D2R = 0.0174532925f;

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Target boat speed from the configured polar, with an analytic fallback when no
// polar file is loaded (mirrors WindScreen's windPolarSpeed).
static float targetSpeed(float absTwa, float tws) {
    if (isnan(absTwa) || isnan(tws)) return NAN;
    float p = gPolar().speedAt(absTwa, tws);
    if (!isnan(p)) return p;
    float a = absTwa, eff;
    if      (a <  28.f) eff = 0.05f;
    else if (a <  50.f) eff = 0.05f + (a - 28.f) / 22.f * 0.50f;
    else if (a < 100.f) eff = 0.55f + (a - 50.f) / 50.f * 0.20f;
    else if (a < 150.f) eff = 0.75f - (a - 100.f) / 50.f * 0.20f;
    else                eff = 0.55f - (a - 150.f) / 30.f * 0.25f;
    eff = clampf(eff, 0.04f, 0.80f);
    return eff * (2.43f * sqrtf(9.0f)) * (1.0f + tws / 100.0f);
}

void VmgScreen::mkTile(lv_obj_t *parent, int x, int y, const char *label, lv_obj_t *&valOut) {
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

void VmgScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    _vmgCap = lv_label_create(container);
    lv_label_set_text(_vmgCap, "VMG");
    lv_obj_set_style_text_font(_vmgCap, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_vmgCap, CLR_TEXT_DIM, 0);
    lv_obj_align(_vmgCap, LV_ALIGN_TOP_MID, 0, 14);

    _vmg = lv_label_create(container);
    lv_label_set_text(_vmg, "-- kn");
    lv_obj_set_style_text_font(_vmg, FONT_XL, 0);
    lv_obj_set_style_text_color(_vmg, CLR_TEXT, 0);
    lv_obj_align(_vmg, LV_ALIGN_TOP_MID, 0, 34);

    _bar = lv_bar_create(container);
    lv_bar_set_range(_bar, 0, 120);
    lv_obj_set_size(_bar, 360, 20);
    lv_obj_align(_bar, LV_ALIGN_TOP_MID, -30, 110);
    lv_obj_set_style_bg_color(_bar, CLR_SURFACE, 0);
    lv_obj_set_style_radius(_bar, 6, 0);
    lv_obj_set_style_radius(_bar, 6, LV_PART_INDICATOR);

    _perf = lv_label_create(container);
    lv_label_set_text(_perf, "--%");
    lv_obj_set_style_text_font(_perf, FONT_MED, 0);
    lv_obj_align(_perf, LV_ALIGN_TOP_RIGHT, -14, 108);

    _guide = lv_label_create(container);
    lv_label_set_text(_guide, "");
    lv_obj_set_style_text_font(_guide, FONT_MED, 0);
    lv_obj_align(_guide, LV_ALIGN_TOP_MID, 0, 150);

    mkTile(container, 11,  196, T(STR_VMG_T_TARGET),       _t[0]);
    mkTile(container, 241, 196, T(STR_VMG_T_TARGET_ANGLE), _t[1]);
    mkTile(container, 11,  308, T(STR_VMG_T_TWA_NOW),      _t[2]);
    mkTile(container, 241, 308, "BSP",                     _t[3]);   // acronym, not translated
}

void VmgScreen::update() {
    float twa, tws, stw;
    {
        auto lk = data.lock();
        twa = data.twa; tws = data.tws; stw = data.stw;
    }
    char b[24];

    bool ok = !isnan(twa) && !isnan(tws) && !isnan(stw) && tws > 1.0f;
    if (!ok) {
        lv_label_set_text(_vmg, "-- kn");
        lv_label_set_text(_vmgCap, "VMG");
        lv_label_set_text(_perf, "--%");
        lv_label_set_text(_guide, T(STR_VMG_LOW_WIND));
        lv_obj_set_style_text_color(_guide, CLR_TEXT_DIM, 0);
        lv_bar_set_value(_bar, 0, LV_ANIM_OFF);
        for (int i = 0; i < 4; i++) lv_label_set_text(_t[i], "--");
        return;
    }

    float absTwa = fabsf(twa);
    while (absTwa > 180.f) absTwa = 360.f - absTwa;
    bool upwind = absTwa < 90.f;

    float vmgMag = fabsf(stw * cosf(absTwa * D2R));
    snprintf(b, sizeof(b), "%.2f kn", vmgMag);
    lv_label_set_text(_vmg, b);
    lv_label_set_text(_vmgCap, upwind ? T(STR_VMG_UPWIND) : T(STR_VMG_DOWNWIND));

    // Scan the polar for the optimal-VMG angle at this wind speed.
    float optA = absTwa, optVmg = 0.f;
    int lo = upwind ? 28 : 95, hi = upwind ? 85 : 175;
    for (int a = lo; a <= hi; a++) {
        float v = targetSpeed((float)a, tws);
        if (isnan(v)) continue;
        float vmg = fabsf(v * cosf((float)a * D2R));
        if (vmg > optVmg) { optVmg = vmg; optA = (float)a; }
    }

    float bsp = targetSpeed(absTwa, tws);          // polar speed at the current angle

    // Tiles.
    snprintf(b, sizeof(b), "%.2f kn", optVmg);                 lv_label_set_text(_t[0], b);
    snprintf(b, sizeof(b), "%d\xc2\xb0", (int)(optA + 0.5f));  lv_label_set_text(_t[1], b);
    snprintf(b, sizeof(b), "%d\xc2\xb0 %s", (int)(absTwa + 0.5f),
             twa >= 0 ? T(STR_WIND_STB) : T(STR_WIND_PT));
    lv_label_set_text(_t[2], b);
    if (isnan(bsp)) snprintf(b, sizeof(b), "%.1f kn", stw);
    else            snprintf(b, sizeof(b), "%.1f / %.1f", stw, bsp);   // actual / target
    lv_label_set_text(_t[3], b);

    // Performance = current VMG / best VMG.
    float perf = (optVmg > 0.05f) ? (vmgMag / optVmg * 100.f) : NAN;
    if (isnan(perf)) { lv_label_set_text(_perf, "--%"); lv_bar_set_value(_bar, 0, LV_ANIM_OFF); }
    else {
        snprintf(b, sizeof(b), "%d%%", (int)(perf + 0.5f));
        lv_label_set_text(_perf, b);
        lv_bar_set_value(_bar, (int)clampf(perf, 0.f, 120.f), LV_ANIM_OFF);
        lv_color_t c = (perf >= 95.f) ? CLR_GREEN : (perf >= 80.f ? CLR_ORANGE : CLR_RED);
        lv_obj_set_style_bg_color(_bar, c, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(_perf, c, 0);
    }

    // Steering guidance: STR_VMG_HIGHER = head up (reduce TWA),
    // STR_VMG_LOWER = bear away.
    float delta = absTwa - optA;
    if (delta > 1.5f) {
        snprintf(b, sizeof(b), "%s %d\xc2\xb0", T(STR_VMG_HIGHER), (int)(delta + 0.5f));
        lv_label_set_text(_guide, b);  lv_obj_set_style_text_color(_guide, CLR_ACCENT, 0);
    } else if (delta < -1.5f) {
        snprintf(b, sizeof(b), "%s %d\xc2\xb0", T(STR_VMG_LOWER), (int)(-delta + 0.5f));
        lv_label_set_text(_guide, b);  lv_obj_set_style_text_color(_guide, CLR_ACCENT, 0);
    } else {
        lv_label_set_text(_guide, T(STR_VMG_OPTIMAL));
        lv_obj_set_style_text_color(_guide, CLR_GREEN, 0);
    }
}

void VmgScreen::resetForRebuild() {
    _vmg = _vmgCap = _bar = _perf = _guide = nullptr;
    for (int i = 0; i < 4; i++) _t[i] = nullptr;
}
