#include "ConfigOverlay.h"
#if defined(BOARD_PANEL_1024X600)
#include "../BoardConfig.h"   // board macros (the geometry comes from Theme.h)
#endif
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
    // (An lv_mem_buf_free_all() lived here briefly - removed: the boot now
    //  PRIMES the draw-buffer caches instead, see main.cpp. Freeing them
    //  would re-expose renders to pool fragmentation.)

#if defined(BOARD_PANEL_1024X600)
    // Full LOGICAL screen, two columns in landscape: WLAN + theme +
    // language/licences on the left, hotspot + QR on the right. The sections
    // keep their proven 480-wide interior layout; only the section ORIGINS
    // move per board and per orientation. A full-size root also absorbs every
    // touch, so the rail and sidebar underneath are unreachable without the
    // old dim shield.
    //
    // Size from uiScreenW()/uiScreenH(), never from LCD_WIDTH/LCD_HEIGHT: the
    // latter are the PHYSICAL panel and stay 1024x600 even at display.rotation
    // 90/270, where LVGL lays out in 600x1024 and clips the root to it - the
    // whole right column and the close button would be off-screen while the
    // bottom of the display kept showing (and touching) the screen below. In
    // landscape the two are identical, so this is a no-op there.
    const bool port = uiPortrait();
    const int W = uiScreenW(), H = uiScreenH();
    const int CX1 = 20,  CY_WIFI = 46, CY_THEME = 280, CY_ROW = 500;
    // Portrait has no room beside the left column (its widest element, the
    // 454 px connect button, already reaches x=474 of 600) but 424 px of
    // spare height, so the second column is STACKED underneath instead: the
    // language/licence row ends at CY_ROW+38 = 538.
    const int CX2   = port ?  20 : 534;
    const int CY_AP = port ? 560 :  46;
    // QR beside the hotspot text (20 + 312 button width + 20 = 352, ending at
    // 544 of 600) and lifted clear of the keyboard, which covers the bottom
    // 280 px from y=744 while an SSID is being typed.
    const int QR_X = port ? 352 : 674, QR_Y = port ? 548 : 230;
    const int QR_CARD = 192, QR_PX = 176;
    // The 600-grid port scaled every font by 1.25 but left these element widths
    // on their 480-grid values, so two controls have been overflowing ever
    // since - in LANDSCAPE as much as in portrait: the WiFi label ran under its
    // own switch, and the licences button (whose caption is FONT_MED, not
    // FONT_SMALL) clipped its text at both ends. Both get honest room here.
    // The listen-only control loses its cramped inline spot next to the
    // language buttons and becomes a row of its own in the empty band between
    // the theme buttons and the language row - the width simply is not there
    // once the button is wide enough to read.
    const int WIFI_LBL_X = CX1 + 326;   // 82 px clear of the switch: fits "WLAN"
    const int LIC_W      = 280;         // measured need is ~230 px, plus margin
    const int RX_LBL_X   = CX1,        RX_LBL_Y = CY_THEME + 110;
    const int RX_SW_X    = CX1 + 130,  RX_SW_Y  = CY_THEME + 106;
#else
    const int W = SCREEN_W, H = SCREEN_H;
    const int CX1 = 14, CY_WIFI = 46, CY_THEME = 358, CY_ROW = 434;
    const int CX2 = 14, CY_AP   = 216;
    const int QR_X = 338, QR_Y = 220, QR_CARD = 128, QR_PX = 112;
    // Unchanged 480-grid values: this is the released 4-inch product, whose
    // fonts were never scaled, so nothing overflows here and nothing may move.
    const int WIFI_LBL_X = CX1 + 372;
    const int LIC_W      = 204;
    const int RX_LBL_X   = CX1 + 326,  RX_LBL_Y = CY_ROW + 10;
    const int RX_SW_X    = CX1 + 406,  RX_SW_Y  = CY_ROW + 6;
