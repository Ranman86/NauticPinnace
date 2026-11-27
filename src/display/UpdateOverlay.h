#pragma once
// ============================================================================
// UpdateOverlay - everything an update over NMEA 2000 puts on the panel
// (docs/N2K-UPDATE.md section 7):
//
//   7.2 the confirmation dialog: modal, "Install" acts only after a 1.5 s
//       hold that begins after a 1.5 s guard (phantom touches from spray on a
//       capacitive panel cannot install anything), declined after 60 s
//   7.3 the progress banner: 44 px at the top of the instrument, with a
//       held "Cancel"
//   7.4 the restart countdown: 10 s, "Restart now" / "Later", paused while
//       an alarm is active - and meanwhile only a strip under the alarm
//       banner, so the instrument the alarm is about stays in view
//   7.5 the verify banner after the restart, the success toast and the
//       rollback notice
//   7.6 the failure box, the web interface toast
//   7.7 the identify screen
//   7.8 "Restart after the update" / "Demo mode after the update"
//
// Built on lv_layer_top() in the display task only. The N2K task never
// touches it: everything arrives through the UpdUi mailbox, and the user's
// answers go back the same way. Rotation, theme and both languages apply;
// the layouts fit 480x480 and 1024x600 in both orientations. Builds in the
// PC simulator as well, where --n2kupdate-demo drives it.
// ============================================================================
#include <lvgl.h>
#include <stdint.h>

class UpdateOverlay {
public:
    // DisplayManager::update(), every display tick.
    void tick();

    // A full-screen modal is up (dialog, countdown, identify screen):
    // DisplayManager stops painting the instrument behind it. A countdown
    // held by an alarm is a strip, not a modal (openCountdownBanner()).
    bool modalOpen() const { return _confirm || (_countdown && !_cdPaused) || _identify; }
    // Anything of the update UI is on screen. While an alarm is active,
    // DisplayManager then lifts the alarm banner above it (7.1).
    bool anyVisible() const {
        return modalOpen() || _banner || _box || _toast || _cdBanner;
    }

    // 7.8, called by DisplayManager.
    void restartDeferred(uint32_t seconds);
    void demoDeferred();

    // A short note at the bottom of the screen for `ms`.
    void toast(const char *text, uint32_t ms = 5000);

#ifdef SIMULATOR
    // --n2kupdate-selftest: where to press (null while no dialog is open).
    lv_obj_t *simInstallButton() const { return _holdInstall.btn; }
#endif

private:
    struct Hold {                        // press-and-hold state of one button
        lv_obj_t *btn = nullptr, *fill = nullptr;
        uint32_t  armAt = 0;             // presses beginning before this are ignored
        uint32_t  start = 0;
        bool      active = false, fired = false;
    };
    static void cbHold(lv_event_t *e);
    static void updateHoldFill(Hold &h, uint32_t now);
    lv_obj_t *makeHoldButton(lv_obj_t *parent, const char *text, Hold &h,
                             lv_coord_t w, lv_coord_t hgt, bool primary,
                             const lv_font_t *font = nullptr);

    void handleEvent(uint8_t type, uint32_t token, uint8_t a, uint8_t b, uint8_t c, uint32_t now);
    void openConfirm(uint32_t now);
    void closeConfirm();
    void tickConfirm(uint32_t now);
    void tickBanner(uint32_t now);
    void closeBanner();
    void openCountdown(uint32_t now);
    void closeCountdown();
    void tickCountdown(uint32_t now);
    void openCountdownBanner();
    void closeCountdownBanner();
    void showBox(const char *text, uint32_t autoCloseMs, bool notice);
    void closeBox();
    void openIdentify(uint8_t seconds, uint32_t now);
    void closeIdentify();
    void wakeBacklight();
    void restoreBacklight();

    static void cbDecline(lv_event_t *e);
    static void cbRestartNow(lv_event_t *e);
    static void cbLater(lv_event_t *e);
    static void cbBoxOk(lv_event_t *e);
    static void cbIdentifyTap(lv_event_t *e);

    // 7.2 dialog
    lv_obj_t *_confirm = nullptr, *_confirmCd = nullptr;
    uint32_t  _confirmToken = 0, _confirmOpenedMs = 0;
    uint16_t  _dialogSeconds = 60;
    int       _cdShown = -1;
    Hold      _holdInstall;
    bool      _declinePending = false;
    // 7.3 / 7.5 banner
    lv_obj_t *_banner = nullptr, *_bannerLbl = nullptr, *_bannerNum = nullptr,
             *_bannerBar = nullptr, *_btnCancel = nullptr;
    lv_coord_t _bannerTextW = 0;         // width shared by the words and the numbers
    uint8_t   _bannerKind = 0;           // 1 progress, 2 verify
    Hold      _holdCancel;
    uint32_t  _bannerTextAt = 0;
    // the latest session snapshot
    bool      _haveSnap = false;
    uint8_t   _snapState = 0, _snapTarget = 0;
    uint32_t  _snapToken = 0, _snapWritten = 0, _snapImage = 0, _snapAccepted = 0,
              _snapStream = 0, _snapStartMs = 0;
    uint16_t  _snapSeconds = 0xFFFF;
    char      _snapVersion[17] = { 0 };
    // 7.4 countdown (and its strip while an alarm holds it)
    lv_obj_t *_countdown = nullptr, *_cdText = nullptr, *_cdBanner = nullptr;
    uint32_t  _cdStartMs = 0;
    bool      _cdPaused = false;
    int       _cdLastShown = -1;
    bool      _restartNow = false, _later = false;
    // 7.5 / 7.6 box and toast
    lv_obj_t *_box = nullptr;
    uint32_t  _boxCloseAt = 0;
    bool      _boxNotice = false, _boxOkPending = false;
    lv_obj_t *_toast = nullptr;
    uint32_t  _toastUntil = 0;
    // 7.7
    lv_obj_t *_identify = nullptr;
    uint32_t  _identifyUntil = 0;
    bool      _identifyTapped = false;
    // backlight woken for a dialog (7.1)
    bool      _woke = false;
};

extern UpdateOverlay updateOverlay;
