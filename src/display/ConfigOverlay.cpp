#include "ConfigOverlay.h"
#include "Icons.h"   // OpenBridge icon trial
#if defined(BOARD_PANEL_1024X600)
#include "../BoardConfig.h"   // board macros (the geometry comes from Theme.h)
#endif
#include "LicenseOverlay.h"
#include "Theme.h"
#include "DisplayManager.h"
#include "../config/Config.h"
#include "../WifiNaming.h"
#include <Arduino.h>

#include "../config/WifiStatus.h"
#ifndef SIMULATOR
#include <WiFi.h>
#include "../config/WebConfig.h"   // fallback state + last disconnect reason
#endif
#ifndef WL_CONNECTED
#define WL_CONNECTED 3
#endif

ConfigOverlay configOverlay;

static bool fitsLine(const char *s, const lv_font_t *font, lv_coord_t maxW) {
    return lv_txt_get_width(s, strlen(s), font, 0, LV_TEXT_FLAG_NONE) <= maxW;
}

// The WiFi status line: "<state>: http://<address>"; in the fallback hotspot
// "WLAN: <why> (<code>) – Ersatz-Hotspot: http://<address>".
//
// With "http://" because that is what has to go into the browser: a bare
// 192.168.4.1 typed into a phone often lands in the search field, and the
// address was the one thing on this line people had to copy by hand.
//
// The fallback line names its two subjects - the network that was not joined,
// and the hotspot that stands in. With the hotspot first and the reason
// behind its address, "nicht gefunden" read as if the HOTSPOT had not been
// found.
//
// The reason is the point of that line, and it is also what does not always
// fit: the 4" has ~450 px of 14 px text here, the 1024x600 boards ~490 px of
// 18 px in landscape, and the line has nowhere to wrap to - the label is one
// line box tall by construction, and the SSID row starts just under that box
// (WIFI_ROW_Y in open()). So the variants are tried longest first and the
// first one that fits wins: everything; then "Hotspot" for "Ersatz-Hotspot";
// then without the code - the words are what a user can act on, the number
// is for a forum answer; then the code alone; then the address alone.
// Measured with the label's own font, so neither a language nor a board
// needs a rule of its own. With the default fonts every reason keeps its
// words on every board, in both languages.
static void wifiStatusLine(char *out, size_t n, const char *state, const char *ip,
                           uint8_t reason, int8_t seen, const lv_font_t *font,
                           lv_coord_t maxW) {
    const bool withReason = reason || seen == 0;
    if (withReason) {
        char code[8] = "";
        if (reason) snprintf(code, sizeof code, " (%u)", (unsigned)reason);
        const char *wl  = T(STR_CFG_WIFI_SHORT);
        const char *why = WifiReason::text(reason, seen);
        const char *hs  = T(STR_CFG_WIFI_HOTSPOT);
        snprintf(out, n, "%s: %s%s – %s: http://%s", wl, why, code, state, ip);
        if (fitsLine(out, font, maxW)) return;
        snprintf(out, n, "%s: %s%s – %s: http://%s", wl, why, code, hs, ip);
        if (fitsLine(out, font, maxW)) return;
        snprintf(out, n, "%s: %s – %s: http://%s", wl, why, hs, ip);
        if (fitsLine(out, font, maxW)) return;
        if (code[0]) {
            snprintf(out, n, "%s%s – %s: http://%s", wl, code, state, ip);
            if (fitsLine(out, font, maxW)) return;
        }
    }
    snprintf(out, n, "%s: http://%s", state, ip);
}

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

#if LV_USE_QRCODE
// The white card a QR code sits on. DELIBERATELY fixed: the quiet zone of a QR
// code must stay white, otherwise no phone camera will recognise it. Also
// applies in night mode.
static lv_obj_t *mkQrCard(lv_obj_t *parent, int x, int y, int size) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, size, size);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// The hotspot login: "WIFI:T:WPA;S:<ssid>;P:<pw>;;", which a phone camera
// offers to join. A heap String for a moment, as it always was.
static void showWifiQr(lv_obj_t *qr) {
    String w = String("WIFI:T:WPA;S:") + wifiApSsid() + ";P:" + wifiApPassword() + ";;";
    lv_qrcode_update(qr, w.c_str(), w.length());
}

