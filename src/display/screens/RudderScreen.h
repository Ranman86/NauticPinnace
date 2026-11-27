#pragma once
#include "../BaseScreen.h"
#include "../../i18n/I18n.h"

class RudderScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_RUDDER); }
    void create(lv_obj_t *parent) override;
    void update() override;

public:
    static constexpr int CS = SCREEN_W;   // canvas spans the full screen width
private:
    lv_obj_t  *_canvas   = nullptr;
    lv_color_t *_cbuf    = nullptr;   // CS x CS, allocated once, reused by every rebuild
    int        _ch       = CS;        // canvas rows in use (canvasRows(), set in create())
    lv_obj_t  *_lblAngle = nullptr;
    lv_obj_t  *_lblDir   = nullptr;
    // Skip-unchanged state (see update()); create() resets it.
    bool       _painted    = false;   // the canvas holds the frame for _paintedKey
    uint32_t   _paintedKey = 0;       // frameKey() of the angle painted last
    int8_t     _lblTone    = -1;      // label colour set written last, -1 = none yet
    void drawRudder(float angle);
};
