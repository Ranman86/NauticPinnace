#pragma once
#include "../BaseScreen.h"
#include "../../PsramArena.h"
#include "../../i18n/I18n.h"

class DepthScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_DEPTH); }
    void create(lv_obj_t *parent) override;
    void update() override;

private:
    static constexpr int CW   = 480;   // main canvas width

    lv_obj_t   *_canvas  = nullptr;   // main 480×480 canvas
    lv_color_t *_cbuf    = nullptr;
    lv_obj_t   *_lblAlarm = nullptr;

    // Render depthStr+unitStr at 96 px (FONT_DEPTH) directly into _cbuf
    void drawTopSection(const char *depthStr, const char *unitStr, bool alarm);
    void drawWaterSection(const float *hist, int histIdx,
                          bool histFull, float currentDepth);
};
