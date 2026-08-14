#pragma once
#include "../BaseScreen.h"
#include "../../nmea/DataModel.h"
#include "../../i18n/I18n.h"

// ============================================================
// VmgScreen – "VMG" (performance optimisation).
//
// Velocity-made-good vs the achievable target from the polar: current VMG, the
// best VMG at this wind speed, a performance bar, the optimal TWA and steering
// guidance (head up / bear away). Widget-based — no canvas.
// ============================================================
class VmgScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_VMG); }
    void create(lv_obj_t *parent) override;
    void update() override;
    void resetForRebuild() override;

private:
    lv_obj_t *_vmg    = nullptr;   // big current VMG
    lv_obj_t *_vmgCap = nullptr;   // "VMG Luv/Lee"
    lv_obj_t *_bar    = nullptr;   // performance %
    lv_obj_t *_perf   = nullptr;   // % label
    lv_obj_t *_guide  = nullptr;   // steering guidance
    lv_obj_t *_t[4]   = { nullptr, nullptr, nullptr, nullptr };   // tile value labels

    void mkTile(lv_obj_t *parent, int x, int y, const char *label, lv_obj_t *&valOut);
};
