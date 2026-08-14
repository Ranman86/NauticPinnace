#include "AutopilotScreen.h"
#include "../../PsramArena.h"
#include "../UiConfig.h"
#include "../CanvasDraw.h"
#include <math.h>
#include <string.h>

// ── colour aliases for this screen ──────────────────────────────────────────
#define C_ARC_BG    (uiTheme.apCompassBg)    // dark ring background
#define C_TICK_MJ   (uiTheme.apCompassMaj)  // major tick
#define C_TICK_MN   (uiTheme.apCompassMin)  // minor tick
#define C_TARGET    (uiTheme.apTarget)        // target arc / heading number

static constexpr float DEG2RAD = (float)M_PI / 180.f;

// ── helpers ──────────────────────────────────────────────────────────────────
static inline lv_point_t sc(float cx, float cy, float r, float deg) {
    float a = deg * DEG2RAD;
    return { (lv_coord_t)(cx + r * sinf(a)),
             (lv_coord_t)(cy - r * cosf(a)) };
}

static void cline(lv_obj_t *cv,
                  float x1, float y1, float x2, float y2,
                  lv_color_t c, lv_coord_t w = 1, lv_opa_t o = LV_OPA_COVER) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = c; d.width = w; d.opa = o;
    lv_point_t p[2] = {{(lv_coord_t)x1,(lv_coord_t)y1},{(lv_coord_t)x2,(lv_coord_t)y2}};
    lv_canvas_draw_line(cv, p, 2, &d);
}

// ── create ───────────────────────────────────────────────────────────────────
void AutopilotScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H - NAV_BAR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, OPA_FULL, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Canvas for compass arc (top portion, full width, 160px tall)
    size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(CW, UI_AP_COMPASS_H);
    if (!_cbuf) _cbuf = (lv_color_t*)PsramArena::alloc(sz);   // reuse on live theme rebuild
    if (_cbuf) {
        _canvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_canvas, _cbuf, CW, UI_AP_COMPASS_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(_canvas, 0, 0);
    }

    // ── "Set Heading" caption ──────────────────────────────────────────────
    _lblTargetLbl = lv_label_create(container);
    lv_label_set_text(_lblTargetLbl, T(STR_AP_SET_HEADING));
    styleLabel(_lblTargetLbl, FONT_MED, CLR_TEXT_DIM);
    lv_obj_align(_lblTargetLbl, LV_ALIGN_TOP_MID, 0, UI_AP_CAPTION_Y);

    // ── HUGE target heading number ─────────────────────────────────────────
    _lblTarget = lv_label_create(container);
    lv_label_set_text(_lblTarget, "---°T");
    lv_obj_set_style_text_font(_lblTarget, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(_lblTarget, C_TARGET, 0);
    lv_obj_align(_lblTarget, LV_ALIGN_TOP_MID, 0, UI_AP_TARGET_Y);

    // ── Mode badge (left side, below compass) ─────────────────────────────
    _modeBox = lv_obj_create(container);
    lv_obj_set_size(_modeBox, UI_AP_MODE_W, UI_AP_MODE_H);
    lv_obj_set_pos(_modeBox, UI_AP_MODE_X, UI_AP_MODE_Y);
    lv_obj_set_style_bg_color(_modeBox, (uiTheme.apModeActive), 0);
    lv_obj_set_style_bg_opa(_modeBox, OPA_FULL, 0);
    lv_obj_set_style_border_color(_modeBox, CLR_GREEN, 0);
    lv_obj_set_style_border_width(_modeBox, UI_AP_MODE_BORDER_W, 0);
    lv_obj_set_style_radius(_modeBox, UI_AP_MODE_RADIUS, 0);
    lv_obj_clear_flag(_modeBox, LV_OBJ_FLAG_SCROLLABLE);

    _lblMode = lv_label_create(_modeBox);
    lv_label_set_text(_lblMode, "STANDBY");
    styleLabel(_lblMode, FONT_MED, CLR_GREEN);
    lv_obj_align(_lblMode, LV_ALIGN_CENTER, 0, 0);

    // ── Bottom info cards ──────────────────────────────────────────────────
    // Current HDG | Deviation | Rudder
    int cw = UI_AP_CARD_W, ch = UI_AP_CARD_H, gap = UI_AP_CARD_GAP;
    int y = UI_AP_CARDS_Y;
    auto card = [&](int x, const char *lbl, lv_obj_t **vl, lv_color_t col = CLR_TEXT) {
        lv_obj_t *c = lv_obj_create(container);
        lv_obj_set_size(c, cw, ch); lv_obj_set_pos(c, x, y); styleCard(c);
        lv_obj_t *ll = lv_label_create(c); lv_label_set_text(ll, lbl);
        styleLabel(ll, FONT_SMALL, CLR_TEXT_DIM); lv_obj_align(ll, LV_ALIGN_TOP_MID, 0, 2);
        *vl = lv_label_create(c); lv_label_set_text(*vl, "--");
        lv_obj_set_style_text_font(*vl, &lv_font_montserrat_40, 0);
        lv_obj_set_style_text_color(*vl, col, 0);
        lv_obj_align(*vl, LV_ALIGN_CENTER, 0, 4);
    };
    // "HDG" is the standard abbreviation in both languages — not translated.
    card(gap,             "HDG",                &_lblHdgCard, CLR_TEXT);
    card(gap + cw + gap,  T(STR_AP_DEVIATION),  &_lblDevCard, CLR_YELLOW);
    card(gap+(cw+gap)*2,  T(STR_AP_RUDDER),     &_lblRudCard, CLR_TEXT_DIM);

    // ── Deviation bar ──────────────────────────────────────────────────────
    // A thin horizontal bar centred below the cards showing left/right deviation
    // Use the UI_AP_DEVBAR_* constants (the size was hard-coded here, so the
    // theme-configurable apDevBarH never had any effect).
    lv_obj_t *devBarBg = lv_obj_create(container);
    lv_obj_set_size(devBarBg, SCREEN_W - 16, UI_AP_DEVBAR_H);
    lv_obj_set_pos(devBarBg, 8, UI_AP_DEVBAR_Y);
    lv_obj_set_style_bg_color(devBarBg, (uiTheme.apDevBar), 0);
    lv_obj_set_style_bg_opa(devBarBg, OPA_FULL, 0);
    lv_obj_set_style_border_color(devBarBg, CLR_BORDER, 0);
    lv_obj_set_style_border_width(devBarBg, 1, 0);
    lv_obj_set_style_radius(devBarBg, 4, 0);
    lv_obj_clear_flag(devBarBg, LV_OBJ_FLAG_SCROLLABLE);
}

