#include "BootScreen.h"
#include "../Version.h"

// Boot animation runs before the normal tick source is alive, so it ticks
// LVGL manually - except with LV_TICK_CUSTOM (7B), where LVGL reads millis()
// itself and lv_tick_inc() does not exist.
#if LV_TICK_CUSTOM
  #define BOOT_TICK(ms) ((void)0)
#else
  #define BOOT_TICK(ms) lv_tick_inc(ms)
#endif
#include "../i18n/I18n.h"
#include "../config/Config.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <math.h>

BootScreen bootScreen;

// The boot screen has two fixed clusters: the logo group pinned to the top edge
// and the status/progress group pinned to the bottom edge. Their offsets were
// tuned for a 600 px tall screen; on a taller (portrait) one the two groups just
// drift apart and leave a large empty band in the middle. Move both toward the
// centre by half the surplus height so the composition keeps its proportions.
//
// The landscape zero is stated STRUCTURALLY, not left to arithmetic. It is true
// today that uiScreenH() == SCREEN_H unrotated on all three boards, but that is
// a coincidence of the current panels: SCREEN_H is the INSTRUMENT DESIGN GRID
// (Theme.h), and the boot screen is not laid out on it - none of its offsets go
// through UI_S(). A future board whose panel height differs from the 600 design
// grid would otherwise shift the boot screen in LANDSCAPE too, which is exactly
// the regression this feature must never cause. With the uiPortrait() gate the
// result is 0 on every board today and can never fire unrotated.
static inline lv_coord_t bootVShift() {
    return uiPortrait() ? (uiScreenH() - SCREEN_H) / 2 : 0;
}

// Width of the status line: the screen less 20 px each side, at most 640 px
// so a line on the 1024 px panel stays readable at a glance.
static inline lv_coord_t statusWidth() {
    const lv_coord_t w = uiScreenW() - 40;
    return w > 640 ? 640 : w;
}

// One line "<prefix><quote><name><quote><suffix>" that has to stay ONE line:
// the fallback message is three of them, and a fourth would run into the
// boat name on the 4" (measured: a 31-character network name made line 1
// wrap). So the NAME gives way - shortened character by character, never
// inside a UTF-8 sequence, with "..." - until the line fits. Prefix, reason
// and address are the parts that must survive; the full name is in the
// settings anyway.
static void lineWithName(char *out, size_t n, const char *prefix, const char *name,
                         const char *suffix, const lv_font_t *font, lv_coord_t maxW) {
    const char *qo = T(STR_QUOTE_OPEN);
    const char *qc = T(STR_QUOTE_CLOSE);
    char nm[40];
    strlcpy(nm, name ? name : "", sizeof nm);
    size_t len = strlen(nm);
    bool cut = false;
    for (;;) {
        snprintf(out, n, "%s%s%s%s%s%s", prefix, qo, nm, cut ? "..." : "", qc, suffix);
        if (len == 0 ||
            lv_txt_get_width(out, strlen(out), font, 0, LV_TEXT_FLAG_NONE) <= maxW)
            return;
        do { len--; } while (len > 0 && ((uint8_t)nm[len] & 0xC0) == 0x80);
        nm[len] = '\0';
        cut = true;
    }
}

// ---- Custom logo loader -----------------------------------------------------

bool BootScreen::loadCustomLogo() {
    if (!LittleFS.exists("/logo.bin")) return false;

    File f = LittleFS.open("/logo.bin", "r");
    if (!f) return false;

    uint16_t w = 0, h = 0;
    f.read((uint8_t *)&w, 2);
    f.read((uint8_t *)&h, 2);

    if (w == 0 || h == 0 || w > 400 || h > 400) { f.close(); return false; }

    size_t px = (size_t)w * h * sizeof(lv_color_t);
    _logoBuf = (lv_color_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_logoBuf) { f.close(); return false; }

    f.read((uint8_t *)_logoBuf, px);
    f.close();

    _logoDesc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    _logoDesc.header.always_zero = 0;
    _logoDesc.header.w           = w;
    _logoDesc.header.h           = h;
    _logoDesc.data_size          = px;
    _logoDesc.data               = (const uint8_t *)_logoBuf;
    return true;
}

