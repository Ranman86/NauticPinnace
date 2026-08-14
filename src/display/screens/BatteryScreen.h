#pragma once
#include "../BaseScreen.h"
#include "../../nmea/DataModel.h"
#include "../../i18n/I18n.h"

// ============================================================
// BatteryScreen – "Batterie" (energy monitor).
//
// One card per DC bank seen on the bus (PGN 127508 status + 127506 DC detailed):
// state of charge (bar), voltage, charge/discharge current (green/red) and the
// estimated time to full/empty. Pure LVGL widgets — no canvas.
// ============================================================
class BatteryScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_BATTERY); }
    void create(lv_obj_t *parent) override;
    void update() override;
    void resetForRebuild() override;

private:
    struct Row {
        lv_obj_t *box  = nullptr;
        lv_obj_t *name = nullptr;
        lv_obj_t *soc  = nullptr;
        lv_obj_t *bar  = nullptr;
        lv_obj_t *info = nullptr;   // voltage + time-remaining
        lv_obj_t *cur  = nullptr;   // current (coloured by sign)
    };
    Row       _rows[DataModel::MAX_BATT];
    lv_obj_t *_empty   = nullptr;
    int       _shownCount = -1;

    void layout(int n);
};
