// ============================================================================
// UpdateOverlay.cpp - see UpdateOverlay.h and docs/N2K-UPDATE.md section 7.
// ============================================================================
#include "UpdateOverlay.h"
#include "DisplayManager.h"
#include "Theme.h"
#include "../i18n/I18n.h"
#include "../config/Config.h"
#include "../BoardConfig.h"
#include "../Version.h"
#include "../DisplaySetup.h"
#include "../n2kupdate/UpdUi.h"
#include "../n2kupdate/N2kUpdateProto.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifndef SIMULATOR
#include <esp_mac.h>
#endif

UpdateOverlay updateOverlay;

using namespace n2kupd;

namespace {

// ---- geometry -------------------------------------------------------------------------
// The banners sit on the instrument, like the demo and alarm banners: the
// 600-px middle column on the 1024x600 boards (on top in portrait, below the
// rail), the whole width on the 4-inch.
lv_coord_t colX() {
#if defined(BOARD_PANEL_1024X600)
    return uiPortrait() ? 0 : UI7_CONTENT_X;
#else
    return 0;
#endif
}
lv_coord_t colY() {
#if defined(BOARD_PANEL_1024X600)
    return uiPortrait() ? UI7_RAIL_W : UI7_CONTENT_Y;
#else
    return 0;
#endif
}
lv_coord_t colW() {
#if defined(BOARD_PANEL_1024X600)
    return UI7_CENTER_W;
#else
    return uiScreenW();
#endif
}
constexpr lv_coord_t ALARM_BANNER_H = 30;   // DisplayManager's alarm banner

lv_coord_t cardWidth(lv_coord_t maxW) {
    const lv_coord_t avail = uiScreenW() - 2 * UI_S(10);
    return avail < maxW ? avail : maxW;
}

// ---- text helpers ---------------------------------------------------------------------
// The protocol's board ids (4.1), as people know the boards.
const char *boardName(uint8_t id) {
    switch (id) {
        case BOARD_4INCH: return "Waveshare 4\" 480×480";
        case BOARD_7B:    return "Waveshare 7B 1024×600";
        case BOARD_5B:    return "Waveshare 5B 1024×600";
        default:          return "?";
    }
}

// "2.4 MB" in every language - numbers always take a decimal point, the
// German panel included (owner's decision): tenths, or hundredths below
// 0.1 MB (a web UI is about 0.05 MB). Decimal megabytes, the unit a file
// manager shows.
void fmtMb(char *out, size_t n, uint32_t bytes) {
    if (bytes < 100000UL) {
        const unsigned v = (unsigned)((bytes + 5000UL) / 10000UL);
        snprintf(out, n, "%u.%02u MB", v / 100, v % 100);
    } else {
        const unsigned v = (unsigned)((bytes + 50000UL) / 100000UL);
        snprintf(out, n, "%u.%u MB", v / 10, v % 10);
    }
}

// snprintf onto the end of what `buf` holds; returns the new length, which
// never passes the end of the buffer.
size_t appendf(char *buf, size_t cap, size_t n, const char *fmt, ...) {
    if (n + 1 >= cap) return n;
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (k < 0) return n;
    return (n + (size_t)k < cap) ? n + (size_t)k : cap - 1;
}

void version17(char out[17], const uint8_t f[16]) {
    memcpy(out, f, 16);
    out[16] = 0;
    if (!out[0]) strcpy(out, "?");
}

// Table 4.8. An unknown code is shown by number.
const char *reasonText(uint8_t r, char *buf, size_t n) {
    switch (r) {
        case RSN_DECLINED_BY_USER:     return T(STR_RSN_DECLINED_BY_USER);
        case RSN_CONFIRM_TIMEOUT:      return T(STR_RSN_CONFIRM_TIMEOUT);
        case RSN_DISABLED:             return T(STR_RSN_DISABLED);
        case RSN_BUSY:                 return T(STR_RSN_BUSY);
        case RSN_WRONG_BOARD:          return T(STR_RSN_WRONG_BOARD);
        case RSN_TOO_LARGE:            return T(STR_RSN_TOO_LARGE);
        case RSN_UNSUPPORTED_TARGET:
        case RSN_UNSUPPORTED_ENCODING:
        case RSN_UNSUPPORTED_PROTOCOL: return T(STR_RSN_UNSUPPORTED);
        case RSN_BAD_REQUEST:          return T(STR_RSN_BAD_REQUEST);
        case RSN_PENDING_VERIFY:       return T(STR_RSN_PENDING_VERIFY);
        case RSN_ALARM_ACTIVE:         return T(STR_RSN_ALARM_ACTIVE);
        case RSN_VERSION_MISMATCH:     return T(STR_RSN_VERSION_MISMATCH);
        case RSN_NO_SPACE:             return T(STR_RSN_NO_SPACE);
        case RSN_OUT_OF_MEMORY:        return T(STR_RSN_OUT_OF_MEMORY);
        case RSN_DESCRIPTOR_INVALID:   return T(STR_RSN_DESCRIPTOR_INVALID);
        case RSN_SHA_MISMATCH:         return T(STR_RSN_SHA_MISMATCH);
        case RSN_IMAGE_INVALID:        return T(STR_RSN_IMAGE_INVALID);
        case RSN_SIZE_MISMATCH:        return T(STR_RSN_SIZE_MISMATCH);
        case RSN_FLASH_ERROR:          return T(STR_RSN_FLASH_ERROR);
        case RSN_TIMEOUT:              return T(STR_RSN_TIMEOUT);
        case RSN_USER_CANCELLED:       return T(STR_RSN_USER_CANCELLED);
        case RSN_SENDER_CANCELLED:     return T(STR_RSN_SENDER_CANCELLED);
        case RSN_STREAM_CRC_MISMATCH:  return T(STR_RSN_STREAM_CRC_MISMATCH);
        case RSN_LINK_UNRELIABLE:      return T(STR_RSN_LINK_UNRELIABLE);
        case RSN_APPLY_FAILED:         return T(STR_RSN_APPLY_FAILED);
        case RSN_RESTART_PENDING:      return T(STR_RSN_RESTART_PENDING);
        case RSN_NOT_READY:            return T(STR_RSN_NOT_READY);
        case RSN_INTERNAL_ERROR:       return T(STR_RSN_INTERNAL_ERROR);
        default:
            snprintf(buf, n, "%s 0x%02X", T(STR_UPD_ERROR), (unsigned)r);
            return buf;
    }
}

// ---- object helpers ---------------------------------------------------------------------
lv_obj_t *plainObj(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// A full-screen modal root: opaque, so DisplayManager can stop painting the
// instrument behind it, and clickable, so no touch reaches anything below.
lv_obj_t *modalRoot() {
    lv_obj_t *r = plainObj(lv_layer_top());
    lv_obj_set_size(r, uiScreenW(), uiScreenH());
    lv_obj_set_pos(r, 0, 0);
    lv_obj_set_style_bg_color(r, CLR_BG, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

lv_obj_t *card(lv_obj_t *parent, lv_coord_t w, lv_color_t border) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_width(c, w);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, CLR_SURFACE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, border, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, UI_S(12), 0);
    lv_obj_set_style_pad_row(c, UI_S(5), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t col,
                bool fullWidth = true) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    if (fullWidth) {
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    return l;
}

lv_obj_t *tapButton(lv_obj_t *parent, const char *txt, lv_coord_t w, lv_coord_t h,
                    bool primary, lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, primary ? CLR_ACCENT : CLR_BG, 0);
    lv_obj_set_style_bg_color(b, CLR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, CLR_BORDER, 0);
    lv_obj_set_style_border_width(b, primary ? 0 : 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, FONT_MED, 0);
    lv_obj_set_style_text_color(l, primary ? CLR_ON_ACCENT : CLR_TEXT, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

lv_obj_t *buttonRow(lv_obj_t *parent) {
    lv_obj_t *row = plainObj(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_top(row, UI_S(6), 0);
    lv_obj_set_style_pad_column(row, UI_S(10), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    return row;
}

}  // namespace

// ---- press and hold (7.1) ---------------------------------------------------------------
// "Install" and "Cancel" act only after a 1.5 s hold. A bar inside the button
// fills while it is held; releasing early, or sliding off, resets it. Spray on
// a capacitive panel produces short phantom touches - none of them can last
// 1.5 s at one spot.
void UpdateOverlay::updateHoldFill(Hold &h, uint32_t now) {
    if (!h.active || h.fired || !h.btn || !h.fill) return;
    const uint32_t e = now - h.start;
    const lv_coord_t w = lv_obj_get_width(h.btn);
    if (e >= T_HOLD_MS) {
        lv_obj_set_width(h.fill, w);
        h.fired = true;
        h.active = false;
        return;
    }
    lv_obj_set_width(h.fill, (lv_coord_t)((uint32_t)w * e / T_HOLD_MS));
}

void UpdateOverlay::cbHold(lv_event_t *e) {
    Hold *h = (Hold *)lv_event_get_user_data(e);
    if (!h) return;
    const lv_event_code_t code = lv_event_get_code(e);
    const uint32_t now = millis();
    if (code == LV_EVENT_PRESSED) {
        // A hold has to BEGIN after the guard; a press that started earlier
        // counts for nothing until the finger is lifted.
        if ((int32_t)(now - h->armAt) < 0) { h->active = false; return; }
        h->active = true;
        h->fired = false;
        h->start = now;
        updateHoldFill(*h, now);
    } else if (code == LV_EVENT_PRESSING) {
        updateHoldFill(*h, now);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        h->active = false;
        if (!h->fired && h->fill) lv_obj_set_width(h->fill, 0);
    }
}

lv_obj_t *UpdateOverlay::makeHoldButton(lv_obj_t *parent, const char *text, Hold &h,
                                        lv_coord_t w, lv_coord_t hgt, bool primary,
                                        const lv_font_t *font) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, hgt);
    const lv_color_t bg = primary ? CLR_ACCENT : CLR_BG;
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_color(b, bg, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, primary ? CLR_ACCENT : CLR_BORDER, 0);
    lv_obj_set_style_border_width(b, primary ? 0 : 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_clip_corner(b, true, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    // The fill: under the label, not clickable, so every touch lands on the button.
    lv_obj_t *f = plainObj(b);
    lv_obj_set_size(f, 0, lv_pct(100));
    lv_obj_align(f, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(f, primary ? CLR_ON_ACCENT : CLR_ACCENT, 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_40, 0);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font ? font : FONT_MED, 0);
    lv_obj_set_style_text_color(l, primary ? CLR_ON_ACCENT : CLR_TEXT, 0);
    lv_obj_center(l);
    h.btn = b;
    h.fill = f;
    h.active = h.fired = false;
    lv_obj_add_event_cb(b, cbHold, LV_EVENT_PRESSED, &h);
    lv_obj_add_event_cb(b, cbHold, LV_EVENT_PRESSING, &h);
    lv_obj_add_event_cb(b, cbHold, LV_EVENT_RELEASED, &h);
    lv_obj_add_event_cb(b, cbHold, LV_EVENT_PRESS_LOST, &h);
    return b;
}

// ---- button callbacks: flags only; tick() acts outside the event handler ------------------
void UpdateOverlay::cbDecline(lv_event_t *)     { updateOverlay._declinePending = true; }
void UpdateOverlay::cbRestartNow(lv_event_t *)  { updateOverlay._restartNow = true; }
void UpdateOverlay::cbLater(lv_event_t *)       { updateOverlay._later = true; }
void UpdateOverlay::cbBoxOk(lv_event_t *)       { updateOverlay._boxOkPending = true; }
void UpdateOverlay::cbIdentifyTap(lv_event_t *) { updateOverlay._identifyTapped = true; }

// ---- 7.1 backlight ------------------------------------------------------------------------
void UpdateOverlay::wakeBacklight() {
    if (_woke || appConfig.cfg.brightness >= 96) return;
    setBrightness(200);
    _woke = true;
}

void UpdateOverlay::restoreBacklight() {
    if (!_woke || _confirm || _countdown || _box || _identify) return;
    setBrightness(appConfig.cfg.brightness);
    _woke = false;
}

// ---- 7.2 confirmation dialog ---------------------------------------------------------------
void UpdateOverlay::openConfirm(uint32_t now) {
    ConfirmInfo ci;
    if (!UpdUi::takeConfirm(ci)) return;
    closeConfirm();
    closeIdentify();
    _confirmToken = ci.token;
    _confirmOpenedMs = now;
    _dialogSeconds = ci.dialogSeconds ? ci.dialogSeconds : (uint16_t)(T_DIALOG_MS / 1000);
    _cdShown = -1;
    _declinePending = false;

    _confirm = modalRoot();
    lv_obj_t *c = card(_confirm, cardWidth(UI_S(560)), CLR_ACCENT);
    lv_obj_center(c);
    label(c, T(STR_UPD_TITLE), FONT_LARGE, CLR_ACCENT);

    // The seven facts of 7.2 in ONE label, one per line: a dialog that can
    // open on top of an open menu must not cost the LVGL pool - whose
    // exhaustion restarts the device - more objects than it has to.
    char info[640], a[24], b[24], name[48];
    size_t n = 0;
    // From: the sender's name - remote text, so only glyphs the fonts have (3.0).
    sanitizeText(ci.senderName, 16, name, sizeof(name));
    n = appendf(info, sizeof(info), n, "%s: %s (%s %u)", T(STR_UPD_FROM), name[0] ? name : "?",
                T(STR_UPD_ADDRESS), (unsigned)ci.senderAddress);

    const bool fw = ci.target == TARGET_FIRMWARE;
    n = appendf(info, sizeof(info), n, "\n%s: %s", T(STR_UPD_CONTENT),
                fw ? (ci.companionSize ? T(STR_UPD_FW_PLUS_WEB) : T(STR_UPD_FIRMWARE))
                   : T(STR_UPD_WEBUI));
    // A web UI for "any board" (id 0) is for this one.
    const uint8_t bid = (ci.boardId == BOARD_ANY) ? (uint8_t)NP_BOARD_ID : ci.boardId;
    n = appendf(info, sizeof(info), n, "\n%s: %s", T(STR_UPD_DEVICE), boardName(bid));

    char vRun[17], vNew[17];
    version17(vRun, ci.runningVersion);
    version17(vNew, ci.version);
    n = appendf(info, sizeof(info), n, "\n%s: %s", T(STR_UPD_INSTALLED), vRun);
    const char *rel = ci.versionRelation < 0 ? T(STR_UPD_OLDER)
                    : ci.versionRelation == 0 ? T(STR_UPD_SAME_VERSION) : "";
    n = appendf(info, sizeof(info), n, "\n%s: %s%s%s", T(STR_UPD_NEW), vNew,
                rel[0] ? " " : "", rel);

    fmtMb(a, sizeof(a), ci.imageSize);
    n = appendf(info, sizeof(info), n, "\n%s: %s", T(STR_UPD_SIZE), a);
    if (ci.encoding == ENC_DEFLATE) {
        fmtMb(b, sizeof(b), ci.streamSize);
        n = appendf(info, sizeof(info), n, " (%s %s)", T(STR_UPD_TRANSFERRED), b);
    }
    if (ci.companionSize) {
        fmtMb(b, sizeof(b), ci.companionSize);
        n = appendf(info, sizeof(info), n, " + %s", b);
    }

    if (ci.estSeconds == 0xFFFF) {
        n = appendf(info, sizeof(info), n, "\n%s ?", T(STR_UPD_DURATION));
    } else {
        const unsigned mins = ci.estSeconds < 60 ? 1u : (unsigned)((ci.estSeconds + 59) / 60);
        n = appendf(info, sizeof(info), n, "\n%s %u min", T(STR_UPD_DURATION), mins);
    }
    lv_obj_set_style_text_line_space(label(c, info, FONT_SMALL, CLR_TEXT), UI_S(4), 0);

    label(c, T(fw ? STR_UPD_NOTE_FIRMWARE : STR_UPD_NOTE_WEBUI), FONT_SMALL, CLR_TEXT_DIM);
    // An armed anchor watch: the restart takes it off duty for half a minute.
    if (fw && appConfig.cfg.anchorSet && appConfig.cfg.anchorAlarmOn)
        label(c, T(STR_UPD_NOTE_ANCHOR), FONT_SMALL, CLR_ORANGE);
    _confirmCd = label(c, "", FONT_SMALL, CLR_TEXT_DIM);

    lv_obj_t *row = buttonRow(c);
    // The card's inner width: its width less padding and border (the layout
    // has not run yet, so it cannot be asked).
    const lv_coord_t inner = cardWidth(UI_S(560)) - 2 * UI_S(12) - 4;
    makeHoldButton(row, T(STR_UPD_INSTALL_HOLD), _holdInstall, inner * 6 / 10, UI_S(48), true);
    tapButton(row, T(STR_UPD_DECLINE), inner * 4 / 10 - UI_S(10), UI_S(48), false, cbDecline);
    // The guard (5.11): a hold must begin 1.5 s after the dialog appeared.
    _holdInstall.armAt = now + T_BUTTON_GUARD_MS;

    uiDisableLabelScroll(_confirm);
    lv_obj_move_foreground(_confirm);
    wakeBacklight();
    tickConfirm(now);
}

void UpdateOverlay::closeConfirm() {
    if (!_confirm) return;
    lv_obj_del(_confirm);
    _confirm = _confirmCd = nullptr;
    _holdInstall.btn = _holdInstall.fill = nullptr;
    _holdInstall.active = _holdInstall.fired = false;
    _declinePending = false;
    restoreBacklight();
}

void UpdateOverlay::tickConfirm(uint32_t now) {
    if (!_confirm) return;
    updateHoldFill(_holdInstall, now);
    if (_holdInstall.fired) {
        UpdUi::userCommand(UpdUi::CMD_ACCEPT, _confirmToken);
        closeConfirm();
        return;
    }
    if (_declinePending) {
        UpdUi::userCommand(UpdUi::CMD_DECLINE, _confirmToken);
        closeConfirm();
        return;
    }
    const uint32_t e = now - _confirmOpenedMs;
    const uint32_t limit = (uint32_t)_dialogSeconds * 1000UL;
    const int left = e >= limit ? 0 : (int)((limit - e + 999) / 1000);
    if (left != _cdShown && _confirmCd) {
        char t[80];
        snprintf(t, sizeof(t), "%s %d s", T(STR_UPD_AUTO_DECLINE), left);
        lv_label_set_text(_confirmCd, t);
        _cdShown = left;
    }
    // The receiver ends the dialog at 60 s and says so. Should that message
    // never come (no N2K task any more), the dialog still must not stay.
    if (e > limit + 5000) closeConfirm();
}

// ---- 7.3 progress banner / 7.5 verify banner ------------------------------------------------
void UpdateOverlay::closeBanner() {
    if (!_banner) return;
    lv_obj_del(_banner);
    _banner = _bannerLbl = _bannerNum = _bannerBar = _btnCancel = nullptr;
    _bannerTextW = 0;
    _holdCancel.btn = _holdCancel.fill = nullptr;
    _holdCancel.active = _holdCancel.fired = false;
    _bannerKind = 0;
}

void UpdateOverlay::tickBanner(uint32_t now) {
    uint8_t kind = 0;
    uint16_t vSecs = 0;
    char vVer[17] = { 0 };
    if (_haveSnap && _snapState >= STATE_PREPARING && _snapState <= STATE_APPLYING) kind = 1;
    else if (UpdUi::peekVerify(vSecs, vVer)) kind = 2;
    if (!kind) { closeBanner(); return; }
    if (kind != _bannerKind) {
        closeBanner();
        const lv_coord_t h = UI_S(44);
        _banner = plainObj(lv_layer_top());
        lv_obj_set_size(_banner, colW(), h);
        lv_obj_set_style_bg_color(_banner, CLR_SURFACE, 0);
        lv_obj_set_style_bg_opa(_banner, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(_banner, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(_banner, CLR_ACCENT, 0);
        lv_obj_set_style_border_width(_banner, 2, 0);
        // The Cancel button only as wide as its caption needs (the German one
        // is a third longer), so the progress text keeps the rest of the line.
        lv_coord_t btnW = 0;
        if (kind == 1) {
            lv_point_t sz;
            lv_txt_get_size(&sz, T(STR_UPD_CANCEL_HOLD), FONT_SMALL, 0, 0, LV_COORD_MAX,
                            LV_TEXT_FLAG_NONE);
            btnW = sz.x + UI_S(20);
        }
        // Two labels on one line: the numbers on the right ("42 % – about
        // 3 min left", "45 s") at their natural width, the words on the left
        // ("Update 1.3.0-dev.2") in whatever is left and cut with dots. One
        // label used to carry both and, in the 600-px column of the 1024x600
        // boards next to the Cancel button, a long version string pushed the
        // remaining time - the part that matters - off the end.
        _bannerTextW = colW() - btnW - UI_S(20);
        _bannerLbl = lv_label_create(_banner);
        lv_label_set_text(_bannerLbl, "");
        lv_obj_set_style_text_font(_bannerLbl, FONT_SMALL, 0);
        lv_obj_set_style_text_color(_bannerLbl, CLR_TEXT, 0);
        // One line, cut with dots if it runs long: a fixed height is what
        // keeps LONG_DOT from wrapping instead.
        lv_obj_set_size(_bannerLbl, _bannerTextW, lv_font_get_line_height(FONT_SMALL));
        lv_label_set_long_mode(_bannerLbl, LV_LABEL_LONG_DOT);
        lv_obj_align(_bannerLbl, LV_ALIGN_LEFT_MID, UI_S(8), -2);
        _bannerNum = lv_label_create(_banner);
        lv_label_set_text(_bannerNum, "");
        lv_obj_set_style_text_font(_bannerNum, FONT_SMALL, 0);
        lv_obj_set_style_text_color(_bannerNum, CLR_TEXT, 0);
        lv_label_set_long_mode(_bannerNum, LV_LABEL_LONG_CLIP);
        _bannerBar = lv_bar_create(_banner);
        lv_obj_set_size(_bannerBar, colW() - btnW - UI_S(20), 4);
        lv_obj_align(_bannerBar, LV_ALIGN_BOTTOM_LEFT, UI_S(8), -4);
        lv_obj_set_style_bg_color(_bannerBar, CLR_BORDER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(_bannerBar, CLR_ACCENT, LV_PART_INDICATOR);
        lv_bar_set_range(_bannerBar, 0, 100);
        if (kind == 1) {
            _btnCancel = makeHoldButton(_banner, T(STR_UPD_CANCEL_HOLD), _holdCancel,
                                        btnW, h - UI_S(10), false, FONT_SMALL);
            lv_obj_align(_btnCancel, LV_ALIGN_RIGHT_MID, -UI_S(5), 0);
            _holdCancel.armAt = now;          // the banner itself is the guard here
        }
        // Below any open menu, above the instrument: the menus stay usable,
        // and a dialog or box opened later is drawn on top.
        lv_obj_move_background(_banner);
        _bannerKind = kind;
        _bannerTextAt = 0;
        uiDisableLabelScroll(_banner);
    }

    // An alarm banner (30 px) sits above it: move out from under it (7.1).
    // lv_obj_set_pos() changes nothing when the position is already right.
    const bool shift = DisplayManager::alarmActive();
    lv_obj_set_pos(_banner, colX(), colY() + (shift ? ALARM_BANNER_H : 0));

    if (kind == 1) {
        updateHoldFill(_holdCancel, now);
        if (_holdCancel.fired) {
            _holdCancel.fired = false;
            UpdUi::userCommand(UpdUi::CMD_CANCEL, _snapToken);
        }
        // Cancel exists from PREPARING to READY_TO_APPLY; APPLYING cannot be undone.
        if (_btnCancel) {
            if (_snapState == STATE_APPLYING) lv_obj_add_flag(_btnCancel, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_clear_flag(_btnCancel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (_bannerTextAt && now - _bannerTextAt < 500) return;
    _bannerTextAt = now ? now : 1;
    char t[96], n[64];
    n[0] = 0;
    int pct = 0;
    if (kind == 2) {
        // 7.5: "Checking update 1.3.0" + "45 s", once a second. That redraw is
        // also what guarantees an LVGL flush for the health criterion H3.
        snprintf(t, sizeof(t), "%s %s", T(STR_UPD_CHECKING), vVer[0] ? vVer : FW_VERSION);
        snprintf(n, sizeof(n), "%u s", (unsigned)vSecs);
        lv_obj_add_flag(_bannerBar, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (_snapImage) pct = (int)((uint64_t)_snapWritten * 100 / _snapImage);
        if (pct > 100) pct = 100;
        switch (_snapState) {
            case STATE_PREPARING:
                snprintf(t, sizeof(t), "%s", T(STR_UPD_PREPARING));
                if (_snapSeconds != 0xFFFF && _snapSeconds > 0)
                    snprintf(n, sizeof(n), "%u s", (unsigned)_snapSeconds);
                break;
            case STATE_RECEIVING:
            case STATE_PROCESSING: {
                if (_snapTarget == TARGET_WEBUI) snprintf(t, sizeof(t), "%s", T(STR_UPD_WEBUI));
                else snprintf(t, sizeof(t), "%s %s", T(STR_UPD_UPDATE), _snapVersion);
                // The remaining time from the measured stream rate (flash work
                // included, it is part of the rate).
                const uint32_t el = _snapStartMs ? now - _snapStartMs : 0;
                if (el > 5000 && _snapAccepted > 0 && _snapStream > _snapAccepted) {
                    const uint64_t remMs = (uint64_t)(_snapStream - _snapAccepted) * el / _snapAccepted;
                    const unsigned mins = (unsigned)((remMs + 59999) / 60000);
                    snprintf(n, sizeof(n), "%d %% – %s %u min", pct, T(STR_UPD_REMAINING),
                             mins ? mins : 1u);
                } else {
                    snprintf(n, sizeof(n), "%d %%", pct);
                }
                break;
            }
            case STATE_VERIFYING:
                snprintf(t, sizeof(t), "%s", T(STR_UPD_CHECKING));
                pct = 100;
                break;
            default:   // READY_TO_APPLY, APPLYING
                snprintf(t, sizeof(t), "%s", T(STR_UPD_ACTIVATING));
                pct = 100;
                break;
        }
        lv_obj_clear_flag(_bannerBar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(_bannerBar, pct, LV_ANIM_OFF);
    }
    // Numbers first, at their natural width, right-aligned against the end of
    // the text column; the words get the rest of the line.
    lv_coord_t numW = 0;
    if (n[0]) {
        lv_point_t sz;
        lv_txt_get_size(&sz, n, FONT_SMALL, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        numW = sz.x;
    }
    lv_label_set_text(_bannerNum, n);
    lv_obj_set_size(_bannerNum, numW, lv_font_get_line_height(FONT_SMALL));
    lv_obj_align(_bannerNum, LV_ALIGN_LEFT_MID, UI_S(8) + _bannerTextW - numW, -2);
    const lv_coord_t gap = numW ? UI_S(10) : 0;
    lv_obj_set_width(_bannerLbl, _bannerTextW - numW - gap > 0 ? _bannerTextW - numW - gap : 1);
    lv_label_set_text(_bannerLbl, t);
}

// ---- 7.4 restart countdown ------------------------------------------------------------------
void UpdateOverlay::openCountdown(uint32_t now) {
    closeCountdown();
    closeIdentify();
    _countdown = modalRoot();
    lv_obj_t *c = card(_countdown, cardWidth(UI_S(520)), CLR_ACCENT);
    lv_obj_center(c);
    _cdText = label(c, "", FONT_LARGE, CLR_TEXT);
    lv_obj_set_style_text_align(_cdText, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *row = buttonRow(c);
    const lv_coord_t inner = cardWidth(UI_S(520)) - 2 * UI_S(12) - 4;
    tapButton(row, T(STR_UPD_RESTART_NOW), inner / 2 - UI_S(5), UI_S(48), true, cbRestartNow);
    tapButton(row, T(STR_UPD_LATER), inner / 2 - UI_S(5), UI_S(48), false, cbLater);
    _cdStartMs = now;
    _cdPaused = false;
    _cdLastShown = -1;
    _restartNow = _later = false;
    UpdUi::setCountdownOpen(true);
    uiDisableLabelScroll(_countdown);
    lv_obj_move_foreground(_countdown);
    wakeBacklight();
    tickCountdown(now);
}

void UpdateOverlay::closeCountdown() {
    closeCountdownBanner();
    if (!_countdown) return;
    lv_obj_del(_countdown);
    _countdown = _cdText = nullptr;
    _cdPaused = false;
    UpdUi::setCountdownOpen(false);
    restoreBacklight();
}

// While an alarm holds the countdown, the full-screen dialog steps aside for
// a strip under the alarm banner - "Restart after the alarm" and "Later" -
// so the instrument the alarm is about (the anchor screen) stays in view.
void UpdateOverlay::openCountdownBanner() {
    closeCountdownBanner();
    const lv_coord_t h = UI_S(44);
    _cdBanner = plainObj(lv_layer_top());
    lv_obj_set_size(_cdBanner, colW(), h);
    lv_obj_set_style_bg_color(_cdBanner, CLR_SURFACE, 0);
    lv_obj_set_style_bg_opa(_cdBanner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(_cdBanner, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(_cdBanner, CLR_ACCENT, 0);
    lv_obj_set_style_border_width(_cdBanner, 2, 0);
    lv_point_t sz;
    lv_txt_get_size(&sz, T(STR_UPD_LATER), FONT_MED, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_coord_t btnW = sz.x + UI_S(24);
    lv_obj_t *l = lv_label_create(_cdBanner);
    lv_label_set_text(l, T(STR_UPD_RESTART_AFTER_ALARM));
    lv_obj_set_style_text_font(l, FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, CLR_TEXT, 0);
    lv_obj_set_size(l, colW() - btnW - UI_S(20), lv_font_get_line_height(FONT_SMALL));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, UI_S(8), 0);
    lv_obj_t *b = tapButton(_cdBanner, T(STR_UPD_LATER), btnW, h - UI_S(10), false, cbLater);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -UI_S(5), 0);
    lv_obj_set_pos(_cdBanner, colX(), colY() + ALARM_BANNER_H);
    uiDisableLabelScroll(_cdBanner);
    lv_obj_move_foreground(_cdBanner);
}

void UpdateOverlay::closeCountdownBanner() {
    if (!_cdBanner) return;
    lv_obj_del(_cdBanner);
    _cdBanner = nullptr;
}

void UpdateOverlay::tickCountdown(uint32_t now) {
    if (!_countdown) return;
    if (_restartNow) {
        _restartNow = false;
        closeCountdown();
        UpdUi::restartNow();
        return;
    }
    if (_later) {
        _later = false;
        closeCountdown();
        UpdUi::later();
        toast(T(STR_UPD_ACTIVE_NEXT));
        return;
    }
    // While an alarm is active the countdown stands still; once every alarm
    // has cleared it starts again at 10 s (5.6). Meanwhile it is a strip, not
    // a full-screen dialog (openCountdownBanner()).
    if (DisplayManager::alarmActive()) {
        if (!_cdPaused) {
            _cdPaused = true;
            lv_label_set_text(_cdText, T(STR_UPD_RESTART_AFTER_ALARM));
            lv_obj_add_flag(_countdown, LV_OBJ_FLAG_HIDDEN);
            openCountdownBanner();
        }
        return;
    }
    if (_cdPaused) {
        _cdPaused = false;
        _cdStartMs = now;
        _cdLastShown = -1;
        closeCountdownBanner();
        lv_obj_clear_flag(_countdown, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(_countdown);
    }
    const uint32_t e = now - _cdStartMs;
    if (e >= T_RESTART_COUNTDOWN_MS) {
        closeCountdown();
        UpdUi::restartNow();
        return;
    }
    const int left = (int)((T_RESTART_COUNTDOWN_MS - e + 999) / 1000);
    if (left != _cdLastShown) {
        char t[96];
        snprintf(t, sizeof(t), "%s %d s", T(STR_UPD_READY_RESTART), left);
        lv_label_set_text(_cdText, t);
        _cdLastShown = left;
    }
}

// ---- 7.5 / 7.6 box and toast --------------------------------------------------------------
void UpdateOverlay::showBox(const char *text, uint32_t autoCloseMs, bool notice) {
    closeBox();
    _box = card(lv_layer_top(), cardWidth(UI_S(520)), notice ? CLR_ORANGE : CLR_RED);
    lv_obj_center(_box);
    label(_box, text, FONT_MED, CLR_TEXT);
    lv_obj_t *row = buttonRow(_box);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    tapButton(row, T(STR_UPD_OK), UI_S(120), UI_S(44), true, cbBoxOk);
    const uint32_t now = millis();
    _boxCloseAt = autoCloseMs ? (now + autoCloseMs) | 1u : 0;
    _boxNotice = notice;
    _boxOkPending = false;
    uiDisableLabelScroll(_box);
    lv_obj_move_foreground(_box);
    wakeBacklight();
}

void UpdateOverlay::closeBox() {
    if (!_box) return;
    lv_obj_del(_box);
    _box = nullptr;
    _boxCloseAt = 0;
    restoreBacklight();
}

void UpdateOverlay::toast(const char *text, uint32_t ms) {
    if (_toast) { lv_obj_del(_toast); _toast = nullptr; }
    _toast = lv_obj_create(lv_layer_top());
    lv_obj_set_width(_toast, LV_SIZE_CONTENT);
    lv_obj_set_height(_toast, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(_toast, cardWidth(UI_S(520)), 0);
    lv_obj_set_style_bg_color(_toast, CLR_SURFACE, 0);
    lv_obj_set_style_bg_opa(_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_toast, CLR_ACCENT, 0);
    lv_obj_set_style_border_width(_toast, 1, 0);
    lv_obj_set_style_radius(_toast, 8, 0);
    lv_obj_set_style_pad_all(_toast, UI_S(10), 0);
    lv_obj_clear_flag(_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_toast, LV_OBJ_FLAG_CLICKABLE);   // touches go through
    lv_obj_t *l = lv_label_create(_toast);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, FONT_MED, 0);
    lv_obj_set_style_text_color(l, CLR_TEXT, 0);
    lv_obj_set_style_max_width(l, cardWidth(UI_S(520)) - 2 * UI_S(10), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(_toast, LV_ALIGN_BOTTOM_MID, 0, -UI_S(16));
    uiDisableLabelScroll(_toast);
    lv_obj_move_foreground(_toast);
    _toastUntil = (millis() + ms) | 1u;
}

void UpdateOverlay::restartDeferred(uint32_t seconds) {
    char t[96];
    if (seconds) snprintf(t, sizeof(t), "%s (%u s)", T(STR_UPD_RESTART_AFTER_UPDATE), (unsigned)seconds);
    else         snprintf(t, sizeof(t), "%s", T(STR_UPD_RESTART_AFTER_UPDATE));
    toast(t);
}

void UpdateOverlay::demoDeferred() { toast(T(STR_UPD_DEMO_AFTER_UPDATE)); }

// ---- 7.7 identify screen ----------------------------------------------------------------------
void UpdateOverlay::openIdentify(uint8_t seconds, uint32_t now) {
    if (_confirm || _countdown) return;     // 3.11: ignored while those are open
    closeIdentify();
    _identify = modalRoot();
    lv_obj_add_event_cb(_identify, cbIdentifyTap, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_flex_flow(_identify, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_identify, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(_identify, UI_S(14), 0);
    lv_obj_set_style_border_color(_identify, CLR_ACCENT, 0);
    lv_obj_set_style_border_width(_identify, UI_S(8), 0);
    label(_identify, T(STR_UPD_THIS_DISPLAY), FONT_XL, CLR_ACCENT, false);
#ifdef SIMULATOR
    label(_identify, NP_BOARD_ID ? boardName(NP_BOARD_ID) : "Simulator", FONT_MED, CLR_TEXT, false);
    const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 };
#else
    label(_identify, boardName(NP_BOARD_ID), FONT_MED, CLR_TEXT, false);
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
#endif
    char id[16];
    snprintf(id, sizeof(id), "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
    label(_identify, id, FONT_XXL, CLR_TEXT, false);
    if (appConfig.cfg.bootName[0]) {
        char nm[64];
        sanitizeText((const uint8_t *)appConfig.cfg.bootName, sizeof(appConfig.cfg.bootName),
                     nm, sizeof(nm));
        label(_identify, nm, FONT_LARGE, CLR_TEXT, false);
    }
    uiDisableLabelScroll(_identify);
    lv_obj_move_foreground(_identify);
    _identifyUntil = (now + (uint32_t)seconds * 1000UL) | 1u;
    _identifyTapped = false;
    wakeBacklight();
}

void UpdateOverlay::closeIdentify() {
    if (!_identify) return;
    lv_obj_del(_identify);
    _identify = nullptr;
    _identifyUntil = 0;
    restoreBacklight();
}

// ---- events from the mailbox ---------------------------------------------------------------------
void UpdateOverlay::handleEvent(uint8_t type, uint32_t token, uint8_t a, uint8_t b, uint8_t c,
                                uint32_t now) {
    char t[200], rb[24];
    switch (type) {
        case UpdUi::EV_CONFIRM_OPEN:
            openConfirm(now);
            break;
        case UpdUi::EV_CONFIRM_CLOSE:
            if (_confirm && token == _confirmToken) closeConfirm();
            break;
        case UpdUi::EV_FAILED:
            if (_confirm && token == _confirmToken) closeConfirm();
            // 7.6: WRONG_DEVICE and SESSION_UNKNOWN are protocol housekeeping.
            if (a != RSN_WRONG_DEVICE && a != RSN_SESSION_UNKNOWN) {
                snprintf(t, sizeof(t), "%s\n%s", T(STR_UPD_ABORTED), reasonText(a, rb, sizeof(rb)));
                showBox(t, T_END_REPORT_MS, false);
            }
            break;
        case UpdUi::EV_APPLIED:
            if (a == TARGET_FIRMWARE) {
                if (b == APPLY_NEXT_RESTART) toast(T(STR_UPD_ACTIVE_NEXT));
                else openCountdown(now);
            } else {
                toast(T(c ? STR_UPD_WEB_NEXT_RESTART : STR_UPD_WEB_UPDATED));
            }
            break;
        case UpdUi::EV_IDENTIFY:
            openIdentify(a, now);
            break;
        case UpdUi::EV_VALIDATED: {
            char v[17];
            if (!UpdUi::takeValidatedVersion(v)) strcpy(v, FW_VERSION);
            snprintf(t, sizeof(t), "%s %s", T(STR_UPD_SUCCESS), v);
            toast(t, 5000);
            break;
        }
        case UpdUi::EV_ROLLBACK_NOTICE: {
            UpdUi::Notice n;
            if (!UpdUi::takeNotice(n)) break;
            const char *fv = n.failedVersion[0] ? n.failedVersion : "?";
            const char *head, *why;
            switch (n.cause) {
                case RB_HEALTH:
                    head = T(STR_UPD_RB_FAILED);
                    why = T(n.detail == 5 ? STR_UPD_RB_NO_RX : STR_UPD_RB_SELFTEST);
                    break;
                case RB_CRASH:      head = T(STR_UPD_RB_FAILED);        why = T(STR_UPD_RB_CRASH);   break;
                case RB_POWER:      head = T(STR_UPD_RB_NOT_CONFIRMED); why = T(STR_UPD_RB_POWER);   break;
                case RB_RESTART:    head = T(STR_UPD_RB_NOT_CONFIRMED); why = T(STR_UPD_RB_RESTART); break;
                case RB_CRASH_LOOP: head = T(STR_UPD_RB_REVERTED);      why = T(STR_UPD_RB_CRASHES); break;
                default:            head = T(STR_UPD_RB_FAILED);        why = nullptr;               break;
            }
            if (why) snprintf(t, sizeof(t), "%s %s – %s\n%s %s", head, fv, why,
                              T(STR_UPD_RUNNING_AGAIN), n.runningVersion);
            else     snprintf(t, sizeof(t), "%s %s\n%s %s", head, fv,
                              T(STR_UPD_RUNNING_AGAIN), n.runningVersion);
            showBox(t, 0, true);
            break;
        }
        default:
            break;
    }
}

// ---- the tick ---------------------------------------------------------------------------------
void UpdateOverlay::tick() {
    const uint32_t now = millis();

    UpdUi::Event ev;
    while (UpdUi::takeEvent(ev)) handleEvent(ev.type, ev.token, ev.a, ev.b, ev.c, now);

    Snapshot s;
    if (UpdUi::takeSnapshot(s)) {
        _haveSnap = true;
        _snapState = s.state;
        _snapTarget = s.target;
        _snapToken = s.token;
        _snapWritten = s.imageWritten;
        _snapImage = s.imageSize;
        _snapAccepted = s.streamAccepted;
        _snapStream = s.streamSize;
        _snapStartMs = s.transferStartMs;
        _snapSeconds = s.secondsLeft;
        version17(_snapVersion, s.version);
    }

    tickConfirm(now);
    tickBanner(now);
    tickCountdown(now);

    if (_box) {
        if (_boxOkPending) {
            _boxOkPending = false;
            if (_boxNotice) UpdUi::noticeSeen();   // shown once per rollback (10.5)
            closeBox();
        } else if (_boxCloseAt && (int32_t)(now - _boxCloseAt) >= 0) {
            closeBox();
        }
    }
    if (_toast && (int32_t)(now - _toastUntil) >= 0) {
        lv_obj_del(_toast);
        _toast = nullptr;
    }
    if (_identify && (_identifyTapped || (int32_t)(now - _identifyUntil) >= 0)) {
        // Tapped away: this display has been found. The receiver then ignores
        // IDENTIFY for a minute, so a sender repeating it cannot keep the
        // instruments covered.
        if (_identifyTapped) UpdUi::noteIdentifyClosed();
        closeIdentify();
    }
}