// ---- Built-in mark: the NauticPi signet (pi over a wave) ---------------------
// Drawn with LVGL lines rather than shipped as a bitmap, for three reasons:
// it follows the active theme (a fixed RGB565 image would stay turquoise in the
// red-preserving night mode), it stays crisp at any size, and it costs neither
// flash nor LittleFS space. Shape mirrors logo/nauticpi-icon.svg of the
// NauticPi project: a pi glyph above a two-crest wave.
//
// GOTCHA: lv_line_set_points() stores the POINTER, it does not copy. The arrays
// must outlive the line objects, hence static — fine here, there is exactly one
// boot screen and it is built once.
lv_obj_t *BootScreen::buildMark(lv_obj_t *parent, int size) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_bg_opa(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    const float s = (float)size;
    auto X = [&](float f) { return (lv_coord_t)(f * s + 0.5f); };

    auto mkLine = [&](lv_point_t *pts, uint16_t n, lv_color_t col, float wFrac) {
        lv_obj_t *l = lv_line_create(box);
        lv_line_set_points(l, pts, n);
        lv_obj_set_pos(l, 0, 0);
        lv_obj_set_style_line_width(l, (lv_coord_t)(wFrac * s + 0.5f), 0);
        lv_obj_set_style_line_color(l, col, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        return l;
    };

    // Coordinates transcribed from nauticpi-icon.svg and re-normalised so the
    // mark fills this box: the glyph is 1.6x wider than tall, so it is centred
    // vertically between y 0.22 and 0.78.

    // --- pi glyph: top bar, left leg, right leg ending in a curl -------------
    static lv_point_t barPts[2], legPts[2], curlPts[7];
    barPts[0] = { X(0.200f), X(0.220f) };
    barPts[1] = { X(0.779f), X(0.220f) };
    legPts[0] = { X(0.330f), X(0.220f) };
    legPts[1] = { X(0.330f), X(0.567f) };

    // Right leg: straight down, then a quadratic curl to the right — the
    // detail that makes the glyph read as a pi rather than a bracket.
    const float qx0 = 0.633f, qy0 = 0.505f;   // where the curl starts
    const float qx1 = 0.633f, qy1 = 0.577f;   // control (corner)
    const float qx2 = 0.715f, qy2 = 0.577f;   // tip
    curlPts[0] = { X(qx0), X(0.220f) };       // top of the leg
    for (int i = 0; i < 6; i++) {
        const float t = (float)i / 5.0f, u = 1.0f - t;
        curlPts[i + 1] = { X(u * u * qx0 + 2 * u * t * qx1 + t * t * qx2),
                           X(u * u * qy0 + 2 * u * t * qy1 + t * t * qy2) };
    }

    // --- wave: two cubics, crest then trough, crossing the pi's legs ---------
    // The crossing is deliberate (as in the signet): the wave passes in FRONT
    // of the legs, which is why it is drawn last.
    static lv_point_t wavePts[29];
    const float cub[2][8] = {
        { 0.050f, 0.674f, 0.222f, 0.556f, 0.404f, 0.556f, 0.576f, 0.674f },
        { 0.576f, 0.674f, 0.747f, 0.792f, 0.854f, 0.781f, 0.950f, 0.674f },
    };
    int n = 0;
    for (int c = 0; c < 2; c++) {
        for (int i = (c ? 1 : 0); i <= 14; i++) {   // skip the shared knot
            const float t = (float)i / 14.0f, u = 1.0f - t;
            const float b0 = u * u * u, b1 = 3 * u * u * t,
                        b2 = 3 * u * t * t, b3 = t * t * t;
            wavePts[n++] = {
                X(b0 * cub[c][0] + b1 * cub[c][2] + b2 * cub[c][4] + b3 * cub[c][6]),
                X(b0 * cub[c][1] + b1 * cub[c][3] + b2 * cub[c][5] + b3 * cub[c][7]) };
        }
    }

    mkLine(barPts,  2,            CLR_TEXT,   0.086f);
    mkLine(legPts,  2,            CLR_TEXT,   0.086f);
    mkLine(curlPts, 7,            CLR_TEXT,   0.086f);
    mkLine(wavePts, (uint16_t)n,  CLR_ACCENT, 0.070f);
    return box;
}

// ---- Intro animation ---------------------------------------------------------

void BootScreen::playIntro(const char *boatName) {
    // Zero unless the screen is taller than the design grid (portrait) - see
    // bootVShift(). Top offsets get it added, bottom offsets subtracted.
    const lv_coord_t vShift = bootVShift();

    // --- Spinner (rotating arc, always visible during boot) -------------------
    _spinner = lv_spinner_create(_scr, 1400, 80);   // 1400ms period, 80 deg arc
    lv_obj_set_size(_spinner, 200, 200);
    lv_obj_align(_spinner, LV_ALIGN_TOP_MID, 0, 60 + vShift);
    lv_obj_set_style_arc_color(_spinner, CLR_ACCENT,   LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_spinner, CLR_SURFACE,  LV_PART_MAIN);
    lv_obj_set_style_arc_width(_spinner, 6,  LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(_spinner, 6,  LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_spinner, 0, 0);
    lv_obj_set_style_opa(_spinner, 0, 0);   // start invisible

    // --- Logo or built-in icon inside the spinner circle ----------------------
    if (_logoImg) {
        // Custom logo: centred inside the spinner area
        lv_obj_align(_logoImg, LV_ALIGN_TOP_MID, 0, 100 + vShift);
        lv_obj_set_style_opa(_logoImg, 0, 0);   // start invisible, will fade in
    } else {
        // Built-in NauticPi signet, sized to sit inside the 200 px spinner.
        lv_obj_t *mark = buildMark(_scr, 120);
        lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 100 + vShift);
        lv_obj_set_style_opa(mark, 0, 0);
        _logoImg = mark;   // reuse pointer for fade-in
    }

    // --- Title ----------------------------------------------------------------
    // Configurable (web UI -> Boot logo); defaults to the project name.
    lv_obj_t *title = lv_label_create(_scr);
    lv_label_set_text(title, appConfig.cfg.bootTitle[0] ? appConfig.cfg.bootTitle
                                                        : "NauticPinnace");
    lv_obj_set_style_text_font(title, FONT_XL, 0);
    lv_obj_set_style_text_color(title, CLR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 278 + vShift);
    lv_obj_set_style_opa(title, 0, 0);

    // --- Boat name (optional) -------------------------------------------------
    lv_obj_t *bname = nullptr;
    if (boatName && strlen(boatName) > 0) {
        bname = lv_label_create(_scr);
        lv_label_set_text(bname, boatName);
        lv_obj_set_style_text_font(bname, FONT_LARGE, 0);
        lv_obj_set_style_text_color(bname, CLR_ACCENT, 0);
        lv_obj_align(bname, LV_ALIGN_TOP_MID, 0, 318 + vShift);
        lv_obj_set_style_opa(bname, 0, 0);
    }

    // --- Footer ---------------------------------------------------------------
    lv_obj_t *ver = lv_label_create(_scr);
    // From src/Version.h, not typed out again: this line and the NMEA 2000
    // product information had already drifted apart ("v1.0" here against
    // "1.0.0" on the bus) before they were both pointed at one definition.
    lv_label_set_text(ver, "v" FW_VERSION "  |  NMEA 2000");
    lv_obj_set_style_text_font(ver, FONT_TINY, 0);
    lv_obj_set_style_text_color(ver, CLR_TEXT_DIM, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -14 - vShift);
    lv_obj_set_style_opa(ver, 0, 0);

    // ---- Animate: manual pump over ~1.2 seconds in small steps ---------------
    // We drive the clock manually (LVGL task isn't running yet).
    // Steps: fade in spinner, then icon+title, then footer.

    const int STEP = 16;   // ms per frame ~ 60 fps

    // Phase A (0-300ms): spinner fades in
    for (int t = 0; t <= 300; t += STEP) {
        lv_opa_t opa = (lv_opa_t)((uint32_t)t * 255 / 300);
        lv_obj_set_style_opa(_spinner, opa, 0);
        BOOT_TICK(STEP);
        lv_timer_handler();
        delay(STEP);
    }
    lv_obj_set_style_opa(_spinner, LV_OPA_COVER, 0);

    // Phase B (300-700ms): logo/icon + title fade in, slide up 20px
    for (int t = 0; t <= 400; t += STEP) {
        lv_opa_t opa  = (lv_opa_t)((uint32_t)t * 255 / 400);
        lv_coord_t dy = (lv_coord_t)(20 - (20 * t / 400));
        lv_obj_set_style_opa(_logoImg, opa, 0);
        lv_obj_set_style_opa(title,    opa, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 278 + vShift + dy);
        if (bname) {
            lv_obj_set_style_opa(bname, opa, 0);
            lv_obj_align(bname, LV_ALIGN_TOP_MID, 0, 318 + vShift + dy);
        }
        BOOT_TICK(STEP);
        lv_timer_handler();
        delay(STEP);
    }
    lv_obj_set_style_opa(_logoImg, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(title,    LV_OPA_COVER, 0);
    if (bname) lv_obj_set_style_opa(bname, LV_OPA_COVER, 0);

    // Phase C (700-1000ms): footer fades in, progress bar area prepared
    for (int t = 0; t <= 300; t += STEP) {
        lv_opa_t opa = (lv_opa_t)((uint32_t)t * 255 / 300);
        lv_obj_set_style_opa(ver, opa, 0);
        BOOT_TICK(STEP);
        lv_timer_handler();
        delay(STEP);
    }
    lv_obj_set_style_opa(ver, LV_OPA_COVER, 0);
}

// ---- Public API -------------------------------------------------------------

void BootScreen::show(const char *boatName) {
    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(_scr, OPA_FULL, 0);
    lv_scr_load(_scr);
    tick(10);

    // Try to load custom logo before building the screen
    bool hasLogo = loadCustomLogo();
    if (hasLogo) {
        _logoImg = lv_img_create(_scr);
        lv_img_set_src(_logoImg, &_logoDesc);
        lv_obj_set_size(_logoImg, _logoDesc.header.w, _logoDesc.header.h);
    }

    // Play the intro animation (spinner + logo + title fade-in, ~1.2 s).
    playIntro(boatName);

    // ---- Progress bar (appears after intro) ----------------------------------
    // Same shift as the top cluster, mirrored: 0 in landscape, half the surplus
    // height in portrait, so this group moves up toward the logo group.
    const lv_coord_t vShift = bootVShift();

    _statusLbl = lv_label_create(_scr);
    lv_label_set_text(_statusLbl, T(STR_BOOT_INIT));
    lv_obj_set_style_text_font(_statusLbl, FONT_MED, 0);
    lv_obj_set_style_text_color(_statusLbl, CLR_TEXT_DIM, 0);
    // Up to three centred lines, growing UPWARD from the bar: the alignment is
    // a style in LVGL 8, re-applied whenever the label's height changes, so
    // the bottom edge stays put at -70. Three lines is what the WiFi fallback
    // message needs (reason / own hotspot / address); they end below the boat
    // name on every panel (4": lines from y 356, name ends ~347). Wider than
    // the 340 px bar, because a network name can be 32 characters long; the
    // old LONG_DOT cut every one of those messages off after a few words.
    lv_obj_align(_statusLbl, LV_ALIGN_BOTTOM_MID, 0, -70 - vShift);
    lv_label_set_long_mode(_statusLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_statusLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_statusLbl, statusWidth());
    // ...but only with the DEFAULT fonts. The medium and large fonts can be
    // enlarged in the web interface, and with FONT_MED at 20 px the three
    // lines reach into the boat name on the 4"; at 24 px the third wraps as
    // well. So the room is measured here - from the bottom of the boat name
    // (or of the title, without one) down to the label's bottom edge, less a
    // small gap - and setStatusFitted() steps down to FONT_SMALL for a text
    // that does not fit it.
    {
        const bool hasName = boatName && boatName[0];
        const lv_coord_t clusterBottom = hasName
            ? 318 + vShift + lv_font_get_line_height(FONT_LARGE)
            : 278 + vShift + lv_font_get_line_height(FONT_XL);
        _statusMaxH = (uiScreenH() - 70 - vShift) - clusterBottom - 6;
    }

    _bar = lv_bar_create(_scr);
    lv_obj_set_size(_bar, 340, 8);
    // Right above the percentage below it, measured from that label's font
    // (+2: the line box carries a little room above the digits). That is the
    // old fixed -50 on the 4" (14 px font, line 16); with the 18 px font of
    // the 1024x600 boards -50 put the bar 7 px into "100%" - unnoticed for as
    // long as the bar was never drawn at all (see pump()).
    const lv_coord_t barBottom = -36 + 2 - lv_font_get_line_height(FONT_SMALL);
    lv_obj_align(_bar, LV_ALIGN_BOTTOM_MID, 0, barBottom - vShift);
    lv_bar_set_range(_bar, 0, 100);
    lv_bar_set_value(_bar, 0, LV_ANIM_OFF);
    // The track opaque, in the spinner ring's colour: the default theme gives
    // a bar's MAIN part only 20 % opacity, and at 20 % CLR_SURFACE all but
    // vanishes into CLR_BG - the 10-30 % indicator then read as a stray stub
    // left of centre instead of the start of a 340 px rail.
    lv_obj_set_style_bg_opa(_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(_bar, CLR_SURFACE, 0);
    lv_obj_set_style_bg_color(_bar, CLR_ACCENT,  LV_PART_INDICATOR);
    lv_obj_set_style_radius(_bar, 4, 0);
    lv_obj_set_style_radius(_bar, 4, LV_PART_INDICATOR);

    _pctLbl = lv_label_create(_scr);
    lv_label_set_text(_pctLbl, "0%");
    lv_obj_set_style_text_font(_pctLbl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_pctLbl, CLR_TEXT_DIM, 0);
    lv_obj_align(_pctLbl, LV_ALIGN_BOTTOM_MID, 0, -36 - vShift);

    tick(30);
    // From here on pump() keeps the LVGL clock itself (real elapsed time), so
    // it needs a starting point - and the status line and the bar get drawn
    // at all: until now nothing rendered them on the device (see pump()).
    _lastPumpMs = millis();
    pump();
}

void BootScreen::update(const char *statusMsg, uint8_t progress) {
    if (!_scr) return;
    setStatusFitted(statusMsg);
    if (_bar)       lv_bar_set_value(_bar, progress, LV_ANIM_OFF);
    if (_pctLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", progress);
        lv_label_set_text(_pctLbl, buf);
    }
    pump();
    vTaskDelay(pdMS_TO_TICKS(10));
}

void BootScreen::setStatus(const char *text) {
    if (!_statusLbl || !text) return;
    // Called every ~50 ms with the same text while WiFi waits. Setting it
    // anyway would invalidate the label and redraw it on every frame.
    if (strcmp(lv_label_get_text(_statusLbl), text) != 0)
        lv_label_set_text(_statusLbl, text);
}

// Does `text` fit the status label's room (see show()) in `font`?
bool BootScreen::statusFits(const char *text, const lv_font_t *font) const {
    lv_point_t sz;
    lv_txt_get_size(&sz, text, font, 0, 0, statusWidth(), LV_TEXT_FLAG_NONE);
    return sz.y <= _statusMaxH;
}

void BootScreen::setStatusFont(const lv_font_t *font) {
    if (_statusLbl && lv_obj_get_style_text_font(_statusLbl, LV_PART_MAIN) != font)
        lv_obj_set_style_text_font(_statusLbl, font, 0);
}

void BootScreen::setStatusFitted(const char *text) {
    if (!_statusLbl || !text) return;
    setStatusFont(statusFits(text, FONT_MED) ? FONT_MED : FONT_SMALL);
    setStatus(text);
}

// The text for one WiFi phase, laid out for `font`; returns the bar's percent.
// Composed from fragments, so the format specifiers stay here and out of the
// translation tables (I18n.h). Word order is the same in both languages; only
// the quotes differ, and those come from the table too.
static uint8_t wifiText(char *txt, size_t n, const WifiProgress &p, const lv_font_t *f) {
    const char *qo = T(STR_QUOTE_OPEN);
    const char *qc = T(STR_QUOTE_CLOSE);
    uint8_t pct = 10;
    switch (p.phase) {
        case WifiProgress::CONNECTING:
            snprintf(txt, n, "%s %s%s%s ... (%s %u/%u)",
                     T(STR_BOOT_WIFI_CONNECTING), qo, p.ssid, qc,
                     T(STR_BOOT_WIFI_ATTEMPT), (unsigned)p.attempt, (unsigned)p.attempts);
            break;
        case WifiProgress::SCANNING:
            snprintf(txt, n, "%s %s%s%s ...", T(STR_BOOT_WIFI_SCANNING), qo, p.ssid, qc);
            break;
        case WifiProgress::CONNECTED:
            // With the scheme: it is what has to go into the browser, and a
            // bare IP typed into a phone's address bar often ends up as a
            // search instead.
            snprintf(txt, n, "%s: http://%s", T(STR_CFG_WIFI_CONNECTED), p.ip);
            pct = 20;
            break;
        case WifiProgress::FALLBACK: {
            // Three lines: why, what instead, where to reach it - and that it
            // is not final. The code is printed whenever the driver gave one;
            // it is what a forum answer needs. Lines 1 and 2 are kept to one
            // line each (lineWithName), three is all the space there is. The
            // hotspot is called what the settings call the section with its
            // password and QR code, so the user finds it there.
            const lv_coord_t w = statusWidth();
            char pre[40], suf[64], l1[96], l2[96];
            snprintf(pre, sizeof pre, "%s ", T(STR_CFG_WIFI_SHORT));
            if (p.reason)
                snprintf(suf, sizeof suf, ": %s (%u)", WifiReason::text(p.reason, p.seen),
                         (unsigned)p.reason);
            else
                snprintf(suf, sizeof suf, ": %s", WifiReason::text(p.reason, p.seen));
            lineWithName(l1, sizeof l1, pre, p.ssid, suf, f, w);
            snprintf(pre, sizeof pre, "%s ", T(STR_CFG_HOTSPOT_SECTION));
            snprintf(suf, sizeof suf, " %s", T(STR_BOOT_WIFI_ACTIVE));
            lineWithName(l2, sizeof l2, pre, p.apSsid, suf, f, w);
            snprintf(txt, n, "%s\n%s\nhttp://%s – %s %u s", l1, l2, p.ip,
                     T(STR_BOOT_WIFI_RETRY_IN), (unsigned)p.retryInS);
            pct = 20;
            break;
        }
    }
    return pct;
}

void BootScreen::wifi(const WifiProgress &p) {
    if (!_scr) return;
    // Laid out in FONT_MED; only if that does not fit the room between the
    // boat name and the bar (enlarged fonts, see show()) once more in
    // FONT_SMALL - the fallback lines are shortened per font, so they are
    // composed again rather than just drawn smaller.
    char txt[256];
    const lv_font_t *f = FONT_MED;
    uint8_t pct = wifiText(txt, sizeof txt, p, f);
    if (!statusFits(txt, f)) {
        f = FONT_SMALL;
        pct = wifiText(txt, sizeof txt, p, f);
    }
    setStatusFont(f);
    setStatus(txt);
    if (_bar) lv_bar_set_value(_bar, pct, LV_ANIM_OFF);
    if (_pctLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
        if (strcmp(lv_label_get_text(_pctLbl), buf) != 0) lv_label_set_text(_pctLbl, buf);
    }
    pump();
}

void BootScreen::hold(uint32_t ms) {
    if (!_scr) return;
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        pump();
        delay(40);
    }
}

// One frame of the boot screen, from setup().
//
// Until now the boot screen rendered only during its intro (playIntro() runs
// lv_timer_handler() for ~1 s). Everything after it - the status line, the
// bar, "Lade Bildschirme...", "Bereit." - was set but never drawn: the panel
// kept showing the last intro frame until the main UI replaced it. That was
// tolerable while the boot screen came up AFTER the WiFi join; now it comes
// first and has to say what the device is waiting for.
//
// lv_refr_now(), NOT lv_timer_handler(): it runs exactly two things, the
// animations (the spinner) and the display refresh. lv_timer_handler() would
// also read the touch controller and run every other LVGL timer - and the
// touch callback acts on the instruments (a swipe calls dispMgr.nextScreen()),
// which do not exist yet. That is the "spurious events scramble object
// state" that tick() below warns about; lv_refr_now() cannot cause it.
//
// The LVGL clock: on the 7B LV_TICK_CUSTOM reads millis() itself; on the 4"
// the tick task only starts after setup(), so the clock is advanced here by
// the time that really passed, which keeps the spinner at its true speed no
// matter how long the WiFi code took between two calls.
void BootScreen::pump() {
    if (!_scr) return;
#if !LV_TICK_CUSTOM
    const uint32_t now = millis();
    BOOT_TICK(now - _lastPumpMs);
    _lastPumpMs = now;
#endif
    lv_refr_now(NULL);
}

void BootScreen::dismiss() {
    if (!_scr) return;

    // Deleted immediately, without a fade-out: tick() deliberately does not run
    // lv_timer_handler() (see its comment — calling it from setup() scrambles
    // object state), so an opacity ramp here would never be rendered. It would
    // just delay activate() by ~400 ms for an invisible animation.
    lv_obj_del(_scr);
    _scr = nullptr;

    // Free custom logo buffer if allocated
    if (_logoBuf) {
        heap_caps_free(_logoBuf);
        _logoBuf = nullptr;
    }
}

void BootScreen::tick(uint32_t ms) {
    // Drive the LVGL clock but do NOT call lv_timer_handler() here.
    // The flush callback sends pixels from DRAM buffers, which is safe, but
    // calling lv_timer_handler() from setup() (before the LVGL task is running)
    // causes spurious events that can scramble object internal state.
    // Frames before activate() come from pump() (lv_refr_now, no input
    // processing); from loop() on, displayTick() renders.
    BOOT_TICK(ms);
    delay(ms);
}
