#include "RudderScreen.h"
#include "RenderYield.h"
#include "../../PsramArena.h"
#include "../../i18n/I18n.h"
#include "../UiConfig.h"
#include "../CanvasDraw.h"
#include "PaintPhase.h"
#include <math.h>
#include <string.h>

// ── Paint phases (measurement builds only, see PaintPhase.h) ──────────────────
// Eight phases that tile the whole frame, in draw order: data lock, canvas
// clear, band (the two zone tints), active sector, centre/tick marks + degree
// labels, needle, the final invalidate, and the two LVGL labels. "track" was
// a full-range track pass under the tints and "zone" the tints plus the
// active sector until the band moved to cdSectorAA, and "ps" held the P/S end
// marks as well until those were dropped (they lay wholly off the canvas);
// the names stay so that old and new [ph/rud] lines line up (track + zone
// compare as a sum). A frame whose angle did not change skips the canvas
// (see update()) and charges only pre and lbl, so the canvas phases then
// report x<cnt>.
#if PP_ENABLED
enum { RD_PH_PRE = 0, RD_PH_CLR, RD_PH_TRACK, RD_PH_ZONE, RD_PH_MARKS,
       RD_PH_NDL, RD_PH_PS, RD_PH_LBL, RD_PH_N };
static PaintPhaseSet s_pp;

// Declared extern at the call site (main.cpp), like windPhaseTake().
void rudderPhaseReport() {
    static const char *const names[RD_PH_N] = {
        "pre", "clr", "track", "zone", "marks", "ndl", "ps", "lbl" };
    ppReport("rud", names, RD_PH_N, s_pp, nullptr);
}
#endif

// Canvas spans the full working width, but only the rows the gauge can reach
// (canvasRows()); the container's own CLR_BG shows below it.
// Layout: Wide horizontal arc, centred.
// The arc sweeps from -40° (Port) to +40° (Starboard).
// A filled coloured segment shows the actual rudder angle.
// A sharp needle points to the exact value.
// Below the arc: large numeric angle + direction word (LVGL labels on the
// container, not drawn into the canvas).

static constexpr int CS = RudderScreen::CS;   // SCREEN_W (480; 600 on the 7B)

// ── Canvas height ─────────────────────────────────────────────────────────────
// The pivot lies below the canvas, so nothing drawRudder() paints comes near
// the bottom: with the shipped sizes the lowest painted row is 250 of 600 on
// the 7B/5B and 199 of 480 on the 4" (the needle tip on the canvas edge), and
// the canvas is made 265 and 213 rows tall. Below it the opaque CLR_BG
// container shows - the colour the clear used to write there - so the screen
// looks the same, while a paint clears, and LVGL re-blits, less than half the
// pixels, and the canvas no longer reaches under the two labels.
//
// Computed from the LIVE sizes and fonts: rudderTrackW, rudderNeedleW and
// FONT_SMALL can be changed from the WebUI. They only change in
// applyThemeFromConfig(), and create() runs again after every call of it, so
// this is re-evaluated whenever they do. The ALLOCATION stays CS x CS: a
// rebuild reuses _cbuf, and the next sizes may need more rows.
//
// Each term bounds the lowest row of one element, from the formulas
// drawRudder() uses. A point at radius r and rudder angle a lies at
// y = pivotY - r*cos(a): lowest at the widest angle for r > 0, and at 0° for
// r <= 0 (a track so wide that it reaches past the pivot - then the bound
// passes CS and the whole canvas is used). The ±40° ticks and labels that
// drawRudder() skips are bounded anyway; that costs no row, the needle tip
// lies lower.
static int canvasRows() {
    const float pivotY = CS * UI_RUDDER_PIVOT_OFFS;
    const float R      = CS * UI_RUDDER_RADIUS;
    const float trackW = UI_RUDDER_TRACK_W;
    const float maxAng = UI_RUDDER_MAX_ANG;
    const float rI = R - trackW * 0.5f, rO = R + trackW * 0.5f;
    auto lowY = [&](float r, float aMax) {
        return pivotY - (r > 0.f ? r * cosf(aMax * DEG_TO_RAD) : r);
    };
    // LVGL clips a line to its box widened by width/2 (lv_draw_sw_line.c).
    const float centreW = (float)(UI_S(4) / 2 + 1);
    const float tickW   = (float)(abs(uiSz.rudderNeedleW) / 2 + 1);
    const float smallH  = (float)lv_font_get_line_height(FONT_SMALL);

    // Band (cdSectorAA, see bandRadii()): inner radius less than 1.5 px inside
    // rI, ends at exactly ±maxAng, half a pixel of anti-aliasing - within this
    // bound, which dates from the LVGL arc (whole degrees, a pixel of AA) and
    // is kept so that the canvas height does not change.
    float y = lowY(rI - 2.f, maxAng + 1.f) + 2.f;
    y = max(y, lowY(rI - UI_S(6), 0.f) + centreW);      // centre line, both ends
    y = max(y, lowY(rO + UI_S(6), 0.f) + centreW);
    y = max(y, lowY(rI - UI_S(4), maxAng) + tickW);     // ticks, both ends
    y = max(y, lowY(rO + UI_S(4), maxAng) + tickW);
    // Degree labels: the box starts UI_S(8) above its anchor and holds two
    // lines at most - "10".."30" in a UI_S(28) box only wrap, one digit per
    // line, at the largest fonts.
    y = max(y, lowY(rO + UI_S(18), maxAng) - UI_S(8) + 2.f * smallH + 1.f);
    // Needle: tip and base corners, plus the 1 px it is grown by and its
    // half pixel of anti-aliasing (it used to be a 2-px outline stroke).
    y = max(y, lowY(rI - UI_S(14), maxAng) + 2.f);
    y = max(y, lowY(rO + UI_S(6), maxAng + 3.f) + 2.f);

    const int rows = (int)ceilf(y) + 1 + 2;   // last row -> row count, + 2 rows slack
    return rows < 1 ? 1 : rows > CS ? CS : rows;
}