static lv_coord_t textW(const char *txt, const lv_font_t *font) {
    return lv_txt_get_width(txt, strlen(txt), font, 0, LV_TEXT_FLAG_NONE);
}

// ---- QR captions (1024x600) and the 4" QR hint ----
// The longest-first rule of wifiStatusLine(), for one line of text: in
// FONT_SMALL, else in FONT_TINY - when the theme's tiny font really is the
// smaller one (the WebUI can make the two roles the same size) - else not at
// all. Returns the width, or -1 for "none".
static lv_coord_t fitCaption(const char *txt, lv_coord_t slotW, const lv_font_t **font) {
    if (fitsLine(txt, FONT_SMALL, slotW)) {
        *font = FONT_SMALL;
    } else if (lv_font_get_line_height(FONT_TINY) < lv_font_get_line_height(FONT_SMALL) &&
               fitsLine(txt, FONT_TINY, slotW)) {
        *font = FONT_TINY;
    } else {
        return -1;
    }
    return textW(txt, *font);
}

#if defined(BOARD_PANEL_1024X600)
// Centred under its card, then pulled into [minX, maxX - w], so a caption
// wider than the card neither leaves its column nor runs into its neighbour.
static int captionX(int cardX, int cardW, lv_coord_t w, int minX, int maxX) {
    int x = cardX + (cardW - w) / 2;
    if (x > maxX - w) x = maxX - w;
    if (x < minX) x = minX;
    return x;
}
#else
// 4": room for the hint above the hotspot button - from its x (CX2 = 14) to
// 4 px before the QR card at 338, as in open().
static const lv_coord_t QR4_HINT_W = 338 - 14 - 4;
#endif
#endif   // LV_USE_QRCODE

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
    // Two QR cards: the hotspot login (QR1) and the web interface's address
    // (QR2, only when there is one - see the QR block below).
    // Landscape: the pair is centred in the right column (534..1004), 56 px
    // apart, both under the hotspot button (ends at y 184); the captions share
    // one line under them. The keyboard (y 320..600) covers them while an SSID
    // is being typed, as it always covered the one card here.
    // Portrait: QR1 stays beside the hotspot text (20 + 312 button width + 20
    // = 352, ending at 544 of 600), lifted clear of the keyboard, which covers
    // the bottom 280 px from y=744 while an SSID is being typed. QR2 is stacked
    // under it and goes behind the keyboard while typing. Caption 1 sits 4 px
    // under its card and QR2 27 px under that caption, so the caption reads as
    // QR1's and not as a heading of QR2.
    const int QR_X  = port ? 352 : 549, QR_Y  = port ? 548 : 230;
    const int QR2_X = port ? 352 : 797, QR2_Y = port ? 792 : 230;
    // QR1 166 px, not the 176 of before: the same v4 code at 5 px per module
    // (33 x 5 = 165), and lv_obj_center() puts it at card+13 exactly as before
    // (96 - 166/2 = 13; 176 was centred at 8 plus lv_qrcode's own 5 px margin),
    // so the image is pixel-identical and its canvas 560 B smaller in the LVGL
    // pool. 165 would centre at 14 and move the code by a pixel. QR2: any IPv4
    // URL is a v2 code (25 modules), 5 px per module = 125.
    const int QR_CARD = 192, QR_PX = 166, QR_URL_PX = 125;
    const int QR_CAP_DY = port ? QR_CARD + 4 : QR_CARD + 8;   // caption under its card
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
    // "Accept updates over NMEA 2000" shares that row, right-aligned with the
    // WiFi switch above.
    const int UPD_LBL_X  = CX1 + 200,  UPD_LBL_Y = RX_LBL_Y;
    const int UPD_SW_X   = CX1 + 408,  UPD_SW_Y  = RX_SW_Y;
    // The WiFi input rows start under the status line's REAL line box. The
    // 480-grid offsets (+24 status, +42 first field) were cut for the 4"'s
    // 16 px line, which ends at y 85 and leaves two free rows above the SSID
    // field at 88. FONT_SMALL is 21 px here, so the descenders of "http://"
    // reached y 90 - three rows into the field's border and padding, in both
    // orientations. Derived from the font instead of shifted by a fixed amount,
    // so a FONT_SMALL raised in the WebUI pushes the rows down rather than back
    // into the line. Default fonts: 70 + 21 + UI_S(2) = 94, three free rows;
    // the connect button then ends at y 215, 64 px above the display section
    // and clear of the keyboard (top at y 320 in landscape, 744 in portrait).
    const int WIFI_ST_Y  = CY_WIFI + 24;
    const int WIFI_ROW_Y = WIFI_ST_Y + lv_font_get_line_height(FONT_SMALL) + UI_S(2);
