#pragma once
#include "../BaseScreen.h"
#include "../../config/Config.h"
#include "../../i18n/I18n.h"

// Configurable grid of data fields (up to 3×3 = 9 cells)
class GridScreen : public BaseScreen {
public:
    // Each GridScreen instance renders one config slot (appConfig.cfg.grids[_slot]).
    void setSlot(int s) { _slot = s; }
    // Grid names are user data from config.json — never translated; only the
    // fallback for an unnamed grid is (mirrors DisplayManager::screenName()).
    const char *title() const override {
        const char *n = appConfig.cfg.grids[_slot].name;
        return (n && n[0]) ? n : T(STR_SCREEN_GRID_DEFAULT);
    }
    void create(lv_obj_t *parent) override;
    void onShow() override;  // rebuild cells when config changes
    void update() override;
    void resetForRebuild() override {   // child objects already freed; null ptrs
        for (int i = 0; i < MAX_CELLS; i++) _cells[i] = {};
        _count = 0;
    }

private:
    int _slot = 0;
    static constexpr int MAX_CELLS = 9;

    struct Cell {
        lv_obj_t *container = nullptr;
        lv_obj_t *lblLabel  = nullptr;
        lv_obj_t *lblValue  = nullptr;
        lv_obj_t *lblUnit   = nullptr;
    } _cells[MAX_CELLS];

    int _count = 0;   // number of cells actually built (depends on layout)

    void buildGrid();
    float getFieldValue(const char *key);
};