// ── Band radii ────────────────────────────────────────────────────────────────
// The band keeps the radii it had as an LVGL arc: cdFillRing() rounded the
// outer radius and the width to whole pixels, and lv_draw_arc() caps the
// width at the radius. On the 7B/5B that is 502..555 for rI/rO = 501.5..554.5,
// on the 4" 401..443 for 401.4..443.4. Measured on the old canvas, the LVGL
// arc's 50 % edges lie within about 0.2 px of these radii around the same
// centre (pixel edge 300.0 / 630.0 on the 7B), so the new band sits where
// the old one did. An empty band (rO <= 0 or rO <= rI) stays empty:
// cdSectorAA() draws nothing for rOut <= rIn.
static void bandRadii(float rI, float rO, float &rIn, float &rOut) {
    rIn  = rI < 0.f ? 0.f : rI;
    rOut = rO;
    if (!(rOut > 0.f) || !(rOut > rIn)) return;
    const int rad = (int)(rOut + 0.5f);
    int w = (int)(rOut - rIn + 0.5f);
    if (w < 1)   w = 1;
    if (w > rad) w = rad;
    rOut = (float)rad;
    rIn  = (float)(rad - w);
}

// The band's port/starboard seam is the 0° ray from the pivot at x = CS / 2.
// cdSectorAA() puts the centre of pixel x at x + 0.5, so for an even CS the
// seam runs along a pixel boundary: every pixel lies wholly on one side, and
// the three write-only band calls in drawRudder() partition the band exactly
// (with a pixel-centred seam the shared column would blend the wrong tint,
// see the cdSectorAA() comment in CanvasDraw.h).
static_assert(RudderScreen::CS % 2 == 0, "the band seam must lie on a pixel boundary");

// ── Off-canvas draws ──────────────────────────────────────────────────────────
// With the shipped sizes the ±40° ticks and their "40" labels lie wholly
// beyond the left and right canvas edges on every board (7B/5B: the ticks end
// at x = -17 / start at 617, the label boxes end at -50 / start at 658; 4":
// -14 / 494 and -39 / 526). LVGL clipped them to nothing, but every call still
// paid a fake display and a full invalidate. The two tests below are LVGL's
// own early-outs, made before the call: a draw is skipped exactly when LVGL
// would have returned without touching a pixel, so the canvas stays the same
// for any track width, needle width or font. (From a track width of about
// 84 px on the design grid at the shipped needle width, and from 72-73 px at
// the WebUI's widest needle of 10 px, the inner end of the ±40° tick reaches
// the canvas; then it is drawn.)

