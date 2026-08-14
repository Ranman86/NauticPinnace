#pragma once
#include "../BaseScreen.h"
#include "../../i18n/I18n.h"
#include "../../nmea/DataModel.h"

// ============================================================
// RouteScreen – "Route" (waypoint navigation).
//
// Distance + bearing to the active waypoint, a centred cross-track-error bar
// (CDI), time-to-go and VMC. Fed by a chartplotter via PGN 129283 (XTE) +
// 129284 (navigation data); demo simulates a waypoint ahead. Widget-based.
// ============================================================
class RouteScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_ROUTE); }
    void create(lv_obj_t *parent) override;
    void update() override;
    void resetForRebuild() override;

private:
    lv_obj_t *_wp    = nullptr;   // "Wegpunkt N" / "Waypoint N"
    lv_obj_t *_dtw   = nullptr;   // big distance
    lv_obj_t *_bar   = nullptr;   // XTE (symmetrical CDI)
    lv_obj_t *_xteL  = nullptr;   // XTE caption under the bar
    lv_obj_t *_t[4]  = { nullptr, nullptr, nullptr, nullptr };

    void mkTile(lv_obj_t *parent, int x, int y, const char *label, lv_obj_t *&valOut);
};
