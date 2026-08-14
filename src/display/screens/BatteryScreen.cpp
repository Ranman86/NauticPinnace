#include "BatteryScreen.h"
#include "../Theme.h"
#include "../../config/Config.h"
#include <math.h>
#include <stdio.h>

static void bankName(char *buf, size_t n, uint8_t inst) {
    for (int i = 0; i < 4; i++) {
        const BatteryCfg &c = appConfig.cfg.battCfg[i];
        if (c.used && c.instance == inst && c.name[0]) { snprintf(buf, n, "%s", c.name); return; }
    }
    if      (inst == 0) snprintf(buf, n, "Service");
    else if (inst == 1) snprintf(buf, n, "Starter");
    else                snprintf(buf, n, "Bank %d", inst + 1);
}

static lv_color_t socColor(float soc) {
    if (isnan(soc))  return CLR_TEXT_DIM;
    if (soc < 30.f)  return CLR_RED;
    if (soc < 50.f)  return CLR_ORANGE;
    return CLR_GREEN;
}

void BatteryScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    const int W = SCREEN_W - 20;
    for (int i = 0; i < DataModel::MAX_BATT; i++) {
        Row &r = _rows[i];
        r.box = lv_obj_create(container);
        lv_obj_set_size(r.box, W, 100);
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
        lv_obj_set_pos(r.name, 16, 10);

        r.soc = lv_label_create(r.box);
        lv_obj_set_style_text_font(r.soc, FONT_LARGE, 0);
        lv_obj_align(r.soc, LV_ALIGN_TOP_RIGHT, -16, 4);

        r.bar = lv_bar_create(r.box);
        lv_bar_set_range(r.bar, 0, 100);
        lv_obj_set_style_bg_color(r.bar, CLR_BG, 0);
        lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r.bar, 4, 0);
        lv_obj_set_style_radius(r.bar, 4, LV_PART_INDICATOR);
        lv_obj_set_size(r.bar, W - 32, 22);
        lv_obj_set_pos(r.bar, 16, 50);

        r.info = lv_label_create(r.box);
        lv_obj_set_style_text_font(r.info, FONT_SMALL, 0);
        lv_obj_set_style_text_color(r.info, CLR_TEXT_DIM, 0);
        lv_obj_set_pos(r.info, 16, 80);

        r.cur = lv_label_create(r.box);
        lv_obj_set_style_text_font(r.cur, FONT_MED, 0);
        lv_obj_align(r.cur, LV_ALIGN_TOP_RIGHT, -16, 76);

        lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
    }

    _empty = lv_label_create(container);
    lv_label_set_text(_empty, T(STR_BATT_NO_DATA));
    lv_obj_set_style_text_font(_empty, FONT_MED, 0);
    lv_obj_set_style_text_color(_empty, CLR_TEXT_DIM, 0);
    lv_obj_center(_empty);
    lv_obj_add_flag(_empty, LV_OBJ_FLAG_HIDDEN);
}

void BatteryScreen::layout(int n) {
    const int top = 10, bottom = 470, gap = 10;
    int bh = (n > 0) ? (bottom - top - (n - 1) * gap) / n : 0;
    // Cap only so a SINGLE bank doesn't become one absurd full-height card. The
    // old cap of 150 also applied to two banks and left 160 px empty below them.
    if (bh > 230) bh = 230;
    for (int i = 0; i < DataModel::MAX_BATT; i++) {
        Row &r = _rows[i];
        if (i < n) {
            lv_obj_set_size(r.box, SCREEN_W - 20, bh);
            lv_obj_set_pos(r.box, 10, top + i * (bh + gap));
            // Re-centre the card's contents: they were laid out for a 100 px box
            // (name 10 / soc 4 / bar 50 / info 80 / cur 76), so shifting all five
            // by half the extra height keeps the original spacing intact instead
            // of letting a taller card grow empty space underneath.
            int d = (bh - 100) / 2;
            lv_obj_set_pos(r.name, 16, 10 + d);
            lv_obj_align(r.soc, LV_ALIGN_TOP_RIGHT, -16, 4 + d);
            lv_obj_set_pos(r.bar,  16, 50 + d);
            lv_obj_set_pos(r.info, 16, 80 + d);
            lv_obj_align(r.cur, LV_ALIGN_TOP_RIGHT, -16, 76 + d);
            lv_obj_clear_flag(r.box, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void BatteryScreen::update() {
    BatteryBank list[DataModel::MAX_BATT];
    int n = 0;
    {
        auto lk = data.lock();
        for (int i = 0; i < data.battCount && n < DataModel::MAX_BATT; i++) {
            if (isnan(data.batteries[i].voltage) && isnan(data.batteries[i].soc)) continue;
            list[n++] = data.batteries[i];
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
        const BatteryBank &b = list[i];
        char buf[40];

        bankName(buf, sizeof(buf), b.instance);
        lv_label_set_text(r.name, buf);

        // SOC
        if (isnan(b.soc)) snprintf(buf, sizeof(buf), "--");
        else              snprintf(buf, sizeof(buf), "%d %%", (int)(b.soc + 0.5f));
        lv_label_set_text(r.soc, buf);
        lv_obj_set_style_text_color(r.soc, socColor(b.soc), 0);
        lv_bar_set_value(r.bar, isnan(b.soc) ? 0 : (int)(b.soc + 0.5f), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(r.bar, socColor(b.soc), LV_PART_INDICATOR);

        // Voltage + time remaining
        char vbuf[16], tbuf[20];
        if (isnan(b.voltage)) snprintf(vbuf, sizeof(vbuf), "-- V");
        else                  snprintf(vbuf, sizeof(vbuf), "%.2f V", b.voltage);
        if (isnan(b.timeRemMin) || b.timeRemMin > 6000.f) tbuf[0] = '\0';
        else {
            int hh = (int)b.timeRemMin / 60, mm = (int)b.timeRemMin % 60;
            bool charging = !isnan(b.current) && b.current > 0.1f;
            snprintf(tbuf, sizeof(tbuf), "   %d:%02d h %s", hh, mm,
                     charging ? T(STR_BATT_TO_FULL) : T(STR_BATT_TO_EMPTY));
        }
        snprintf(buf, sizeof(buf), "%s%s", vbuf, tbuf);
        lv_label_set_text(r.info, buf);

        // Current (sign-coloured)
        if (isnan(b.current)) { snprintf(buf, sizeof(buf), "-- A"); lv_obj_set_style_text_color(r.cur, CLR_TEXT_DIM, 0); }
        else {
            snprintf(buf, sizeof(buf), "%+.1f A", b.current);
            lv_color_t c = (b.current > 0.1f) ? CLR_GREEN : (b.current < -0.1f ? CLR_RED : CLR_TEXT_DIM);
            lv_obj_set_style_text_color(r.cur, c, 0);
        }
        lv_label_set_text(r.cur, buf);
    }
}

void BatteryScreen::resetForRebuild() {
    for (int i = 0; i < DataModel::MAX_BATT; i++) _rows[i] = Row{};
    _empty = nullptr;
    _shownCount = -1;
}
