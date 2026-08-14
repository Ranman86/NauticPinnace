#include "WeatherScreen.h"
#include "../Theme.h"
#include "../../i18n/I18n.h"
#include <math.h>
#include <stdio.h>

void WeatherScreen::makeTile(lv_obj_t *parent, int x, const char *label, lv_obj_t *&valOut) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 148, 158);
    lv_obj_set_pos(c, x, 304);
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
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 14);

    valOut = lv_label_create(c);
    lv_label_set_text(valOut, "--");
    lv_obj_set_style_text_font(valOut, FONT_LARGE, 0);
    lv_obj_set_style_text_color(valOut, CLR_TEXT, 0);
    lv_obj_align(valOut, LV_ALIGN_CENTER, 0, 10);
}

void WeatherScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *capt = lv_label_create(container);
    lv_label_set_text(capt, T(STR_WEA_PRESSURE));
    lv_obj_set_style_text_font(capt, FONT_SMALL, 0);
    lv_obj_set_style_text_color(capt, CLR_TEXT_DIM, 0);
    lv_obj_set_pos(capt, 16, 10);

    _pressVal = lv_label_create(container);
    lv_label_set_text(_pressVal, "-- hPa");
    lv_obj_set_style_text_font(_pressVal, FONT_LARGE, 0);
    lv_obj_set_style_text_color(_pressVal, CLR_TEXT, 0);
    lv_obj_set_pos(_pressVal, 14, 28);

    _trend = lv_label_create(container);
    lv_label_set_text(_trend, "");
    lv_obj_set_style_text_font(_trend, FONT_MED, 0);
    lv_obj_align(_trend, LV_ALIGN_TOP_RIGHT, -16, 16);

    _trendSub = lv_label_create(container);
    lv_label_set_text(_trendSub, "");
    lv_obj_set_style_text_font(_trendSub, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_trendSub, CLR_TEXT_DIM, 0);
    lv_obj_align(_trendSub, LV_ALIGN_TOP_RIGHT, -16, 50);

    _chart = lv_chart_create(container);
    lv_obj_set_size(_chart, 458, 196);
    lv_obj_set_pos(_chart, 11, 92);
    lv_obj_set_style_bg_color(_chart, CLR_SURFACE, 0);
    lv_obj_set_style_border_color(_chart, CLR_BORDER, 0);
    lv_obj_set_style_border_width(_chart, 1, 0);
    lv_obj_set_style_radius(_chart, 8, 0);
    lv_chart_set_type(_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(_chart, 4, 0);
    lv_chart_set_point_count(_chart, DataModel::PRESS_HIST);
    lv_obj_set_style_size(_chart, 0, LV_PART_INDICATOR);   // hide point markers
    lv_obj_set_style_line_width(_chart, 3, LV_PART_ITEMS);
    _series = lv_chart_add_series(_chart, CLR_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

    makeTile(container, 11,  T(STR_WEA_AIR),      _airVal);
    makeTile(container, 167, T(STR_WEA_WATER),    _waterVal);
    makeTile(container, 323, T(STR_WEA_HUMIDITY), _humVal);
}

void WeatherScreen::update() {
    float air, water, hum, press;
    float ring[DataModel::PRESS_HIST];
    int   n;
    {
        auto lk = data.lock();
        air = data.airTemp; water = data.waterTemp; hum = data.humidity; press = data.pressure;
        bool full = data.pressHistFull;
        int  idx  = data.pressHistIdx;
        n = full ? DataModel::PRESS_HIST : idx;
        int start = full ? idx : 0;
        for (int k = 0; k < n; k++) ring[k] = data.pressHistVal[(start + k) % DataModel::PRESS_HIST];
    }

    char b[24];
    if (isnan(press)) snprintf(b, sizeof(b), "-- hPa");
    else              snprintf(b, sizeof(b), "%d hPa", (int)(press + 0.5f));
    lv_label_set_text(_pressVal, b);

    if (isnan(air))   snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%.1f", air);
    lv_label_set_text(_airVal, b);
    if (isnan(water)) snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%.1f", water);
    lv_label_set_text(_waterVal, b);
    if (isnan(hum))   snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%d", (int)(hum + 0.5f));
    lv_label_set_text(_humVal, b);

    if (n >= 2) {
        float delta = ring[n - 1] - ring[0];
        const char *word; lv_color_t col;
        if      (delta >  1.0f) { word = T(STR_WEA_TREND_RISING);  col = CLR_GREEN; }
        else if (delta < -1.0f) { word = T(STR_WEA_TREND_FALLING); col = CLR_RED; }
        else                    { word = T(STR_WEA_TREND_STEADY);  col = CLR_TEXT_DIM; }
        lv_label_set_text(_trend, word);
        lv_obj_set_style_text_color(_trend, col, 0);
        snprintf(b, sizeof(b), "%+.1f hPa", delta);
        lv_label_set_text(_trendSub, b);
    } else {
        lv_label_set_text(_trend, "");
        lv_label_set_text(_trendSub, "");
    }

    float mn = 1e9f, mx = -1e9f;
    for (int k = 0; k < n; k++) { if (ring[k] < mn) mn = ring[k]; if (ring[k] > mx) mx = ring[k]; }
    if (n < 2) { float p = isnan(press) ? 1013.f : press; mn = p - 5.f; mx = p + 5.f; }
    int ymin = (int)floorf(mn) - 1, ymax = (int)ceilf(mx) + 1;
    if (ymax <= ymin) ymax = ymin + 2;
    lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);

    const int N = DataModel::PRESS_HIST;
    for (int idx = 0; idx < N; idx++) {
        int k = idx - (N - n);                          // right-align newest samples
        if (k < 0) lv_chart_set_value_by_id(_chart, _series, idx, LV_CHART_POINT_NONE);
        else       lv_chart_set_value_by_id(_chart, _series, idx, (lv_coord_t)(ring[k] + 0.5f));
    }
    lv_chart_refresh(_chart);
}

void WeatherScreen::resetForRebuild() {
    _pressVal = _trend = _trendSub = _chart = nullptr;
    _series = nullptr;
    _airVal = _waterVal = _humVal = nullptr;
}
