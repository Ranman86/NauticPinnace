#pragma once
#include <lvgl.h>

// ============================================================
// LicenseOverlay - shows the licences of the modules in use.
//
// Two modes of operation:
//   * openFirstRun()  - on the very first start after flashing. Modal, must be
//                       confirmed with "Verstanden" (understood); the confirmation
//                       is recorded in the configuration (cfg.licenseAccepted).
//   * open()          - any time from the gear menu, only with "Schliessen" (close).
//
// Like ConfigOverlay it sits on lv_layer_top(), i.e. above all screens and
// independent of the PSRAM canvas arena. It is deleted completely on close,
// so no memory stays occupied in the idle state.
// ============================================================
class LicenseOverlay {
public:
    void openFirstRun();
    void open();
    void close();
    bool isOpen() const { return _open; }

    // Call from the LVGL task (DisplayManager::update), never from an event.
    void requestOpenFirstRun() { _pendingFirstRun = true; }
    void update();

private:
    void build(bool firstRun);
    static void cbAccept(lv_event_t *e);
    static void cbClose(lv_event_t *e);

    lv_obj_t *_root = nullptr;
    bool _open = false;
    bool _pendingFirstRun = false;
};

extern LicenseOverlay licenseOverlay;
