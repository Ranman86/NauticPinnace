#include "BootScreen.h"
#include "../i18n/I18n.h"
#include "../config/Config.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <math.h>

BootScreen bootScreen;

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
    // --- Spinner (rotating arc, always visible during boot) -------------------
    _spinner = lv_spinner_create(_scr, 1400, 80);   // 1400ms period, 80 deg arc
    lv_obj_set_size(_spinner, 200, 200);
    lv_obj_align(_spinner, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_arc_color(_spinner, CLR_ACCENT,   LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_spinner, CLR_SURFACE,  LV_PART_MAIN);
    lv_obj_set_style_arc_width(_spinner, 6,  LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(_spinner, 6,  LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_spinner, 0, 0);
    lv_obj_set_style_opa(_spinner, 0, 0);   // start invisible

    // --- Logo or built-in icon inside the spinner circle ----------------------
    if (_logoImg) {
        // Custom logo: centred inside the spinner area
        lv_obj_align(_logoImg, LV_ALIGN_TOP_MID, 0, 100);
        lv_obj_set_style_opa(_logoImg, 0, 0);   // start invisible, will fade in
    } else {
        // Built-in NauticPi signet, sized to sit inside the 200 px spinner.
        lv_obj_t *mark = buildMark(_scr, 120);
        lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 100);
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 278);
    lv_obj_set_style_opa(title, 0, 0);

    // --- Boat name (optional) -------------------------------------------------
    lv_obj_t *bname = nullptr;
    if (boatName && strlen(boatName) > 0) {
        bname = lv_label_create(_scr);
        lv_label_set_text(bname, boatName);
        lv_obj_set_style_text_font(bname, FONT_LARGE, 0);
        lv_obj_set_style_text_color(bname, CLR_ACCENT, 0);
        lv_obj_align(bname, LV_ALIGN_TOP_MID, 0, 318);
        lv_obj_set_style_opa(bname, 0, 0);
    }

    // --- Footer ---------------------------------------------------------------
    lv_obj_t *ver = lv_label_create(_scr);
    lv_label_set_text(ver, "v1.0  |  NMEA 2000");
    lv_obj_set_style_text_font(ver, FONT_TINY, 0);
    lv_obj_set_style_text_color(ver, CLR_TEXT_DIM, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_opa(ver, 0, 0);

    // ---- Animate: manual pump over ~1.2 seconds in small steps ---------------
    // We drive the clock manually (LVGL task isn't running yet).
    // Steps: fade in spinner, then icon+title, then footer.

    const int STEP = 16;   // ms per frame ~ 60 fps

    // Phase A (0-300ms): spinner fades in
    for (int t = 0; t <= 300; t += STEP) {
        lv_opa_t opa = (lv_opa_t)((uint32_t)t * 255 / 300);
        lv_obj_set_style_opa(_spinner, opa, 0);
        lv_tick_inc(STEP);
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
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 278 + dy);
        if (bname) {
            lv_obj_set_style_opa(bname, opa, 0);
            lv_obj_align(bname, LV_ALIGN_TOP_MID, 0, 318 + dy);
        }
        lv_tick_inc(STEP);
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
        lv_tick_inc(STEP);
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
    _statusLbl = lv_label_create(_scr);
    lv_label_set_text(_statusLbl, T(STR_BOOT_INIT));
    lv_obj_set_style_text_font(_statusLbl, FONT_MED, 0);
    lv_obj_set_style_text_color(_statusLbl, CLR_TEXT_DIM, 0);
    lv_obj_align(_statusLbl, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_label_set_long_mode(_statusLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_statusLbl, 340);

    _bar = lv_bar_create(_scr);
    lv_obj_set_size(_bar, 340, 8);
    lv_obj_align(_bar, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_bar_set_range(_bar, 0, 100);
    lv_bar_set_value(_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_bar, CLR_SURFACE, 0);
    lv_obj_set_style_bg_color(_bar, CLR_ACCENT,  LV_PART_INDICATOR);
    lv_obj_set_style_radius(_bar, 4, 0);
    lv_obj_set_style_radius(_bar, 4, LV_PART_INDICATOR);

    _pctLbl = lv_label_create(_scr);
    lv_label_set_text(_pctLbl, "0%");
    lv_obj_set_style_text_font(_pctLbl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_pctLbl, CLR_TEXT_DIM, 0);
    lv_obj_align(_pctLbl, LV_ALIGN_BOTTOM_MID, 0, -36);

    tick(30);
}

void BootScreen::update(const char *statusMsg, uint8_t progress) {
    if (!_scr) return;
    if (_statusLbl) lv_label_set_text(_statusLbl, statusMsg);
    if (_bar)       lv_bar_set_value(_bar, progress, LV_ANIM_OFF);
    if (_pctLbl) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", progress);
        lv_label_set_text(_pctLbl, buf);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
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
    // Rendering starts from loop() via displayTick() after activate().
    lv_tick_inc(ms);
    delay(ms);
}
