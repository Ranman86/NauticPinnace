#pragma once
#include "../BaseScreen.h"
#include "../../i18n/I18n.h"

// ============================================================
// FusionScreen – remote control for a Fusion marine stereo (RA-670).
//   • source list (lv_dropdown, auto-filled from the radio / demo)
//   • now-playing: title (scrolling) + artist/album + transport + time
//   • 4 volume rows, each with a MUTE button: Master (scales/mutes all) +
//     Zone 1/2/3. Master mute = mute-all / unmute-all.
// Pure LVGL widgets. Control via Fusion:: (DataModel +, when CAN is live, N2K).
// ============================================================
class FusionScreen : public BaseScreen {
public:
    void create(lv_obj_t *parent) override;
    void onShow() override;      // pull fresh state from the radio (status request)
    void update() override;
    void resetForRebuild() override;
    const char *title() const override { return T(STR_SCREEN_FUSION); }

private:
    lv_obj_t *_srcDropdown = nullptr;
    lv_obj_t *_titleLabel  = nullptr;
    lv_obj_t *_metaLabel   = nullptr;   // "Artist - Album"
    lv_obj_t *_timeLabel   = nullptr;
    lv_obj_t *_playIcon    = nullptr;   // label inside play/pause button
    lv_obj_t *_masterSlider = nullptr;
    lv_obj_t *_masterVal    = nullptr;
    lv_obj_t *_masterMuteIcon = nullptr;
    lv_obj_t *_zoneSlider[3]   = { nullptr, nullptr, nullptr };
    lv_obj_t *_zoneVal[3]      = { nullptr, nullptr, nullptr };
    lv_obj_t *_zoneMuteIcon[3] = { nullptr, nullptr, nullptr };

    // change-detection caches
    char    _lastTitle[48] = {1,0};
    char    _lastMeta[88]  = {1,0};
    int     _lastPlay      = -1;
    int     _lastSrcSel    = -2;
    int     _lastSrcCount  = -1;
    int     _lastMasterMute = -1;
    int     _lastZoneMute[3] = { -1, -1, -1 };

    uint32_t _lastReq = 0;   // throttle for periodic status requests while shown

    void syncFromData();

    static void cbSourceDropdown(lv_event_t *e);
    static void cbPlay(lv_event_t *e);
    static void cbNext(lv_event_t *e);
    static void cbPrev(lv_event_t *e);
    static void cbMasterMute(lv_event_t *e);
    static void cbZoneMute(lv_event_t *e);   // user_data = zone index
    static void cbMaster(lv_event_t *e);
    static void cbZone(lv_event_t *e);        // user_data = zone index
};
