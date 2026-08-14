#pragma once
#include "../BaseScreen.h"
#include "../../nmea/DataModel.h"
#include "../../i18n/I18n.h"

// ============================================================
// TankScreen – "Tanks" (fluid levels from PGN 127505).
//
// One horizontal bar gauge per tank present on the bus (diesel, fresh water,
// black water, grey water, …). Pure LVGL widgets (lv_bar) — no canvas, so it costs
// nothing in the PSRAM arena. The row layout adapts to the number of tanks seen.
// ============================================================
class TankScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_TANK); }
    void create(lv_obj_t *parent) override;
    void update() override;
    void resetForRebuild() override;

private:
    struct Row {
        lv_obj_t *box  = nullptr;
        lv_obj_t *name = nullptr;
        lv_obj_t *bar  = nullptr;
        lv_obj_t *val  = nullptr;
    };
    Row       _rows[DataModel::MAX_TANKS];
    lv_obj_t *_empty   = nullptr;   // STR_TANK_NO_DATA placeholder
    int       _shownCount = -1;     // current row count laid out

    void layout(int n);
};