// ── drawCompassArc ────────────────────────────────────────────────────────────
// Draws a curved compass band showing ±50° around current heading.
// The arc pivot is below the canvas → only the upper sweep is visible.
void AutopilotScreen::drawCompassArc(float heading, float target) {
    if (!_canvas || !_cbuf) return;

    // Clear
    {
        lv_color_t *p = _cbuf;
        for (int row = 0; row < UI_AP_COMPASS_H; row += 8) {
            int rows = (UI_AP_COMPASS_H - row < 8) ? UI_AP_COMPASS_H - row : 8;
            lv_color_t *end = p + (size_t)rows * CW;
            while (p < end) *p++ = CLR_BG;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        lv_obj_invalidate(_canvas);
    }

    // Arc pivot is below canvas bottom, creating a wide shallow curve
    float pivotX = CW / 2.0f;
    float pivotY = UI_AP_PIVOT_Y;          // far below → gentle curve
    float R_outer = UI_AP_R_OUTER;         // outer track edge
    float R_inner = UI_AP_R_INNER;         // inner track edge
    float R_mid   = (R_outer + R_inner) / 2.0f;
    float span    = UI_AP_SPAN_DEG;        // show ±50° around heading

    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);

    // Helper: bearing relative to heading → screen point
    auto bp = [&](float bearingOffset, float r) -> lv_point_t {
        // convert bearing offset to angle from pivot
        // bearing 0° = top of circle (pivot to top)
        // We map: offset 0 → directly above pivot
        float ang = -(90.0f + bearingOffset);  // 0° heading offset → angle pointing up
        float rad = ang * DEG2RAD;
        return {
            (lv_coord_t)(pivotX + r * cosf(-bearingOffset * DEG2RAD)),
            (lv_coord_t)(pivotY - r * cosf(bearingOffset * DEG2RAD * 0.0f) +
                          r * sinf(bearingOffset * DEG2RAD) * 0.0f)
        };
    };

    // Better helper using the actual pivot geometry
    auto arcPt = [&](float offsetDeg, float r) -> lv_point_t {
        // offsetDeg: degrees from "heading direction" (+ = clockwise)
        // Map to angle in screen coords: heading direction = straight up from pivot
        float a = offsetDeg;  // angle around pivot in degrees
        // screen x = pivotX + r*sin(a), screen y = pivotY - r*cos(a)
        float rad = a * DEG2RAD;
        return {
            (lv_coord_t)(pivotX + r * sinf(rad)),
            (lv_coord_t)(pivotY - r * cosf(rad))
        };
    };

    // Band + highlight are real filled ring sectors (were chains of thick 1°
    // strokes, which left the inner/outer edges scalloped).
    // arcPt: offset 0 = straight up from the pivot → LVGL angle = offset − 90.
    auto lvA = [](float offsetDeg) { return offsetDeg - 90.0f; };

    cdFillRing(_canvas, pivotX, pivotY, R_inner, R_outer, lvA(-span), lvA(span), C_ARC_BG);

    // Target heading arc highlight
    if (!isnan(target) && !isnan(heading)) {
        float tOff = target - heading;
        while (tOff >  180.f) tOff -= 360.f;
        while (tOff < -180.f) tOff += 360.f;
        if (fabsf(tOff) <= span + 8) {
            cdFillRing(_canvas, pivotX, pivotY, R_inner, R_outer,
                       lvA(tOff - 3.f), lvA(tOff + 3.f), C_TARGET);
            // Target marker: solid triangle sitting on the band's outer edge.
            // Sized in PIXELS, not degrees — the pivot is ~600 px away, so the
            // old ±4° base spanned ~84 px against a 9 px height (an unreadable
            // sliver once it is actually filled).
            lv_point_t b   = arcPt(tOff, R_outer + 2);
            lv_point_t tip = { b.x, (lv_coord_t)(b.y - 15) };
            lv_point_t bl  = { (lv_coord_t)(b.x - 10), b.y };
            lv_point_t br  = { (lv_coord_t)(b.x + 10), b.y };
            cdFillTri(_canvas, tip, bl, br, C_TARGET);
        }
    }

    // Tick marks and labels
    td.opa = OPA_FULL;
    for (int offs = -(int)span; offs <= (int)span; offs += 5) {
        bool cardinal5 = (offs % 30 == 0);
        bool major      = (offs % 10 == 0);
        float r1 = cardinal5 ? R_inner - 14 : major ? R_inner - 8 : R_inner - 4;
        float r2 = R_inner - 1;
        ld.color = cardinal5 ? CLR_TEXT : major ? C_TICK_MJ : C_TICK_MN;
        ld.width = cardinal5 ? 3 : 1; ld.opa = OPA_FULL;
        lv_point_t p1 = arcPt((float)offs, r1);
        lv_point_t p2 = arcPt((float)offs, r2);
        lv_point_t _l[2] = {p1, p2};
        lv_canvas_draw_line(_canvas, _l, 2, &ld);

        // Degree labels for major ticks. Only with a known heading: NaN + offs is
        // NaN, both normalise loops are skipped (every NaN comparison is false)
        // and (int)roundf(NaN) saturates to INT32_MAX — which printed as the
        // bogus "2147483" (truncated into tb[8]) when no compass is on the bus.
        // Also skipped when the glyph box would cross the canvas edge.
        if (major && !isnan(heading)) {
            float bear = heading + (float)offs;
            while (bear < 0) bear += 360.f;
            while (bear >= 360) bear -= 360.f;
            char tb[8]; snprintf(tb, sizeof(tb), "%d", (int)roundf(bear));
            td.font = FONT_SMALL; td.color = cardinal5 ? CLR_TEXT : C_TICK_MJ;
            lv_point_t lp = arcPt((float)offs, r1 - 14);
            lv_coord_t top = lp.y - 8, lineH = 18;
            if (top >= 0 && top + lineH <= UI_AP_COMPASS_H && lp.x >= 16 && lp.x <= CW - 16)
                lv_canvas_draw_text(_canvas, lp.x - 14, top, 32, &td, tb);
        }
    }

    // Heading indicator: RED triangle at the centre of the arc. Solid fill —
    // the old hand-rolled 14-line "fan" scaled x by 0.01*(13-i) but y by
    // (13-i)/13, so it never converged on the tip: it smeared past one edge and
    // left a wedge of the other unpainted.
    {
        // Pixel-sized for the same reason as the target marker above.
        lv_point_t b   = arcPt(0.0f, R_inner + 14);
        lv_point_t tip = { b.x, (lv_coord_t)(b.y + 18) };
        lv_point_t bl  = { (lv_coord_t)(b.x - 13), b.y };
        lv_point_t br  = { (lv_coord_t)(b.x + 13), b.y };
        cdFillTri(_canvas, tip, bl, br, CLR_RED);
        ld.color = CLR_RED; ld.width = 2; ld.opa = OPA_FULL;
        lv_point_t _ta[2] = {tip, bl}; lv_canvas_draw_line(_canvas, _ta, 2, &ld);
        lv_point_t _tb[2] = {tip, br}; lv_canvas_draw_line(_canvas, _tb, 2, &ld);
        lv_point_t _tc[2] = {bl, br};  lv_canvas_draw_line(_canvas, _tc, 2, &ld);
    }

    // Current heading value above the indicator triangle
    if (!isnan(heading)) {
        char hbuf[8]; snprintf(hbuf, sizeof(hbuf), "%.0f", heading);
        td.font = FONT_MED; td.color = CLR_TEXT; td.opa = OPA_FULL;
        lv_point_t lp = arcPt(0.0f, R_inner + 24);
        lv_canvas_draw_text(_canvas, lp.x - 18, lp.y - 8, 40, &td, hbuf);
    }

    lv_obj_invalidate(_canvas);
}

