#pragma once
#include <lvgl.h>

// ============================================================
// HomeOverlay – 7B-only launcher, opened by the rail's home button.
//
// Shows every ACTIVE screen (nav order) as a labelled module tile – like the
// start page of a chartplotter MFD – plus one settings tile. Tapping a tile
// jumps to that screen; the settings tile opens the config overlay instead.
//
// Built on lv_layer_top(), created on open() and fully deleted on close(),
// so the steady-state memory footprint is unchanged. On the other boards the
// class is an inert stub: nothing ever requests it there.
// ============================================================
#if defined(BOARD_PANEL_1024X600)
class HomeOverlay {
public:
    void open();
    void close();
    bool isOpen() const { return _open; }

private:
    bool      _open = false;
    lv_obj_t *_root = nullptr;   // modal container on lv_layer_top(), sized to
                                 // the LOGICAL screen (rotation-aware)

    static void cbClose(lv_event_t *e);
    static void cbTile(lv_event_t *e);        // user_data = screen id
    static void cbConfigTile(lv_event_t *e);
    static void cbRootGesture(lv_event_t *e);
};
#else
class HomeOverlay {
public:
    void open() {}
    void close() {}
    bool isOpen() const { return false; }
};
#endif

extern HomeOverlay homeOverlay;
