#pragma once
#include "../BaseScreen.h"
#include "../../i18n/I18n.h"

// Modern professional marine autopilot display
// Layout (480×430 canvas):
//   Top ~140px : curved compass arc showing ~90° window with degree scale
//   Middle      : HUGE set-heading number (target) centred
//   Left badge  : mode/status (NO DRIFT / HEADING / STANDBY)
//   Bottom row  : current hdg | deviation | rudder cards

class AutopilotScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_AUTOPILOT); }
    void create(lv_obj_t *parent) override;
    void update() override;

private:
    static constexpr int CW = 480, CH = 430;

    lv_obj_t  *_canvas      = nullptr;
    lv_color_t *_cbuf       = nullptr;

    // LVGL widgets (updated every frame, no canvas redraws needed for text)
    lv_obj_t  *_lblTarget   = nullptr;  // HUGE set heading  e.g. "106°"
    lv_obj_t  *_lblTargetLbl= nullptr;  // "Set Heading" caption
    lv_obj_t  *_lblMode     = nullptr;  // mode badge text
    lv_obj_t  *_modeBox     = nullptr;  // coloured mode box
    lv_obj_t  *_lblHdgCard  = nullptr;  // current hdg card value
    lv_obj_t  *_lblDevCard  = nullptr;  // deviation card value
    lv_obj_t  *_lblRudCard  = nullptr;  // rudder card value

    void drawCompassArc(float heading, float target);
};
