#include "SpeedScreen.h"
#include "../../config/Config.h"
#include "../../PolarTable.h"
#include "../UiConfig.h"
#include <math.h>

// Target boat speed from the configured polar (loaded from /polar.json).
float SpeedScreen::getPolarSpeed(float twa, float tws) {
    if (isnan(twa) || isnan(tws)) return NAN;
    if (fabsf(twa) < 1) return 0;
    return gPolar().speedAt(twa, tws);
}

float SpeedScreen::getVmg(float twa, float sow) {
    if (isnan(twa) || isnan(sow)) return NAN;
    return sow * cosf(fabsf(twa) * DEG_TO_RAD);
}

void SpeedScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H - NAV_BAR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, OPA_FULL, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top half: 2×2 grid of key values ─────────────────────────────────────
    // SOG | STW
    // Polar | VMG
    int cw = (SCREEN_W - 12) / 2;   // ~234px
    int ch = UI_SPEED_CARD_H;
    int gap = UI_SPEED_CARD_GAP;

    auto addCard = [&](int col, int row, const char *lbl, lv_obj_t **vl, lv_color_t col_) {
        int x = gap + col * (cw + gap);
        int y = gap + row * (ch + gap);
        lv_obj_t *c = lv_obj_create(container);
        lv_obj_set_size(c, cw, ch); lv_obj_set_pos(c, x, y); styleCard(c);
        lv_obj_t *ll = lv_label_create(c); lv_label_set_text(ll, lbl);
        styleLabel(ll, FONT_SMALL, CLR_TEXT_DIM); lv_obj_align(ll, LV_ALIGN_TOP_MID, 0, 0);
        *vl = lv_label_create(c); lv_label_set_text(*vl, "--");
        styleLabel(*vl, FONT_HUGE, col_); lv_obj_align(*vl, LV_ALIGN_CENTER, 0, 4);
    };
    addCard(0, 0, "SOG kn",   &_lblSog,   CLR_ACCENT);
    addCard(1, 0, "STW kn",   &_lblStw,   CLR_TEXT);
    addCard(0, 1, "Polar kn", &_lblPolar, CLR_GREEN);
    addCard(1, 1, "VMG kn",   &_lblVmg,   CLR_WIND);

    // ── Bottom half: tall perf bar + large % value ────────────────────────────
    int topH = 2 * ch + 3 * gap;                    // ~212px used by cards
    int botY = topH + gap;
    int botH = (SCREEN_H - NAV_BAR_H) - botY - 8;  // remaining height

    // Perf % label – very large
    lv_obj_t *perfCard = lv_obj_create(container);
    lv_obj_set_size(perfCard, SCREEN_W - 2*gap, botH);
    lv_obj_set_pos(perfCard, gap, botY);
    styleCard(perfCard);

    lv_obj_t *perfTitle = lv_label_create(perfCard);
    lv_label_set_text(perfTitle, "Perf %");
    styleLabel(perfTitle, FONT_MED, CLR_TEXT_DIM);
    lv_obj_align(perfTitle, LV_ALIGN_TOP_MID, 0, 4);

    _lblPerf = lv_label_create(perfCard);
    lv_label_set_text(_lblPerf, "--");
    lv_obj_set_style_text_font(_lblPerf, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(_lblPerf, CLR_YELLOW, 0);
    lv_obj_align(_lblPerf, LV_ALIGN_CENTER, 0, -10);

    // Horizontal performance bar – wide and tall
    _bar = lv_bar_create(perfCard);
    lv_obj_set_size(_bar, lv_obj_get_width(perfCard) - 24, UI_SPEED_BAR_H);
    lv_obj_align(_bar, LV_ALIGN_BOTTOM_MID, 0, UI_SPEED_BAR_BOTTOM);
    lv_bar_set_range(_bar, 0, 120);
    lv_bar_set_value(_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_bar, CLR_SURFACE, 0);
    lv_obj_set_style_bg_color(_bar, CLR_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(_bar, UI_SPEED_BAR_RADIUS, 0);
    lv_obj_set_style_radius(_bar, UI_SPEED_BAR_RADIUS, LV_PART_INDICATOR);
}

void SpeedScreen::update() {
    float sog, stw, twa, tws;
    { auto lk=data.lock(); sog=data.sog; stw=data.stw; twa=data.twa; tws=data.tws; }

    char buf[16];
    fmtVal(buf, sizeof(buf), sog, 1); lv_label_set_text(_lblSog, buf);
    fmtVal(buf, sizeof(buf), stw, 1); lv_label_set_text(_lblStw, buf);

    float polar = getPolarSpeed(twa, tws);
    fmtVal(buf, sizeof(buf), polar, 1); lv_label_set_text(_lblPolar, buf);

    float vmg = getVmg(twa, stw);
    fmtVal(buf, sizeof(buf), vmg, 1); lv_label_set_text(_lblVmg, buf);

    if (!isnan(polar) && polar > 0 && !isnan(stw)) {
        int perf = (int)(stw / polar * 100.0f);
        snprintf(buf, sizeof(buf), "%d%%", perf);
        lv_label_set_text(_lblPerf, buf);
        lv_bar_set_value(_bar, perf, LV_ANIM_ON);
        lv_color_t barCol = (perf >= UI_SPEED_PERF_GREEN) ? CLR_GREEN : (perf >= UI_SPEED_PERF_YELLOW) ? CLR_YELLOW : CLR_RED;
        lv_obj_set_style_bg_color(_bar, barCol, LV_PART_INDICATOR);
        // Colour the percentage text
        lv_obj_set_style_text_color(_lblPerf,
            (perf >= UI_SPEED_PERF_GREEN) ? CLR_GREEN : (perf >= UI_SPEED_PERF_YELLOW) ? CLR_YELLOW : CLR_RED, 0);
    } else {
        lv_label_set_text(_lblPerf, "--");
        lv_bar_set_value(_bar, 0, LV_ANIM_OFF);
    }
}