#else
    const int W = SCREEN_W, H = SCREEN_H;
    const int CX1 = 14, CY_WIFI = 46, CY_THEME = 358, CY_ROW = 434;
    const int CX2 = 14, CY_AP   = 216;
    const int QR_X = 338, QR_Y = 220, QR_CARD = 128, QR_PX = 112;
    // Unchanged 480-grid values: this is the released 4-inch product, whose
    // fonts were never scaled, so nothing overflows here and nothing may move.
    // One exception: the 36 px up to the switch were too few for the German
    // "WLAN" (48 px), whose N lay under it. 54 px leave it 6 clear.
    const int WIFI_LBL_X = CX1 + 354;
    const int LIC_W      = 204;
    const int RX_LBL_X   = CX1 + 326,  RX_LBL_Y = CY_ROW + 10;
    const int RX_SW_X    = CX1 + 406,  RX_SW_Y  = CY_ROW + 6;
    // "Accept updates over NMEA 2000": the free right half of the theme
    // section's title line - between the hotspot button (ends at y 354) and
    // the theme buttons (start at y 382), right-aligned with the switches.
    const int UPD_LBL_X  = CX1 + 200,  UPD_LBL_Y = CY_THEME + 2;
    const int UPD_SW_X   = CX1 + 408,  UPD_SW_Y  = CY_THEME - 2;
    // The released WiFi rows, kept fixed: the 16 px status line at y 70 ends
    // at 85 and the SSID field follows at 88. The formula above would also
    // give 88 with the default font, but a FONT_SMALL raised in the WebUI
    // would push the connect button (6 px above the hotspot title at 216)
    // into that title - and the released 4-inch must not move.
    const int WIFI_ST_Y  = CY_WIFI + 24;
    const int WIFI_ROW_Y = CY_WIFI + 42;
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
    mkButton(_root, npSym(NP_ICON_CLOSE), W - 12 - 44, 8, 44, 36, CLR_SURFACE, CLR_TEXT, cbClose);   // OpenBridge icon trial

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
    // configured network fails, WebConfig::connect() itself switches over to
    // the hotspot - but cfg.apMode then still stays false. Previously the line
    // reported "Nicht verbunden" (not connected) in exactly this case, even
    // though the device was very much reachable at 192.168.4.1.
    //
    // Width it may take: the whole row on the 4" and in portrait, up to the
    // hotspot column in landscape on 1024x600.
#if defined(BOARD_PANEL_1024X600)
    const lv_coord_t stMaxW = port ? (W - 2 * CX1) : (CX2 - CX1 - 20);
#else
    const lv_coord_t stMaxW = W - 2 * CX1;
