#pragma once
#include <lvgl.h>

// ============================================================
// LanguageOverlay - first step of the initial setup.
//
// Sequence on the very first start after flashing:
//     start in English -> THIS language picker -> confirm licences
//
// The device starts in English (default cfg.lang = "en") because that is the
// language most likely to be understood by everyone; then the user chooses.
// The choice is saved immediately, and only then does the licence screen
// open - which therefore already appears in the chosen language.
//
// Like ConfigOverlay and LicenseOverlay it sits on lv_layer_top(), is modal
// and is deleted completely on close.
// ============================================================
class LanguageOverlay {
public:
    void open();
    void close();
    bool isOpen() const { return _open; }

    // Call from the LVGL task (DisplayManager::update), never from an event.
    void requestOpen() { _pendingOpen = true; }
    void update();

private:
    void build();
    static void cbPickDe(lv_event_t *e);
    static void cbPickEn(lv_event_t *e);
    static void pick(bool english);

    lv_obj_t *_root = nullptr;
    bool _open = false;
    bool _pendingOpen = false;
};

extern LanguageOverlay languageOverlay;
