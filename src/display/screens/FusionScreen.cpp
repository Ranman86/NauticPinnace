#include "FusionScreen.h"
#include "../Theme.h"
#include "../../config/Config.h"  // n2kListenOnly
#include "../../i18n/I18n.h"
#include "../../DisplaySetup.h"   // swipeSuppress()
#include "../../nmea/DataModel.h"
#include "../../nmea/FusionN2k.h"
#include <string.h>
#include <stdio.h>

static FusionScreen *s_self = nullptr;

// ---- local widget helpers ---------------------------------------------------
static lv_obj_t *mkBtn(lv_obj_t *parent, const char *sym, int x, int y, int w, int h,
                       lv_event_cb_t cb, bool accent, const lv_font_t *font, void *user = nullptr) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, accent ? CLR_ACCENT : CLR_SURFACE, 0);
    lv_obj_set_style_bg_color(b, CLR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, CLR_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, accent ? CLR_ON_ACCENT : CLR_TEXT, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_center(l);
    if (user) lv_obj_set_user_data(b, user);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

// One volume row: [label] [mute btn] [slider] [value%]. zone<0 → Master row.
//
// Horizontal budget: the floating prev/next nav buttons sit at x 0..80 and
// 400..480 (UI_NAV_BTN_W = 480/6, y 180..300) and swallow touches there. The
// Master and Zone-1 rows fall inside that y band, so every TAPPABLE element
// must stay within x 80..400 — the mute buttons used to be at x 8..50 and were
// simply unreachable under the prev arrow. Only the (non-clickable) name label
// is allowed to live in the left gutter.
static void mkVolRow(lv_obj_t *parent, const char *name, int y, int zone,
                     lv_event_cb_t volCb, lv_event_cb_t muteCb,
                     lv_obj_t **sliderOut, lv_obj_t **valOut, lv_obj_t **muteIconOut) {
    bool master = (zone < 0);
    void *ud = master ? nullptr : (void *)(intptr_t)zone;

    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, name);
    lv_obj_set_pos(lbl, 8, y);
    lv_obj_set_style_text_font(lbl, FONT_MED, 0);
    lv_obj_set_style_text_color(lbl, master ? CLR_TEXT : CLR_TEXT_DIM, 0);

    lv_obj_t *mb = mkBtn(parent, LV_SYMBOL_VOLUME_MAX, 84, y - 10, 42, 40, muteCb, false, FONT_MED, ud);
    *muteIconOut = lv_obj_get_child(mb, 0);

    // The knob is 24 px wide (12 px track + 6 px pad each side) and is centred on
    // the value position, so at 0 % it sticks out 12 px LEFT of the track start —
    // at x=132 that ran into the mute button (ends at 126). Start the track far
    // enough right that the 0 %-knob clears it, and stop it early enough that the
    // 100 %-knob clears the value label.
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_size(s, 190, 12);
    lv_obj_set_pos(s, 146, y + 6);
    lv_slider_set_range(s, 0, 100);
    lv_color_t fill = master ? CLR_GREEN : CLR_ACCENT;
    lv_obj_set_style_bg_color(s, CLR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, fill, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, fill, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    if (!master) lv_obj_set_user_data(s, ud);
    if (volCb) lv_obj_add_event_cb(s, volCb, LV_EVENT_VALUE_CHANGED, nullptr);
    // A slider owns its horizontal drag: without this, dragging the volume far
    // enough sideways also fired the global swipe-to-change-screen gesture.
    lv_obj_add_event_cb(s, [](lv_event_t *) { swipeSuppress(); }, LV_EVENT_PRESSED, nullptr);
    *sliderOut = s;

    lv_obj_t *val = lv_label_create(parent);
    lv_label_set_text(val, "0%");
    lv_obj_set_pos(val, 356, y);
    lv_obj_set_style_text_font(val, FONT_MED, 0);
    lv_obj_set_style_text_color(val, master ? CLR_TEXT : CLR_TEXT_DIM, 0);
    *valOut = val;
}

// ---- create -----------------------------------------------------------------
void FusionScreen::create(lv_obj_t *parent) {
    s_self = this;
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // ---- source list (dropdown) ----
    _srcDropdown = lv_dropdown_create(container);
    lv_dropdown_set_options(_srcDropdown, "--");
    lv_obj_set_size(_srcDropdown, 320, 42);
    lv_obj_set_pos(_srcDropdown, 80, 18);
    lv_obj_set_style_text_align(_srcDropdown, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_srcDropdown, FONT_LARGE, 0);
    lv_obj_set_style_bg_color(_srcDropdown, CLR_SURFACE, 0);
    lv_obj_set_style_text_color(_srcDropdown, CLR_ACCENT, 0);
    lv_obj_set_style_border_color(_srcDropdown, CLR_BORDER, 0);
    lv_obj_set_style_border_width(_srcDropdown, 1, 0);
    lv_obj_set_style_radius(_srcDropdown, 8, 0);
    lv_obj_add_event_cb(_srcDropdown, cbSourceDropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    if (lv_obj_t *list = lv_dropdown_get_list(_srcDropdown)) {
        lv_obj_set_style_bg_color(list, CLR_SURFACE, 0);
        lv_obj_set_style_text_color(list, CLR_TEXT, 0);
        lv_obj_set_style_text_font(list, FONT_MED, 0);
        lv_obj_set_style_border_color(list, CLR_BORDER, 0);
        lv_obj_set_style_bg_color(list, CLR_ACCENT, LV_PART_SELECTED | LV_STATE_CHECKED);
    }

    // ---- now playing ----
    _titleLabel = lv_label_create(container);
    lv_label_set_long_mode(_titleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(_titleLabel, 460);
    lv_obj_set_pos(_titleLabel, 10, 70);
    lv_obj_set_style_text_align(_titleLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_titleLabel, FONT_LARGE, 0);
    lv_obj_set_style_text_color(_titleLabel, CLR_TEXT, 0);
    lv_label_set_text(_titleLabel, "--");

    _metaLabel = lv_label_create(container);
    lv_label_set_long_mode(_metaLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_metaLabel, 460);
    lv_obj_set_pos(_metaLabel, 10, 108);
    lv_obj_set_style_text_align(_metaLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_metaLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_metaLabel, CLR_TEXT_DIM, 0);
    lv_label_set_text(_metaLabel, "");

    // ---- transport ----
    mkBtn(container, LV_SYMBOL_PREV, 140, 134, 60, 48, cbPrev, false, FONT_LARGE);
    lv_obj_t *play = mkBtn(container, LV_SYMBOL_PLAY, 210, 130, 68, 56, cbPlay, true, FONT_LARGE);
    _playIcon = lv_obj_get_child(play, 0);
    mkBtn(container, LV_SYMBOL_NEXT, 288, 134, 60, 48, cbNext, false, FONT_LARGE);

    _timeLabel = lv_label_create(container);
    lv_obj_set_width(_timeLabel, 460);
    lv_obj_set_pos(_timeLabel, 10, 190);
    lv_obj_set_style_text_align(_timeLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_timeLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_timeLabel, CLR_TEXT_DIM, 0);
    lv_label_set_text(_timeLabel, "0:00 / 0:00");

    // ---- volume rows (with per-zone mute) ----
    // Row pitch 64 (was 52): the block used to end at y≈400 and leave 80 px of
    // dead space; now it reaches ≈436 and the rows are easier to hit.
    mkVolRow(container, "Master", 214, -1, cbMaster, cbMasterMute, &_masterSlider, &_masterVal, &_masterMuteIcon);
    mkVolRow(container, "Zone 1", 278,  0, cbZone,   cbZoneMute,   &_zoneSlider[0], &_zoneVal[0], &_zoneMuteIcon[0]);
    mkVolRow(container, "Zone 2", 342,  1, cbZone,   cbZoneMute,   &_zoneSlider[1], &_zoneVal[1], &_zoneMuteIcon[1]);
    mkVolRow(container, "Zone 3", 406,  2, cbZone,   cbZoneMute,   &_zoneSlider[2], &_zoneVal[2], &_zoneMuteIcon[2]);

    // Listen-only: all control paths in FusionN2k are muted via n2kActive.
    // Without a note the controls would just look "broken" — so say why.
    if (appConfig.cfg.n2kListenOnly) {
        lv_obj_t *n = lv_label_create(container);
        lv_label_set_text(n, T(STR_FUS_LISTEN_NOTE));
        lv_obj_set_style_text_font(n, FONT_TINY, 0);
        lv_obj_set_style_text_color(n, CLR_YELLOW, 0);
        // Bottom instead of top: y=2 would sit under the demo banner (sim/demo mode).
        lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -2);
    }

    syncFromData();
}

void FusionScreen::resetForRebuild() {
    _srcDropdown = _titleLabel = _metaLabel = _timeLabel = _playIcon = nullptr;
    _masterSlider = _masterVal = _masterMuteIcon = nullptr;
    for (int i = 0; i < 3; i++) { _zoneSlider[i] = _zoneVal[i] = _zoneMuteIcon[i] = nullptr; }
    _lastTitle[0] = 1; _lastTitle[1] = 0; _lastMeta[0] = 1; _lastMeta[1] = 0;
    _lastPlay = -1; _lastSrcSel = -2; _lastSrcCount = -1; _lastMasterMute = -1;
    _lastZoneMute[0] = _lastZoneMute[1] = _lastZoneMute[2] = -1;
}

// ---- per-tick refresh -------------------------------------------------------
static void fmtTime(char *buf, size_t sz, uint32_t ms) {
    uint32_t s = ms / 1000;
    snprintf(buf, sz, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

void FusionScreen::syncFromData() {
    if (!_titleLabel) return;

    int  src, srcCount; char srcNames[DataModel::FUSION_MAX_SOURCES][16];
    char title[48], artist[40], album[40];
    uint32_t elapsed, total; int play; int zv[3], zm[3]; int master;
    {
        auto lk = data.lock();
        src = data.fusionSource; srcCount = data.fusionSourceCount;
        for (int i = 0; i < DataModel::FUSION_MAX_SOURCES; i++) {
            strncpy(srcNames[i], data.fusionSourceName[i], 15); srcNames[i][15] = 0;
        }
        strncpy(title, data.fusionTitle, sizeof(title) - 1); title[sizeof(title)-1] = 0;
        strncpy(artist, data.fusionArtist, sizeof(artist) - 1); artist[sizeof(artist)-1] = 0;
        strncpy(album, data.fusionAlbum, sizeof(album) - 1); album[sizeof(album)-1] = 0;
        elapsed = data.fusionElapsedMs; total = data.fusionTotalMs;
        play = data.fusionPlayState;
        for (int i = 0; i < 3; i++) { zv[i] = data.fusionZoneVol[i]; zm[i] = data.fusionZoneMute[i] ? 1 : 0; }
        master = data.fusionMasterVol();
    }

    // source dropdown: rebuild options on count change, set selection (not while open)
    if (!lv_dropdown_is_open(_srcDropdown)) {
        if (srcCount != _lastSrcCount) {
            char opts[DataModel::FUSION_MAX_SOURCES * 17 + 4];
            opts[0] = 0;
            for (int i = 0; i < srcCount; i++) {
                if (i) strncat(opts, "\n", sizeof(opts) - strlen(opts) - 1);
                strncat(opts, srcNames[i], sizeof(opts) - strlen(opts) - 1);
            }
            if (srcCount <= 0) strcpy(opts, "--");
            lv_dropdown_set_options(_srcDropdown, opts);
            _lastSrcCount = srcCount; _lastSrcSel = -2;
        }
        if (src != _lastSrcSel) {
            if (src >= 0 && src < srcCount) lv_dropdown_set_selected(_srcDropdown, src);
            _lastSrcSel = src;
        }
    }

    // title (only on change → don't restart the scroll)
    if (strncmp(title, _lastTitle, sizeof(_lastTitle)) != 0) {
        lv_label_set_text(_titleLabel, (title[0] ? title : "--"));
        strncpy(_lastTitle, title, sizeof(_lastTitle) - 1); _lastTitle[sizeof(_lastTitle)-1] = 0;
    }

    char meta[88];
    if (artist[0] && album[0])      snprintf(meta, sizeof(meta), "%s - %s", artist, album);
    else if (artist[0])             snprintf(meta, sizeof(meta), "%s", artist);
    else                            snprintf(meta, sizeof(meta), "%s", album);
    if (strncmp(meta, _lastMeta, sizeof(_lastMeta)) != 0) {
        lv_label_set_text(_metaLabel, meta);
        strncpy(_lastMeta, meta, sizeof(_lastMeta) - 1); _lastMeta[sizeof(_lastMeta)-1] = 0;
    }

    char te[12], tt[12], tbuf[28];
    fmtTime(te, sizeof(te), elapsed);
    if (total > 0) { fmtTime(tt, sizeof(tt), total); snprintf(tbuf, sizeof(tbuf), "%s / %s", te, tt); }
    else           snprintf(tbuf, sizeof(tbuf), "%s", te);
    lv_label_set_text(_timeLabel, tbuf);

    if (play != _lastPlay && _playIcon) {
        lv_label_set_text(_playIcon, (play == 1) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        _lastPlay = play;
    }

    // mute icons (per zone + master = any-muted)
    bool any = (zm[0] || zm[1] || zm[2]);
    for (int i = 0; i < 3; i++) {
        if (zm[i] != _lastZoneMute[i] && _zoneMuteIcon[i]) {
            lv_label_set_text(_zoneMuteIcon[i], zm[i] ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
            lv_obj_set_style_text_color(_zoneMuteIcon[i], zm[i] ? CLR_RED : CLR_TEXT, 0);
            _lastZoneMute[i] = zm[i];
        }
    }
    int mAll = any ? 1 : 0;
    if (mAll != _lastMasterMute && _masterMuteIcon) {
        lv_label_set_text(_masterMuteIcon, mAll ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_style_text_color(_masterMuteIcon, mAll ? CLR_RED : CLR_TEXT, 0);
        _lastMasterMute = mAll;
    }

    // sliders (skip the one being dragged); muted zones show a dimmed value
    char pct[12];
    if (!(lv_obj_get_state(_masterSlider) & LV_STATE_PRESSED)) {
        lv_slider_set_value(_masterSlider, master, LV_ANIM_OFF);
        snprintf(pct, sizeof(pct), "%d%%", master); lv_label_set_text(_masterVal, pct);
    }
    for (int i = 0; i < 3; i++) {
        if (lv_obj_get_state(_zoneSlider[i]) & LV_STATE_PRESSED) continue;
        lv_slider_set_value(_zoneSlider[i], zv[i], LV_ANIM_OFF);
        snprintf(pct, sizeof(pct), "%d%%", zv[i]);
        lv_label_set_text(_zoneVal[i], pct);
        lv_obj_set_style_text_color(_zoneVal[i], zm[i] ? CLR_RED : CLR_TEXT_DIM, 0);  // red = muted
    }
}

void FusionScreen::onShow() { _lastReq = 0; syncFromData(); }

void FusionScreen::update() {
    uint32_t now = millis();
    if (now - _lastReq > 5000) { _lastReq = now; Fusion::requestStatus(); }
    syncFromData();
}

// ---- callbacks --------------------------------------------------------------
void FusionScreen::cbPlay(lv_event_t *e) { Fusion::playPause();  if (s_self) s_self->syncFromData(); }
void FusionScreen::cbNext(lv_event_t *e) { Fusion::nextTrack();  if (s_self) s_self->syncFromData(); }
void FusionScreen::cbPrev(lv_event_t *e) { Fusion::prevTrack();  if (s_self) s_self->syncFromData(); }

void FusionScreen::cbSourceDropdown(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    Fusion::setSource((int)lv_dropdown_get_selected(dd));
    if (s_self) s_self->syncFromData();
}

void FusionScreen::cbMasterMute(lv_event_t *e) { Fusion::toggleAllMute(); if (s_self) s_self->syncFromData(); }

void FusionScreen::cbZoneMute(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    Fusion::toggleZoneMute((int)(intptr_t)lv_obj_get_user_data(b));
    if (s_self) s_self->syncFromData();
}

void FusionScreen::cbMaster(lv_event_t *e) {
    Fusion::setMasterVolume(lv_slider_get_value(lv_event_get_target(e)));
    if (s_self) s_self->syncFromData();
}

void FusionScreen::cbZone(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target(e);
    Fusion::setZoneVolume((int)(intptr_t)lv_obj_get_user_data(s), lv_slider_get_value(s));
    if (s_self) s_self->syncFromData();
}
