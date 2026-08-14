#pragma once
#include "../BaseScreen.h"
#include "../../i18n/I18n.h"

// Wind rose: historical TWD/TWS distribution as polar histogram
class WindPlotScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_WINDPLOT); }
    void create(lv_obj_t *parent) override;
    void update() override;

private:
    lv_obj_t  *_canvas   = nullptr;
    lv_color_t *_cbuf    = nullptr;
    lv_obj_t  *_statsLbl = nullptr;
    static constexpr int CS = 400;

    void drawWindRose();
};