#endif

    _root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_root, W, H);
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
    mkButton(_root, LV_SYMBOL_CLOSE, W - 12 - 44, 8, 44, 36, CLR_SURFACE, CLR_TEXT, cbClose);

    // ---- WLAN section ----
    // Section title on the left; the WLAN radio switch sits on the otherwise-
    // empty right side of the title line, so it stays clear of the SSID/password
    // input fields below. A single status line goes underneath.
    // (A Bluetooth switch used to live here — removed: the firmware has no BT
    // code, so it could only reserve memory for nothing.)
    mkLabel(_root, T(STR_CFG_WIFI_SECTION), CX1, CY_WIFI, FONT_MED, CLR_ACCENT);
    mkLabel(_root, T(STR_CFG_WIFI_SHORT), WIFI_LBL_X, CY_WIFI + 2, FONT_SMALL, CLR_TEXT);
    mkSwitch(_root, CX1 + 408, CY_WIFI - 2, appConfig.cfg.wifiEnabled, cbWifiToggle);

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
    mkLabel(_root, st, CX1, CY_WIFI + 24, FONT_SMALL, CLR_TEXT_DIM);

    mkLabel(_root, "SSID", CX1, CY_WIFI + 52, FONT_SMALL, CLR_TEXT_DIM);
    _taSsid = mkField(_root, appConfig.cfg.wifiSsid, T(STR_CFG_NETWORK_NAME), CX1 + 78, CY_WIFI + 42, 376, 38, cbTaClicked);

    mkLabel(_root, T(STR_CFG_PASSWORD_SHORT), CX1, CY_WIFI + 94, FONT_SMALL, CLR_TEXT_DIM);
    _taPass = mkField(_root, appConfig.cfg.wifiPassword, T(STR_CFG_PASSWORD), CX1 + 78, CY_WIFI + 84, 376, 38, cbTaClicked);

    mkButton(_root, T(STR_CFG_CONNECT_REBOOT), CX1, CY_WIFI + 126, 454, 38, CLR_ACCENT, CLR_ON_ACCENT, cbConnect);

    // ---- internal hotspot section (auto-connect QR on the right) ----
    mkLabel(_root, T(STR_CFG_HOTSPOT_SECTION), CX2, CY_AP, FONT_MED, CLR_ACCENT);
    {
        String apS = wifiApSsid();
        String apP = wifiApPassword();
        char nm[64], pw[64];
        snprintf(nm, sizeof(nm), "Name:  %s", apS.c_str());
        snprintf(pw, sizeof(pw), "Pass:  %s", apP.c_str());
        mkLabel(_root, nm, CX2, CY_AP + 26, FONT_SMALL, CLR_TEXT);
        mkLabel(_root, pw, CX2, CY_AP + 48, FONT_SMALL, CLR_TEXT);
        mkLabel(_root, T(STR_CFG_SCAN_QR), CX2, CY_AP + 76, FONT_SMALL, CLR_TEXT_DIM);
        mkButton(_root, T(STR_CFG_HOTSPOT_REBOOT), CX2, CY_AP + 100, 312, 38, CLR_SURFACE, CLR_TEXT, cbHotspot);

#if LV_USE_QRCODE
        // WiFi auto-connect QR: "WIFI:T:WPA;S:<ssid>;P:<pw>;;". Fixed black-on-white
        // on a white card (quiet-zone border) so any phone camera scans it in either
        // theme. Encodes the hotspot's credentials (random per-device password).
        lv_obj_t *qrCard = lv_obj_create(_root);
        lv_obj_set_size(qrCard, QR_CARD, QR_CARD);
        lv_obj_set_pos(qrCard, QR_X, QR_Y);
        // DELIBERATELY fixed: the quiet zone of a QR code must stay white,
        // otherwise no phone camera will recognise it. Also applies in night mode.
        lv_obj_set_style_bg_color(qrCard, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(qrCard, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(qrCard, 0, 0);
        lv_obj_set_style_radius(qrCard, 4, 0);
        lv_obj_set_style_pad_all(qrCard, 0, 0);
        lv_obj_clear_flag(qrCard, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *qr = lv_qrcode_create(qrCard, QR_PX, lv_color_black(), lv_color_white());
        lv_obj_center(qr);
        String w = String("WIFI:T:WPA;S:") + apS + ";P:" + apP + ";;";
        lv_qrcode_update(qr, w.c_str(), w.length());
#endif
    }

    // ---- display (theme) section: Auto / Light / Dark / Night ----
    // Four buttons in the same span the WLAN block uses, starting at CX1:
    // b=107, gap 8, so the row ends at CX1+3*115+107 = CX1+452 (472 on the
    // 1024x600 boards in either orientation, 466 on the 4").
    mkLabel(_root, T(STR_CFG_DISPLAY_SECTION), CX1, CY_THEME, FONT_MED, CLR_ACCENT);
    bool autoT = appConfig.cfg.themeAuto;
    bool light = !autoT && (strcmp(appConfig.cfg.themeActive, "light") == 0);
    bool night = !autoT && (strcmp(appConfig.cfg.themeActive, "night") == 0);
    bool dark  = !autoT && !light && !night;      // fallback, no longer a catch-all
    const int bw = 107, gap = 8;
    mkButton(_root, T(STR_CFG_THEME_AUTO), CX1, CY_THEME + 24, bw, 42,
             autoT ? CLR_ACCENT : CLR_SURFACE, autoT ? CLR_ON_ACCENT : CLR_TEXT, cbThemeAuto);
    mkButton(_root, T(STR_CFG_THEME_LIGHT), CX1 + (bw + gap), CY_THEME + 24, bw, 42,
             light ? CLR_ACCENT : CLR_SURFACE, light ? CLR_ON_ACCENT : CLR_TEXT, cbThemeLight);
    mkButton(_root, T(STR_CFG_THEME_DARK), CX1 + 2 * (bw + gap), CY_THEME + 24, bw, 42,
             dark ? CLR_ACCENT : CLR_SURFACE, dark ? CLR_ON_ACCENT : CLR_TEXT, cbThemeDark);
    mkButton(_root, T(STR_CFG_THEME_NIGHT), CX1 + 3 * (bw + gap), CY_THEME + 24, bw, 42,
             night ? CLR_ACCENT : CLR_SURFACE, night ? CLR_ON_ACCENT : CLR_TEXT, cbThemeNight);

    // ---- language + licences share the last row ----
    // On the 4" board, only 480 px tall, there is no room for a row of their
    // own. DE/EN are the same in both languages, so they need no translation
    // themselves.
    const bool isEn = (i18nLang() == Lang::EN);
    mkButton(_root, "DE", CX1, CY_ROW, 52, 38,
             isEn ? CLR_SURFACE : CLR_ACCENT, isEn ? CLR_TEXT : CLR_ON_ACCENT, cbLangDe);
    mkButton(_root, "EN", CX1 + 56, CY_ROW, 52, 38,
             isEn ? CLR_ACCENT : CLR_SURFACE, isEn ? CLR_ON_ACCENT : CLR_TEXT, cbLangEn);
    mkButton(_root, T(STR_CFG_LICENSES_BTN), CX1 + 114, CY_ROW, LIC_W, 38,
             CLR_SURFACE, CLR_TEXT, cbLicenses);
    // Listen-only: N2km_ListenOnly — the device then sends nothing onto the bus
    // (no address claim, no heartbeat, no media control). For other people's
    // boats, charter, workshop appointments. Takes effect after reboot.
    mkLabel(_root, T(STR_CFG_N2K_LISTEN), RX_LBL_X, RX_LBL_Y, FONT_SMALL, CLR_TEXT);
    mkSwitch(_root, RX_SW_X, RX_SW_Y, appConfig.cfg.n2kListenOnly, cbN2kListenToggle);

    uiDisableLabelScroll(_root);   // see the note in Theme.h
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
#if defined(BOARD_PANEL_1024X600)
        // Full screen width: at 1024x600 the keys grow to real finger size.
        // The LOGICAL width, so a rotated panel gets a 600 px keyboard instead
        // of a 1024 px one that hangs over both edges and loses its outer key
        // columns - backspace and enter among them - to the clip. The 280 px
        // height is fine in portrait too (280 of 1024).
        lv_obj_set_size(_kb, uiScreenW(), 280);
#else
        lv_obj_set_size(_kb, SCREEN_W, 200);
#endif
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