// lv_draw_sw_line(): the segment's box widened by width/2, clipped to the canvas.
static bool lineOnCanvas(lv_point_t a, lv_point_t b, lv_coord_t width, lv_coord_t h) {
    lv_area_t box, cv = { 0, 0, CS - 1, (lv_coord_t)(h - 1) }, out;
    box.x1 = LV_MIN(a.x, b.x) - width / 2;
    box.x2 = LV_MAX(a.x, b.x) + width / 2;
    box.y1 = LV_MIN(a.y, b.y) - width / 2;
    box.y2 = LV_MAX(a.y, b.y) + width / 2;
    return _lv_area_intersect(&out, &box, &cv);
}

// lv_canvas_draw_text() -> lv_draw_label(): the box from (x, y) maxW wide down
// to the canvas bottom, clipped to the canvas.
static bool textOnCanvas(lv_coord_t x, lv_coord_t y, lv_coord_t maxW, lv_coord_t h) {
    lv_area_t box, cv = { 0, 0, CS - 1, (lv_coord_t)(h - 1) }, out;
    box.x1 = x;
    box.y1 = y;
    box.x2 = x + maxW - 1;
    box.y2 = h - 1;
    return _lv_area_intersect(&out, &box, &cv);
}

// ── Frame key ─────────────────────────────────────────────────────────────────
// The canvas depends on the angle alone: colours, sizes and fonts only change
// through create(). The key is the float's bit pattern, with every NaN folded
// into one - a NaN draws no sector and no needle, whatever its payload.
static uint32_t frameKey(float angle) {
    if (isnan(angle)) return 0x7FC00000u;   // itself a NaN pattern: no number has it
    uint32_t k;
    memcpy(&k, &angle, sizeof k);
    return k;
}

// Colour sets of the two labels (_lblTone).
enum : int8_t { TONE_MID = 0, TONE_PORT, TONE_STBD };

void RudderScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H - NAV_BAR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, OPA_FULL, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    _ch = canvasRows();
    size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(CS, CS);   // the full square, see canvasRows()
    if (!_cbuf) _cbuf = (lv_color_t *)PsramArena::alloc(sz);   // reuse on live theme rebuild
    _canvas = nullptr;   // a rebuild deleted the old one with its container
    if (_cbuf) {
        _canvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_canvas, _cbuf, CS, _ch, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(_canvas, LV_ALIGN_TOP_MID, 0, 0);
    }

    _lblAngle = lv_label_create(container);
    lv_label_set_text(_lblAngle, "0.0°");
    lv_obj_set_style_text_font(_lblAngle, montserratBySize(UI_S(48)), 0);
    lv_obj_set_style_text_color(_lblAngle, CLR_TEXT, 0);
    lv_obj_align(_lblAngle, LV_ALIGN_TOP_MID, 0, CS + UI_RUDDER_ANGLE_Y);

    _lblDir = lv_label_create(container);
    lv_label_set_text(_lblDir, T(STR_RUD_MIDSHIPS));
    styleLabel(_lblDir, FONT_LARGE, CLR_TEXT_DIM);
    lv_obj_align(_lblDir, LV_ALIGN_TOP_MID, 0, CS + UI_RUDDER_DIR_Y);

    // New objects, possibly a new theme: the next update() paints the canvas
    // and writes the label colours, whatever the angle.
    _painted = false;
    _lblTone = -1;
}

