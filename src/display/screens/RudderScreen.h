#pragma once
#include "../BaseScreen.h"
#include "../../i18n/I18n.h"

class RudderScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_RUDDER); }
    void create(lv_obj_t *parent) override;
    void update() override;

public:
    static constexpr int CS = 480;
private:
    lv_obj_t  *_canvas   = nullptr;
    lv_color_t *_cbuf    = nullptr;
    lv_obj_t  *_lblAngle = nullptr;
    lv_obj_t  *_lblDir   = nullptr;
    void drawRudder(float angle);
};
