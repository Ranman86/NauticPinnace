#pragma once
#include "../BaseScreen.h"
#include "../../i18n/I18n.h"

// Modern professional marine autopilot display
// Layout (480 design grid, UI_S-scaled on the 7B):
//   Top ~140px : curved compass arc showing ~90° window with degree scale
//   Middle      : HUGE setpoint centred - the pilot's held heading, or its
//                 held wind angle with the side next to it
//   Left badge  : the pilot computer's mode (STANDBY / AUTO / WIND / NAV /
//                 NO DRIFT / ACTIVE = engaged, mode not known), "--" while
//                 nobody has reported one (see ApDecode.h)
//   Bottom row  : current hdg | deviation | rudder cards

class AutopilotScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_AUTOPILOT); }
    void create(lv_obj_t *parent) override;
    void update() override;

private:
    static constexpr int CW = SCREEN_W;   // compass canvas width (full screen)

    lv_obj_t  *_canvas      = nullptr;
    lv_color_t *_cbuf       = nullptr;

    // LVGL widgets (updated every frame, no canvas redraws needed for text)
    lv_obj_t  *_lblTarget   = nullptr;  // HUGE setpoint  e.g. "106°"
    lv_obj_t  *_lblSide     = nullptr;  // side of a held wind angle ("Stb")
    lv_obj_t  *_lblTargetLbl= nullptr;  // "Set Heading" / "Set Wind Angle"
    lv_obj_t  *_lblMode     = nullptr;  // mode badge text
    lv_obj_t  *_modeBox     = nullptr;  // coloured mode box
    lv_obj_t  *_lblHdgCard  = nullptr;  // current hdg card value
    lv_obj_t  *_lblDevCard  = nullptr;  // deviation card value
    lv_obj_t  *_lblRudCard  = nullptr;  // rudder card value

    // What is on screen now, so text, font and position are only touched
    // when they change. Cleared in create() (a theme reload rebuilds).
    char _shownMode[48]    = {};
    char _shownTarget[16]  = {};
    char _shownSide[16]    = {};
    int  _shownBox         = -1;     // 0 standby, 1 active, 2 manual
    bool _shownWindCaption = false;

    void setMode(const char *text, int box);
    void setTarget(const char *num, const char *side, lv_color_t sideCol, bool wind);
    void drawCompassArc(float heading, float target);
};
