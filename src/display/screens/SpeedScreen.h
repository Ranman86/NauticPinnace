#pragma once
#include "../BaseScreen.h"
#include "../../i18n/I18n.h"

class SpeedScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_SPEED); }
    void create(lv_obj_t *parent) override;
    void update() override;

private:
    lv_obj_t *_lblSog     = nullptr;
    lv_obj_t *_lblStw     = nullptr;
    lv_obj_t *_lblPolar   = nullptr;
    lv_obj_t *_lblPerf    = nullptr;
    lv_obj_t *_lblVmg     = nullptr;
    lv_obj_t *_bar        = nullptr;   // performance % bar (tall, bottom half)

    float getPolarSpeed(float twa, float tws);
    float getVmg(float twa, float sow);
};
