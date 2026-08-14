#include "ConfigOverlay.h"
#include "LicenseOverlay.h"
#include "Theme.h"
#include "DisplayManager.h"
#include "../config/Config.h"
#include "../WifiNaming.h"
#include <Arduino.h>

#ifndef SIMULATOR
#include <WiFi.h>
#endif
#ifndef WL_CONNECTED
#define WL_CONNECTED 3
#endif

ConfigOverlay configOverlay;

// ---- small build helpers ----------------------------------------------------
static lv_obj_t *mkLabel(lv_obj_t *parent, const char *txt, int x, int y,
                         const lv_font_t *font, lv_color_t col) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

static lv_obj_t *mkButton(lv_obj_t *parent, const char *txt, int x, int y, int w, int h,
                          lv_color_t bg, lv_color_t fg, lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_color(b, CLR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_set_style_text_font(l, FONT_MED, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

static lv_obj_t *mkField(lv_obj_t *parent, const char *value, const char *placeholder,
                         int x, int y, int w, int h, lv_event_cb_t cb) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, value ? value : "");
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, h);
    lv_obj_set_style_text_font(ta, FONT_MED, 0);
    lv_obj_set_style_bg_color(ta, CLR_SURFACE, 0);
    lv_obj_set_style_text_color(ta, CLR_TEXT, 0);
    lv_obj_set_style_border_color(ta, CLR_BORDER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_pad_top(ta, 6, 0);
    lv_obj_set_style_pad_bottom(ta, 6, 0);
    if (cb) lv_obj_add_event_cb(ta, cb, LV_EVENT_CLICKED, nullptr);
    return ta;
}

static lv_obj_t *mkSwitch(lv_obj_t *parent, int x, int y, bool on, lv_event_cb_t cb) {
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 46, 24);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_style_bg_color(sw, CLR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, CLR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, CLR_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return sw;
}

// ---- open / build -----------------------------------------------------------
void ConfigOverlay::open() {
    if (_open) return;
    _open = true;

    _root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_root, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, CLR_BG, 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_root, LV_OBJ_FLAG_CLICKABLE);   // modal: absorb touches
    lv_obj_add_event_cb(_root, cbRootGesture, LV_EVENT_GESTURE, nullptr);

    // ---- header ----
    mkLabel(_root, T(STR_CFG_TITLE), 14, 12, FONT_LARGE, CLR_TEXT);
    mkButton(_root, LV_SYMBOL_CLOSE, SCREEN_W - 12 - 44, 8, 44, 36, CLR_SURFACE, CLR_TEXT, cbClose);

    // ---- WLAN section ----
    // Section title on the left; the WLAN radio switch sits on the otherwise-
    // empty right side of the title line, so it stays clear of the SSID/password
    // input fields below. A single status line goes underneath.
    // (A Bluetooth switch used to live here — removed: the firmware has no BT
    // code, so it could only reserve memory for nothing.)
    mkLabel(_root, T(STR_CFG_WIFI_SECTION), 14, 46, FONT_MED, CLR_ACCENT);
    mkLabel(_root, T(STR_CFG_WIFI_SHORT), 386, 48, FONT_SMALL, CLR_TEXT);
    mkSwitch(_root, 422, 44, appConfig.cfg.wifiEnabled, cbWifiToggle);

    // Status line with the address at which the web interface is reachable.
    // The state is derived from the RADIO, not from cfg.apMode: if joining the
    // configured network fails, WebConfig::begin() itself switches over to the
    // hotspot - but cfg.apMode then still stays false. Previously the line
    // reported "Nicht verbunden" (not connected) in exactly this case, even
    // though the device was very much reachable at 192.168.4.1.
    char st[64];
    if (!appConfig.cfg.wifiEnabled) {
        snprintf(st, sizeof(st), "%s", T(STR_CFG_WIFI_OFF));
    } else {
#ifndef SIMULATOR
        const wifi_mode_t m = WiFi.getMode();
        // Associated but no DHCP lease reads as NOT connected: reporting
        // "connected: 0.0.0.0" is what made a dead link look healthy here.
        if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0))
            snprintf(st, sizeof(st), "%s: %s", T(STR_CFG_WIFI_CONNECTED),
                     WiFi.localIP().toString().c_str());
        else if (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA)
            snprintf(st, sizeof(st), "%s: %s", T(STR_CFG_WIFI_AP_ACTIVE),
                     WiFi.softAPIP().toString().c_str());
        else
            snprintf(st, sizeof(st), "%s", T(STR_CFG_WIFI_NOT_CONN));
