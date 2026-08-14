#include "EngineScreen.h"
#include "../../config/Config.h"
#include "../../i18n/I18n.h"
#include "../UiConfig.h"
#include <string.h>
#include <math.h>

void EngineScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H - NAV_BAR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, OPA_FULL, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // RPM arc gauge – larger for better readability
    _arc = lv_arc_create(container);
    lv_obj_set_size(_arc, UI_ENGINE_ARC_SIZE, UI_ENGINE_ARC_SIZE);
    lv_obj_align(_arc, LV_ALIGN_TOP_MID, 0, UI_ENGINE_ARC_Y);
    lv_arc_set_bg_angles(_arc, UI_ENGINE_ARC_START, UI_ENGINE_ARC_END);   // 270° sweep
    lv_arc_set_range(_arc, 0, 100);
    lv_arc_set_value(_arc, 0);
    lv_obj_remove_style(_arc, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_width(_arc, UI_ENGINE_ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_width(_arc, UI_ENGINE_ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_arc, CLR_BORDER,  LV_PART_MAIN);
    lv_obj_set_style_arc_color(_arc, CLR_GREEN,   LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(_arc, 0, 0);

    // RPM number in centre
    _lblRpm = lv_label_create(container);
    lv_label_set_text(_lblRpm, "----");
    styleLabel(_lblRpm, FONT_HUGE, CLR_TEXT);
    lv_obj_align(_lblRpm, LV_ALIGN_TOP_MID, 0, UI_ENGINE_RPM_Y);

    lv_obj_t *rpmUnit = lv_label_create(container);
    lv_label_set_text(rpmUnit, "RPM");
    styleLabel(rpmUnit, FONT_SMALL, CLR_TEXT_DIM);
    lv_obj_align(rpmUnit, LV_ALIGN_TOP_MID, 0, UI_ENGINE_RPM_UNIT_Y);

    // Status (alarm etc)
    _status = lv_label_create(container);
    lv_label_set_text(_status, "");
    styleLabel(_status, FONT_LARGE, CLR_RED);
    lv_obj_align(_status, LV_ALIGN_TOP_MID, 0, UI_ENGINE_STATUS_Y);

    // Configurable bottom fields (count + data point from config), bottom-aligned.
    buildFields();
}

// Build the N configurable info cards along the BOTTOM edge. Card width adapts
// to the field count so 1..6 fields fit across the screen.
void EngineScreen::buildFields() {
    for (int i = 0; i < MAX_FIELDS; i++)
        if (_fields[i].card) { lv_obj_del(_fields[i].card); _fields[i] = {}; }

    int n = max(1, min(appConfig.cfg.engineFieldCount, MAX_FIELDS));
    _fieldCount = n;
    const int W = SCREEN_W, H = SCREEN_H - NAV_BAR_H, gap = UI_ENGINE_CARD_GAP;
    int cw = (W - (n + 1) * gap) / n;
    int ch = UI_ENGINE_CARD_H;
    int y  = H - ch - gap;   // flush to the bottom edge

    for (int i = 0; i < n; i++) {
        const GridCell &fc = appConfig.cfg.engineFields[i];
        lv_obj_t *c = lv_obj_create(container);
        lv_obj_set_size(c, cw, ch);
        lv_obj_set_pos(c, gap + i * (cw + gap), y);
        styleCard(c);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *l = lv_label_create(c);
        // The user's own label takes precedence and is NEVER translated.
        // Without a custom label, show the translated default name instead of
        // the bare key ("oil").
        lv_label_set_text(l, strlen(fc.label) > 0 ? fc.label : i18nFieldName(fc.pgn));
        styleLabel(l, FONT_SMALL, CLR_TEXT_DIM);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t *u = lv_label_create(c);
        lv_label_set_text(u, fc.unit);
        styleLabel(u, FONT_TINY, CLR_TEXT_DIM);
        lv_obj_align(u, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

        lv_obj_t *vobj = lv_label_create(c);
        lv_label_set_text(vobj, "--");
        const lv_font_t *vf = (cw > 100) ? FONT_LARGE : (cw > 76) ? FONT_MED : FONT_SMALL;
        styleLabel(vobj, vf, CLR_TEXT);
        lv_obj_align(vobj, LV_ALIGN_CENTER, 0, 6);

        _fields[i].card = c;
        _fields[i].val  = vobj;
    }
}

void EngineScreen::onShow() {
    buildFields();   // pick up any config change (count / data points)
}

void EngineScreen::update() {
    float rpm;
    { auto lk = data.lock(); rpm = data.rpm; }
    EngineConfig ec = appConfig.cfg.engine;

    // Arc colour zones based on RPM bands
    lv_color_t arcCol = CLR_GREEN;
    String status = "";
    if (!isnan(rpm)) {
        if (rpm > ec.rpmMaxCont) { arcCol = CLR_RED;    status = String(LV_SYMBOL_WARNING " ") + T(STR_ENG_OVER_REV); }
        else if (rpm > ec.rpmCruise)  { arcCol = CLR_YELLOW; }
        else if (rpm < ec.rpmIdle + 100) { arcCol = CLR_TEXT_DIM; }
        else { arcCol = CLR_GREEN; }
    }
    lv_obj_set_style_arc_color(_arc, arcCol, LV_PART_INDICATOR);

    // Map RPM to 0-100 for arc
    if (!isnan(rpm)) {
        int pct = (int)(rpm / ec.rpmMax * 100);
        pct = max(0, min(100, pct));
        lv_arc_set_value(_arc, pct);

        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)rpm);
        lv_label_set_text(_lblRpm, buf);
    } else {
        lv_arc_set_value(_arc, 0);
        lv_label_set_text(_lblRpm, "----");
    }

    lv_label_set_text(_status, status.c_str());

    // Configurable bottom fields
    char buf[20];
    for (int i = 0; i < _fieldCount; i++) {
        if (!_fields[i].val) continue;
        const GridCell &fc = appConfig.cfg.engineFields[i];
        float val = dmFieldByKey(fc.pgn);
        fmtVal(buf, sizeof(buf), val, fc.decimals);
        lv_label_set_text(_fields[i].val, buf);

        // Per-data-point alarm / stale colouring
        lv_color_t col = CLR_TEXT;
        if      (!strcmp(fc.pgn, "coolant") && !isnan(val) && val > UI_ENGINE_ALARM_COOL) col = CLR_RED;
        else if (!strcmp(fc.pgn, "oil")     && !isnan(val) && val < UI_ENGINE_ALARM_OIL)  col = CLR_RED;
        else if (isnan(val)) col = CLR_TEXT_DIM;
        lv_obj_set_style_text_color(_fields[i].val, col, 0);
    }
}