#endif
    // The address the line names is captured once, for the QR code of the web
    // interface below: ip stays "" where the line names none (WiFi off, link
    // down or reconnecting), and then there is no such QR code. hotspotUp: the
    // own hotspot runs (hotspot mode or the fallback), so its two QR codes are
    // two steps - join, then open.
    // Known edge: after a background join the hotspot lingers while a phone
    // is still on it (WebConfig::fallbackJoined()). The first branch then
    // names the network's address, which that phone cannot reach - but that
    // phone already has the web interface open.
    char st[160];
    char ip[16] = "";
    bool hotspotUp = false;
    if (!appConfig.cfg.wifiEnabled) {
        snprintf(st, sizeof(st), "%s", T(STR_CFG_WIFI_OFF));
    } else {
#ifndef SIMULATOR
        const wifi_mode_t m = WiFi.getMode();
        // Associated but no DHCP lease reads as NOT connected: reporting
        // "connected: 0.0.0.0" is what made a dead link look healthy here.
        if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
            strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));
            wifiStatusLine(st, sizeof(st), T(STR_CFG_WIFI_CONNECTED),
                           ip, 0, -1, FONT_SMALL, stMaxW);
        } else if ((m == WIFI_MODE_AP || m == WIFI_MODE_APSTA) && webCfg.fallbackActive()) {
            // The network was not joined; the hotspot stands in while it is
            // retried. Say why - the reason used to be in the serial log only.
            strlcpy(ip, WiFi.softAPIP().toString().c_str(), sizeof(ip));
            hotspotUp = true;
            wifiStatusLine(st, sizeof(st), T(STR_CFG_WIFI_AP_FALLBACK),
                           ip, WebConfig::outageReason(),
                           webCfg.bootScanSeen(), FONT_SMALL, stMaxW);
        } else if (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA) {
            strlcpy(ip, WiFi.softAPIP().toString().c_str(), sizeof(ip));
            hotspotUp = true;
            wifiStatusLine(st, sizeof(st), T(STR_CFG_WIFI_AP_ACTIVE),
                           ip, 0, -1, FONT_SMALL, stMaxW);
        } else if (WebConfig::outageReason())
            // Station mode, link lost, the supervisor is reconnecting: why, in
            // the same words as the boot screen. Only a reason of THIS outage -
            // one left over from before the link last came up (a failed boot
            // attempt hours ago, say) would name a cause that is long gone.
            // Fits on every board ("Nicht verbunden: Passwort falsch? (202)"
            // is 363 px at 18 px).
            snprintf(st, sizeof(st), "%s: %s (%u)", T(STR_CFG_WIFI_NOT_CONN),
                     WifiReason::text(WebConfig::outageReason(), -1),
                     (unsigned)WebConfig::outageReason());
        else
            snprintf(st, sizeof(st), "%s", T(STR_CFG_WIFI_NOT_CONN));
#else
        // No radio on the PC: show what the configuration requests.
        hotspotUp = appConfig.cfg.apMode;
        strlcpy(ip, (hotspotUp ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str(), sizeof(ip));
        wifiStatusLine(st, sizeof(st), T(hotspotUp ? STR_CFG_WIFI_AP_ACTIVE : STR_CFG_WIFI_CONNECTED),
                       ip, 0, -1, FONT_SMALL, stMaxW);
#endif
    }
    mkLabel(_root, st, CX1, WIFI_ST_Y, FONT_SMALL, CLR_TEXT_DIM);

    // Everything below hangs off WIFI_ROW_Y with the original spacing: each
    // label 10 px below its field's top, rows 42 px apart.
    mkLabel(_root, "SSID", CX1, WIFI_ROW_Y + 10, FONT_SMALL, CLR_TEXT_DIM);
    _taSsid = mkField(_root, appConfig.cfg.wifiSsid, T(STR_CFG_NETWORK_NAME), CX1 + 78, WIFI_ROW_Y, 376, 38, cbTaClicked);

    mkLabel(_root, T(STR_CFG_PASSWORD_SHORT), CX1, WIFI_ROW_Y + 52, FONT_SMALL, CLR_TEXT_DIM);
    _taPass = mkField(_root, appConfig.cfg.wifiPassword, T(STR_CFG_PASSWORD), CX1 + 78, WIFI_ROW_Y + 42, 376, 38, cbTaClicked);

    mkButton(_root, T(STR_CFG_CONNECT_REBOOT), CX1, WIFI_ROW_Y + 84, 454, 38, CLR_ACCENT, CLR_ON_ACCENT, cbConnect);

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
        lv_obj_t *hint = mkLabel(_root, T(STR_CFG_SCAN_QR), CX2, CY_AP + 76, FONT_SMALL, CLR_TEXT_DIM);
        (void)hint;   // the 4" retexts it for its QR toggle
        mkButton(_root, T(STR_CFG_HOTSPOT_REBOOT), CX2, CY_AP + 100, 312, 38, CLR_SURFACE, CLR_TEXT, cbHotspot);

