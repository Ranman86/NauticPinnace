#pragma once
#include "../BaseScreen.h"
#include "../../config/Config.h"
#include "../../PsramArena.h"
#include "../../i18n/I18n.h"

// ============================================================
// AnchorScreen – "Ankerwache" (anchor watch).
//
// Round drift view: anchor fixed at the centre, concentric range rings + the
// alarm-radius circle, the boat plotted at its current bearing/distance from the
// anchor, and a breadcrumb track of the swing. North-up.
//
// On-screen controls: "Anker setzen" (capture current GPS), radius -/+ and an
// alarm on/off toggle. All anchor state lives in AppConfig (persisted), so the
// watch resumes after a reboot. The actual DRIFT ALARM is evaluated globally in
// DisplayManager::update() (so it fires on any screen, with a blinking banner +
// buzzer); this screen is the UI to arm/adjust and visualise it.
// ============================================================
class AnchorScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_ANCHOR); }
    void create(lv_obj_t *parent) override;
    void onShow() override;
    void update() override;
    void resetForRebuild() override;

    static constexpr int CS = 420;          // square canvas (centred drift view)

private:
    lv_obj_t   *_canvas   = nullptr;
    lv_color_t *_cbuf     = nullptr;
    lv_obj_t   *_btnSet   = nullptr;
    lv_obj_t   *_btnMinus = nullptr;
    lv_obj_t   *_btnPlus  = nullptr;
    lv_obj_t   *_btnAlarm = nullptr;        // toggles cfg.anchorAlarmOn
    lv_obj_t   *_btnAlarmLbl = nullptr;

    // Breadcrumb track: boat offsets from the anchor in metres (North, East).
    static constexpr int TRACK_N = 240;
    float    _trkN[TRACK_N];
    float    _trkE[TRACK_N];
    int      _trkIdx  = 0;
    bool     _trkFull = false;
    uint32_t _lastTrkMs = 0;
    float    _maxDist = 0.f;                // largest drift seen this session [m]

    void draw();
    void refreshAlarmBtn();                 // label/colour for the alarm toggle

    static void cbSet(lv_event_t *e);
    static void cbMinus(lv_event_t *e);
    static void cbPlus(lv_event_t *e);
    static void cbAlarm(lv_event_t *e);
};
