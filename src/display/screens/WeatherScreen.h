#pragma once
#include "../BaseScreen.h"
#include "../../nmea/DataModel.h"
#include "../../i18n/I18n.h"

// ============================================================
// WeatherScreen – "Wetter" (barometer + environment).
//
// Big barometric pressure + trend word/change, an lv_chart trend curve of the
// pressure ring, and three tiles (air temp, water temp, humidity). Widget-based
// (lv_chart / lv_label) — no canvas. PGN 130310 / 130311 / 130314.
// ============================================================
class WeatherScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_WEATHER); }
    void create(lv_obj_t *parent) override;
    void update() override;
    void resetForRebuild() override;

private:
    lv_obj_t *_pressVal = nullptr;
    lv_obj_t *_trend    = nullptr;   // rising / falling / steady (STR_WEA_TREND_*)
    lv_obj_t *_trendSub = nullptr;   // change in hPa
    lv_obj_t *_chart    = nullptr;
    lv_chart_series_t *_series = nullptr;
    lv_obj_t *_airVal   = nullptr;
    lv_obj_t *_waterVal = nullptr;
    lv_obj_t *_humVal   = nullptr;

    void makeTile(lv_obj_t *parent, int x, const char *label, lv_obj_t *&valOut);
};
