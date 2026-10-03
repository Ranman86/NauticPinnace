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

    static constexpr int CS = UI_S(420);    // square canvas (centred drift view)

private:
    lv_obj_t   *_canvas   = nullptr;
    lv_color_t *_cbuf     = nullptr;
    lv_obj_t   *_btnSet   = nullptr;
    lv_obj_t   *_btnMinus = nullptr;
    lv_obj_t   *_btnPlus  = nullptr;
    lv_obj_t   *_btnAlarm = nullptr;        // toggles cfg.anchorAlarmOn
    lv_obj_t   *_btnAlarmLbl = nullptr;
    lv_obj_t   *_btnSetLbl   = nullptr;     // "set" / "lift" - see refreshSetBtn()

    // Breadcrumb track: boat offsets from the anchor in metres (North, East).
    // The two 240-sample rings used to be members, i.e. ~1.9 KB of internal
    // DRAM held for the whole app lifetime. They now come from the PSRAM arena
    // (allocated in create()): one 8-byte sample every 3 s written from the UI
    // task, read only by draw() - no ISR, no DMA, so PSRAM is safe here.
    // Nullable, therefore: never dereference without the guard in update().
    static constexpr int TRACK_N = 240;
    float   *_trkN    = nullptr;
    float   *_trkE    = nullptr;
    int      _trkIdx  = 0;
    bool     _trkFull = false;
    uint32_t _lastTrkMs = 0;
    float    _maxDist = 0.f;                // largest drift seen this session [m]

    // What the two buttons were last drawn for. The anchor state can change
    // without anyone touching this screen - NauticPi sets or lifts the anchor
    // and DisplayManager adopts it - and a button whose label says "set" while
    // the code behind it lifts is worse than no button at all. Compared rather
    // than refreshed every frame: lv_label_set_text copies the string each
    // time, and this runs at the frame rate.
    bool     _btnSetShown   = false;
    bool     _btnAlarmShown = false;

    void draw();
    void refreshAlarmBtn();                 // label/colour for the alarm toggle
    // The set button does double duty. There is no room for a fifth button in
    // the bottom band, and "lift" is only ever wanted when an anchor is down,
    // so the one button carries both. Without it this device could receive a
    // lifted anchor from NauticPi but never send one.
    void refreshSetBtn();
    void syncButtons();                     // re-label when the state moved

    static void cbSet(lv_event_t *e);
    static void cbMinus(lv_event_t *e);
    static void cbPlus(lv_event_t *e);
    static void cbAlarm(lv_event_t *e);
};