#if LV_USE_QRCODE
        // Fixed black-on-white on a white card (quiet-zone border) so any phone
        // camera scans it in either theme. QR1 always carries the hotspot's
        // credentials (random per-device password); the web interface's
        // address, where there is one, is the second code.
        //
        // Built here once and never refreshed, like the status line: a fallback
        // hotspot that goes off while this overlay is open (the background
        // retry joined the network) leaves the numbered captions and the
        // hotspot address standing until the overlay is opened again. Accepted -
        // the status line has always been stale in the same way.
        char url[24] = "";   // "http://255.255.255.255" is 22 bytes
        if (ip[0]) snprintf(url, sizeof(url), "http://%s", ip);
        lv_obj_t *qrCard = mkQrCard(_root, QR_X, QR_Y, QR_CARD);
        lv_obj_t *qr = lv_qrcode_create(qrCard, QR_PX, lv_color_black(), lv_color_white());
        lv_obj_center(qr);
#if defined(BOARD_PANEL_1024X600)
        showWifiQr(qr);
        // ---- captions ----
        // Hotspot up: two steps, "1. Hotspot beitreten" / "2. Weboberfläche
        // öffnen". Station mode with an address: what each code is, and that
        // the phone has to be on the same network. No address: QR1's caption
        // only. Numbered both or neither, and only when both numbered texts
        // fit their slots in FONT_SMALL; otherwise each caption takes the first
        // of plain FONT_SMALL, plain FONT_TINY, nothing that fits.
        // Slots: portrait has one caption per line, over the whole width
        // (20..580; the area left of the cards is empty below the hotspot
        // button). Landscape shares one line: caption 1 owns the column up to
        // 8 px before the middle of the gap between the cards - its texts are
        // short in every state and language - and caption 2 gets the rest, to
        // W - 20, from 16 px after caption 1's real right edge but never from
        // further left than 8 px past QR1's card. A long caption 2 is centred
        // on its card and pulled left at most into the gap, so it cannot start
        // under QR1 and read as the tail of caption 1.
        // The shared line also shares a size: when one caption only fits in
        // FONT_TINY, the other is set in FONT_TINY too - an 18 px and a 14 px
        // caption side by side, baselines apart, looked like a mistake. With
        // the default fonts that is station mode in both languages ("Web
        // interface (same network)" is 275 px at 18 px, the slot 255).
        const int capMaxX   = W - 20;
        const int gapMid    = QR_X + QR_CARD + (QR2_X - QR_X - QR_CARD) / 2;
        const int cap1Max   = port ? capMaxX : gapMid - 8;
        const int cap2Floor = port ? CX2 : QR_X + QR_CARD + 8;
        auto cap2Min = [&](int right1) { return port ? cap2Floor : LV_MAX(cap2Floor, right1 + 16); };
        const char *t1 = T(hotspotUp ? STR_CFG_QR_JOIN : STR_CFG_QR_AP);
        const char *t2 = T(hotspotUp ? STR_CFG_QR_WEB : STR_CFG_QR_WEB_LAN);
        char c1[64], c2[64];
        bool numbered = false;
        if (hotspotUp && url[0]) {
            snprintf(c1, sizeof(c1), "1. %s", t1);
            snprintf(c2, sizeof(c2), "2. %s", t2);
            if (fitsLine(c1, FONT_SMALL, cap1Max - CX2)) {
                const lv_coord_t w1 = textW(c1, FONT_SMALL);
                const int right1 = captionX(QR_X, QR_CARD, w1, CX2, cap1Max) + w1;
                numbered = fitsLine(c2, FONT_SMALL, capMaxX - cap2Min(right1));
            }
        }
        if (!numbered) {
            strlcpy(c1, t1, sizeof(c1));
            strlcpy(c2, t2, sizeof(c2));
        }
        const lv_font_t *f1 = FONT_SMALL, *f2 = FONT_SMALL;
        lv_coord_t w1 = fitCaption(c1, cap1Max - CX2, &f1), w2 = -1;
        int x1   = (w1 >= 0) ? captionX(QR_X, QR_CARD, w1, CX2, cap1Max) : 0;
        int min2 = cap2Min((w1 >= 0) ? x1 + w1 : -1);
        if (url[0]) {
            w2 = fitCaption(c2, capMaxX - min2, &f2);
            if (!port && w1 >= 0 && w2 >= 0 && f1 != f2) {
                // fitCaption() picks one of two fonts, so the other one is the
                // tiny one. Both texts only get narrower, caption 1 moves no
                // further right, and caption 2's slot only grows: both fit.
                f1 = f2 = FONT_TINY;
                w1   = textW(c1, FONT_TINY);
                x1   = captionX(QR_X, QR_CARD, w1, CX2, cap1Max);
                min2 = cap2Min(x1 + w1);
                w2   = textW(c2, FONT_TINY);
            }
        }
        if (w1 >= 0) mkLabel(_root, c1, x1, QR_Y + QR_CAP_DY, f1, CLR_TEXT_DIM);
        // QR2: the web interface's address - the one the status line names.
        if (url[0]) {
            lv_obj_t *qr2 = lv_qrcode_create(mkQrCard(_root, QR2_X, QR2_Y, QR_CARD),
                                             QR_URL_PX, lv_color_black(), lv_color_white());
            lv_obj_center(qr2);
            lv_qrcode_update(qr2, url, strlen(url));
            if (w2 >= 0)
                mkLabel(_root, c2, captionX(QR2_X, QR_CARD, w2, min2, capMaxX),
                        QR2_Y + QR_CAP_DY, f2, CLR_TEXT_DIM);
        }