// ── update ───────────────────────────────────────────────────────────────────
void AutopilotScreen::update() {
    float hdg, target, rudder;
    bool  engaged;
    uint8_t mode;
    {
        auto lk = data.lock();
        hdg     = isnan(data.hdg) ? data.cog : data.hdg;
        target  = data.apTargetHeading;
        rudder  = data.apRudder;
        engaged = data.apEngaged;
        mode    = data.apMode;
    }

    // ── Mode badge ──────────────────────────────────────────────────────────
    const char *modeStrs[] = {"STANDBY","HEADING","WIND","TRACK"};
    const char *modeStr = (mode < 4) ? modeStrs[mode] : "?";
    lv_label_set_text(_lblMode, modeStr);

    if (!engaged) {
        lv_obj_set_style_bg_color(_modeBox, (uiTheme.apModeStandby), 0);
        lv_obj_set_style_border_color(_modeBox, CLR_TEXT_DIM, 0);
        lv_obj_set_style_text_color(_lblMode, CLR_TEXT_DIM, 0);
    } else if (mode == 1) {  // HEADING
        lv_obj_set_style_bg_color(_modeBox, (uiTheme.apModeActive), 0);
        lv_obj_set_style_border_color(_modeBox, CLR_GREEN, 0);
        lv_obj_set_style_text_color(_lblMode, CLR_GREEN, 0);
    } else {
        lv_obj_set_style_bg_color(_modeBox, (uiTheme.apModeManual), 0);
        lv_obj_set_style_border_color(_modeBox, CLR_ACCENT, 0);
        lv_obj_set_style_text_color(_lblMode, CLR_ACCENT, 0);
    }

    // ── Large target heading ────────────────────────────────────────────────
    if (!isnan(target)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f\xC2\xB0T", target);   // °T
        lv_label_set_text(_lblTarget, buf);
    } else {
        lv_label_set_text(_lblTarget, "---°T");
    }

    // ── Bottom cards ────────────────────────────────────────────────────────
    char buf[16];
    if (!isnan(hdg)) {
        snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", hdg);
        lv_label_set_text(_lblHdgCard, buf);
    } else {
        lv_label_set_text(_lblHdgCard, "--°");
    }

    if (!isnan(hdg) && !isnan(target)) {
        float dev = target - hdg;
        while (dev >  180.f) dev -= 360.f;
        while (dev < -180.f) dev += 360.f;
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", dev);
        lv_label_set_text(_lblDevCard, buf);
        lv_color_t dc = (fabsf(dev) > UI_AP_DEV_THRESH_HI) ? CLR_ORANGE :
                         (fabsf(dev) > UI_AP_DEV_THRESH_LO)  ? CLR_YELLOW : CLR_GREEN;
        lv_obj_set_style_text_color(_lblDevCard, dc, 0);
    } else {
        lv_label_set_text(_lblDevCard, "--°");
    }

    if (!isnan(rudder)) {
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", rudder);
        lv_label_set_text(_lblRudCard, buf);
        lv_color_t rc = (rudder < -1.0f) ? CLR_PORT :
                         (rudder >  1.0f) ? CLR_STARBOARD : CLR_TEXT_DIM;
        lv_obj_set_style_text_color(_lblRudCard, rc, 0);
    } else {
        lv_label_set_text(_lblRudCard, "--°");
    }

    // ── Compass arc canvas ───────────────────────────────────────────────────
    drawCompassArc(hdg, target);
}
