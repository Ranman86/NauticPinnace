#pragma once
#include "../BaseScreen.h"
#include "../../config/Config.h"
#include "../../i18n/I18n.h"

class AisScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_AIS); }
    void create(lv_obj_t *parent) override;
    void update() override;

private:
    lv_obj_t  *_canvas   = nullptr;
    lv_color_t *_cbuf    = nullptr;
    lv_obj_t  *_infoBox  = nullptr;
    lv_obj_t  *_infoLbl  = nullptr;
    lv_obj_t  *_scaleLbl = nullptr;
    lv_obj_t  *_countLbl = nullptr;

    int  _rangeNm  = 5;
    int  _selIdx   = -1;  // selected AIS target index

    static constexpr int CS = 390;

    void drawRadar();
    void showTargetInfo(int idx);
    static void onCanvasClick(lv_event_t *e);
};