#else
        // 4": the released layout has no room for a second card, so the one
        // card switches between the two codes on a tap. Without an address it
        // is exactly the screen it always was: hotspot code, no tap handler.
        if (url[0]) {
            _qr = qr;
            _qrHint = hint;
            strlcpy(_url, url, sizeof(_url));
            // A real tap only: cbQrToggle() ignores a release off the card or
            // after a swipe. The card KEEPS LVGL's default PRESS_LOCK. Without
            // it a finger sliding off the card hands the press to whatever it
            // reaches, and that object clicks on release - the connect and the
            // hotspot buttons (both save and restart) and the N2K-updates
            // switch sit 8-12 px from the card, less than a swipe moves
            // between two touch reads (30 ms), so the gap does not catch it.
            lv_obj_add_event_cb(qrCard, cbQrToggle, LV_EVENT_CLICKED, nullptr);
            // Station mode: the hotspot is not running, so the code that works
            // - the address - comes first.
            showQr4(!hotspotUp);
        } else {
            showWifiQr(qr);
        }
#endif
#else
        (void)hotspotUp;
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
    // Accept updates over NMEA 2000 (N2K-UPDATE.md 7.9). Live, no restart: the
    // receiver reads the flag on every OFFER and every tick. Dimmed in
    // listen-only mode, where no update can reach the display anyway.
    mkLabel(_root, T(STR_CFG_N2K_UPDATES), UPD_LBL_X, UPD_LBL_Y, FONT_SMALL,
            appConfig.cfg.n2kListenOnly ? CLR_TEXT_DIM : CLR_TEXT);
    mkSwitch(_root, UPD_SW_X, UPD_SW_Y, appConfig.cfg.n2kAcceptUpdates, cbN2kUpdatesToggle);

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
#if !defined(BOARD_PANEL_1024X600)
    _qr = _qrHint = nullptr;              // children of _root as well
    _qrIsUrl = false;
    _url[0] = 0;
