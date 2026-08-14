#include "TankScreen.h"
#include "../Theme.h"
#include "../../config/Config.h"
#include "../../i18n/I18n.h"
#include <math.h>
#include <stdio.h>

static const TankCfg *findTankCfg(uint8_t inst, uint8_t ft) {
    for (int i = 0; i < 6; i++) {
        const TankCfg &c = appConfig.cfg.tankCfg[i];
        if (c.used && c.instance == inst && c.fluidType == ft) return &c;
    }
    return nullptr;
}

// fluid-type → default label (tN2kFluidType values). Only used when the tank has
// no user-given name in config.json; never written back to the config.
static const char *tankName(uint8_t ft) {
    switch (ft) {
        case 0: return T(STR_TANK_FT_DIESEL);
        case 1: return T(STR_TANK_FT_FRESH);
        case 2: return T(STR_TANK_FT_GREY);
        case 3: return T(STR_TANK_FT_LIVEWELL);
        case 4: return T(STR_TANK_FT_OIL);
        case 5: return T(STR_TANK_FT_BLACK);
        case 6: return T(STR_TANK_FT_PETROL);
        default: return T(STR_TANK_FT_OTHER);
    }
}

// Colour by remaining "good" headroom: supply tanks (fuel/water/…) are good when
// full; waste tanks (gray/black) are good when empty.
static lv_color_t tankColor(uint8_t ft, float lvl) {
    bool  waste = (ft == 2 || ft == 5);
    float good  = waste ? (100.f - lvl) : lvl;
    if (good < 20.f) return CLR_RED;
    if (good < 40.f) return CLR_ORANGE;
    return CLR_GREEN;
}

void TankScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < DataModel::MAX_TANKS; i++) {
        Row &r = _rows[i];
        r.box = lv_obj_create(container);
        lv_obj_set_style_radius(r.box, 8, 0);
        lv_obj_set_style_border_width(r.box, 1, 0);
        lv_obj_set_style_border_color(r.box, CLR_BORDER, 0);
        lv_obj_set_style_bg_color(r.box, CLR_SURFACE, 0);
        lv_obj_set_style_bg_opa(r.box, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(r.box, 0, 0);
        lv_obj_clear_flag(r.box, LV_OBJ_FLAG_SCROLLABLE);

        r.name = lv_label_create(r.box);
        lv_obj_set_style_text_font(r.name, FONT_MED, 0);
        lv_obj_set_style_text_color(r.name, CLR_TEXT, 0);

        r.val = lv_label_create(r.box);
        lv_obj_set_style_text_font(r.val, FONT_MED, 0);
        lv_obj_set_style_text_color(r.val, CLR_TEXT_DIM, 0);

        r.bar = lv_bar_create(r.box);
        lv_bar_set_range(r.bar, 0, 100);
        lv_obj_set_style_bg_color(r.bar, CLR_BG, 0);              // track
        lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r.bar, 4, 0);
        lv_obj_set_style_radius(r.bar, 4, LV_PART_INDICATOR);

        lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
    }

    _empty = lv_label_create(container);
    lv_label_set_text(_empty, T(STR_TANK_NO_DATA));
    lv_obj_set_style_text_font(_empty, FONT_MED, 0);
    lv_obj_set_style_text_color(_empty, CLR_TEXT_DIM, 0);
    lv_obj_center(_empty);
    lv_obj_add_flag(_empty, LV_OBJ_FLAG_HIDDEN);
}

void TankScreen::layout(int n) {
    const int top = 10, bottom = 470, gap = 8;
    int bh = (n > 0) ? (bottom - top - (n - 1) * gap) / n : 0;
    for (int i = 0; i < DataModel::MAX_TANKS; i++) {
        Row &r = _rows[i];
        if (i < n) {
            int y = top + i * (bh + gap);
            lv_obj_set_size(r.box, SCREEN_W - 20, bh);
            lv_obj_set_pos(r.box, 10, y);
            lv_obj_clear_flag(r.box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(r.name, 14, 8);
            lv_obj_align(r.val, LV_ALIGN_TOP_RIGHT, -14, 8);
            int barH = (bh > 92) ? 26 : 18;
            int barY = bh - barH - 12;
            if (barY < 40) barY = 40;
            lv_obj_set_size(r.bar, (SCREEN_W - 20) - 28, barH);
            lv_obj_set_pos(r.bar, 14, barY);
        } else {
            lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void TankScreen::update() {
    // Snapshot present tanks under the lock.
    struct T { uint8_t ft, inst; float lvl, cap; };
    T list[DataModel::MAX_TANKS];
    int n = 0;
    {
        auto lk = data.lock();
        for (int i = 0; i < data.tankCount && n < DataModel::MAX_TANKS; i++) {
            if (isnan(data.tanks[i].level)) continue;
            list[n].ft   = data.tanks[i].fluidType;
            list[n].inst = data.tanks[i].instance;
            list[n].lvl  = data.tanks[i].level;
            list[n].cap  = data.tanks[i].capacity;
            n++;
        }
    }

    if (n == 0) {
        if (_shownCount != 0) { layout(0); lv_obj_clear_flag(_empty, LV_OBJ_FLAG_HIDDEN); _shownCount = 0; }
        return;
    }
    lv_obj_add_flag(_empty, LV_OBJ_FLAG_HIDDEN);
    if (n != _shownCount) { layout(n); _shownCount = n; }

    for (int i = 0; i < n; i++) {
        Row &r = _rows[i];
        const TankCfg *tc = findTankCfg(list[i].inst, list[i].ft);

        char nm[24];
        if (tc && tc->name[0])     snprintf(nm, sizeof(nm), "%s", tc->name);
        else if (list[i].inst > 0) snprintf(nm, sizeof(nm), "%s %d", tankName(list[i].ft), list[i].inst + 1);
        else                       snprintf(nm, sizeof(nm), "%s", tankName(list[i].ft));
        lv_label_set_text(r.name, nm);

        int pct = (int)(list[i].lvl + 0.5f);                       // sender level
        lv_bar_set_value(r.bar, pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(r.bar, tankColor(list[i].ft, list[i].lvl), LV_PART_INDICATOR);

        // Litres via the calibration curve (or linear) + configured/bus capacity.
        float litres  = tc ? tc->liters(list[i].lvl, list[i].cap)
                           : (!isnan(list[i].cap) && list[i].cap > 0.f ? list[i].lvl / 100.f * list[i].cap : NAN);
        float capDisp = (tc && !isnan(tc->capacity)) ? tc->capacity : list[i].cap;
        char vb[32];
        if (!isnan(litres) && !isnan(capDisp) && capDisp > 0.f)
            snprintf(vb, sizeof(vb), "%d %%   %d/%d L", pct, (int)(litres + 0.5f), (int)(capDisp + 0.5f));
        else
            snprintf(vb, sizeof(vb), "%d %%", pct);
        lv_label_set_text(r.val, vb);
    }
}

void TankScreen::resetForRebuild() {
    for (int i = 0; i < DataModel::MAX_TANKS; i++) _rows[i] = Row{};
    _empty = nullptr;
    _shownCount = -1;
}
