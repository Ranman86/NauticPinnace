#pragma once
#include "../BaseScreen.h"
#include "../../nmea/DataModel.h"
#include "../../PsramArena.h"
#include "../../i18n/I18n.h"

// ============================================================
// ClockScreen – "Uhr" (clock + sun + globe).
//
// Top: an equirectangular world map (primitive continents) with the live
// day/night terminator + the boat's position + the subsolar point + a moon-
// phase disc. Below: local time/date, sunrise/sunset, moon phase. The map is
// redrawn every few seconds (the terminator moves slowly); labels tick every
// update. Sun/moon astronomy from SunCalc.h.
// ============================================================
class ClockScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_CLOCK); }
    void create(lv_obj_t *parent) override;
    void update() override;
    void resetForRebuild() override;

private:
    static constexpr int MW = 480, MH = 200;   // world-map canvas size
    static constexpr int TW = 452, TH = 48;    // tide-curve canvas size

    lv_obj_t   *_canvas     = nullptr;
    lv_color_t *_cbuf       = nullptr;
    lv_obj_t   *_clock      = nullptr;
    lv_obj_t   *_date       = nullptr;
    lv_obj_t   *_sunLine    = nullptr;
    lv_obj_t   *_moonLine   = nullptr;
    lv_obj_t   *_tideCanvas = nullptr;
    lv_color_t *_tcbuf      = nullptr;
    lv_obj_t   *_tideBig    = nullptr;
    lv_obj_t   *_tideLine   = nullptr;
    lv_obj_t   *_tideNote   = nullptr;

    uint32_t _lastMapMs  = 0;
    uint32_t _lastTideMs = 0;

    void drawMap(float lat, float lon, float decDeg, float sunLonDeg,
                 float moonIllum, bool moonWax, bool gpsOk);
    void drawTide(long utcDays, double utcSec, float lon);
};