#endif
}

#if !defined(BOARD_PANEL_1024X600)
// 4": show the web interface's address (url) or the hotspot login on the one
// QR canvas - lv_qrcode_update() clears and invalidates it first - and say
// in the hint what is shown and what a tap brings, where that text fits the
// room beside the card: in FONT_SMALL, else in a smaller FONT_TINY, as the
// 1024x600 captions do (a FONT_SMALL raised to 16 px in the WebUI makes both
// German hints 325 px). Otherwise the hotspot code gets the old "scan to
// connect" in FONT_SMALL and the address code keeps the text that was there.
void ConfigOverlay::showQr4(bool url) {
    if (!_qr) return;
#if LV_USE_QRCODE
    _qrIsUrl = url;
    if (url) lv_qrcode_update(_qr, _url, strlen(_url));
    else     showWifiQr(_qr);
    if (!_qrHint) return;
    const char *h = T(url ? STR_CFG_QR_TAP_HOTSPOT : STR_CFG_QR_TAP_WEB);
    const lv_font_t *f = FONT_SMALL;
    if (fitCaption(h, QR4_HINT_W, &f) >= 0) {
        lv_obj_set_style_text_font(_qrHint, f, 0);
        lv_label_set_text(_qrHint, h);
    } else if (!url) {
        lv_obj_set_style_text_font(_qrHint, FONT_SMALL, 0);
        lv_label_set_text(_qrHint, T(STR_CFG_SCAN_QR));
    }
#endif
}

// CLICKED comes on release wherever the finger is by then - the card keeps
// its PRESS_LOCK (see open()) - so only a release on the card, after no
// recognised swipe, switches the code. A swipe that starts on the card does
// nothing, whether it ends on the card or beside it.
void ConfigOverlay::cbQrToggle(lv_event_t *e) {
    ConfigOverlay &s = configOverlay;
    if (!s._open || !s._qr) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) {
        if (lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) return;
        lv_point_t p;
        lv_area_t a;
        lv_indev_get_point(indev, &p);   // the last pressed point, logical
        lv_obj_get_coords(lv_event_get_current_target(e), &a);
        if (p.x < a.x1 || p.x > a.x2 || p.y < a.y1 || p.y > a.y2) return;
    }
    s.showQr4(!s._qrIsUrl);
}
#endif

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
    // Close first, then show the licences - one tick later, not here. close()
    // only queues the deletion of this overlay (lv_obj_del_async, run by the
    // same lv_timer_handler() pass that delivers this event), so opening the
    // licences right here built their whole tree while this one was still in
    // the LVGL pool: both at once, the worst moment of the pool. Deferred, the
    // licences are built by DisplayManager::update(), after this tree is gone,
    // on the very next loop pass: requestUpdate() forces it, as
    // requestOpenConfig() does, instead of waiting up to 100 ms for the next
    // regular one. Costs at most one frame without either overlay.
    configOverlay.close();
    licenseOverlay.requestOpen();
    dispMgr.requestUpdate();
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

void ConfigOverlay::cbN2kUpdatesToggle(lv_event_t *e) {
    // No restart and no close: OFF takes effect on the next OFFER, and ends a
    // running session with ABORT(DISABLED).
    appConfig.cfg.n2kAcceptUpdates = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    appConfig.save();
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

