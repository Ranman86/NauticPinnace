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
    static constexpr int CW   = SCREEN_W;   // main canvas width

    lv_obj_t   *_canvas  = nullptr;   // main full-width canvas
    lv_color_t *_cbuf    = nullptr;
    lv_obj_t   *_lblAlarm = nullptr;

    // Echogram scratch, PSRAM arena, allocated once in create() and reused on
    // a live theme rebuild like _cbuf. Draw-time only, CPU-only on the UI task.
    int        *_profY  = nullptr;   // per column: profile row 0..H-1 (was a static in drawWaterSection)
    uint32_t   *_rcp    = nullptr;   // per column: bedRcp() of its seabed fade length
    lv_color_t *_bedPal = nullptr;   // 256 seabed colours, see bedPalette()
    bool        _palOk  = false;     // _bedPal holds the colours of _palBed/_palBg
    lv_color_t  _palBed, _palBg;

    // Frame skip, as in RudderScreen: the canvas holds the frame for
    // _paintedKey. Everything the picture depends on that can change without
    // create() running again (theme, fonts and language all rebuild through it).
    struct FrameKey {
        uint32_t histSeq;   // DataModel::depthHistSeq: the history ring
        int      guideY;    // row of the current-depth guide line, -1 = none
        bool     ft;        // depth unit
        bool     alarm;     // depth value drawn in red
        char     num[12];   // depth value text
    };
    bool     _painted = false;
    FrameKey _paintedKey;

    // The six scale labels for the maxM they were printed for (float bits).
    bool     _scaleOk  = false;
    uint32_t _scaleFor = 0;
    char     _scaleTxt[6][10];

    // Render depthStr+unitStr at 96 px (FONT_DEPTH) directly into _cbuf
    void drawTopSection(const char *depthStr, const char *unitStr, bool alarm);
    void drawWaterSection(const float *hist, int histIdx, bool histFull,
                          float maxM, int guideY);
    void bedPalette();
};