void RudderScreen::drawRudder(float angle) {
    // No canvas: nothing is drawn, and the labels in update() must not be
    // charged for the time since the data lock.
    if (!_canvas || !_cbuf) { PP_SKIP(s_pp); return; }

    // Geometry: large arc, centred horizontally, in the top 2/3 of canvas
    // Arc pivot is BELOW the visible canvas so only the upper portion shows
    // (giving a wide shallow arc that spans the full width).
    float pivotX = CS / 2.0f;
    float pivotY = CS * UI_RUDDER_PIVOT_OFFS;   // pivot well below canvas → shallow arc
    float R      = CS * UI_RUDDER_RADIUS;       // radius of arc track centreline
    float trackW = UI_RUDDER_TRACK_W;           // arc track width in pixels
    float maxAng = UI_RUDDER_MAX_ANG;           // ±40° range
    const float rI = R - trackW * 0.5f, rO = R + trackW * 0.5f;
    float bandIn, bandOut;
    bandRadii(rI, rO, bandIn, bandOut);

    // The clear leaves out the band's interior, about a quarter of the
    // canvas: the two tints below write every pixel of it as a plain store
    // (cdSectorAA's fully covered runs; nothing there reads the buffer), so
    // clearing it first was work thrown away. Left out per row is what lies
    // at least 1.5 px inside every edge of the tints - inner and outer arc,
    // the ±maxAng ends and the 0° seam - where cdSectorAA's own test for full
    // coverage (0.5 px inside) passes with a pixel to spare; float rounding
    // at the edges therefore lands on the cleared side. X/Y are cdSectorAA's
    // pixel-centre offsets from the pivot.
    {
        constexpr float IN = 1.5f;
        const float dIn = bandIn + IN, dOut = bandOut - IN;
        const float sA = sinf(maxAng * DEG_TO_RAD), cA = cosf(maxAng * DEG_TO_RAD);
        const bool  skip = bandOut > bandIn && dOut > dIn && cA > 0.1f;
        const lv_color_t bgColor = CLR_BG;
        for (int row = 0; row < _ch; row += 8) {
            const int rowEnd = (_ch - row < 8) ? _ch : row + 8;
            for (int y = row; y < rowEnd; y++) {
                lv_color_t *line = _cbuf + (size_t)y * CS;
                // [a0, a1] and [b0, b1]: the interior left and right of the
                // seam, as pixel columns (empty when a1 < a0).
                int a0 = 0, a1 = -1, b0 = 0, b1 = -1;
                if (skip) {
                    const float Y   = (float)y + 0.5f - pivotY;
                    const float xo2 = dOut * dOut - Y * Y;
                    const float xa  = (-Y * sA - IN) / cA;   // within ±maxAng
                    if (xo2 > 0.f) {
                        const float xi2 = dIn * dIn - Y * Y;
                        const float xi  = fmaxf(xi2 > 0.f ? sqrtf(xi2) : 0.f, IN);
                        const float xm  = fminf(sqrtf(xo2), xa);
                        if (xm > xi) {
                            a0 = max(0,      (int)ceilf (pivotX - xm - 0.5f));
                            a1 = min(CS - 1, (int)floorf(pivotX - xi - 0.5f));
                            b0 = max(0,      (int)ceilf (pivotX + xi - 0.5f));
                            b1 = min(CS - 1, (int)floorf(pivotX + xm - 0.5f));
                        }
                    }
                }
                int x = 0;
                if (a1 >= a0) { while (x < a0) line[x++] = bgColor; x = a1 + 1; }
                if (b1 >= b0) { while (x < b0) line[x++] = bgColor; x = b1 + 1; }
                while (x < CS) line[x++] = bgColor;
            }
            renderYield();   // at most one tick per 20 ms - see RenderYield.h
        }
        lv_obj_invalidate(_canvas);
    }
    PP_END(s_pp, RD_PH_CLR);

    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);

    // Helper: angle in "rudder convention" → screen point on arc
    // 0° = directly above pivot (= top-centre), + = starboard, - = port
    auto pt = [&](float deg, float r) -> lv_point_t {
        float a = (90.0f + deg) * DEG_TO_RAD;   // 0° at top means angle from right
        // Actually: 0° → top of circle; left = −, right = +
        // angle from pivot: 0° = straight up = -90° from horizontal
        float rad = (deg - 90.0f) * DEG_TO_RAD;
        return {
            (lv_coord_t)(pivotX + r * cosf(rad)),
            (lv_coord_t)(pivotY + r * sinf(rad))
        };
    };

    // The gauge band: three anti-aliased ring sectors written straight into
    // the buffer (cdSectorAA) - port tint, starboard tint, then the active
    // sector on top. They replace four LVGL arcs (a full-range track that the
    // tints painted over completely, the two tints and the active sector),
    // which built circle and angle masks per pass and rounded the ends to
    // whole degrees, so the active edge moved in 1° steps (~9 px at the outer
    // radius on the 7B, mostly hidden under the needle) while the needle moved
    // smoothly. Now every angle is an exact float.
    //
    // Write-only: the clear above left CLR_BG around the band (its interior,
    // which nothing reads, it skipped), so bgOut = bg, and the active sector
    // names the tint it lies on as bgSide. Every pixel
    // is a plain store of its final colour, the edge pixels blended against
    // the known neighbours - no buffer read, no double blend. Exact because
    // the 0° seam lies on a pixel boundary (static_assert above): there each
    // pixel is wholly port or wholly starboard, so the tints need no bgSide
    // and their ±maxAng ends fade into bg; the active sector's 0° edge is
    // just as sharp, and its moving edge fades into the tint beyond it - or
    // into bg once it reaches the band's end at ±maxAng.
    // Angles are in the rudder (= compass) convention, as cdSectorAA takes them.
    const CdTarget   tg = cdTargetOf(_canvas);
    const lv_color_t bg = CLR_BG;

    // Tints are pre-mixed against the old track colour (the band's backdrop
    // before the tints) and drawn opaque, as they always were.
    const lv_color_t trackCol = cdMix(CLR_BORDER, CLR_BG, OPA_50);
    const lv_color_t portCol  = cdMix(CLR_PORT,      trackCol, UI_RUDDER_PORT_OPA);
    const lv_color_t stbdCol  = cdMix(CLR_STARBOARD, trackCol, UI_RUDDER_STB_OPA);

    cdSectorAA(tg, pivotX, pivotY, bandIn, bandOut, -maxAng, 0.f,    portCol, LV_OPA_COVER, &bg);
    cdSectorAA(tg, pivotX, pivotY, bandIn, bandOut, 0.f,     maxAng, stbdCol, LV_OPA_COVER, &bg);
    PP_END(s_pp, RD_PH_TRACK);

    // Active rudder-angle sector (0 → actual angle), the primary readout.
    // Below 0.15° it would be a sliver under the zero mark; it is skipped there
    // as before.
    if (!isnan(angle)) {
        const float a1 = max(-maxAng, min(maxAng, angle));
        if (fabsf(a1) > 0.15f) {
            const bool        port = a1 < 0.f;
            const lv_color_t  col  = port ? CLR_PORT : CLR_STARBOARD;
            const lv_color_t *side = (fabsf(a1) < maxAng) ? (port ? &portCol : &stbdCol) : nullptr;
            cdSectorAA(tg, pivotX, pivotY, bandIn, bandOut, min(0.f, a1), max(0.f, a1),
                       col, LV_OPA_COVER, &bg, side);
        }
    }
    PP_END(s_pp, RD_PH_ZONE);

    // Centre line (thick white zero mark)
    ld.color = CLR_TEXT; ld.width = UI_S(4); ld.opa = OPA_FULL;
    {
        lv_point_t p1 = pt(0.0f, R - trackW/2 - UI_S(6));
        lv_point_t p2 = pt(0.0f, R + trackW/2 + UI_S(6));
        lv_point_t _l[2] = {p1, p2};
        lv_canvas_draw_line(_canvas, _l, 2, &ld);
    }

    // Tick marks every 10°, each with its degree label. The ones that cannot
    // touch the canvas are skipped (lineOnCanvas/textOnCanvas above).
    ld.width = uiSz.rudderNeedleW; ld.opa = OPA_FULL;
    for (int a = -(int)maxAng; a <= (int)maxAng; a += 10) {
        if (a == 0) continue;
        ld.color = (a < 0) ? CLR_PORT : CLR_STARBOARD;
        lv_point_t p1 = pt((float)a, R - trackW/2 - UI_S(4));
        lv_point_t p2 = pt((float)a, R + trackW/2 + UI_S(4));
        if (lineOnCanvas(p1, p2, ld.width, _ch)) {
            lv_point_t _l[2] = {p1, p2};
            lv_canvas_draw_line(_canvas, _l, 2, &ld);
        }
        // Degree label
        td.font = FONT_SMALL; td.color = (a < 0) ? CLR_PORT : CLR_STARBOARD;
        td.opa = OPA_FULL;
        char tbuf[6]; snprintf(tbuf, sizeof(tbuf), "%d", abs(a));
        lv_point_t lp = pt((float)a, R + trackW/2 + UI_S(18));
        const lv_coord_t tx = lp.x - UI_S(10), ty = lp.y - UI_S(8);
        if (textOnCanvas(tx, ty, UI_S(28), _ch))
            lv_canvas_draw_text(_canvas, tx, ty, UI_S(28), &td, tbuf);
    }
    PP_END(s_pp, RD_PH_MARKS);

    // Needle pointer: a SOLID anti-aliased triangle (cdFillConvexAA) on float
    // vertices around the band's own pivot, so its centre line runs exactly
    // along the active sector's moving edge. It replaces a scanline triangle
    // on truncated integer corners plus a 2-px LVGL outline stroke on each
    // side; grow = 1 widens the triangle by the 1 px that outline added
    // outside the edges, so the needle keeps its weight (same area within
    // 0.5 %, measured over -40..40°). The corners come out rounded with a 1-px
    // radius instead of the strokes' overlapping flat ends, which makes the
    // tip about 1 px longer.
    if (!isnan(angle)) {
        float ang = max(-maxAng, min(maxAng, angle));
        lv_color_t ncol = (ang < -0.5f) ? CLR_PORT : (ang > 0.5f) ? CLR_STARBOARD : CLR_TEXT;
        // pt() without the truncation; cdFillConvexAA puts pixel x's centre
        // at x + 0.5, so the old integer corner p, a pixel centre, is p + 0.5
        // here - on average the same place. The base corners sit half a pixel
        // further out: the old outline was centred that far outward, and
        // without it the zero mark's outer end falls into the base's soft AA
        // row and shows through as a 5-px nub near 0°.
        auto ptf = [&](float deg, float r, float *xy) {
            const float rad = (deg - 90.0f) * DEG_TO_RAD;
            xy[0] = pivotX + r * cosf(rad);
            xy[1] = pivotY + r * sinf(rad);
        };
        float tri[6];
        ptf(ang,        R - trackW/2 - UI_S(14),        tri + 0);   // tip
        ptf(ang - 3.0f, R + trackW/2 + UI_S(6) + 0.5f, tri + 2);   // base, port corner
        ptf(ang + 3.0f, R + trackW/2 + UI_S(6) + 0.5f, tri + 4);   // base, starboard corner
        cdFillConvexAA(tg, tri, 3, ncol, LV_OPA_COVER, 1.f);
    }
    PP_END(s_pp, RD_PH_NDL);

    // No P/S end marks: they were placed beyond the arc ends at ±42°, which put
    // both text boxes wholly off the canvas on every board (7B/5B x = -88..-51
    // and 656..693, 4" -70..-41 and 524..553) whatever the sizes and fonts,
    // so they never showed.

    lv_obj_invalidate(_canvas);
    PP_END(s_pp, RD_PH_PS);
}

