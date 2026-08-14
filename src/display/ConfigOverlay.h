#pragma once
#include <lvgl.h>
#include "../i18n/I18n.h"   // Lang, for setLanguage()

// ============================================================
// ConfigOverlay – on-screen configuration menu.
//
// Opened by swiping DOWN from the top edge of any screen (see DisplayManager's
// top hot-zone). Built on lv_layer_top() so it floats above all screen content
// and is independent of the PSRAM canvas arena; created on open() and fully
// deleted on close() so the steady-state memory footprint is unchanged.
//
// Provides:
//   * WLAN connection: SSID + password via an on-screen keyboard -> save+reboot
//   * Internal hotspot (AP): switch to AP mode (SSID/pw derived from MAC, shown
//     in plaintext) -> save+reboot
//   * Light/Dark override: applied live (no reboot) via dispMgr theme reload
// ============================================================
class ConfigOverlay {
public:
    void open();
    void close();
    bool isOpen() const { return _open; }
#ifdef SIMULATOR
    void simShowKeyboard() { if (_taSsid) showKeyboard(_taSsid); }  // layout test only
#endif

private:
    bool      _open   = false;
    lv_obj_t *_root   = nullptr;   // full-screen modal container on lv_layer_top()
    lv_obj_t *_kb     = nullptr;   // on-screen keyboard (child of _root)
    lv_obj_t *_taSsid = nullptr;
    lv_obj_t *_taPass = nullptr;

    void showKeyboard(lv_obj_t *ta);
    void hideKeyboard();
    static void setLanguage(Lang l);   // save + rebuild screens live

    static void cbClose(lv_event_t *e);
    static void cbConnect(lv_event_t *e);
    static void cbHotspot(lv_event_t *e);
    static void cbThemeLight(lv_event_t *e);
    static void cbThemeDark(lv_event_t *e);
    static void cbThemeNight(lv_event_t *e);   // red-preserving night mode
    static void cbLicenses(lv_event_t *e);     // show licences and modules
    static void cbLangDe(lv_event_t *e);       // language German
    static void cbLangEn(lv_event_t *e);       // language English
    static void cbThemeAuto(lv_event_t *e);
    static void cbTaClicked(lv_event_t *e);
    static void cbKbEvent(lv_event_t *e);
    static void cbRootGesture(lv_event_t *e);
    static void cbWifiToggle(lv_event_t *e);   // WiFi radio on/off (save + reboot)
    static void cbN2kListenToggle(lv_event_t *e);  // listen-only (save + reboot)
};

extern ConfigOverlay configOverlay;