#else
        // No radio on the PC: show what the configuration requests.
        if (appConfig.cfg.apMode)
            snprintf(st, sizeof(st), "%s: %s", T(STR_CFG_WIFI_AP_ACTIVE),
                     WiFi.softAPIP().toString().c_str());
        else
            snprintf(st, sizeof(st), "%s: %s", T(STR_CFG_WIFI_CONNECTED),
                     WiFi.localIP().toString().c_str());
#endif
    }
    mkLabel(_root, st, 14, 70, FONT_SMALL, CLR_TEXT_DIM);

    mkLabel(_root, "SSID", 14, 98, FONT_SMALL, CLR_TEXT_DIM);
    _taSsid = mkField(_root, appConfig.cfg.wifiSsid, T(STR_CFG_NETWORK_NAME), 92, 88, 376, 38, cbTaClicked);

    mkLabel(_root, T(STR_CFG_PASSWORD_SHORT), 14, 140, FONT_SMALL, CLR_TEXT_DIM);
    _taPass = mkField(_root, appConfig.cfg.wifiPassword, T(STR_CFG_PASSWORD), 92, 130, 376, 38, cbTaClicked);

    mkButton(_root, T(STR_CFG_CONNECT_REBOOT), 14, 172, 454, 38, CLR_ACCENT, CLR_ON_ACCENT, cbConnect);

    // ---- internal hotspot section (auto-connect QR on the right) ----
    mkLabel(_root, T(STR_CFG_HOTSPOT_SECTION), 14, 216, FONT_MED, CLR_ACCENT);
    {
        String apS = wifiApSsid();
        String apP = wifiApPassword();
        char nm[64], pw[64];
        snprintf(nm, sizeof(nm), "Name:  %s", apS.c_str());
        snprintf(pw, sizeof(pw), "Pass:  %s", apP.c_str());
        mkLabel(_root, nm, 14, 242, FONT_SMALL, CLR_TEXT);
        mkLabel(_root, pw, 14, 264, FONT_SMALL, CLR_TEXT);
        mkLabel(_root, T(STR_CFG_SCAN_QR), 14, 292, FONT_SMALL, CLR_TEXT_DIM);
        mkButton(_root, T(STR_CFG_HOTSPOT_REBOOT), 14, 316, 312, 38, CLR_SURFACE, CLR_TEXT, cbHotspot);

#if LV_USE_QRCODE
        // WiFi auto-connect QR: "WIFI:T:WPA;S:<ssid>;P:<pw>;;". Fixed black-on-white
        // on a white card (quiet-zone border) so any phone camera scans it in either
        // theme. Encodes the hotspot's credentials (random per-device password).
        lv_obj_t *qrCard = lv_obj_create(_root);
        lv_obj_set_size(qrCard, 128, 128);
        lv_obj_set_pos(qrCard, 338, 220);
        // DELIBERATELY fixed: the quiet zone of a QR code must stay white,
        // otherwise no phone camera will recognise it. Also applies in night mode.
        lv_obj_set_style_bg_color(qrCard, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(qrCard, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(qrCard, 0, 0);
        lv_obj_set_style_radius(qrCard, 4, 0);
        lv_obj_set_style_pad_all(qrCard, 0, 0);
        lv_obj_clear_flag(qrCard, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *qr = lv_qrcode_create(qrCard, 112, lv_color_black(), lv_color_white());
        lv_obj_center(qr);
        String w = String("WIFI:T:WPA;S:") + apS + ";P:" + apP + ";;";
        lv_qrcode_update(qr, w.c_str(), w.length());
#endif
    }

    // ---- display (theme) section: Auto / Light / Dark / Night ----
    // Four buttons in the same span 14…466: b=107, gap 8.
    mkLabel(_root, T(STR_CFG_DISPLAY_SECTION), 14, 358, FONT_MED, CLR_ACCENT);
    bool autoT = appConfig.cfg.themeAuto;
    bool light = !autoT && (strcmp(appConfig.cfg.themeActive, "light") == 0);
    bool night = !autoT && (strcmp(appConfig.cfg.themeActive, "night") == 0);
    bool dark  = !autoT && !light && !night;      // fallback, no longer a catch-all
    const int bw = 107, gap = 8;
    mkButton(_root, T(STR_CFG_THEME_AUTO), 14, 382, bw, 42,
             autoT ? CLR_ACCENT : CLR_SURFACE, autoT ? CLR_ON_ACCENT : CLR_TEXT, cbThemeAuto);
    mkButton(_root, T(STR_CFG_THEME_LIGHT), 14 + (bw + gap), 382, bw, 42,
             light ? CLR_ACCENT : CLR_SURFACE, light ? CLR_ON_ACCENT : CLR_TEXT, cbThemeLight);
    mkButton(_root, T(STR_CFG_THEME_DARK), 14 + 2 * (bw + gap), 382, bw, 42,
             dark ? CLR_ACCENT : CLR_SURFACE, dark ? CLR_ON_ACCENT : CLR_TEXT, cbThemeDark);
    mkButton(_root, T(STR_CFG_THEME_NIGHT), 14 + 3 * (bw + gap), 382, bw, 42,
             night ? CLR_ACCENT : CLR_SURFACE, night ? CLR_ON_ACCENT : CLR_TEXT, cbThemeNight);

    // ---- language + licences share the last row ----
    // Only 480 px tall: there is no room for a row of their own. DE/EN are the
    // same in both languages, so they need no translation themselves.
    const bool isEn = (i18nLang() == Lang::EN);
    mkButton(_root, "DE", 14, 434, 52, 38,
             isEn ? CLR_SURFACE : CLR_ACCENT, isEn ? CLR_TEXT : CLR_ON_ACCENT, cbLangDe);
    mkButton(_root, "EN", 70, 434, 52, 38,
             isEn ? CLR_ACCENT : CLR_SURFACE, isEn ? CLR_ON_ACCENT : CLR_TEXT, cbLangEn);
    mkButton(_root, T(STR_CFG_LICENSES_BTN), 128, 434, 204, 38,
             CLR_SURFACE, CLR_TEXT, cbLicenses);
    // Listen-only: N2km_ListenOnly — the device then sends nothing onto the bus
    // (no address claim, no heartbeat, no Fusion control). For other people's
    // boats, charter, workshop appointments. Takes effect after reboot.
    mkLabel(_root, T(STR_CFG_N2K_LISTEN), 340, 444, FONT_SMALL, CLR_TEXT);
    mkSwitch(_root, 420, 440, appConfig.cfg.n2kListenOnly, cbN2kListenToggle);
}

// Language change: apply live, like the theme. requestThemeReload() rebuilds
// the screens so the labels are created in the new language; the overlay itself
// is closed because its own labels were set when it was opened.
void ConfigOverlay::setLanguage(Lang l) {
    if (i18nLang() == l) { configOverlay.close(); return; }
    i18nSetLang(l);
    strlcpy(appConfig.cfg.lang, i18nLangCode(l), sizeof(appConfig.cfg.lang));
    appConfig.save();
    dispMgr.requestThemeReload();   // live, no reboot
    configOverlay.close();
}

void ConfigOverlay::cbLangDe(lv_event_t *e) { configOverlay.setLanguage(Lang::DE); }
void ConfigOverlay::cbLangEn(lv_event_t *e) { configOverlay.setLanguage(Lang::EN); }

void ConfigOverlay::close() {
    if (!_open) return;
    _open = false;
    if (_root) lv_obj_del_async(_root);   // also deletes _kb / textareas (children)
    _root = _kb = _taSsid = _taPass = nullptr;
}

// ---- keyboard ---------------------------------------------------------------
void ConfigOverlay::showKeyboard(lv_obj_t *ta) {
    if (!_root) return;
    if (!_kb) {
        _kb = lv_keyboard_create(_root);
        lv_obj_set_size(_kb, SCREEN_W, 200);
        lv_obj_align(_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(_kb, cbKbEvent, LV_EVENT_READY, nullptr);
        lv_obj_add_event_cb(_kb, cbKbEvent, LV_EVENT_CANCEL, nullptr);
    }
    lv_keyboard_set_textarea(_kb, ta);
    lv_obj_clear_flag(_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_kb);
}

void ConfigOverlay::hideKeyboard() {
    if (_kb) {
        lv_keyboard_set_textarea(_kb, nullptr);
        lv_obj_add_flag(_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---- event callbacks --------------------------------------------------------
void ConfigOverlay::cbClose(lv_event_t *e)     { configOverlay.close(); }
void ConfigOverlay::cbTaClicked(lv_event_t *e) { configOverlay.showKeyboard(lv_event_get_target(e)); }
void ConfigOverlay::cbKbEvent(lv_event_t *e)   { configOverlay.hideKeyboard(); }

void ConfigOverlay::cbRootGesture(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) configOverlay.close();
}

void ConfigOverlay::cbConnect(lv_event_t *e) {
    ConfigOverlay &s = configOverlay;
    if (s._taSsid) strlcpy(appConfig.cfg.wifiSsid,     lv_textarea_get_text(s._taSsid), sizeof(appConfig.cfg.wifiSsid));
    if (s._taPass) strlcpy(appConfig.cfg.wifiPassword, lv_textarea_get_text(s._taPass), sizeof(appConfig.cfg.wifiPassword));
    appConfig.cfg.apMode = false;
    // Pressing "connect" IS the request to switch the radio on. Without this
    // the device saved the credentials, rebooted, and main.cpp then skipped
    // webCfg.begin() because wifiEnabled defaults to false ("Funk aus" for the
    // boat) — the user saw a restart but never a network.
    appConfig.cfg.wifiEnabled = true;
    appConfig.save();
#ifndef SIMULATOR
    s.close();
    dispMgr.requestReboot(T(STR_CFG_RB_CONNECT));
#else
    Serial.printf("[sim] connect ssid='%s' wifiEnabled=%d apMode=%d -> reboot (skipped)\n",
                  appConfig.cfg.wifiSsid, (int)appConfig.cfg.wifiEnabled,
                  (int)appConfig.cfg.apMode);
    s.close();
#endif
}

void ConfigOverlay::cbHotspot(lv_event_t *e) {
    ConfigOverlay &s = configOverlay;
    appConfig.cfg.apMode = true;
    appConfig.cfg.wifiEnabled = true;   // same as cbConnect: the button means "radio on"
    appConfig.save();
#ifndef SIMULATOR
    s.close();
    dispMgr.requestReboot(T(STR_CFG_RB_HOTSPOT));
#else
    Serial.printf("[sim] switch to AP wifiEnabled=%d -> reboot (skipped)\n",
                  (int)appConfig.cfg.wifiEnabled);
    s.close();
#endif
}

void ConfigOverlay::cbThemeLight(lv_event_t *e) {
    appConfig.cfg.themeAuto = false;
    strlcpy(appConfig.cfg.themeActive, "light", sizeof(appConfig.cfg.themeActive));
    appConfig.save();
    dispMgr.requestThemeReload();   // live, no reboot
    configOverlay.close();
}

void ConfigOverlay::cbThemeDark(lv_event_t *e) {
    appConfig.cfg.themeAuto = false;
    strlcpy(appConfig.cfg.themeActive, "dark", sizeof(appConfig.cfg.themeActive));
    appConfig.save();
    dispMgr.requestThemeReload();   // live, no reboot
    configOverlay.close();
}

void ConfigOverlay::cbLicenses(lv_event_t *e) {
    configOverlay.close();      // close first, then show the licences
    licenseOverlay.open();
}

void ConfigOverlay::cbThemeNight(lv_event_t *e) {
    appConfig.cfg.themeAuto = false;
    strlcpy(appConfig.cfg.themeActive, "night", sizeof(appConfig.cfg.themeActive));
    appConfig.save();
    dispMgr.requestThemeReload();   // live, no reboot
    configOverlay.close();
}

void ConfigOverlay::cbThemeAuto(lv_event_t *e) {
    appConfig.cfg.themeAuto = true;   // DisplayManager switches light/dark by sun
    appConfig.save();
    configOverlay.close();
}

// Radio toggles: WiFi/BT state is applied at boot (WiFi init + BT mem-release),
// so a flip saves + reboots to take effect cleanly.
// Toggling listen-only: save + reboot like WiFi/BT, because the bus mode is set
// in NMEA2000.Open() at startup.
void ConfigOverlay::cbN2kListenToggle(lv_event_t *e) {
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    appConfig.cfg.n2kListenOnly = on;
    appConfig.save();
#ifndef SIMULATOR
    configOverlay.close();
    dispMgr.requestReboot(T(on ? STR_CFG_N2K_LISTEN_ON : STR_CFG_N2K_LISTEN_OFF));
#else
    Serial.printf("[sim] N2K listen-only %s -> reboot (skipped)\n", on ? "on" : "off");
    configOverlay.close();
#endif
}

void ConfigOverlay::cbWifiToggle(lv_event_t *e) {
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    appConfig.cfg.wifiEnabled = on;
    appConfig.save();
#ifndef SIMULATOR
    configOverlay.close();
    dispMgr.requestReboot(T(on ? STR_CFG_RB_WIFI_ON : STR_CFG_RB_WIFI_OFF));
#else
    Serial.printf("[sim] WiFi %s -> reboot (skipped)\n", on ? "on" : "off");
    configOverlay.close();
#endif
}