void RudderScreen::update() {
    PP_BEGIN(s_pp);
    float angle;
    { auto lk = data.lock(); angle = data.rudderAngle; }
    PP_END(s_pp, RD_PH_PRE);

    // Skip a frame whose angle has the same bits as the one on the canvas: no
    // clear, no paint, no invalidate. The buffer belongs to this screen alone,
    // so that frame is still in it - also after the screen was hidden - and
    // LVGL goes on showing it. That is every frame at a steady helm, and about
    // every second one in demo mode (the angle steps every 200 ms, update()
    // runs up to every 100 ms).
    const uint32_t key = frameKey(angle);
    if (!_painted || key != _paintedKey) {
        drawRudder(angle);
        _painted    = true;
        _paintedKey = key;
    }

    // The labels are written only when what they show changes: every
    // lv_label_set_text frees, allocates and re-measures, and every write
    // invalidates the label. The texts are compared with what the labels hold
    // (that also follows a language switch); the colours depend on the side.
    char buf[16];
    const char *txt, *dir;
    int8_t tone;
    if (isnan(angle)) {
        txt = "--°";
        dir = "";
        tone = TONE_MID;
    } else {
        snprintf(buf, sizeof(buf), "%.1f°", fabsf(angle));
        txt = buf;
        if (fabsf(angle) < 0.5f) {
            dir = T(STR_RUD_MIDSHIPS);  tone = TONE_MID;
        } else if (angle < 0) {
            dir = T(STR_RUD_PORT);      tone = TONE_PORT;
        } else {
            dir = T(STR_RUD_STARBOARD); tone = TONE_STBD;
        }
    }
    if (strcmp(lv_label_get_text(_lblAngle), txt) != 0) lv_label_set_text(_lblAngle, txt);
    if (strcmp(lv_label_get_text(_lblDir), dir) != 0)   lv_label_set_text(_lblDir, dir);
    if (tone != _lblTone) {
        _lblTone = tone;
        lv_obj_set_style_text_color(_lblAngle, tone == TONE_PORT ? CLR_PORT
                                             : tone == TONE_STBD ? CLR_STARBOARD : CLR_TEXT, 0);
        lv_obj_set_style_text_color(_lblDir,   tone == TONE_PORT ? CLR_PORT
                                             : tone == TONE_STBD ? CLR_STARBOARD : CLR_TEXT_DIM, 0);
    }
    PP_END(s_pp, RD_PH_LBL);
    PP_FRAME(s_pp);
}
