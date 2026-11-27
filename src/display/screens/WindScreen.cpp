// WindScreen.cpp – circular wind-angle instrument for ESP32-S3
// Ported from the author's own NauticPi wind renderer (same author, MIT; no SkiaSharp code)
// LVGL 8 + dark theme (CLR_BG = 0x0A0A0F)

#include "WindScreen.h"
#include "SpiGeom.h"
#include "RenderYield.h"
#include "PaintPhase.h"
#include "../../config/Config.h"
#include "../../PolarTable.h"
#include "../../i18n/I18n.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../PsramArena.h"

WindScreen windScreen;

// ── Draw-phase instrumentation (1024x600 boards only) ─────────────────────────
// The wind screen is the heaviest instrument and the one both 600-grid boards
// are showing while they render at 2.3 fps. dispPaintTake() (DisplayManager.cpp)
// can only say that the instrument owns most of the paint; this says WHICH part
// of the instrument, in five coarse phases that follow the draw order of
// drawInstrument() - background fill, zone ring, ticks, boat/centre, overlays.
//
// Deliberately coarse: a timestamp pair per phase is five pairs per frame,
// nothing prints, nothing allocates, and no float is touched here. Finer
// buckets would start measuring esp_timer_get_time() itself.
//
// The times INCLUDE any renderYield() taken inside the phase (RenderYield.h
// allows one ~1 ms sleep per 20 ms of wall clock), because that is what the
// phase really costs the frame.
//
// Both WindScreen instances (Wind & Trim, and the Schiffslage attitude variant)
// share these accumulators. They are never visible at the same time and only
// the current screen is painted, so a window never mixes the two.
#if defined(BOARD_PANEL_1024X600)
#include <esp_timer.h>

// The heartbeat prints all five under fixed names, so no name table is needed
// here - the order below IS the order of the ph fields on that line.
enum { WS_PH_BG = 0, WS_PH_ZONE, WS_PH_TICK, WS_PH_BOAT, WS_PH_TRACE, WS_PH_OVL,
       WS_PH_N };
static uint32_t s_phMaxUs[WS_PH_N] = { 0, 0, 0, 0, 0, 0 };

#define WS_PHASE_BEGIN()  int64_t _phT = esp_timer_get_time()
#define WS_PHASE_END(ix)  do {                                          \
        const int64_t  _phNow = esp_timer_get_time();                   \
        const uint32_t _phD   = (uint32_t)(_phNow - _phT);              \
        if (_phD > s_phMaxUs[ix]) s_phMaxUs[ix] = _phD;                 \
        _phT = _phNow;                                                  \
    } while (0)

// ALL FIVE phases of the window, reset on read - same contract as
// dispPerfTake() in DisplaySetup_7B.cpp.
//
// This used to hand back only the WINNER, which meant four of the five phases
// of the hottest function in this firmware had never once been observed. That
// is how an entire optimisation was nearly aimed at the wrong target: the only
// phase anyone had ever seen was `zone` at 106 ms, so `zone` became the thing
// to fix - while bg, tick, boat and ovl were merely bounded as "at most 106",
// and any one of them could have been larger on a different frame.
//
// Only ONE consumer reads these, so clearing them here is safe (see the
// two-consumer trap around s_refrTotal in DisplaySetup_7B.cpp). A window in
// which the wind screen was never painted reports all zeroes rather than
// pretending the background fill won.
// Declared extern at the call site (main.cpp), not in WindScreen.h, because
// that header is shared with the 4" board, which has no such counters.
void windPhaseTake(uint32_t out[WS_PH_N]) {
    for (int i = 0; i < WS_PH_N; i++) {
        if (out) out[i] = s_phMaxUs[i];
        s_phMaxUs[i] = 0;
    }
}

#else
#define WS_PHASE_BEGIN()  do { } while (0)
#define WS_PHASE_END(ix)  do { } while (0)
#endif

// ── Finer paint phases (measurement builds only, see PaintPhase.h) ───────────
// The WS_PHASE stopwatch above feeds the [HB] line and stays as it is; these
// sets break its coarse phases down, in draw order, and print "[ph/..]" lines
// from windPhaseReport() (main.cpp, next to the other screens' reports):
//   wz / az  bg .. inner: corner clear, bezel, base ring, zone sectors,
//            highlight band, 5-degree ticks, degree ring with its labels,
//            inner circle - Wind & Trim / Schiffslage. aux lblus = the nine
//            degree labels alone (us), lvctx = LVGL draw contexts opened in
//            the frame (CdLvBatch, see ctext()).
//   wb       the Wind & Trim centre: flow lines, compass ring, trim
//            smoothing, hull, foresail, mainsail with boom and mast, rudder,
//            compass letters (+ butterfly label and the yield after them).
//            aux kite = a downwind sail was drawn, halo = halo text draws.
//   ac       the Schiffslage centre: compass card, sky/ground fill, horizon
//            line, pitch ladder, bank scale and pointer, aircraft symbol,
//            rate-of-turn strip (+ the yield after it). aux segs = sky/ground
//            segments, trir = fillTri() rows so far (index, bank, ROT).
//   wo / ao  the overlays: inner and outer border, VMG markers, both
//            pointers, corner KPIs, heading box (+ trim advice, reef badge),
//            rudder arc, polar bar (in Schiffslage only the close of the
//            overlay's draw context). The wind trace is left out, it is the
//            [HB] trace phase. aux ftri = fillTri() rows of the overlays.
// Both instances share these: only the current screen is painted, and
// _centerMode picks the set.
#if PP_ENABLED
enum { WZ_PH_BG = 0, WZ_PH_BEZEL, WZ_PH_BASE, WZ_PH_SECT, WZ_PH_HILITE, WZ_PH_SUB5,
       WZ_PH_DTICK, WZ_PH_INNER, WZ_PH_N };
enum { WZ_AUX_LBLUS = 0, WZ_AUX_LVCTX };
enum { WB_PH_FLOW = 0, WB_PH_CARD, WB_PH_TRIM, WB_PH_HULL, WB_PH_FORE, WB_PH_MAIN,
       WB_PH_RUD, WB_PH_LETTER, WB_PH_N };
enum { WB_AUX_KITE = 0, WB_AUX_HALO };
enum { AC_PH_CARD = 0, AC_PH_SKY, AC_PH_HORIZ, AC_PH_LADDER, AC_PH_BANK, AC_PH_ACFT,
       AC_PH_ROT, AC_PH_N };
enum { AC_AUX_SEGS = 0, AC_AUX_TRIR };
enum { WO_PH_RINGI = 0, WO_PH_RINGO, WO_PH_VMG, WO_PH_PTR, WO_PH_KPI, WO_PH_HDG,
       WO_PH_RUDARC, WO_PH_POLAR, WO_PH_N };
enum { WO_AUX_FTRI = 0 };
static PaintPhaseSet  s_ppZ[2], s_ppB, s_ppA, s_ppO[2];   // [0] Wind & Trim, [1] Schiffslage
static PaintPhaseSet *s_ppz = &s_ppZ[0], *s_ppo = &s_ppO[0];
static uint32_t       s_ppTriRows, s_ppTriRowsO, s_ppSegs, s_ppHalo, s_ppCtx;

// Declared extern at the call site (main.cpp), like windPhaseTake().
void windPhaseReport() {
    static const char *const zN[WZ_PH_N] = {
        "bg", "bezel", "base", "sect", "hilite", "sub5", "dtick", "inner" };
    static const char *const zA[PP_MAX_AUX] = { "lblus", "lvctx" };
    static const char *const bN[WB_PH_N] = {
        "flow", "card", "trim", "hull", "fore", "main", "rud", "letter" };
    static const char *const bA[PP_MAX_AUX] = { "kite", "halo" };
    static const char *const aN[AC_PH_N] = {
        "card", "sky", "horiz", "ladder", "bank", "acft", "rot" };
    static const char *const aA[PP_MAX_AUX] = { "segs", "trir" };
    static const char *const oN[WO_PH_N] = {
        "ringi", "ringo", "vmg", "ptr", "kpi", "hdg", "rudarc", "polar" };
    static const char *const oA[PP_MAX_AUX] = { "ftri", nullptr };
    ppReport("wz", zN, WZ_PH_N, s_ppZ[0], zA);
    ppReport("wb", bN, WB_PH_N, s_ppB,    bA);
    ppReport("wo", oN, WO_PH_N, s_ppO[0], oA);
    ppReport("az", zN, WZ_PH_N, s_ppZ[1], zA);
    ppReport("ac", aN, AC_PH_N, s_ppA,    aA);
    ppReport("ao", oN, WO_PH_N, s_ppO[1], oA);
}
#endif

// The paint's LVGL draw context (see ctext()), counted for the lvctx aux of a
// measurement build; in every other build exactly a CdLvBatch that does not
// invalidate (drawInstrument() invalidates the whole canvas itself).
struct WsBatch : CdLvBatch {
    explicit WsBatch(lv_obj_t *canvas) : CdLvBatch(canvas, false) {
        PP_ONLY(if (open()) s_ppCtx++;);
    }
};

// ── Compile-time constants ────────────────────────────────────────────────────
static constexpr float DEG2RAD = (float)M_PI / 180.f;

// Zone colours (dark-theme adaptation of the NauticPi renderer's palette)
// Using lv_color_hex() – safe for any LV_COLOR_DEPTH
// All wind-instrument colours now come from the runtime theme (uiTheme),
// so they switch with the active light/dark theme.
#define C_BASE    (uiTheme.windRingBg)     // ring background
#define C_NOGO    (uiTheme.windZoneNogo)   // No-Go
#define C_CLOSE   (uiTheme.windZoneClose)  // close-hauled
#define C_CLOSER  (uiTheme.windZoneCloser) // close reach
#define C_BEAM    (uiTheme.windZoneBeam)   // beam reach
#define C_BROAD   (uiTheme.windZoneBroad)  // broad reach
#define C_RUN     (uiTheme.windZoneRun)    // running
#define C_BEZEL   (uiTheme.windBezel)      // bezel background
#define C_INNER   (uiTheme.windInner)      // inner circle background
#define C_TICK_MJ (uiTheme.windTickMaj)    // major tick
#define C_TICK_MN (uiTheme.windTickMin)    // minor tick
#define C_TWA_PTR (uiTheme.windTwa)        // TWA pointer
#define C_AWA_PTR (uiTheme.windAwa)        // AWA pointer
#define C_VMG     (uiTheme.windVmg)        // VMG optimal
#define C_HULL    (uiTheme.windHull)       // boat hull
#define C_SAIL    (uiTheme.windSail)       // sails
#define C_WIND_LN (uiTheme.windFlow)       // wind flow lines

// ── Low-level helpers (static, not methods) ───────────────────────────────────

// Wind-angle → screen point: 0° = top, CW positive (y increases downward)
static inline lv_point_t wp(float cx, float cy, float r, float angleDeg) {
    float a = angleDeg * DEG2RAD;
    return { (lv_coord_t)(cx + r * sinf(a)),
             (lv_coord_t)(cy - r * cosf(a)) };
}

// cline() - the old detour through lv_canvas_draw_line() - was REMOVED on
// 2026-08-30. All 28 calls of that time were moved to linePx(), which forwards to
// cdLineAA() in CanvasDraw.cpp and writes straight into the canvas buffer.
// Careful when porting older code: the argument order differs, cdLineAA()
// (and linePx()) take the opacity BEFORE the stroke width.

// Filled triangle by horizontal scanlines.
//
// Opaque rows are stored straight into the canvas. Each row used to be an
// lv_canvas_draw_line() of width 1 from (xL, y) to (xR, y), cast to
// lv_coord_t - a whole fake display per row (see cdSpanH() in CanvasDraw.cpp):
// 31-43 rows for each wind pointer, about 100 a frame with the VMG markers,
// plus the bank and rate-of-turn pointers of the attitude screen. The float
// loop, the casts and the yield offers are unchanged; what changed is that
// the row's pixels are stored here, exactly the ones LVGL 8.4 filled for
// that line:
//   - equal end points draw nothing (lv_draw_sw_line() returns at once);
//   - otherwise draw_line_hor() fills [min, max - 1] of the row - the right
//     end point is left out - clipped to the canvas, and with no mask and
//     full opacity that blend is a plain store of the colour (fill_normal()
//     -> lv_color_fill()).
// So a row is [X0, X1 - 1] when X1 > X0, and nothing otherwise. X1 < X0
// cannot come up, so cdSpanH() never swaps the ends: xL is the smaller
// crossing and the cast never reverses an order, and with a NaN crossing
// both ends are that same NaN, i.e. X0 == X1. cdSpanH() drops a row off the
// canvas and clips the span to it, as LVGL's clip area (the whole canvas)
// did. AnchorScreen's fillTri() is the same rewrite.
//
// Below full opacity the LVGL line stays: LVGL blends an unmasked
// translucent fill with lv_color_mix_premult() on a rounded opacity
// (fill_normal() in lv_draw_sw_blend.c), which is not what cdSpanH()'s
// lv_color_mix() computes. No caller draws a translucent triangle today; if
// one does, its rows share one LVGL draw context per stretch between two
// yield offers (CdLvBatch, CanvasDraw.h), closed around every renderYield().
// That path is fillTriBlend(), kept out of line so the batch (a whole fake
// display, about 460 B) sits in the loop task's stack only while a
// translucent triangle is drawn, not on every opaque call.
// No invalidate of its own: drawInstrument() invalidates the whole canvas
// before and after the paint.

// Row y's two edge crossings of a triangle sorted by y (ay <= by <= cy):
// xL the smaller, xR the larger. hFull = cy - ay.
static inline void triRowX(float y, float ax, float ay, float bx, float by,
                           float cx, float cy, float hFull, float &xL, float &xR) {
    float xAC = (hFull > 0.f) ? ax + (y-ay)/hFull*(cx-ax) : ax;
    float xABBC;
    if (y <= by) {
        float h = by - ay;
        xABBC = (h > 0.f) ? ax + (y-ay)/h*(bx-ax) : ax;
    } else {
        float h = cy - by;
        xABBC = (h > 0.f) ? bx + (y-by)/h*(cx-bx) : bx;
    }
    xL = (xAC < xABBC) ? xAC : xABBC;
    xR = (xAC > xABBC) ? xAC : xABBC;
}

static void __attribute__((noinline))
fillTriBlend(lv_obj_t *cv, float ax, float ay, float bx, float by,
             float cx, float cy, lv_color_t col, lv_opa_t opa) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = col; d.width = 1; d.opa = opa;
    WsBatch b(cv);
    const float hFull = cy - ay;
    int row = 0;
    for (float y = ay; y <= cy; y += 1.f) {
        float xL, xR;
        triRowX(y, ax, ay, bx, by, cx, cy, hFull, xL, xR);
        const lv_coord_t X0 = (lv_coord_t)xL, X1 = (lv_coord_t)xR, Y = (lv_coord_t)y;
        PP_ONLY(s_ppTriRows++;);
        lv_point_t pts[2] = {{ X0, Y }, { X1, Y }};
        cdLvLine(b, pts, 2, &d);
        if (++row % 20 == 0) {
            cdLvEnd(b);
            renderYield();
            cdLvBegin(b, cv, false);
        }
    }
}

static void fillTri(lv_obj_t *cv,
                    float ax, float ay, float bx, float by,
                    float cx, float cy, lv_color_t col, lv_opa_t opa = LV_OPA_75) {
    if (ay > by) { float t; t=ax;ax=bx;bx=t; t=ay;ay=by;by=t; }
    if (ay > cy) { float t; t=ax;ax=cx;cx=t; t=ay;ay=cy;cy=t; }
    if (by > cy) { float t; t=bx;bx=cx;cx=t; t=by;by=cy;cy=t; }
    if (opa < LV_OPA_COVER) {
        fillTriBlend(cv, ax, ay, bx, by, cx, cy, col, opa);
        return;
    }
    const CdTarget tg = cdTargetOf(cv);
    const float hFull = cy - ay;
    int row = 0;
    for (float y = ay; y <= cy; y += 1.f) {
        float xL, xR;
        triRowX(y, ax, ay, bx, by, cx, cy, hFull, xL, xR);
        const lv_coord_t X0 = (lv_coord_t)xL, X1 = (lv_coord_t)xR, Y = (lv_coord_t)y;
        PP_ONLY(s_ppTriRows++;);
        if (X1 > X0) cdSpanH(tg, Y, X0, X1 - 1, col, LV_OPA_COVER);
        // Offer a yield every 20 rows; renderYield decides whether this one has
        // earned a tick (RenderYield.h). The 4-inch board still sleeps 1 ms here.
        if (++row % 20 == 0) renderYield();
    }
}

// ── LVGL draws: one draw context per stretch (CdLvBatch) ─────────────────────
// Every text, the remaining LVGL lines (degree ticks, the 5-degree subdivision
// ticks, the polar bar's 100 % mark), the rects and the rudder arc go through
// a CdLvBatch (CanvasDraw.h): each cdLv* call is the lv_canvas_draw_* it
// replaces, with the same descriptor and the same coordinates, but without
// the fake display that one built and tore down around every single draw -
// about 145 of them a frame here - and without its invalidate of the whole
// canvas, which drawInstrument() does itself before and after the paint.
// A batch is open only over a stretch with no renderYield() and no lv_obj_*
// call in it, i.e. never across fillTri(), fillPolygon(), the ellipse and
// annulus fills (all of which offer yields) or a layer boundary with a
// renderYield(); the direct-buffer helpers (linePx(), ringAA(), cdSpanH())
// may run inside one. So a frame opens a handful of draw contexts: the
// subdivision ticks, the degree ring, the compass letters, the mast (in
// Schiffslage the ladder labels, the aircraft dot and the ROT text instead),
// the two pointer labels and the overlay block at the end.
static void ctext(CdLvBatch &b, float x, float y, float maxW,
                  const lv_font_t *font, lv_color_t col,
                  lv_text_align_t align, const char *txt) {
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = font; d.color = col; d.align = align;
    cdLvText(b, (lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)maxW, &d, txt);
}

// The low-level raster helpers that used to be defined here (fillEllipse,
// spanH, linePx, ringAA, discAA, fillAnnulus, fillPolygon) now live in
// CanvasDraw.cpp as cdFillEllipse, cdSpanH, cdLineAA, cdRingAA, cdDiscAA,
// cdFillAnnulus and cdFillPolyOpa, moved unchanged. WindScreen.h keeps the
// old member names as inline forwarders, so no call site below changed.

// ── Sail draft (camber) by point of sail ──────────────────────────────────────
// Optimally trimmed sails are flat upwind (sheeted hard) and progressively fuller
// off the wind. Returns the belly depth as a fraction of the leech-chord length.
static float camberForTwa(float absTwa) {
    if (absTwa <  35.f) return 0.10f;   // close-hauled: flat
    if (absTwa <  60.f) return 0.15f;   // close reach
    if (absTwa < 100.f) return 0.19f;   // beam reach: medium
    if (absTwa < 150.f) return 0.24f;   // broad reach: full
    return 0.28f;                        // running: max draft
}

// ── Sail foil (cambered airfoil seen from above) ──────────────────────────────
// One sail = a filled lens between the straight luff(tack)->clew chord and a
// leeward-bulging quadratic Bézier (the draft, peak ~45% back). The belly sits
// on the lee side; the chord direction is the trim angle. ONE polygon pass
// (cheap — no per-row yields) plus a leeward edge outline.
void WindScreen::drawSailFoil(float tackX, float tackY, float clewX, float clewY,
                              float leeSign, float camberFrac,
                              lv_color_t col, lv_opa_t opa,
                              lv_color_t edgeCol, lv_coord_t edgeW) {
    float lx = clewX - tackX, ly = clewY - tackY;
    float len = sqrtf(lx * lx + ly * ly);
    if (len < 2.f) return;
    // Leeward-pointing chord normal (force its x-sign to match leeSign).
    float nx = -ly / len, ny = lx / len;
    if ((nx < 0.f) != (leeSign < 0.f)) { nx = -nx; ny = -ny; }
    float belly = camberFrac * len;
    float ctrlX = tackX + lx * 0.45f + nx * belly;   // draft peak ~45% aft of luff
    float ctrlY = tackY + ly * 0.45f + ny * belly;

    // Polygon = tack + Bézier(tack->ctrl->clew); closing edge clew->tack is the
    // chord, so the filled area is the cambered sliver (lens) on the lee side.
    const int N = 10;
    lv_point_t pts[12];
    pts[0].x = (lv_coord_t)tackX; pts[0].y = (lv_coord_t)tackY;
    for (int i = 1; i <= N; i++) {
        float t = (float)i / (float)N, u = 1.f - t;
        float qx = u * u * tackX + 2.f * u * t * ctrlX + t * t * clewX;
        float qy = u * u * tackY + 2.f * u * t * ctrlY + t * t * clewY;
        pts[i].x = (lv_coord_t)qx; pts[i].y = (lv_coord_t)qy;
    }
    fillPolygon(pts, N + 1, col, opa);

    // Outline the leeward (curved) sail edge for crisp definition.
    if (edgeW > 0)
        for (int i = 0; i < N; i++)
            linePx(pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, edgeCol, LV_OPA_COVER, edgeW);
}

// ── Lee side, held through a dead run ─────────────────────────────────────────
// computeSailState() takes the lee side from the sign of the wind angle. Dead
// downwind that sign flips with every wobble of the wind across 180 - and with
// it the whole rig: boom, jib and the spinnaker pole jumped across the bow
// several times a second. So beyond 175 deg the side the rig is already on is
// kept; only an angle of 175 or less decides a new one, which is where a real
// gybe is complete anyway. With no side kept yet - the first frame, or after
// the wind was lost altogether - the sign is taken as it is.
//   sideAng   TWA if known, else AWA (signed, -180..180); NaN = no wind
//   fromSign  the side computeSailState() derived from that sign
//   prev      the side kept so far (+1 / -1, 0 = none)
// Returns the side to keep and draw (0 when there is no wind).
static float latchLeeSide(float sideAng, float fromSign, float prev) {
    if (isnan(sideAng)) return 0.f;
    if (fabsf(sideAng) > 175.f && prev != 0.f) return prev;
    return fromSign;
}

// ── computeSailState ──────────────────────────────────────────────────────────
WindScreen::SailState WindScreen::computeSailState(float absTwa, float twa,
                                                    float awa, float tws, float stw) const {
    SailState s;
    // twa / absTwa may be NaN (no boat speed), and awa too (no wind at all).
    // TWA and AWA always lie on the same side - the boat's own speed only
    // adds a component along the bow - so either one tells the lee side.
    const bool  haveTwa = !isnan(twa);
    const bool  haveAwa = !isnan(awa);
    const float sideAng = haveTwa ? twa : (haveAwa ? awa : 0.f);
    s.leewardSide   = (sideAng >= 0.f) ? -1.f : 1.f;   // wind from stbd → lee = port
    if (haveTwa) {
        s.luffing   = (absTwa < 28.f);             // head-to-wind: sails flap
        s.running   = (absTwa > 150.f);
    } else if (haveAwa) {
        // Apparent wind only. The limits above are TRUE wind angles; the
        // boat's own speed draws the apparent wind forward, so TWA 28 / 150
        // are roughly AWA 20 / 135-140 at cruising speed. Kept on the safe
        // side, so a boat hard on the wind is not told to bear away.
        s.luffing   = (fabsf(awa) < 18.f);
        s.running   = (fabsf(awa) > 140.f);
    }
    // Neither: no wind known - neutral sails, neither flapping red nor running.

    // ── Optimal trim = ease each sail to a constant angle of attack to the
    //    APPARENT wind. CONTINUOUS in AWA (no buckets → no jumping). The boom
    //    swings from ~4° (close-hauled, sheeted hard) to ~86° (running). ──────
    float absAwa = isnan(awa) ? 0.f : fabsf(awa);
    s.awaAbs = haveAwa ? fabsf(awa) : (haveTwa ? absTwa : 180.f);
    float boom = absAwa - 13.f;                 // ~13° angle of attack for the main
    if (boom <  4.f) boom =  4.f;
    if (boom > 86.f) boom = 86.f;
    s.boomAngleDeg     = boom;
    s.headsailAngleDeg = boom * 0.82f;          // jib leads inboard of the main
    s.flutterDeg       = 0.f;                   // (luffing flap handled in drawBoat)

    // ── Sail inventory ────────────────────────────────────────
    bool hasTws = !isnan(tws);
    // Downwind sail: whatever the boat carries (WebUI, "sail.spi_type"). A
    // NaN absTwa fails every comparison, so no true wind = no kite.
    //   symmetric (panels, balloon): broad reach 100..150 below 25 kn, run
    //     from 150 below 20 kn - the window the single spinnaker always had;
    //   gennaker: from 90 (it is a reaching sail too) to 150 below 25 kn, and
    //     150..160 below 20 kn. Not beyond 160: that deep, an asymmetric kite
    //     is blanketed by the main, and a cruiser gybes downwind instead of
    //     running dead - so the normal running picture is drawn there.
    // One read of the setting per frame; drawSpinnaker() draws s.spiType.
    const SpiType spi = appConfig.cfg.spiType;
    s.spiType = spi;
    bool spiWindow = false;
    if (spi == SpiType::Gores || spi == SpiType::Balloon)
        spiWindow = (absTwa > 100.f && absTwa < 150.f && tws < 25.f) ||
                    (absTwa >= 150.f && tws < 20.f);
    else if (spi == SpiType::Gennaker)
        spiWindow = (absTwa > 90.f && absTwa < 150.f && tws < 25.f) ||
                    (absTwa >= 150.f && absTwa <= 160.f && tws < 20.f);
    s.useSpinnaker = spiWindow && !s.luffing && hasTws;
    s.useCodeZero  = !s.useSpinnaker && appConfig.cfg.allowCodeZero && hasTws &&
                     absTwa > 50.f && absTwa < 95.f && tws > 2.f && tws < 14.f;

    // ── Reef count (hull-speed model) ─────────────────────────
    float lwl       = appConfig.cfg.waterlineLengthM;
    float hullSpeed = 2.43f * sqrtf(lwl > 0.f ? lwl : 9.f);
    float ratio     = (!isnan(stw) && hullSpeed > 0.f) ? stw / hullSpeed : 0.f;

    s.reefCount = 0;
    if (hasTws) {
        if      (tws > 28.f || ratio > 0.97f) s.reefCount = 3;
        else if (tws > 22.f || ratio > 0.93f) s.reefCount = 2;
        else if (tws > 16.f || ratio > 0.88f) s.reefCount = 1;
    }

    // ── Sail draft (belly) — flat upwind, progressively fuller off the wind ──
    // By TWA only; without one the SailState defaults (a medium draft) stay.
    if (haveTwa) {
        s.mainCamber = camberForTwa(absTwa);
        s.jibCamber  = s.mainCamber * 0.85f;   // headsails trim a touch flatter
    }

    return s;
}

// ── Public API ────────────────────────────────────────────────────────────────

void WindScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H - NAV_BAR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(CW, CH);
#if defined(BOARD_PANEL_1024X600)
    // 7B: BOTH WindScreen instances (Wind & Trim + the Schiffslage attitude
    // variant) share ONE canvas buffer - they are never visible at the same
    // time and each update() fully repaints the canvas, so the only cost is
    // one repaint-tick showing the sibling's last frame after a switch.
    // Saves 720 KB of the 600-grid arena budget (measured: total demand
    // 5.03 MB unshared vs the 4.4 MB arena). The 4" keeps two buffers -
    // its released behavior stays untouched.
    static lv_color_t *s_sharedCbuf = nullptr;
    if (!s_sharedCbuf) s_sharedCbuf = (lv_color_t *)PsramArena::alloc(sz);
    if (!_cbuf) _cbuf = s_sharedCbuf;
#else
    if (!_cbuf) _cbuf = (lv_color_t *)PsramArena::alloc(sz);   // reuse on live theme rebuild
#endif
    if (_cbuf) {
        // Buffer is already zeroed by PsramArena::init() (before WiFi starts).
        _canvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_canvas, _cbuf, CW, CH, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(_canvas, 0, 0);
        // Tap the compass card to switch it between north-up and course-up.
        // lv_canvas is an image, and images are NOT clickable by default, so
        // without this flag no pointer event reaches the canvas at all.
        lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);
        // Two events, one handler: PRESSED records where the finger landed,
        // CLICKED (release) decides. See onCanvasTouch for why the press point
        // is needed. Registered here, so a live theme rebuild - which destroys
        // and recreates the container and the canvas - re-registers with it and
        // never ends up with two callbacks on one object.
        lv_obj_add_event_cb(_canvas, onCanvasTouch, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(_canvas, onCanvasTouch, LV_EVENT_CLICKED, this);
    }
}

void WindScreen::update() {
    float twa, awa, tws, stw, hdg;
    {
        auto lk = data.lock();
        twa = data.twa; awa = data.awa; tws = data.tws;
        stw = data.stw; hdg = data.hdg;
    }
    drawInstrument(twa, awa, tws, stw, hdg);
}

// ── Master draw orchestrator ──────────────────────────────────────────────────

// ── Polar performance bar (right side, outside ring) ─────────────────────────
// Vertical filled bar showing STW / polarSpeed as percentage.
static float windPolarSpeed(float absTwa, float tws) {
    if (isnan(absTwa) || isnan(tws)) return NAN;
    // Prefer the configured polar table (loaded from /polar.json).
    float p = gPolar().speedAt(absTwa, tws);
    if (!isnan(p)) return p;
    // Fallback: simplified analytic estimate when no polar is loaded.
    float a = absTwa;
    float eff;
    if      (a <  28.f) eff = 0.05f;
    else if (a <  50.f) eff = 0.05f + (a-28.f)/22.f * 0.50f;
    else if (a < 100.f) eff = 0.55f + (a-50.f)/50.f * 0.20f;
    else if (a < 150.f) eff = 0.75f - (a-100.f)/50.f * 0.20f;
    else                eff = 0.55f - (a-150.f)/30.f * 0.25f;
    eff = (eff < 0.04f) ? 0.04f : (eff > 0.80f ? 0.80f : eff);
    float hullSpd = 2.43f * sqrtf(9.0f);
    return eff * hullSpd * (1.0f + tws / 100.0f);  // mild TWS boost
}

// The ATTITUDE horizon (sky/ground, horizon line, pitch ladder) is drawn only
// for a known roll AND pitch. One test for the two places that depend on it:
// drawCenter_Attitude(), which draws the fill, and drawInstrument(), which
// leaves the disc to that fill (horizonHole). Were they to disagree, the disc
// would keep the previous frame's pixels.
static inline bool attitudeKnown(float roll, float pitch) {
    return !isnan(roll) && !isnan(pitch);
}

void WindScreen::drawInstrument(float twa, float awa, float tws, float stw, float hdg) {
    if (!_canvas || !_cbuf) return;
    // Phase stopwatch. Each WS_PHASE_END() closes one phase and opens the next,
    // so the five phases tile the whole function with no gaps and no overlap;
    // on the 4" board every one of them expands to nothing. See the block at
    // the top of this file.
    WS_PHASE_BEGIN();
    PP_ONLY(s_ppz = &s_ppZ[_centerMode == CenterMode::ATTITUDE ? 1 : 0];
            s_ppo = &s_ppO[_centerMode == CenterMode::ATTITUDE ? 1 : 0];
            s_ppTriRows = s_ppSegs = s_ppHalo = s_ppCtx = 0;);
    PP_BEGIN(*s_ppz);
    PP_BEGIN(*s_ppo);

    // Not painted for a while although never hidden - a modal overlay was
    // open, and the screen behind it gets neither update() nor onShow() -
    // counts as coming back on screen: the trims and the lee side kept from
    // before would otherwise swing the rig in from a wind that is long gone.
    // RIG_STALE_MS is several frame periods even on the boards that paint
    // this screen at ~2 fps, so a slow frame never triggers it.
    if (lv_tick_elaps(_lastPaintTick) > RIG_STALE_MS) resetRig();
    _lastPaintTick = lv_tick_get();

    // ── Chunked background fill ───────────────────────────────────────────────
    // The fill is broken into 8-row chunks so there is a place to yield at all:
    // lv_canvas_fill_bg() would write the whole canvas (480×480×2 = 450 KB on the
    // 4-inch grid, 720 KB here) in one uninterruptible loop.
    //
    // On the 4-inch board every chunk sleeps a tick, for the SPI0 reason spelled
    // out in RenderYield.h. On these boards that reason does not apply and the
    // sleeps were the single largest item in the frame: 600/8 = 75 chunks, one
    // tick each, ~75 ms of doing nothing. renderYield() keeps a yield here but
    // takes it at most every 20 ms, so the fill now costs about what the pixel
    // writes cost (~0.5 ms per full-width chunk at the ~20 MB/s measured
    // below) plus 2-3 ticks.
    {
        // spanH() stores two pixels per 32-bit word, but the store width is
        // not what limits this fill. Device measurements (August 2026): 32-bit
        // instead of 16-bit stores gained 0 ms here, and clearing the whole
        // canvas took bg = 36 ms for 720 KB, ~20 MB/s, because the RGB panel
        // streams 50.8 MB/s from the same PSRAM. Only writing fewer pixels
        // helps (bg 36 -> 19 ms with the rule below), hence: clear ONLY
        // OUTSIDE THE INSTRUMENT DISC.
        //
        // drawBezel() fills the disc out to R_OUTER+1 right after this, so
        // anything written inside it here is overwritten within the same
        // frame. Worked out: the disc covers pi*253.5^2 = 201,850 of the
        // 360,000 pixels, i.e. 56 % of the canvas. Exactly that share was
        // memory written for nothing on every frame - and memory bandwidth,
        // not computing time, is the bottleneck on this board.
        //
        // The corners are still cleared in full: the corner values, the
        // heading display and the polar bar live there, and nothing may be
        // left standing there.
        const lv_color_t bgColor = CLR_BG;
        const int        CHUNK   = 8;   // rows per yield window
        const float      rKeep   = R_OUTER + 1.f;   // exactly what drawBezel fills
        for (int row = 0; row < CH; row += CHUNK) {
            const int rows = (CH - row < CHUNK) ? CH - row : CHUNK;
            for (int y = row; y < row + rows; y++) {
                const float dy = (float)y + 0.5f - CY;
                if (fabsf(dy) >= rKeep) {           // row does not touch the disc
                    spanH(y, 0, CW - 1, bgColor, LV_OPA_COVER);
                    continue;
                }
                const int hx = (int)floorf(sqrtf(rKeep * rKeep - dy * dy));
                const int xa = (int)CX - hx;
                const int xb = (int)CX + hx;
                if (xa > 0)      spanH(y, 0,      xa - 1,  bgColor, LV_OPA_COVER);
                if (xb < CW - 1) spanH(y, xb + 1, CW - 1,  bgColor, LV_OPA_COVER);
            }
            renderYield();
        }
        lv_obj_invalidate(_canvas);
    }
    WS_PHASE_END(WS_PH_BG);
    PP_END(*s_ppz, WZ_PH_BG);

    // Normalise TWA to –180 … +180 (negative = port, positive = starboard)
    float nTwa = isnan(twa) ? 0.f : twa;
    while (nTwa >  180.f) nTwa -= 360.f;
    while (nTwa < -180.f) nTwa += 360.f;
    float absTwa = fabsf(nTwa);

    float nAwa = isnan(awa) ? 0.f : awa;

    // No true wind angle - no boat speed to compute it from, or a sensor that
    // went quiet - is a normal state now that the display computes true wind
    // itself. The 0 above only keeps the geometry finite; nothing may READ it
    // as "head to wind". Every statement about the true wind angle (its
    // corner value, the point-of-sail name, the zone highlight, the VMG
    // markers, the polar bar) is left out or shows "--". The sail picture
    // goes by the apparent wind instead, which is what sails are trimmed to
    // anyway, with limits of its own for "flapping" and "running"; with no
    // wind at all the sails stand neutral. Before, it fell back to TWA 0 and
    // flapped red, head to wind.
    const bool  haveTwa  = !isnan(twa);
    const bool  haveAwa  = !isnan(awa);
    const float shownTwa = haveTwa ? nTwa : NAN;
    const float shownAwa = haveAwa ? nAwa : NAN;
    const float knownAbs = haveTwa ? absTwa : NAN;

    float rudder, roll, pitch, rot, waveH, waveT;
    {
        auto lk = data.lock();
        rudder = data.rudderAngle;
        roll   = data.roll;  pitch = data.pitch;  rot = data.rateOfTurn;
        waveH  = data.waveHeight;  waveT = data.wavePeriod;
    }

    // Sail state computed once, shared by all boat sub-layers
    SailState sail = computeSailState(knownAbs, shownTwa, shownAwa, tws, stw);
    // ... with the lee side held through a dead run (latchLeeSide). Here and
    // not in computeSailState(), which stays a const function of one frame's
    // values: the latch is the one piece of the sail state with a memory.
    // It covers the whole rig - main, jib and kite share leewardSide.
    {
        const float sideAng = haveTwa ? shownTwa : (haveAwa ? shownAwa : NAN);
        _leeSide = latchLeeSide(sideAng, sail.leewardSide, _leeSide);
        if (_leeSide != 0.f) sail.leewardSide = _leeSide;
    }

    // ── Draw order: back → front ──────────────────────────────────────────────
    // A yield offered between the heavy layers, for the same reason the fill
    // loops offer one. On the 4-inch board each of these is a 1 ms sleep; here
    // renderYield() collapses them into whatever the 20 ms budget allows, which
    // for a whole instrument repaint is one or two of the six.
    // Measured in detail (7B, 2026-08-30): bezel 23.1 ms, zoneRing 36.0 ms,
    // the two renderYield() together 1.5 ms. So the pauses are negligible -
    // what this phase costs is real drawing work.
    drawBezel();
    renderYield();
    PP_END(*s_ppz, WZ_PH_BEZEL);
    drawZoneRing(knownAbs);     // base / sect / hilite / sub5 inside
    renderYield();
    WS_PHASE_END(WS_PH_ZONE);   // bezel + the coloured zone sectors
    drawTicksAndLabels();
    renderYield();
    PP_END(*s_ppz, WZ_PH_DTICK);
    // ATTITUDE: the sky/ground disc is left to drawCenter_Attitude() - unless
    // the roll is infinite, which makes that fill's split point NaN and the
    // disc rows empty (see horizonHole()), or roll or pitch is not known,
    // when there is no sky/ground fill at all (attitudeKnown()). A hole
    // without the fill would leave the last horizon standing in the disc:
    // this screen has no frame skip, but the hole is the same trap.
    drawInnerBg(_centerMode == CenterMode::ATTITUDE && attitudeKnown(roll, pitch)
                && !isinf(roll));
    WS_PHASE_END(WS_PH_TICK);   // degree ticks/labels + inner circle
    PP_END(*s_ppz, WZ_PH_INNER);
    if (_centerMode == CenterMode::ATTITUDE) {
        PP_BEGIN(s_ppA);
        // The whole card first. The horizon disc (radius 110 UI_SF) and the
        // bank scale (100 UI_SF) end well inside the band of the letters
        // (from about 119 UI_SF), so the order can stay. Only a pitch-ladder
        // label, drawn over them, can reach a letter, at attitudes far off
        // a sailing yacht's: never within 30 deg of heel; with up to 45 deg
        // of heel from |pitch| 22 deg (4") / 33 deg (7B) on. Within 45 deg of
        // heel and 15 deg of pitch it stays 2 px (4") / 5 px (7B) clear.
        drawCompassRose(hdg);       // keep the heading-up compass card
        renderYield();
        PP_END(s_ppA, AC_PH_CARD);
        drawCenter_Attitude(roll, pitch, rot);   // artificial horizon replaces the boat
    } else {
        PP_BEGIN(s_ppB);
        // Flow lines: apparent or true - and none at all without that angle,
        // rather than a breeze from dead ahead that nobody measured.
        const float lineAng = appConfig.cfg.windLinesApparent ? shownAwa : shownTwa;
        if (!isnan(lineAng)) drawWindLines(lineAng);
        PP_END(s_ppB, WB_PH_FLOW);
        drawCompassRose(hdg, CARD_RING);   // ticks (and north-up index) under the boat
        renderYield();
        PP_END(s_ppB, WB_PH_CARD);
        drawBoat(sail);                    // trim / hull / fore / main / rud inside
        // The N/E/S/W letters over the boat. They stand in the band from about
        // 31 to 35.5 boat units, and the crest of a downwind sail (32.45),
        // the stem, the boom end and an eased jib reach into it:
        // drawn under the boat, a letter near the bow lost its lower part to
        // the kite. Over a kite a letter gets a halo in the inner colour (see
        // drawCompassRose). Still under everything drawn after the boat (wind
        // traces, borders, pointers, value boxes, reef badge).
        drawCompassRose(hdg, CARD_LETTERS);
        // The "Butterfly" label stands over the bow, in the letters' band: a
        // letter within about 15 deg of the bow overlaps it. Printed here,
        // after the letters, it keeps the place over them it had while it
        // was drawn inside drawBoat() and the letters under the boat.
        if (_butterflyDrawn) {
            WsBatch b(_canvas);
            ctext(b, CX - 36.f * UI_SF, CY - 35.f * BOAT_S, 72.f * UI_SF,
                  FONT_TINY, CLR_ACCENT, LV_TEXT_ALIGN_CENTER, "Butterfly");
        }
    }
    renderYield();
    // Centre of the instrument: flow lines + compass ring + boat with sails +
    // compass letters (+ butterfly label), or the artificial horizon in
    // ATTITUDE mode.
    WS_PHASE_END(WS_PH_BOAT);
    if (_centerMode == CenterMode::ATTITUDE) {
        PP_END(s_ppA, AC_PH_ROT);
        PP_AUX(s_ppA, AC_AUX_SEGS, s_ppSegs);
        PP_AUX(s_ppA, AC_AUX_TRIR, s_ppTriRows);
        PP_FRAME(s_ppA);
    } else {
        PP_END(s_ppB, WB_PH_LETTER);
        PP_AUX(s_ppB, WB_AUX_KITE, _spiDrawn ? 1 : 0);
        PP_AUX(s_ppB, WB_AUX_HALO, s_ppHalo);
        PP_FRAME(s_ppB);
    }

    // The two wind history curves, on top of the boat but under the pointers
    // and the value boxes: they are context, not something to read a number
    // off, and must never hide the live indicators.
    drawWindTrace(shownTwa, shownAwa);      // a NaN base skips that curve
    WS_PHASE_END(WS_PH_TRACE);
    PP_SKIP(*s_ppo);                        // the trace is the [HB] trace phase
    PP_ONLY(s_ppTriRowsO = s_ppTriRows;);
    drawInnerBorder();
    PP_END(*s_ppo, WO_PH_RINGI);
    drawOuterBorder();
    PP_END(*s_ppo, WO_PH_RINGO);
    if (!isnan(tws) && haveTwa) drawVmgMarkers(tws, absTwa);
    PP_END(*s_ppo, WO_PH_VMG);
    if (haveTwa) drawTwaPointer(nTwa);
    if (haveAwa) drawAwaPointer(nAwa);   // pointers on TOP of the VMG markers (no overpaint)
    PP_END(*s_ppo, WO_PH_PTR);
    // From here to the end of the paint nothing yields: every LVGL draw of
    // the KPIs, the heading box, the trim advice, the reef badge, the rudder
    // arc and the polar bar shares ONE draw context (see ctext()). It closes
    // with this scope, before the invalidate below.
    {
        WsBatch b(_canvas);
        if (_centerMode == CenterMode::ATTITUDE) {
            drawAttitudeKpis(b, roll, pitch, waveH, waveT);
        } else {
            // NaN, not the 0 stand-ins: the corners print "--" for a NaN. Fed the
            // stand-ins they printed "0° Stb" for a wind nobody knew.
            drawCornerKpis(b, stw, shownTwa, shownAwa, tws);
        }
        PP_END(*s_ppo, WO_PH_KPI);
        drawHeadingBelow(b, hdg);
        if (_centerMode != CenterMode::ATTITUDE) {
            drawTrimAdvice(b, knownAbs, shownTwa, tws);
            drawReefBadge(b, sail.reefCount, _spiDrawn);   // _spiDrawn: set by drawBoat() above
        }
        PP_END(*s_ppo, WO_PH_HDG);
        // Measured in detail (7B, 2026-08-30, before the switch to ringAA):
        // rims 25.2 ms, pointers 9.8, KPIs 12.1, rudder arc 4.9. The two rim
        // circles were thus half of this phase - hence ringAA().
        drawRudderArc(b, rudder);       // curved rudder-angle gauge below the rose, above the polar bar
        PP_END(*s_ppo, WO_PH_RUDARC);

        // ── Polar performance bar ─────────────────────────────────────────────
        // Vertical bar on the right side, between the ring and canvas edge.
        // Bar track: x=450..468, y=CY-80..CY+80 (height 160px)
        if (_centerMode != CenterMode::ATTITUDE) {
            // Horizontal performance bar along the bottom, between the AWA (left) and
            // TWS (right) corner fields. Left = 0 %, right = 120 %; fill grows right.
            const float bx0 = 108.f * UI_SF, bx1 = 342.f * UI_SF;   // ~30 % wider than before
            const float bw  = bx1 - bx0;
            const float by  = 456.f * UI_SF, bh = 20.f * UI_SF;

            // Dark track (fixed colour, so the white label reads on light AND dark theme)
            lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
            rd.bg_color = (uiTheme.windRingBg); rd.bg_opa = 235;
            rd.radius = UI_S(4); rd.border_width = 0;
            cdLvRect(b, (lv_coord_t)bx0, (lv_coord_t)by,
                     (lv_coord_t)bw, (lv_coord_t)bh, &rd);

            // Filled portion (0..perf..120 %)
            float polar = windPolarSpeed(knownAbs, tws);
            char pb[16];
            if (!isnan(stw) && !isnan(polar) && polar > 0.1f) {
                float perf = stw / polar;
                if (perf > 1.2f) perf = 1.2f;
                float fillW = perf / 1.2f * bw;
                lv_color_t barCol = (perf >= 0.95f) ? CLR_GREEN :
                                     (perf >= 0.75f) ? CLR_ORANGE : CLR_RED;
                rd.bg_color = barCol; rd.bg_opa = 230; rd.radius = UI_S(4);
                cdLvRect(b, (lv_coord_t)bx0, (lv_coord_t)by,
                         (lv_coord_t)fillW, (lv_coord_t)bh, &rd);
                snprintf(pb, sizeof(pb), "Polar  %d %%", (int)(perf*100));
            } else {
                snprintf(pb, sizeof(pb), "Polar  --");
            }

            // 100 % mark (vertical tick)
            lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
            ld.color = CLR_ON_ACCENT; ld.width = 1; ld.opa = LV_OPA_60;
            float x100 = bx0 + bw * 100.0f / 120.0f;
            lv_point_t pl[2] = {{(lv_coord_t)x100,(lv_coord_t)by},
                                 {(lv_coord_t)x100,(lv_coord_t)(by+bh)}};
            cdLvLine(b, pl, 2, &ld);

            // Label INSIDE the bar (single line, white = high contrast over fill + track)
            lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);
            td.font = FONT_SMALL; td.color = CLR_ON_ACCENT; td.opa = OPA_FULL;
            td.align = LV_TEXT_ALIGN_CENTER;
            cdLvText(b, (lv_coord_t)bx0, (lv_coord_t)(by + 2.f * UI_SF), (lv_coord_t)bw, &td, pb);
        }
    }
    // Everything stacked on top: borders, VMG markers, both pointers, corner
    // KPIs, heading box, trim advice, reef badge, rudder arc and polar bar.
    WS_PHASE_END(WS_PH_OVL);
    PP_END(*s_ppo, WO_PH_POLAR);            // with the close of the draw context

    lv_obj_invalidate(_canvas);
    PP_AUX(*s_ppo, WO_AUX_FTRI, s_ppTriRows - s_ppTriRowsO);
    PP_AUX(*s_ppz, WZ_AUX_LVCTX, s_ppCtx);
    PP_FRAME(*s_ppo);
    PP_FRAME(*s_ppz);
}

// ── Layer 1: Bezel background ─────────────────────────────────────────────────

void WindScreen::drawBezel() {
    // A radius-CIRCLE lv_canvas_draw_rect() this size is an LVGL radius mask
    // evaluated over the full 600x600 bounding box; fillEllipse() writes the
    // same disc as spans. Same reasoning as spanH().
    //
    // A RING, not a disc: drawInnerBg() repaints the middle out to R_INNER
    // right after this anyway. That was pi*185^2 = 107,500 of 201,850 pixels,
    // over half of this fill, written for nothing on every frame. One pixel
    // of overlap inwards, so nothing flashes through at the seam.
    //
    // And TWO rings, not one: the zone band in between (R_ZONE_I .. R_ZONE_O)
    // is left out, because drawZoneRing() covers every pixel of it - see the
    // comment on its base ring there. The bezel used to fill that band as
    // well, and the base ring and the sectors wrote over it again: three
    // writes for every one of its ~48,800 pixels (7B) where one is enough.
    // Each of the two rings reaches one pixel into the band, so the seam
    // never depends on two radii comparing equal; the base ring writes over
    // that overlap. Every pixel outside the band gets exactly what the one
    // ring gave it: both rings keep the old ring's own inner and outer radius,
    // and per row a span of a cdFillAnnulus() can only grow with its outer
    // radius and shrink with its inner one, so together they cover the old
    // ring minus the band. Bezel and base ring together now write 61,700
    // instead of 145,200 pixels a frame on the 7B (41,900 instead of 93,800
    // on the 4"), counted in the simulator.
    //
    // Yield cadence 80 instead of 40: each of these rings writes far fewer
    // pixels per row than the full one did, and on the 4" board every
    // renderYield() is a 1 ms sleep - with 40 the four thin rings of bezel
    // and base would sleep 33 times a frame instead of the old 19, with 80
    // it is 15.
    fillAnnulus(CX, CY, R_INNER - 1.f, R_ZONE_I + 1.f, C_BEZEL, LV_OPA_COVER, 80);
    fillAnnulus(CX, CY, R_ZONE_O,      R_OUTER + 1.f,  C_BEZEL, LV_OPA_COVER, 80);
}

// ── Layer 2: Coloured zone arcs ───────────────────────────────────────────────

void WindScreen::drawZoneRing(float absTwa) {
    // Base ring (dark)
    //
    // The six zone colours below cover exactly 360 degrees together
    // (2ng + 2(45-ng) + 2*20 + 2*35 + 2*50 + 60), but they do NOT cover the
    // whole base ring, which was written as one annulus R_ZONE_I .. R_ZONE_O+1
    // (arcRing()'s full-ring path adds the 1). Two parts of it stay visible:
    //   - its outer rim, a 1-px dark line: the base reaches R_ZONE_O + 1, the
    //     sectors' outer chords stop short of R_ZONE_O (from about r 228.3 on
    //     the 7B);
    //   - single pixels along the inner arc (r below about 194.1 on the 7B),
    //     where a chord or a truncated vertex dips inside R_ZONE_I.
    // There are no gaps at the radial seams between two sectors: they share
    // their integer vertices and the spans are inclusive.
    //
    // So the base ring is written only where it can show: two thin rings at
    // the exact radii of the old one, R_ZONE_I .. R_ZONE_I+3 and
    // R_ZONE_O-2 .. R_ZONE_O+1. Both lie within the old ring (same inner
    // resp. outer radius), and every old base pixel the sectors leave
    // uncovered lies in one of them: checked pixel by pixel in the simulator
    // for every noGoAngle 10..44 on both grids, with margin - every sector
    // pixel outside the two rings has all four neighbours inside the
    // sectors too, so a sector edge moved by a pixel (a vertex truncated the
    // other way, a crossing rounded differently on the device) still cannot
    // uncover a pixel outside them. The middle of the band was written
    // twice for nothing (9.3 ms of this phase, measured on the 7B), and the
    // bezel under it a third time (drawBezel()).
    fillAnnulus(CX, CY, R_ZONE_I,       R_ZONE_I + 3.f, C_BASE, LV_OPA_COVER, 80);
    fillAnnulus(CX, CY, R_ZONE_O - 2.f, R_ZONE_O + 1.f, C_BASE, LV_OPA_COVER, 80);
    PP_END(*s_ppz, WZ_PH_BASE);

    // The six zone colours were painted with an opacity ONTO that base ring,
    // which costs a read-modify-write per pixel. They tile the ring edge to
    // edge and never overlap each other, so every one of those pixels was
    // blending against the same constant C_BASE - mixing once here and then
    // writing opaque gives a bit-identical result for a plain store.
    #define ON_BASE(c, o)  lv_color_mix((c), C_BASE, (o))

    // No-Go: configurable half-angle (WebUI), centred at 0° / top.
    float ng = (float)appConfig.cfg.noGoAngle;
    if (ng < 10.f) ng = 10.f;
    if (ng > 44.f) ng = 44.f;
    arcRing(R_ZONE_I, R_ZONE_O, -ng, 2.f * ng, ON_BASE(C_NOGO, 220), LV_OPA_COVER);

    // Symmetric zones (Stb + Bb mirrored)
    symArcRing(R_ZONE_I, R_ZONE_O, ng, 45.f - ng, ON_BASE(C_CLOSE, 210), LV_OPA_COVER);  // close-hauled ng–45°
    symArcRing(R_ZONE_I, R_ZONE_O, 45.f, 20.f, ON_BASE(C_CLOSER, 195), LV_OPA_COVER);  // close reach 45–65°
    symArcRing(R_ZONE_I, R_ZONE_O, 65.f, 35.f, ON_BASE(C_BEAM,   190), LV_OPA_COVER);  // beam reach  65–100°
    symArcRing(R_ZONE_I, R_ZONE_O, 100.f,50.f, ON_BASE(C_BROAD,  190), LV_OPA_COVER);  // broad reach 100–150°

    // Running: 150° to 210° (symmetric around 180°)
    arcRing(R_ZONE_I, R_ZONE_O, 150.f, 60.f, ON_BASE(C_RUN, 200), LV_OPA_COVER);
    #undef ON_BASE
    PP_END(*s_ppz, WZ_PH_SECT);

    // Highlight current zone with a bright outer stroke. Not without a true
    // wind angle: pointOfSail(NaN) falls through every test to "running".
    lv_color_t hi = CLR_TEXT;
    float hiW = 4.f;
    if (!isnan(absTwa)) switch (pointOfSail(absTwa)) {
        case PoS::CloseHauled:
            symArcRing(R_ZONE_O - hiW, R_ZONE_O + 1, 30.f,  15.f, hi, 130); break;
        case PoS::CloseReach:
            symArcRing(R_ZONE_O - hiW, R_ZONE_O + 1, 45.f,  20.f, hi, 120); break;
        case PoS::BeamReach:
            symArcRing(R_ZONE_O - hiW, R_ZONE_O + 1, 65.f,  35.f, hi, 120); break;
        case PoS::BroadReach:
            symArcRing(R_ZONE_O - hiW, R_ZONE_O + 1, 100.f, 50.f, hi, 110); break;
        case PoS::Running:
            arcRing(R_ZONE_O - hiW, R_ZONE_O + 1, 150.f, 60.f,    hi, 120); break;
        default: break;
    }
    PP_END(*s_ppz, WZ_PH_HILITE);

    // Fine 5° subdivision ticks across the coloured zone band: 72 LVGL
    // lines in one draw context (see ctext()).
    lv_draw_line_dsc_t sd; lv_draw_line_dsc_init(&sd);
    sd.color = lv_color_hex(0x000000); sd.width = 1; sd.opa = LV_OPA_30;
    WsBatch b(_canvas);
    for (int w = 0; w < 360; w += 5) {
        lv_point_t p1 = wp(CX, CY, R_ZONE_I, (float)w);
        lv_point_t p2 = wp(CX, CY, R_ZONE_O, (float)w);
        lv_point_t pp[2] = { p1, p2 };
        cdLvLine(b, pp, 2, &sd);
    }
    PP_END(*s_ppz, WZ_PH_SUB5);   // the close of the draw context goes to dtick
}

// ── Layer 3: Degree ticks + labels ───────────────────────────────────────────

void WindScreen::drawTicksAndLabels() {
    WsBatch b(_canvas);   // 36 lines + 9 labels, one draw context (see ctext())
    PP_LAP_DECL(lblLap);
    for (int w = 0; w < 360; w += 10) {
        bool major = (w % 30) == 0;
        float iR   = major ? R_TICK_MAJ : R_TICK_MIN;
        lv_point_t p1 = wp(CX, CY, iR,      (float)w);
        lv_point_t p2 = wp(CX, CY, R_TICK_O, (float)w);
        lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
        ld.color = major ? C_TICK_MJ : C_TICK_MN;
        ld.width = major ? 2 : 1;
        ld.opa   = LV_OPA_COVER;
        { lv_point_t _dl[2]={p1,p2}; cdLvLine(b,_dl,2,&ld); }

        if (major && (w < 150 || w > 210)) {   // hide 150/180/210 — rudder gauge sits there
            char lbl[4];
            snprintf(lbl, sizeof(lbl), "%03d", w);
            float a  = w * DEG2RAD;
            float lx = CX + R_LABEL * sinf(a);
            float ly = CY - R_LABEL * cosf(a);
            // Centre text: use a 26-design-px wide box centred on label point
            PP_LAP_START(lblLap);
            ctext(b, lx - 13.f * UI_SF, ly - 6.f * UI_SF, 26.f * UI_SF,
                  FONT_TINY, C_TICK_MJ, LV_TEXT_ALIGN_CENTER, lbl);
            PP_LAP_STOP(lblLap);
        }
    }
    PP_AUX(*s_ppz, WZ_AUX_LBLUS, lblLap.us);
}

// ── Layer 4: Inner circle background ─────────────────────────────────────────

void WindScreen::drawInnerBg(bool skipHorizon) {
    // see drawBezel(). In ATTITUDE mode the sky/ground disc of
    // drawCenter_Attitude() covers 59,400 of these 107,500 pixels (7B) a few
    // layers later with opaque spans; horizonHole() cuts exactly the pixels
    // it is certain to overwrite out of each row, so they are written once
    // instead of twice. What lies between the two calls - the compass card -
    // stays outside the disc, and where it did not, the sky would overwrite
    // it alike in both cases. The ellipse's own span ends do not change.
    fillEllipse(CX, CY, R_INNER, R_INNER, C_INNER, LV_OPA_COVER,
                (skipHorizon && HORIZON_HOLE) ? horizonHole : nullptr);
}

// Row y of the sky/ground fill in drawCenter_Attitude(), recomputed: that loop
// runs y = CY - R .. CY + R in steps of 1, i.e. through whole numbers (see
// HORIZON_HOLE), so its row y is this y, with these very expressions - and
// they are exact, so they give the loop's xL, xR to the bit. The fill writes
// every pixel of [XL + 1, XR - 2] with an opaque store, whatever the roll and
// pitch: one segment covers [XL, XR - 1]; two segments split at XS = (int)xs
// cover [XL, XS - 1] and [XS, XR - 1]; a segment is left out only when xs
// lies within 0.5 of an end, so (int)xs is then at most XL + 1, or at least
// XR - 1, and the other segment still reaches XL + 1 resp. XR - 2. Only the
// two rim pixels XL and XR - 1 may stay unwritten, and so the inner fill
// keeps them. (A non-finite split, which leaves the row empty, needs an
// infinite roll; drawInstrument() then keeps the inner fill whole.)
bool WindScreen::horizonHole(int y, int &x0, int &x1) {
    const float R  = R_HORIZON;
    const float yf = (float)y;
    if (yf < CY - R || yf > CY + R) return false;
    const float dyc = yf - CY;
    const float h2  = R * R - dyc * dyc;
    if (h2 <= 0.f) return false;
    const float hx = sqrtf(h2);
    const float xL = CX - hx, xR = CX + hx;
    x0 = (lv_coord_t)xL + 1;
    x1 = (lv_coord_t)xR - 2;
    return x0 <= x1;
}

// ── Wind history traces (apparent + true, last ~60 s) ────────────────────────
//
// AN ORDINARY WIND STRIP CHART, TIPPED ONTO ITS SIDE AND LAID ALONG THE
// POINTER. Take the usual plot - time down one axis, wind speed across the
// other - and rotate it so that its TIME axis runs from the middle of the rose
// outward to the live pointer. The newest sample sits out at the rose and each
// sample migrates inward as it ages, reaching the centre after the full window.
// Across that axis stands the wind SPEED. One curve per pointer, drawn on that
// pointer's own axis, so each history belongs visibly to its arrow.
//
// THE SPEED SCALE IS ABSOLUTE AND SPANS THE WHOLE BAND: 0 kn at one edge, the
// full scale at the other, exactly like the printed plot. It is NOT measured
// from the current value. An earlier version anchored the newest point on the
// axis and drew everything as a deviation from it; that pins the curve to the
// pointer very neatly and is wrong, because the same bulge then means a
// different wind every minute and the absolute strength cannot be read at all.
//
// The reason the curve nevertheless runs roughly from the centre to the arrow -
// which is the whole point of the shape - is the choice of scale, not a trick
// of the geometry: the maximum is rounded UP to whole 5 kn, so a wind peaking
// at 7 kn is drawn against a 10 kn band and therefore sits near mid-band. That
// is the same reason the reference plot shows 4.3 kn on a 0-10 axis.
//
// THE SCALE STEPS, IT DOES NOT BREATHE. A continuously self-scaling band looks
// lively and is useless: the same curve would mean 4 kn one minute and 25 the
// next. Rounded up to whole 5 kn and never below 10, the picture holds still
// for minutes at a time and two glances an hour apart are comparable, while
// light airs still use most of the band.
//
// A LINE, NOT AN AREA. The printed plot fills under the curve; here that would
// put a large opaque wedge over the boat and the flow lines. The trace is
// context beneath the live pointers and must never hide them.
//
// NaN entries are gaps, not zeros - the polyline breaks there. A wind sensor
// that dropped out must leave a hole, never a straight line implying a steady
// wind that was never measured.
void WindScreen::drawWindTrace(float nTwa, float nAwa) {
    if (!_cbuf) return;
    const uint8_t mode = appConfig.cfg.windTraceMode;   // 0 off, 1 true, 2 both, 3 apparent
    if (mode == 0) return;

    // Copied under the lock, then released: the drawing below is far too long
    // to hold the model against the bus task. 120 * 16 bytes = 1.9 KB of the
    // loop task's 8 KB stack - see the note in DataModel.h on why this ring is
    // separate from (and much smaller than) windHistory.
    DataModel::WindTracePoint pts[DataModel::WIND_TRACE];
    int  idx; bool full;
    {
        auto lk = data.lock();
        if (!data.windTrace) return;
        idx  = data.windTraceIdx;
        full = data.windTraceFull;
        memcpy(pts, data.windTrace,
               sizeof(DataModel::WindTracePoint) * DataModel::WIND_TRACE);
    }
    const int n = full ? DataModel::WIND_TRACE : idx;
    if (n < 2) return;                       // nothing to connect yet

    // One scale for both curves, so apparent and true stay comparable - the
    // apparent wind is the faster of the two and therefore sets the band.
    float vmax = 0.f;
    for (int k = 0; k < n; k++) {
        const int i = full ? (idx + k) % DataModel::WIND_TRACE : k;
        if (!isnan(pts[i].aws) && pts[i].aws > vmax) vmax = pts[i].aws;
        if (!isnan(pts[i].tws) && pts[i].tws > vmax) vmax = pts[i].tws;
    }
    if (vmax <= 0.f) return;
    float scale = ceilf(vmax / 5.f) * 5.f;
    if (scale < 10.f) scale = 10.f;

    // Two passes so the true-wind curve always ends up on top of the apparent
    // one: they cross constantly, and a consistent order reads calmer than
    // whichever happened to be drawn last.
    // Shortest segment worth a draw call - see the note at the linePx below.
    static constexpr float MIN_SEG = 1.6f * UI_SF;

    const int passFrom = (mode == 1) ? 1 : 0;       // 1 = true only
    const int passTo   = (mode == 3) ? 1 : 2;       // 3 = apparent only
    for (int pass = passFrom; pass < passTo; pass++) {
        const bool       trueWind = (pass == 1);
        const lv_color_t col      = trueWind ? C_TWA_PTR : C_AWA_PTR;
        const float      base     = trueWind ? nTwa : nAwa;
        if (isnan(base)) continue;

        // Time axis along the live pointer, speed axis across it. Screen
        // convention as in wp(): 0 deg = up, y grows downward.
        const float a  = base * DEG2RAD;
        const float ux =  sinf(a), uy = -cosf(a);   // outward, towards the rose
        const float vx =  cosf(a), vy =  sinf(a);   // across: 0 kn left, full right

        float px = 0.f, py = 0.f; bool have = false;
        for (int k = 0; k < n; k++) {
            const int i = full ? (idx + k) % DataModel::WIND_TRACE : k;
            const float spd = trueWind ? pts[i].tws : pts[i].aws;
            if (isnan(spd)) { have = false; continue; }

            // Age in samples counted back from the newest. The divisor is the
            // FULL window, not n: while the ring is still filling the tail must
            // stop short of the centre instead of stretching to fill it.
            const int   agek = (n - 1) - k;
            float       fAge = (float)agek / (float)(DataModel::WIND_TRACE - 1);
            if (fAge > 1.f) fAge = 1.f;
            const float r = R_TRACE_MAX * (1.f - fAge);

            // Speed across the band: 0 kn at -W_TRACE, full scale at +W_TRACE.
            float f = spd / scale;
            if (f < 0.f) f = 0.f;
            if (f > 1.f) f = 1.f;
            float d = W_TRACE * (2.f * f - 1.f);

            // Close the strip to a point at the centre. Without this the oldest
            // sample sits a full half-width off to the side and the curve looks
            // like a stray scratch across the boat instead of something that
            // starts in the middle of the rose. The ramp costs the amplitude of
            // the last ~12 s of history, which is the part nobody reads.
            const float fR = r / R_TRACE_MAX;
            if (fR < 0.25f) d *= fR / 0.25f;

            float x = CX + ux * r + vx * d;
            float y = CY + uy * r + vy * d;

            // Keep the corner case inside the dial: the outermost sample at the
            // far edge of the band would otherwise land on the degree ring.
            const float dx = x - CX, dy = y - CY;
            const float rr = sqrtf(dx * dx + dy * dy);
            if (rr > R_TRACE_MAX) {
                const float s = R_TRACE_MAX / rr;
                x = CX + dx * s; y = CY + dy * s;
            }

            if (have) {
                // Do not draw a segment shorter than the line is wide. 120
                // samples over R_TRACE_MAX means a radial step of ~1.5 px, so
                // most segments are sub-pixel and cost a full linePx() call
                // each for nothing: 238 calls per frame, and the per-call setup
                // - not the pixels - was 16 ms of an 188 ms frame. Holding the
                // anchor until the pen has actually moved keeps the identical
                // picture. The last sample is always drawn so the curve still
                // ends exactly at its pointer.
                const float sx = x - px, sy = y - py;
                if ((sx * sx + sy * sy) < MIN_SEG * MIN_SEG && k < n - 1) continue;
                // Age fade over the whole window: the newest segment at full
                // strength, the oldest at a third, so the eye finds "now"
                // without a legend.
                lv_opa_t opa = (lv_opa_t)((float)LV_OPA_80 * (1.f - 0.55f * fAge));
                linePx(px, py, x, y, col, opa, 1.4f * UI_SF);
            }
            px = x; py = y; have = true;
        }
    }
    renderYield();
}

// ── Layer 5: Wind flow lines (parallel to TWA) ────────────────────────────────

void WindScreen::drawWindLines(float twaDeg) {
    // Screen convention: angle (twaDeg-90)*DEG2RAD gives standard math angle.
    // Along-wind unit vector in screen space: (sinf(a), -cosf(a))
    // Perpendicular (CCW rotate): (cosf(a), sinf(a))
    float a    = twaDeg * DEG2RAD;
    float sinA = sinf(a), cosA = cosf(a);
    // Perp vector (for lateral offset between lines)
    float pX = cosA, pY = sinA;

    // Each flow line is a chord CLIPPED to the inner circle, so it never paints
    // over the wind rose / ticks. Drawn faint as a background layer (behind boat).
    const float Rin  = R_INNER - 3.f;          // -3 pairs with the (unscaled) 3px inner border
    const float step = 24.f * UI_SF;
    for (float off = -(Rin - 4.f); off <= (Rin - 4.f); off += step) {
        float half = sqrtf(Rin * Rin - off * off);   // chord half-length inside the circle
        float x1 = CX - half * sinA + off * pX;
        float y1 = CY + half * cosA + off * pY;
        float x2 = CX + half * sinA + off * pX;
        float y2 = CY - half * cosA + off * pY;
        linePx(x1, y1, x2, y2, C_WIND_LN, LV_OPA_40);
    }
}

// ── Layer 6: Boat bird's-eye ─────────────────────────────────────────────────
//
//   Coordinate origin: CX, CY  (= UI_WIND_CX/CY: design 240,226 × UI_SF).
//   Bow: CY – 30,  Stern: CY + 28.  Scale ≈ 0.333 px / BoatPainter unit.
//
//   Draw order inside drawBoat():
//     hull fill → hull outline → forestay → foresail (butterfly jib,
//     downwind sail, Code Zero or jib, each with its own lines) → mainsail
//     → boom → mast → rudder
//   (the "Butterfly" label follows later, over the compass letters - see
//   drawInstrument())

void WindScreen::drawBoat(const SailState &sail) {
    const float BS   = BOAT_S;                       // boat scale factor
    const float ss   = sail.leewardSide;             // –1 = port lee, +1 = stbd lee
    const uint32_t tick = lv_tick_get();

    // ── Bird's-eye geometry (bow up). Mast ≈ 40% aft of bow ≈ ship centre. ──
    const float bowX  = CX, bowY  = CY - 30.f * BS;  // forestay / jib tack (bow)
    const float mastX = CX, mastY = CY -  6.f * BS;  // MAST = single point, centred

    // ── Continuous + low-pass-smoothed trim (signed deg, + = clew to Stb) ──
    // Targets come from the apparent-wind trim model; the filter removes any
    // stepping and lets a tack swing smoothly through the centreline.
    // Butterfly (wing-on-wing): on a dead run with the option enabled, the jib
    // is poled out to WINDWARD — opposite the main — so the rig spreads like a
    // butterfly. The smoothing then swings the jib across the foredeck.
    bool butterfly = appConfig.cfg.allowButterfly && sail.running;
    float tgtBoom, tgtJib;
    if (sail.luffing) {                              // head-to-wind → flap around centre
        tgtBoom = sinf((float)tick * 0.018f)        * 15.f;
        tgtJib  = sinf((float)tick * 0.018f + 0.7f) * 17.f;
    } else {
        tgtBoom = ss * sail.boomAngleDeg;
        tgtJib  = butterfly ? (-ss * 84.f)           // poled out to windward
                            : ( ss * sail.headsailAngleDeg);
    }
    // Downwind sail targets, signed like the boom. Computed - and smoothed
    // below - on EVERY frame, whether a kite is drawn or not, and for both
    // kinds at once: when the kite appears (wind drawn aft into its window)
    // or the type is changed in the WebUI, its smoothed trim already sits
    // where the boom's smoothing would have it, and the two move on together.
    // Two things keep it from swinging in from a wind that is gone:
    //  - resetRig() (back on screen, or not painted for RIG_STALE_MS) makes
    //    the next frame start every trim at its target;
    //  - the gennaker never swings ACROSS the boat. A target on the other
    //    side of its smoothed angle - the lee side changed, in a gybe while it
    //    was hidden (its window ends at 160 deg) or after a jump of the wind -
    //    is taken at once. Swinging through 0, the clew would sweep the deck
    //    and the mainsail, and the gennaker could reappear on the new side
    //    halfway across. The symmetric kite does swing: it stays up on a dead
    //    run, and turning through 0 is its square-run picture.
    const float tgtSpiRot = ss * SpiGeom::symRotationDeg(sail.awaAbs);
    const float tgtGen    = ss * SpiGeom::genSheetDeg(sail.awaAbs);
    if (!_trimInit) {
        _smBoomDeg = tgtBoom;   _smJibDeg = tgtJib;
        _smSpiRot  = tgtSpiRot; _smGenDeg = tgtGen;
        _trimInit  = true;
    } else {
        // k is per FRAME, so the time constant follows the frame rate: ~0.5 s
        // at 10 Hz, longer on a board that paints slower. Boom, jib and kite
        // share it so the rig moves as one.
        const float k = 0.18f;
        _smBoomDeg += (tgtBoom   - _smBoomDeg) * k;
        _smJibDeg  += (tgtJib    - _smJibDeg ) * k;
        _smSpiRot  += (tgtSpiRot - _smSpiRot ) * k;
        if ((tgtGen > 0.f) == (_smGenDeg > 0.f))
            _smGenDeg += (tgtGen - _smGenDeg) * k;
        else
            _smGenDeg  = tgtGen;              // other side: no swing across
    }
    _spiDrawn = !butterfly && sail.useSpinnaker;   // as the branch below decides
    _butterflyDrawn = butterfly;                   // its label: drawInstrument()
    PP_END(s_ppB, WB_PH_TRIM);

    // ── Hull ───────────────────────────────────────────────────────────────
    // ── Hull — smooth deck-plan outline (mirrored half-beam stations) ──────
    // There used to be a keel here first, a dark oval with a 3-px line
    // through it, "behind" the hull. It could never be seen: the hull fill
    // below is opaque, and every pixel the keel could touch lies deep inside
    // it - the oval is 1.8 boat units wide, the line reaches 2.5 px either
    // side of the centreline, between -1 and 7.6 units, where the hull is
    // 10 to 11 units wide. Neither depends on anything but the board's
    // geometry, so that holds for every wind, trim and theme (checked pixel
    // by pixel in the simulator on both grids). Dropping it saves an
    // ellipse fill and a wide anti-aliased line every frame.
    // Deck-plan half-beam stations (bow→stern), traced 1:1 from the reference
    // PNG via tools/extract_hull.py (aspect 2.86: fine bow point, full body,
    // rounded wide stern). {y, half-beam} in BS units.
    static const float ST[18][2] = {
        { -32.0f,  0.07f}, { -28.2f,  2.43f}, { -24.5f,  4.36f},
        { -20.7f,  6.06f}, { -16.9f,  7.47f}, { -13.2f,  8.65f},
        {  -9.4f,  9.53f}, {  -5.6f, 10.13f}, {  -1.9f, 10.57f},
        {   1.9f, 10.88f}, {   5.6f, 11.08f}, {   9.4f, 11.19f},
        {  13.2f, 11.12f}, {  16.9f, 10.90f}, {  20.7f, 10.53f},
        {  24.5f, 10.06f}, {  28.2f,  9.35f}, {  32.0f,  4.82f},
    };
    lv_point_t hp[36];
    for (int i = 0; i < 18; i++) {          // right side, bow → stern
        hp[i].x = (lv_coord_t)(CX + ST[i][1] * BS);
        hp[i].y = (lv_coord_t)(CY + ST[i][0] * BS);
    }
    for (int i = 0; i < 18; i++) {          // left side, stern → bow
        hp[18 + i].x = (lv_coord_t)(CX - ST[17 - i][1] * BS);
        hp[18 + i].y = (lv_coord_t)(CY + ST[17 - i][0] * BS);
    }
    fillPolygon(hp, 36, C_INNER, LV_OPA_COVER);
    for (int i = 0; i < 36; i++)
        linePx(hp[i].x, hp[i].y, hp[(i+1)%36].x, hp[(i+1)%36].y, C_HULL, LV_OPA_COVER, 2);
    PP_END(s_ppB, WB_PH_HULL);


    // ── Forestay (bow → mast, on the centreline = the jib luff) ────────────
    lv_color_t cRig = (uiTheme.windRigging);
    linePx(bowX, bowY, mastX, mastY, cRig, LV_OPA_60, 1);

    // ── Resolve the two clews from the smoothed trim ───────────────────────
    float bss   = (_smBoomDeg >= 0.f) ? 1.f : -1.f;
    float bAng  = fabsf(_smBoomDeg) * DEG2RAD;
    const float boomLen = 32.f * BS;
    float clewMX = mastX + bss * boomLen * sinf(bAng);   // MAINSAIL clew (boom end)
    float clewMY = mastY + boomLen * cosf(bAng);

    float fp = appConfig.cfg.foresailPercent / 100.f;    // headsail size → graphic size
    if (fp < 0.25f) fp = 0.25f;
    if (fp > 1.60f) fp = 1.60f;
    float jss   = (_smJibDeg >= 0.f) ? 1.f : -1.f;
    float jAng  = fabsf(_smJibDeg) * DEG2RAD;
    const float jibLen = 30.f * BS * fp;
    float clewJX = bowX + jss * jibLen * sinf(jAng);     // JIB clew
    float clewJY = bowY + jibLen * cosf(jAng);

    lv_color_t sailCol  = sail.luffing ? CLR_RED : C_SAIL;   // red = luffing → bear away
    lv_color_t leechCol = sail.luffing ? CLR_RED : CLR_TEXT; // bright leech edge → reads as a distinct sail
    lv_opa_t   sailOpa  = sail.luffing ? LV_OPA_40 : LV_OPA_50;  // translucent: overlaps stay readable
    float      mCam     = sail.luffing ? 0.05f : sail.mainCamber;
    float      jCam     = sail.luffing ? 0.05f : sail.jibCamber;

    // ── FORESAIL at the BOW ──────────────────────────────────────────────
    // Butterfly first (explicit user choice on a run); else Spi / Code Zero;
    // else the normal leeward jib (hidden when running).
    if (butterfly) {
        drawSailFoil(bowX, bowY, clewJX, clewJY, jss, jCam, sailCol, sailOpa, leechCol, 2);
        linePx(mastX, mastY, clewJX, clewJY, cRig, LV_OPA_70, 2);   // whisker pole
        // The "Butterfly" label is printed by drawInstrument(), over the
        // compass letters that are drawn after the boat.
    } else if (sail.useSpinnaker) {
        drawSpinnaker(sail);
    } else if (sail.useCodeZero) {
        drawCodeZero(sail);
    } else if (!sail.running) {
        drawSailFoil(bowX, bowY, clewJX, clewJY, jss, jCam, sailCol, sailOpa, leechCol, 2);
        linePx(clewJX, clewJY, CX + jss * 9.f * BS, CY + 4.f * BS, CLR_GREEN, LV_OPA_50, 1); // jib sheet
    }
    PP_END(s_ppB, WB_PH_FORE);   // forestay included

    // ── MAINSAIL at the MAST (centre) ──────────────────────────────────────
    drawSailFoil(mastX, mastY, clewMX, clewMY, bss, mCam, sailCol, sailOpa, leechCol, 2);
    linePx(mastX, mastY, clewMX, clewMY, cRig, LV_OPA_COVER, 2);   // boom spar (on top)

    // ── Mast: a single point at the ship's centre (NOT a fore-aft line) ────
    { lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
      rd.bg_color = cRig; rd.bg_opa = LV_OPA_COVER; rd.radius = LV_RADIUS_CIRCLE; rd.border_width = 0;
      WsBatch b(_canvas);
      cdLvRect(b, (lv_coord_t)(mastX - UI_S(3)), (lv_coord_t)(mastY - UI_S(3)), UI_S(6), UI_S(6), &rd); }
    PP_END(s_ppB, WB_PH_MAIN);

    // ── Rudder ─────────────────────────────────────────────────────────────
    // No blade without a rudder angle: drawn at 0 it said "midships" for a
    // sensor that is missing or has gone quiet (BusWatch blanks it after
    // 10 s). Read under the lock, drawn after it - the blade needs no data.
    float ra;
    {
        auto lk = data.lock();
        ra = data.rudderAngle;
    }
    if (!isnan(ra)) drawRudder(ra);
    PP_END(s_ppB, WB_PH_RUD);
}

// Rudder blade at the stern: only blade and pivot, no degree figure - that
// was on screen twice, the arc below (drawRudderArc) shows the value. The
// blade stays as a direction indicator that reads at a glance, and it is
// larger than in the first version (half-length 4 -> 6, stroke 2 -> 3).
void WindScreen::drawRudder(float rudderDeg) {
    const float BS   = BOAT_S;
    const float pX   = CX;
    const float pY   = CY + 24.7f * BS;
    const float half = 6.0f * BS;
    const float ra   = rudderDeg * DEG2RAD;
    const float dx   = half * sinf(ra);
    const float dy   = half * cosf(ra);

    const lv_color_t col = (uiTheme.windFlow);
    linePx(pX - dx, pY - dy, pX + dx, pY + dy, col, LV_OPA_COVER, 3.f);
    discAA(pX, pY, 2.5f * UI_SF, col, LV_OPA_COVER);
}

// ── Rudder (cyan, pivots at scaled Cy=74 * 0.333 = CY+24.7) ─────────────────


// ── Sheet lines: jib clew → fairlead → winch ─────────────────────────────────
void WindScreen::drawSheetLines(const SailState &sail, float clewX, float clewY) {
    const float BS  = BOAT_S;
    const float ss  = sail.leewardSide;
    const float flX = CX + ss * 8.7f * BS,  flY = CY + 3.3f * BS;
    const float wX  = CX + ss * 7.0f * BS,  wY  = CY + 14.7f * BS;

    lv_color_t col = CLR_GREEN;   // green sheet
    linePx(clewX, clewY, flX, flY, col, LV_OPA_60, 1);
    linePx(flX, flY, wX, wY, col, LV_OPA_50, 1);
}

// ── Downwind sail ─────────────────────────────────────────────────────────────
// What the boat carries is the user's choice (WebUI: Display tab, screen
// list, Wind & Trim; config "sail.spi_type"): nothing, a symmetric spinnaker
// drawn as panels ("gores") or as a shaded balloon, or a gennaker. All
// geometry - outline, trim laws, tack, clew, blocks and winches - is in
// SpiGeom.h, in boat units; these functions turn it into pixels and nothing
// else, so a port to another renderer starts from SpiGeom, not from here.
//
// Colours: everything is CLR_ORANGE plus that orange mixed into the inner
// background (C_INNER), so the kite follows the light, dark and night themes
// without a palette entry of its own. Pole in the rigging colour, sheet and
// guy in the green of the jib sheet.
//
// Replaces an orange ellipse at 82 % opacity that read as an egg yolk and
// blended every one of its ~21,700 pixels (7B) - read, mix, write. Counted
// in the simulator per frame, kite with pole and lines, mean over its window
// (opaque pixel = one 2-byte write, blended pixel = 2-byte read + write):
//            7B (1024x600)   4" (480x480)
//   ellipse      87 KB          57 KB
//   panels       31 KB          22 KB    (panels opaque, only edges blended)
//   balloon      31 KB          22 KB    (bands opaque, only edges blended)
//   gennaker     13 KB           9 KB    (body blended at 215)
// Yields: the kite's polygons are filled without the per-20-row
// renderYield() of fillPolygon() and drawSpinnaker() yields ONCE after the
// whole kite, which writes about as much as one 20-row chunk of the
// background fill. On the 4" board every renderYield() is a 1 ms sleep, and
// the per-row offers of the five balloon bands alone came to 13-15 of them a
// frame - against the ellipse's 5, for less memory traffic.

static inline lv_coord_t spiRnd(float v) { return (lv_coord_t)floorf(v + 0.5f); }

// Side of the symmetric kite's pole, guy and sheet: the side the boom is on
// NOW, i.e. the sign of the smoothed boom angle, not leewardSide. In a gybe
// leewardSide flips at once while the boom swings across over several frames;
// the pole and guy taking the new side at once would cross the mainsail still
// standing on the old one. Following the boom is also what a crew does: the
// pole is switched when the main has come across. In a steady state the two
// agree - the boom target is leewardSide * 4..86 deg. (The crescent's own
// rotation is smoothed with the boom's k, so on a reach it crosses 0 in the
// same frame as the boom.)
static inline float spiRigSide(float smBoomDeg) { return smBoomDeg >= 0.f ? 1.f : -1.f; }

// Symmetric outline in canvas pixels for a signed rotation.
static void spiSymPx(float cx, float cy, float bs, float rotDeg,
                     float fx[], float fy[], float lx[], float ly[]) {
    SpiGeom::Pt foot[SpiGeom::SYM_N + 1], lead[SpiGeom::SYM_N + 1];
    SpiGeom::symOutline(rotDeg, foot, lead);
    for (int i = 0; i <= SpiGeom::SYM_N; i++) {
        fx[i] = cx + foot[i].x * bs;  fy[i] = cy + foot[i].y * bs;
        lx[i] = cx + lead[i].x * bs;  ly[i] = cy + lead[i].y * bs;
    }
}

void WindScreen::drawSpinnaker(const SailState &sail) {
    // sail.spiType, not appConfig: the same read that opened the window.
    switch (sail.spiType) {
        case SpiType::Gores:    drawSpiGores(sail);   break;
        case SpiType::Balloon:  drawSpiBalloon(sail); break;
        case SpiType::Gennaker: drawGennaker(sail);   break;
        case SpiType::None:     return;   // never asked: its window is empty
    }
    renderYield();   // once for the whole kite, see the block above
}

// Sheet: clew -> quarter block -> winch on the lee side. Guy (symmetric kite
// only): tack -> twing at the widest beam -> winch on the windward side. Same
// green, widths and opacities as the jib sheet in drawSheetLines().
void WindScreen::spiLines(float tackX, float tackY, float clewX, float clewY,
                          float leeSide, bool guy) {
    const float BS = BOAT_S;
    const lv_color_t col = CLR_GREEN;
    const SpiGeom::Pt q = SpiGeom::quarterBlock(leeSide);
    const SpiGeom::Pt w = SpiGeom::sheetWinch(leeSide);
    const float qX = CX + q.x * BS, qY = CY + q.y * BS;
    const float wX = CX + w.x * BS, wY = CY + w.y * BS;
    linePx(clewX, clewY, qX, qY, col, LV_OPA_60, 1);
    linePx(qX, qY, wX, wY, col, LV_OPA_50, 1);
    if (guy) {
        const SpiGeom::Pt t = SpiGeom::twing(leeSide);
        const SpiGeom::Pt g = SpiGeom::guyWinch(leeSide);
        const float tX = CX + t.x * BS, tY = CY + t.y * BS;
        linePx(tackX, tackY, tX, tY, col, LV_OPA_60, 1);
        linePx(tX, tY, CX + g.x * BS, CY + g.y * BS, col, LV_OPA_50, 1);
    }
}

// ---- Symmetric spinnaker, panels ("gores") ----------------------------------
// Six vertical panels in two tones: orange, and orange mixed into the inner
// background. Both tones are what the prototype's panels at 215 opacity
// showed over the inner background, mixed ONCE and written opaque - the
// same colours, but the hull's bow and the forestay no longer show through
// the kite, and no pixel is read back. Seams over the tone changes hide the
// polygon stair where two panels meet; leading edge 2 px, foot 1 px. Pole
// from the mast to the tack (the windward clew), guy and sheet.
void WindScreen::drawSpiGores(const SailState &) {
    const int   N  = SpiGeom::SYM_N;
    const float BS = BOAT_S, side = spiRigSide(_smBoomDeg);
    float fx[N + 1], fy[N + 1], lx[N + 1], ly[N + 1];
    spiSymPx(CX, CY, BS, _smSpiRot, fx, fy, lx, ly);

    const lv_color_t c1 = CLR_ORANGE;
    const lv_color_t p1 = lv_color_mix(c1, C_INNER, 215);
    const lv_color_t p2 = lv_color_mix(lv_color_mix(c1, C_INNER, 140), C_INNER, 215);
    const int G = SpiGeom::SYM_GORES, PER = N / G;
    for (int g = 0; g < G; g++) {
        const int i0 = g * PER, i1 = i0 + PER;
        lv_point_t p[2 * (N / SpiGeom::SYM_GORES + 1)];
        int k = 0;
        for (int i = i0; i <= i1; i++) { p[k].x = spiRnd(lx[i]); p[k].y = spiRnd(ly[i]); k++; }
        for (int i = i1; i >= i0; i--) { p[k].x = spiRnd(fx[i]); p[k].y = spiRnd(fy[i]); k++; }
        fillPolygon(p, k, (g & 1) ? p2 : p1, LV_OPA_COVER, 0);
    }
    for (int g = 1; g < G; g++) {
        const int i = g * PER;
        linePx(fx[i], fy[i], lx[i], ly[i], c1, 170, 1);
    }
    for (int i = 0; i < N; i++) {
        linePx(lx[i], ly[i], lx[i + 1], ly[i + 1], c1, LV_OPA_COVER, 2);
        linePx(fx[i], fy[i], fx[i + 1], fy[i + 1], c1, LV_OPA_COVER, 1);
    }
    const int iT = SpiGeom::symTackIndex(side), iC = SpiGeom::symClewIndex(side);
    linePx(CX, CY + SpiGeom::MAST_Y * BS, fx[iT], fy[iT], (uiTheme.windRigging), LV_OPA_COVER, 2);
    spiLines(fx[iT], fy[iT], fx[iC], fy[iC], side, true);
}

// ---- Symmetric spinnaker, shaded balloon ------------------------------------
// The same outline, filled in five bands from the foot (pale) to the leading
// edge (full orange), so the kite reads as a filled dome. The tones are mixed
// ONCE against the inner background and the bands written OPAQUE, like the
// panels' two tones - no read-modify-write per pixel. Two light seams,
// leading edge 2 px, foot 1 px; pole, guy and sheet as with the panels.
void WindScreen::drawSpiBalloon(const SailState &) {
    const int   N  = SpiGeom::SYM_N;
    const int   B  = SpiGeom::BALLOON_BANDS;
    const float BS = BOAT_S, side = spiRigSide(_smBoomDeg);
    float fx[N + 1], fy[N + 1], lx[N + 1], ly[N + 1];
    spiSymPx(CX, CY, BS, _smSpiRot, fx, fy, lx, ly);

    float tb[B + 1];
    for (int j = 0; j <= B; j++) tb[j] = SpiGeom::balloonBandEdge(j);
    lv_point_t p[2 * (N + 1)];
    for (int j = 0; j < B; j++) {
        // 140/255 orange at the foot -> full orange at the leading edge
        const lv_opa_t m = (lv_opa_t)(140 + (255 - 140) * j / (B - 1));
        const lv_color_t c = lv_color_mix(CLR_ORANGE, C_INNER, m);
        int k = 0;
        for (int i = 0; i <= N; i++) {
            p[k].x = spiRnd(fx[i] + (lx[i] - fx[i]) * tb[j + 1]);
            p[k].y = spiRnd(fy[i] + (ly[i] - fy[i]) * tb[j + 1]); k++;
        }
        for (int i = N; i >= 0; i--) {
            p[k].x = spiRnd(fx[i] + (lx[i] - fx[i]) * tb[j]);
            p[k].y = spiRnd(fy[i] + (ly[i] - fy[i]) * tb[j]); k++;
        }
        fillPolygon(p, k, c, LV_OPA_COVER, 0);
    }
    const lv_color_t cs = lv_color_mix(CLR_ORANGE, C_INNER, 100);
    const int sa = SpiGeom::BALLOON_SEAM_A, sb = SpiGeom::BALLOON_SEAM_B;
    linePx(fx[sa], fy[sa], lx[sa], ly[sa], cs, LV_OPA_COVER, 1);
    linePx(fx[sb], fy[sb], lx[sb], ly[sb], cs, LV_OPA_COVER, 1);
    for (int i = 0; i < N; i++) {
        linePx(lx[i], ly[i], lx[i + 1], ly[i + 1], CLR_ORANGE, LV_OPA_COVER, 2);
        linePx(fx[i], fy[i], fx[i + 1], fy[i + 1], CLR_ORANGE, LV_OPA_COVER, 1);
    }
    const int iT = SpiGeom::symTackIndex(side), iC = SpiGeom::symClewIndex(side);
    linePx(CX, CY + SpiGeom::MAST_Y * BS, fx[iT], fy[iT], (uiTheme.windRigging), LV_OPA_COVER, 2);
    spiLines(fx[iT], fy[iT], fx[iC], fy[iC], side, true);
}

// ---- Gennaker ---------------------------------------------------------------
// Tack on the centreline at the stem tip, clew out to leeward and aft; no
// pole, no guy, the sheet to the leeward quarter. Leeward part in orange, luff
// panel in the second tone (both at 215), three seams fanning from the luff
// panel across the body, leeward edge 2 px, luff 1 px. The sheet's side comes
// from the smoothed sheeting angle itself, so it always leads from the clew
// that is drawn (drawBoat() never lets that angle swing across the boat).
void WindScreen::drawGennaker(const SailState &) {
    const int   N  = SpiGeom::GEN_N;
    const float BS = BOAT_S;
    SpiGeom::GenOutline o;
    if (!SpiGeom::genOutline(_smGenDeg, o)) return;
    float ex[N + 1], ey[N + 1], wx[N + 1], wy[N + 1], mx[N + 1], my[N + 1];
    for (int i = 0; i <= N; i++) {
        ex[i] = CX + o.lee[i].x   * BS;  ey[i] = CY + o.lee[i].y   * BS;
        wx[i] = CX + o.luff[i].x  * BS;  wy[i] = CY + o.luff[i].y  * BS;
        mx[i] = CX + o.panel[i].x * BS;  my[i] = CY + o.panel[i].y * BS;
    }
    const lv_color_t c1 = CLR_ORANGE;
    const lv_color_t c2 = lv_color_mix(CLR_ORANGE, C_INNER, 140);
    lv_point_t p[2 * (N + 1)];
    int k = 0;
    for (int i = 0; i <= N; i++) { p[k].x = spiRnd(ex[i]); p[k].y = spiRnd(ey[i]); k++; }
    for (int i = N; i >= 0; i--) { p[k].x = spiRnd(mx[i]); p[k].y = spiRnd(my[i]); k++; }
    fillPolygon(p, k, c1, 215, 0);
    k = 0;
    for (int i = 0; i <= N; i++) { p[k].x = spiRnd(mx[i]); p[k].y = spiRnd(my[i]); k++; }
    for (int i = N; i >= 0; i--) { p[k].x = spiRnd(wx[i]); p[k].y = spiRnd(wy[i]); k++; }
    fillPolygon(p, k, c2, 215, 0);

    for (int s = 0; s < SpiGeom::GEN_SEAMS; s++) {
        const int e = SpiGeom::genSeamEnd(s), b = e - SpiGeom::GEN_SEAM_BACK;
        linePx(mx[b], my[b], ex[e], ey[e], c2, LV_OPA_COVER, 1);
    }
    for (int i = 0; i < N; i++) {
        linePx(ex[i], ey[i], ex[i + 1], ey[i + 1], c1, LV_OPA_COVER, 2);
        linePx(wx[i], wy[i], wx[i + 1], wy[i + 1], c1, LV_OPA_COVER, 1);
    }
    const float gs = (_smGenDeg >= 0.f) ? 1.f : -1.f;
    spiLines(ex[0], ey[0], ex[N], ey[N], gs, false);
}

// ── Code Zero ─────────────────────────────────────────────────────────────────
//   BoatPainter: armLen = JibArmLen(52) * 1.68 = ~87 units
//   angleAbs = headsailAngleDeg * π/180 * 1.18  (flatter profile than jib)
//   head at mast top, tack at bow, clew at (leeSign*armLen*sin(a), MastY+armLen*cos(a))
void WindScreen::drawCodeZero(const SailState &sail) {
    const float BS = BOAT_S;
    const float ss = sail.leewardSide;
    const float bowX = CX, bowY = CY - 30.f * BS;     // tack at the bow
    const float armLen = 34.f * BS;                   // big, flat-ish reaching headsail
    const float aAbs   = sail.headsailAngleDeg * DEG2RAD;
    float clewX = bowX + ss * armLen * sinf(aAbs);
    float clewY = bowY + armLen * cosf(aAbs);

    lv_color_t gold = CLR_YELLOW;
    drawSailFoil(bowX, bowY, clewX, clewY, ss, 0.13f, gold, 200, gold, 1);

    float lbx = (bowX + clewX) * 0.5f, lby = (bowY + clewY) * 0.5f;
    { WsBatch b(_canvas);
      ctext(b, lbx - 8.f * UI_SF, lby - 5.f * UI_SF, 16.f * UI_SF, FONT_TINY, gold, LV_TEXT_ALIGN_CENTER, "C0"); }

    drawSheetLines(sail, clewX, clewY);
}

// ── Layer 7+8: Circle borders ─────────────────────────────────────────────────

void WindScreen::drawInnerBorder() {
    ringAA(CX, CY, R_INNER, 3.f, (uiTheme.windMast), LV_OPA_COVER);
}

void WindScreen::drawOuterBorder() {
    ringAA(CX, CY, R_OUTER, 2.f, (uiTheme.windHull), LV_OPA_COVER);
}

// ── Layer 9: TWA pointer (blue triangle, tip points inward at zone outer) ────

void WindScreen::drawTwaPointer(float twaDeg) {
    triPointer(twaDeg, R_ZONE_O, R_OUTER + 11.f * UI_SF, 12.f * UI_SF, C_TWA_PTR);   // big arrow, outer band
    // "T" label on the pointer body
    float a  = twaDeg * DEG2RAD;
    float lx = CX + (R_OUTER + 1.f) * sinf(a);
    float ly = CY - (R_OUTER + 1.f) * cosf(a);
    WsBatch b(_canvas);   // after triPointer(), which may yield
    ctext(b, lx - 7.f * UI_SF, ly - 8.f * UI_SF, 14.f * UI_SF,
          FONT_SMALL, CLR_ON_ACCENT, LV_TEXT_ALIGN_CENTER, "T");
}

// ── Layer 10: AWA pointer (red, inside zone ring, tip at inner circle edge) ──

void WindScreen::drawAwaPointer(float awaDeg) {
    triPointer(awaDeg, R_INNER + 1.f, R_ZONE_O - 2.f, 12.f * UI_SF, C_AWA_PTR);  // same size as TWA, inner band
    float a  = awaDeg * DEG2RAD;
    float lx = CX + (R_ZONE_O - 12.f * UI_SF) * sinf(a);   // on the pointer body (centroid), not at the tip
    float ly = CY - (R_ZONE_O - 12.f * UI_SF) * cosf(a);
    WsBatch b(_canvas);   // after triPointer(), which may yield
    ctext(b, lx - 7.f * UI_SF, ly - 7.f * UI_SF, 14.f * UI_SF,
          FONT_SMALL, CLR_ON_ACCENT, LV_TEXT_ALIGN_CENTER, "A");
}

// ── Layer 11: VMG markers ─────────────────────────────────────────────────────

void WindScreen::drawVmgMarkers(float tws, float absTwa) {
    float up   = optUpwindTwa(tws);
    float down = optDownwindTwa(tws);

    PoS pos = pointOfSail(absTwa);
    bool showUp   = (pos == PoS::CloseHauled || pos == PoS::CloseReach || pos == PoS::BeamReach);
    bool showDown = (pos == PoS::BroadReach  || pos == PoS::Running);

    // VMG markers: filled arc segments (not dashed lines) centred on the optimal angle
    if (showUp) {
        // Filled wedge: ±3° sweep in the zone ring
        arcRing(R_ZONE_I - 8.f * UI_SF, R_ZONE_O + 2.f,  up - 3.f,        6.f, C_VMG, LV_OPA_COVER);
        arcRing(R_ZONE_I - 8.f * UI_SF, R_ZONE_O + 2.f, -up + 360.f - 3.f, 6.f, C_VMG, LV_OPA_COVER);
        triPointer( up,         R_ZONE_O + 1.f, R_ZONE_O + 9.f * UI_SF, 5.f * UI_SF, C_VMG, LV_OPA_COVER);
        triPointer(-up + 360.f, R_ZONE_O + 1.f, R_ZONE_O + 9.f * UI_SF, 5.f * UI_SF, C_VMG, LV_OPA_COVER);
    }
    if (showDown) {
        arcRing(R_ZONE_I - 8.f * UI_SF, R_ZONE_O + 2.f,  down - 3.f,        6.f, C_VMG, LV_OPA_COVER);
        arcRing(R_ZONE_I - 8.f * UI_SF, R_ZONE_O + 2.f, -down + 360.f - 3.f, 6.f, C_VMG, LV_OPA_COVER);
        triPointer( down,        R_ZONE_O + 1.f, R_ZONE_O + 9.f * UI_SF, 5.f * UI_SF, C_VMG, LV_OPA_COVER);
        triPointer(-down + 360.f, R_ZONE_O + 1.f, R_ZONE_O + 9.f * UI_SF, 5.f * UI_SF, C_VMG, LV_OPA_COVER);
    }
}

// ── Layer 12: Centre TWA value ────────────────────────────────────────────────


// ── ATTITUDE centre: aircraft-style artificial horizon ───────────────────────
// A sky/ground disc that rotates with roll and translates with pitch, plus a
// pitch ladder, a fixed aircraft reference, a bank scale + pointer and a
// rate-of-turn strip. Drawn with scanline spans + cline()/fillTri() only — no
// lv_canvas_draw_polygon (it segfaults in the PC simulator). The disc (R=110
// design px) sits inside the compass rose, so the outer ring stays fully visible.
//
// In two parts: drawHorizonDisc() is the moving picture - sky/ground disc,
// horizon line, pitch ladder - and needs a known roll AND pitch;
// drawCenter_Attitude() draws it when it has them, then the fixed frame.
void WindScreen::drawHorizonDisc(float roll, float pitch) {
    const float R   = R_HORIZON;         // horizon-disc radius (clear of the compass rose)
    const float PPD = 3.4f * UI_SF;      // screen px per degree of pitch
    float pc   = fmaxf(-35.f, fminf(35.f, pitch));
    float phi  = -roll * DEG2RAD;            // outside world counter-rotates vs the boat
    float sphi = sinf(phi), cphi = cosf(phi);
    float horY = CY + pc * PPD;              // horizon centre y (pitch up -> world drops)

    // Previously hard-wired and even declared "theme-independent" in the comment.
    // The disc is about 38 000 pixels — with the ship upright that is roughly
    // 19 000 pixels of saturated blue, more area than the world map. In night
    // mode that cancelled the dark adaptation the mode exists for.
    // LINEC covers the horizon line, pitch ladder, bank marks and slip indicator.
    const lv_color_t SKY    = (uiTheme.attSky);
    const lv_color_t GROUND = (uiTheme.attGround);
    const lv_color_t LINEC  = (uiTheme.text);

    // ── Sky / ground fill (scanline, clipped to the disc) ────────────────────
    // Every segment is stored straight into the canvas. Each one used to be
    // an lv_canvas_draw_line() of width 1 from (a, y) to (b, y), cast to
    // lv_coord_t: one fake display per segment, 275 to 410 a frame depending
    // on the heel - the largest single item of this screen. The segment is
    // written exactly as LVGL 8.4 filled that line (see fillTri() above):
    // nothing for equal end points, otherwise [A, B - 1] of the row, clipped
    // to the canvas, as a plain store at full opacity. A <= B always holds -
    // xs is clamped into [xL, xR] and the cast never reverses an order - so
    // cdSpanH() never swaps the ends. The float loop, the 0.5 tests, the
    // midpoint colour test and the yield offers are unchanged.
    const CdTarget tg = tgt();
    int rowc = 0;
    for (float y = CY - R; y <= CY + R; y += 1.f) {
        float dyc = y - CY;
        float h2  = R * R - dyc * dyc;
        if (h2 <= 0.f) continue;
        float hx  = sqrtf(h2);
        float xL  = CX - hx, xR = CX + hx;
        const lv_coord_t Y = (lv_coord_t)y, XL = (lv_coord_t)xL, XR = (lv_coord_t)xR;
        // ground where  d(x,y) = (y-horY)*cphi - (x-CX)*sphi  > 0  (sky otherwise)
        if (fabsf(sphi) < 1e-4f) {
            bool ground = ((y - horY) * cphi) > 0.f;
            if (XR > XL) cdSpanH(tg, Y, XL, XR - 1, ground ? GROUND : SKY, LV_OPA_COVER);
            PP_ONLY(s_ppSegs++;);
        } else {
            float xs = CX + ((y - horY) * cphi) / sphi;   // x where d == 0
            if (xs < xL) xs = xL; else if (xs > xR) xs = xR;
            // Colour each segment by the sign of d at its midpoint (robust even
            // when the true split lies outside the row and xs was clamped).
            if (xs > xL + 0.5f) {
                float mid = 0.5f * (xL + xs);
                const lv_color_t c = ((y - horY) * cphi - (mid - CX) * sphi > 0.f) ? GROUND : SKY;
                const lv_coord_t XS = (lv_coord_t)xs;
                if (XS > XL) cdSpanH(tg, Y, XL, XS - 1, c, LV_OPA_COVER);
                PP_ONLY(s_ppSegs++;);
            }
            if (xR > xs + 0.5f) {
                float mid = 0.5f * (xs + xR);
                const lv_color_t c = ((y - horY) * cphi - (mid - CX) * sphi > 0.f) ? GROUND : SKY;
                const lv_coord_t XS = (lv_coord_t)xs;
                if (XR > XS) cdSpanH(tg, Y, XS, XR - 1, c, LV_OPA_COVER);
                PP_ONLY(s_ppSegs++;);
            }
        }
        if (++rowc % 20 == 0) renderYield();
    }
    PP_END(s_ppA, AC_PH_SKY);

    // ── Horizon line across the disc (intersect line with the circle) ────────
    {
        float b = pc * PPD;                       // vertical offset of P0 from centre
        float B = 2.f * b * sphi, Cq = b * b - R * R;
        float disc = B * B - 4.f * Cq;
        if (disc > 0.f) {
            float sd = sqrtf(disc);
            float t1 = (-B - sd) * 0.5f, t2 = (-B + sd) * 0.5f;
            linePx(CX + t1 * cphi, horY + t1 * sphi, CX + t2 * cphi, horY + t2 * sphi, LINEC, LV_OPA_COVER, 2);
        }
    }
    PP_END(s_ppA, AC_PH_HORIZ);

    // ── Pitch ladder (rungs at ±10/±20°, rotated by roll, clipped) ───────────
    const float ux = sphi, uy = -cphi;            // unit "up" (sky) normal
    const int   pv[4]    = { 10, 20, -10, -20 };
    const float halfL[4] = { 22.f * UI_SF, 15.f * UI_SF, 22.f * UI_SF, 15.f * UI_SF };
    char lbl[4];
    {
        WsBatch b(_canvas);   // the four rung labels, one draw context
        for (int i = 0; i < 4; i++) {
            float off = pv[i] * PPD;
            float mx = CX + off * ux, my = horY + off * uy;        // rung centre
            if ((mx-CX)*(mx-CX)+(my-CY)*(my-CY) > (R-6.f*UI_SF)*(R-6.f*UI_SF)) continue;
            float hl = halfL[i];
            float ax = mx - hl*cphi, ay = my - hl*sphi;
            float bx = mx + hl*cphi, by = my + hl*sphi;
            linePx(ax, ay, bx, by, LINEC, LV_OPA_80, 1);
            snprintf(lbl, sizeof(lbl), "%d", pv[i] < 0 ? -pv[i] : pv[i]);
            ctext(b, bx + 2.f * UI_SF, by - 6.f * UI_SF, 18.f * UI_SF, FONT_TINY, LINEC, LV_TEXT_ALIGN_LEFT, lbl);
        }
    }
    PP_END(s_ppA, AC_PH_LADDER);
}

void WindScreen::drawCenter_Attitude(float roll, float pitch, float rot) {
    // Roll or pitch not known - no attitude sensor, or BusWatch blanked them
    // after 10 s without a reading: no horizon at all. Both used to be forced
    // to 0 here, which drew a LEVEL horizon under corner values reading "--",
    // so a sensor gone quiet looked like a boat lying perfectly still. What
    // stays is the fixed frame - bank scale, aircraft symbol, rate-of-turn
    // strip - over the plain inner circle, which drawInstrument() then fills
    // in full (no horizonHole), so nothing of the last horizon stays behind.
    // The sky/ground phases are not charged in such a frame (x<cnt>).
    if (attitudeKnown(roll, pitch)) drawHorizonDisc(roll, pitch);
    const lv_color_t LINEC = (uiTheme.text);      // as in drawHorizonDisc()

    // ── Bank scale (fixed) at the top + moving pointer at the current roll ───
    const float rB = 100.f * UI_SF;               // scale radius (over the sky)
    const int   bt[5] = { 10, 20, 30, 45, 60 };
    { lv_point_t p1 = wp(CX,CY,rB,0.f), p2 = wp(CX,CY,rB-11.f*UI_SF,0.f);  // 0° major tick
      linePx(p1.x, p1.y, p2.x, p2.y, LINEC, LV_OPA_70, 2); }
    for (int i = 0; i < 5; i++) {
        for (int s = -1; s <= 1; s += 2) {
            float ang = (float)(s * bt[i]);
            bool maj = (bt[i] % 30) == 0;
            lv_point_t p1 = wp(CX, CY, rB, ang);
            lv_point_t p2 = wp(CX, CY, rB - (maj ? 11.f : 7.f) * UI_SF, ang);
            linePx(p1.x, p1.y, p2.x, p2.y, LINEC, LV_OPA_70, maj ? 2 : 1);
        }
    }
    // The pointer only for a known roll: fminf(60, NaN) is 60, so a NaN roll
    // would put it at 60 degrees of heel.
    if (!isnan(roll)) {
        float a = fmaxf(-60.f, fminf(60.f, roll));
        lv_point_t tip = wp(CX, CY, rB - 12.f * UI_SF, a);
        lv_point_t bl  = wp(CX, CY, rB - 22.f * UI_SF, a - 4.f);
        lv_point_t br  = wp(CX, CY, rB - 22.f * UI_SF, a + 4.f);
        fillTri(_canvas, tip.x, tip.y, bl.x, bl.y, br.x, br.y, CLR_YELLOW, LV_OPA_COVER);
    }
    PP_END(s_ppA, AC_PH_BANK);

    // ── Fixed aircraft reference (does NOT rotate) ───────────────────────────
    linePx(CX - 38.f * UI_SF, CY, CX - 14.f * UI_SF, CY, CLR_YELLOW, LV_OPA_COVER, 3);
    linePx(CX - 14.f * UI_SF, CY, CX - 14.f * UI_SF, CY + 7.f * UI_SF, CLR_YELLOW, LV_OPA_COVER, 3);
    linePx(CX + 38.f * UI_SF, CY, CX + 14.f * UI_SF, CY, CLR_YELLOW, LV_OPA_COVER, 3);
    linePx(CX + 14.f * UI_SF, CY, CX + 14.f * UI_SF, CY + 7.f * UI_SF, CLR_YELLOW, LV_OPA_COVER, 3);
    { lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
      rd.bg_color = CLR_YELLOW; rd.bg_opa = LV_OPA_COVER; rd.radius = UI_S(2); rd.border_width = 0;
      WsBatch b(_canvas);
      cdLvRect(b, (lv_coord_t)(CX-UI_S(3)), (lv_coord_t)(CY-UI_S(3)), UI_S(6), UI_S(6), &rd); }
    PP_END(s_ppA, AC_PH_ACFT);

    // ── Rate-of-turn strip (below the aircraft symbol) ───────────────────────
    {
        const float yb = CY + 62.f * UI_SF, FS = 60.f, halfW = 46.f * UI_SF;
        linePx(CX - halfW, yb, CX + halfW, yb, LINEC, LV_OPA_70, 1);
        linePx(CX, yb - 5.f * UI_SF, CX, yb + 5.f * UI_SF, LINEC, LV_OPA_70, 1);   // centre mark
        // No pointer without a rate of turn: parked on the centre mark it said
        // "steady course" next to "ROT --".
        if (!isnan(rot)) {
            float r  = fmaxf(-FS, fminf(FS, rot));
            float xr = CX + (r / FS) * halfW;
            lv_color_t rc = (fabsf(rot) > 30.f) ? CLR_ORANGE : CLR_GREEN;
            fillTri(_canvas, xr, yb - 8.f * UI_SF, xr - 5.f * UI_SF, yb, xr + 5.f * UI_SF, yb, rc, LV_OPA_COVER);
        }
        char rb[20];
        if (isnan(rot)) snprintf(rb, sizeof(rb), "ROT --");
        else snprintf(rb, sizeof(rb), "ROT %+d\xc2\xb0/min", (int)(rot + (rot>=0?0.5f:-0.5f)));
        WsBatch b(_canvas);   // after fillTri(), which may yield
        ctext(b, CX - 60.f * UI_SF, yb + 6.f * UI_SF, 120.f * UI_SF, FONT_TINY, CLR_TEXT, LV_TEXT_ALIGN_CENTER, rb);
    }
}

// Corner KPIs for the ATTITUDE screen: Roll / Pitch / wave height / wave period.
void WindScreen::drawAttitudeKpis(CdLvBatch &b, float roll, float pitch, float waveH, float waveT) {
    char buf[20];
    const float TY = 4.f * UI_SF, TV = 22.f * UI_SF, BY = 430.f * UI_SF, BV = 446.f * UI_SF;
    const float LX = 6.f * UI_SF, RX = 474.f * UI_SF, TW = 152.f * UI_SF,
                BWL = 120.f * UI_SF, BWR = 132.f * UI_SF;

    // Top-left: Roll
    ctext(b, LX, TY, TW, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, T(STR_WIND_KPI_ROLL));
    if (isnan(roll)) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d\xc2\xb0 %s", (int)(fabsf(roll)+0.5f),
                  roll >= 0.f ? T(STR_WIND_STB) : T(STR_WIND_PT));
    drawTopKpiValue(b, false, LX, TV, TW,
                    isnan(roll) ? CLR_TEXT : (roll >= 0.f ? CLR_STARBOARD : CLR_PORT), buf,
                    "DD\xc2\xb0", T(STR_WIND_STB), T(STR_WIND_PT));

    // Top-right: Pitch (bow up / stern up)
    ctext(b, RX - TW, TY, TW, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, T(STR_WIND_KPI_PITCH));
    if (isnan(pitch)) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d\xc2\xb0 %s", (int)(fabsf(pitch)+0.5f),
                  pitch >= 0.f ? T(STR_WIND_BOW) : T(STR_WIND_STERN));
    drawTopKpiValue(b, true, RX - TW, TV, TW, CLR_TEXT, buf, "DD\xc2\xb0",
                    T(STR_WIND_BOW), T(STR_WIND_STERN));

    // Bottom-left: estimated wave height
    ctext(b, LX, BY, BWL, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, T(STR_WIND_KPI_WAVE));
    if (isnan(waveH)) snprintf(buf, sizeof(buf), "--");
    else              snprintf(buf, sizeof(buf), "%.1f m", waveH);
    ctext(b, LX, BV, BWL, FONT_XL, CLR_WIND, LV_TEXT_ALIGN_LEFT, buf);

    // Bottom-right: estimated wave period
    ctext(b, RX - BWR, BY, BWR, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, T(STR_WIND_KPI_PERIOD));
    if (isnan(waveT)) snprintf(buf, sizeof(buf), "--");
    else              snprintf(buf, sizeof(buf), "%.1f s", waveT);
    ctext(b, RX - BWR, BV, BWR, FONT_XL, CLR_TEXT, LV_TEXT_ALIGN_RIGHT, buf);
}

// ── Layer 13: Corner KPIs ─────────────────────────────────────────────────────
//
//   Top-left : STW        Top-right : TWA
//   Bot-left : AWA        Bot-right : TWS

// ── "Heading" line (directly from DrawHeadingBelow in the NauticPi renderer)

void WindScreen::drawHeadingBelow(CdLvBatch &b, float hdg) {
    // Lubber heading readout at the very top of the compass card (always "up").
    char buf[8];
    if (isnan(hdg)) snprintf(buf, sizeof(buf), "---");
    else            snprintf(buf, sizeof(buf), "%03d", (int)(hdg + 0.5f) % 360);
    const float w = 48.f * UI_SF, h = 21.f * UI_SF;
    const float bx = CX - w * 0.5f, by = CY - R_INNER + 1.f;
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = (uiTheme.windDepthBg); rd.bg_opa = 235;
    rd.radius = UI_S(5); rd.border_width = 1; rd.border_color = (uiTheme.windMast);
    cdLvRect(b, (lv_coord_t)bx, (lv_coord_t)by, (lv_coord_t)w, (lv_coord_t)h, &rd);
    ctext(b, bx, by + 3.f * UI_SF, w, FONT_SMALL, CLR_TEXT, LV_TEXT_ALIGN_CENTER, buf);
}

// Compass card inside the inner circle: ticks every 10° (major 30°) plus
// N/O/S/W. Two orientations, switched by tapping the card (onCanvasTouch):
//
//   course-up (default) – the card turns under a fixed lubber point, so the
//                         current heading is always at the top;
//   north-up            – the card stands still with N at the top and the
//                         heading is marked by an index on its rim.
//
// ONLY this card switches. The outer ring is the RELATIVE-wind scale: 0° is the
// bow, and the zone sectors and both wind pointers are read against it. Turning
// that with the compass would leave the whole primary reading of the screen
// pointing at the wrong angle, so the ring, the boat and the flow lines stay
// boat-fixed in both modes.
//
// layers: CARD_RING = ticks and the north-up heading index, CARD_LETTERS = the
// cardinal letters, CARD_ALL = both. The Wind & Trim centre draws the ring
// under the boat and the letters over it; both calls compute the same card
// rotation from the same heading.
void WindScreen::drawCompassRose(float hdg, uint8_t layers) {
    const bool haveHdg = !isnan(hdg);
    const bool northUp = appConfig.cfg.compassNorthUp;
    // Course-up without a compass fix has nothing to rotate by. The ring is
    // drawn as it stands at 0 - its ticks look alike at any rotation - but the
    // letters are left out: N at the top under the lubber box, the old
    // "isnan -> 0", read as a heading of 000 that nobody measured, right
    // beside the "---" in that box. North-up needs no heading and keeps them.
    const float rot = (northUp || !haveHdg) ? 0.f : hdg;
    const bool  letters = northUp || haveHdg;
    const float rt0 = R_INNER -  8.f * UI_SF;   // minor tick inner end (short → gap to labels)
    const float rtM = R_INNER - 11.f * UI_SF;   // major tick inner end
    const float rt1 = R_INNER -  2.f;   // tick outer end (-2 hugs the unscaled 3px border)
    const float rl  = R_INNER - 21.f * UI_SF;   // cardinal labels (clear gap from the ticks)
    lv_color_t cTick = (uiTheme.windTickMin);
    lv_color_t cMaj  = (uiTheme.windTickMaj);
    if (layers & CARD_RING) {
        for (int b = 0; b < 360; b += 10) {
            float a = (float)b - rot;       // bearing b → screen angle (b − rot)
            bool major = (b % 30) == 0;
            lv_point_t p1 = wp(CX, CY, major ? rtM : rt0, a);
            lv_point_t p2 = wp(CX, CY, rt1, a);
            linePx(p1.x, p1.y, p2.x, p2.y, major ? cMaj : cTick, LV_OPA_70, major ? 2 : 1);
        }
    }
    // Cardinal letters: German Ost = "O", English East = "E" (not static — T() is
    // language-dependent and the language can change at runtime).
    if ((layers & CARD_LETTERS) && letters) {
        WsBatch b(_canvas);   // 4 letters (+ up to 8 halo copies), one draw context
        const char *card[4] = { T(STR_WIND_CARD_N), T(STR_WIND_CARD_E),
                                T(STR_WIND_CARD_S), T(STR_WIND_CARD_W) };
        for (int i = 0; i < 4; i++) {
            float a = (float)(i * 90) - rot;
            float lx = CX + rl * sinf(a * DEG2RAD);
            float ly = CY - rl * cosf(a * DEG2RAD);
            lv_color_t cc = (i == 0) ? CLR_RED : cMaj;   // North in red
            const float tx = lx - 8.f * UI_SF, ty = ly - 8.f * UI_SF, tw = 16.f * UI_SF;
            // Halo over a downwind sail. A kite reaches the bottom quarter of a
            // letter that stands near the bow, and there neither colour reads:
            // red or windTickMaj on the kite's orange is 1.1:1 to 2.2:1 in the
            // three themes, a night-mode S or O lost its lower half. Four copies
            // in the inner background colour, one pixel left, right, up and
            // down, put a 1-px rim of that colour round every stroke - 3.2:1
            // to 7.9:1 against the orange, invisible on the bare inner circle.
            // Only while a kite is drawn (_spiDrawn, this frame) and only for a
            // letter within LETTER_HALO_DEG of the bow, the sector in which a
            // kite reaches the letters (see WindScreen.h): at most 2 letters,
            // so at most 8 extra small text draws a frame - in the letters'
            // draw context, so no fake-display set-up of their own - and none
            // without a kite.
            if (_spiDrawn) {
                float rel = fmodf(a, 360.f);
                if (rel > 180.f) rel -= 360.f; else if (rel < -180.f) rel += 360.f;
                if (fabsf(rel) <= LETTER_HALO_DEG) {
                    static const int8_t HX[4] = { -1, 1, 0, 0 }, HY[4] = { 0, 0, -1, 1 };
                    for (int h = 0; h < 4; h++)
                        ctext(b, tx + HX[h], ty + HY[h], tw, FONT_SMALL, C_INNER, LV_TEXT_ALIGN_CENTER, card[i]);
                    PP_ONLY(s_ppHalo += 4;);
                }
            }
            ctext(b, tx, ty, tw, FONT_SMALL, cc, LV_TEXT_ALIGN_CENTER, card[i]);
        }
    }
    if (!(layers & CARD_RING)) return;   // the heading index belongs to the ring
    // Heading index — north-up only. Without it a north-up card is a static
    // picture that says nothing about where the boat is pointing; the heading
    // is at the top only in course-up mode. Drawn in the accent colour because
    // red is already taken by the North letter and the two must never be
    // confused. It marks the heading as a BEARING ON THE CARD, so it does NOT
    // line up with the bow of the boat symbol — the boat is drawn bow-up with
    // the relative-wind ring, and that is deliberate.
    //
    // No heading, no mark: an index at a made-up bearing would be worse than
    // none at all. The lubber box above (drawHeadingBelow) then reads "---".
    // Colour: the card's own major-tick tone, NOT the accent. In night mode the
    // accent (0xE2643E) and the red used for North (0xE2543A) differ by 0x10 of
    // green - the same colour to the eye. Night is precisely when a helmsman is
    // most likely to misread a compass card, and "the index looks like North"
    // is the one confusion this mark must never cause. windTickMaj is clearly
    // darker than North-red in all three palettes and reads as part of the card,
    // which is what it is.
    if (northUp && haveHdg)
        triPointer(hdg, R_INNER - 14.f * UI_SF, rt1, 7.f * UI_SF,
                   (uiTheme.windTickMaj), LV_OPA_COVER);
}

// Tap on the compass card toggles north-up and course-up.
//
// The whole instrument is ONE full-screen canvas, so the hit test happens here:
// only a tap inside the inner circle (radius R_INNER about CX,CY) counts. The
// rest of the canvas carries the corner KPIs, the polar bar, the rudder gauge
// and the outer ring, and a whole-canvas toggle would fire on taps meant for
// none of this.
//
// PRESSED records where the finger landed, CLICKED (release) decides. Both are
// needed because screen navigation is a SEPARATE tracker living in the touch
// read-callback (DisplaySetup.cpp / DisplaySetup_7B.cpp), which knows nothing
// about LVGL objects: a horizontal drag that starts and ends inside the card
// changes the screen AND arrives here as an LV_EVENT_CLICKED. Anything that
// moved more than half the swipe threshold is therefore treated as a drag and
// ignored, which keeps the two gestures disjoint with a margin on every board.
void WindScreen::onCanvasTouch(lv_event_t *e) {
    WindScreen *self = (WindScreen *)lv_event_get_user_data(e);
    if (!self || !self->_canvas) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;            // event sent programmatically: no touch point

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    // The tap arrives in SCREEN coordinates. The canvas is not necessarily at
    // the screen origin — on the 1024x600 boards the instrument block starts at
    // (88, 0) in landscape and (0, 88) in portrait — so its absolute position
    // has to come off BOTH axes before comparing against canvas geometry. This
    // is the correction AisScreen::onCanvasClick had to learn the hard way;
    // lv_obj_get_coords() is right on every board and in every rotation.
    lv_area_t ca;
    lv_obj_get_coords(self->_canvas, &ca);
    const float dx = (float)(p.x - ca.x1) - CX;
    const float dy = (float)(p.y - ca.y1) - CY;
    const float d2 = dx * dx + dy * dy;
    // The target is the card's RING, not the whole disc. Two reasons, and the
    // second one is the deciding one:
    //
    //  - The compass card a user means when they say "tap the compass" is the
    //    band carrying N/E/S/W and the ticks. The middle is the boat symbol.
    //  - A tap in the dead centre is the documented way to bring the faded-out
    //    navigation arrows back (every touch calls requestShowNavArrows()).
    //    Had the whole disc toggled, every one of those wake-up taps would also
    //    have flipped the card and written ~12 KB of config.json to flash. That
    //    is wear, not taste.
    //
    // Inner bound is generous enough to clear the boat symbol and still leave a
    // band that is easy to hit with a gloved finger.
    const float rIn  = R_INNER * 0.45f;
    const bool  onCard = (d2 <= (R_INNER * R_INNER)) && (d2 >= (rIn * rIn));

    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        self->_pressX      = p.x;
        self->_pressY      = p.y;
        self->_pressOnCard = onCard;
        return;
    }

    const bool wasOnCard = self->_pressOnCard;
    self->_pressOnCard = false;
    if (!wasOnCard || !onCard) return;      // started or ended off the card
    const int mx = (int)p.x - (int)self->_pressX;
    const int my = (int)p.y - (int)self->_pressY;
    int slop = UI_SWIPE_THRESHOLD / 2;      // stays clear of a real swipe
    if (slop < 4) slop = 4;                 // the threshold is WebUI-settable
    if (mx * mx + my * my > slop * slop) return;   // a drag, not a tap

    appConfig.cfg.compassNorthUp = !appConfig.cfg.compassNorthUp;
    // Written straight from the touch callback, the same way the anchor
    // screen's buttons write theirs (AnchorScreen::cbAlarm): one flash write
    // per tap, never per frame. No repaint is kicked off here — main.cpp
    // repaints the current screen unconditionally at up to 10 Hz, so the new
    // card is on the very next frame, and repainting from inside the input
    // handler would run the whole instrument, renderYield()'s vTaskDelay
    // included, before LVGL has finished processing the release.
    appConfig.save();
}

// Rudder-angle gauge on the bottom arc (150°..210°). Stbd (+) fills GREEN toward
// 150° (right), Port (−) fills RED toward 210° (left). ±35° rudder → ±30° arc.
void WindScreen::drawRudderArc(CdLvBatch &b, float rudderDeg) {
    // No rudder angle, no gauge. Taken as 0, the empty track with "+0°" on it
    // read as "rudder midships" for a sensor that is missing or has gone quiet,
    // like the blade on the boat (drawBoat()). The 150/180/210 ring labels
    // stay hidden: the gap is where the gauge comes back with the next value.
    if (isnan(rudderDeg)) return;
    // Curved gauge hugging the rose's outer edge (no gap) at the bottom, drawn with
    // lv_canvas_draw_arc → anti-aliased edges. ±30° of arc ≙ ±35° of rudder; Stbd
    // green (right/150°), Port red (left/210°). The 150/180/210 labels are hidden
    // here (drawTicksAndLabels) so the gauge sits flush on the rose.
    const float rI = R_OUTER + 8.f * UI_SF, rO = R_OUTER + 24.f * UI_SF, rM = (rI + rO) * 0.5f;   // moved out by ½ thickness
    const lv_coord_t W = (lv_coord_t)(rO - rI);
    const float HALF = 30.f;
    lv_draw_arc_dsc_t ad; lv_draw_arc_dsc_init(&ad);
    ad.width = W; ad.opa = LV_OPA_COVER;
    ad.color = (uiTheme.windRingBg);   // dark track
    // wind(0=top,CW) → LVGL(0=right,CW): −90. Bottom 180 → 90.
    cdLvArc(b, (lv_coord_t)CX, (lv_coord_t)CY, (lv_coord_t)rM,
            (int)(180.f - HALF - 90.f), (int)(180.f + HALF - 90.f), &ad);
    float r = rudderDeg; if (r > 35.f) r = 35.f; if (r < -35.f) r = -35.f;
    float sp = (fabsf(r) / 35.f) * HALF;
    if (sp > 0.5f) {
        if (r > 0.f) { ad.color = CLR_GREEN;     // Stbd: wind 180−sp..180 → LVGL 90−sp..90
            cdLvArc(b,(lv_coord_t)CX,(lv_coord_t)CY,(lv_coord_t)rM,(int)(90.f-sp),90,&ad); }
        else { ad.color = CLR_RED;               // Port: wind 180..180+sp → LVGL 90..90+sp
            cdLvArc(b,(lv_coord_t)CX,(lv_coord_t)CY,(lv_coord_t)rM,90,(int)(90.f+sp),&ad); }
    }
    { lv_point_t p1 = wp(CX, CY, rI - 2.f, 180.f), p2 = wp(CX, CY, rO + 2.f, 180.f);  // amidships tick
      linePx(p1.x, p1.y, p2.x, p2.y, CLR_TEXT, LV_OPA_COVER, 2); }
    lv_point_t sp2 = wp(CX, CY, rM, 180.f - HALF + 4.f);   // stbd end (right)
    ctext(b, sp2.x - 13.f * UI_SF, sp2.y - 7.f * UI_SF, 26.f * UI_SF, FONT_TINY, CLR_GREEN, LV_TEXT_ALIGN_CENTER, T(STR_WIND_STB));
    lv_point_t bp2 = wp(CX, CY, rM, 180.f + HALF - 4.f);   // port end (left)
    ctext(b, bp2.x - 13.f * UI_SF, bp2.y - 7.f * UI_SF, 26.f * UI_SF, FONT_TINY, CLR_RED, LV_TEXT_ALIGN_CENTER, T(STR_WIND_PT));
    const int shown = (int)(rudderDeg + (rudderDeg >= 0.f ? 0.5f : -0.5f));
    char buf[10]; snprintf(buf, sizeof(buf), "%+d\xc2\xb0", shown);
    lv_point_t vp = wp(CX, CY, rM, 180.f);
    // The value sits on a small box in the heading box's colours (see
    // drawHeadingBelow()). Written straight onto the gauge in CLR_ON_ACCENT it
    // was white on the light track of the light theme - next to unreadable -
    // and the amidships tick ran through it in every theme. The box covers the
    // first few degrees of the fill, so its border takes the colour of the
    // side the number shows.
    const float ty = vp.y - 15.f * UI_SF;              // the text's top, as before
    lv_point_t ts; lv_txt_get_size(&ts, buf, FONT_SMALL, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const float bw = (float)ts.x + 8.f * UI_SF, bh = (float)ts.y + 2.f;
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = (uiTheme.windDepthBg); rd.bg_opa = 235;
    rd.radius = UI_S(4); rd.border_width = UI_S(2);
    rd.border_color = shown > 0 ? CLR_GREEN : shown < 0 ? CLR_RED : (uiTheme.windMast);
    cdLvRect(b, (lv_coord_t)(vp.x - bw * 0.5f), (lv_coord_t)(ty - 1.f), (lv_coord_t)bw, (lv_coord_t)bh, &rd);
    ctext(b, vp.x - 26.f * UI_SF, ty, 52.f * UI_SF, FONT_SMALL, CLR_TEXT, LV_TEXT_ALIGN_CENTER, buf);
}

// ── Top corner values ─────────────────────────────────────────────────────────
// The value line of a top corner block shares about 15 px of height with the
// degree label at 330 (left) or 030 (right) just below it. A long value in
// FONT_XL - "4° Stern", "13° Stb", "10.5 kn" - ran into that label. Where the
// field's widest value would not fit beside the label in FONT_XL, the part
// after the first space (the side or the unit) is set smaller, on the
// number's baseline. The decision is made per field from that worst case -
// worstNum with the wider of its two words - not per value, so the layout
// does not change while the number grows or the side changes.
static float textW(const char *s, const lv_font_t *f) {
    lv_point_t p; lv_txt_get_size(&p, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return (float)p.x;
}

// The widest digit of a font: the worst-case patterns below write 'D' for it
// (in Montserrat that is not the 8). Measured once per font.
static char widestDigit(const lv_font_t *f) {
    static const lv_font_t *s_font = nullptr;
    static char s_digit = '0';
    if (f != s_font) {
        float best = -1.f;
        for (char c = '0'; c <= '9'; c++) {
            const char s[2] = {c, '\0'};
            const float w = textW(s, f);
            if (w > best) { best = w; s_digit = c; }
        }
        s_font = f;
    }
    return s_digit;
}

void WindScreen::drawTopKpiValue(CdLvBatch &b, bool right, float x, float y, float w,
                                 lv_color_t col, const char *txt, const char *worstNum,
                                 const char *wordA, const char *wordB) {
    const lv_text_align_t al = right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
    const char *sp = strchr(txt, ' ');
    if (!sp) { ctext(b, x, y, w, FONT_XL, col, al, txt); return; }   // "--"
    // The 330 / 030 label as drawTicksAndLabels() places it, centred on
    // CX -/+ R_LABEL * sin 30; plus a small gap.
    const float lblEdge = R_LABEL * 0.5f + textW("030", FONT_TINY) * 0.5f + 4.f * UI_SF;
    const float room = right ? (x + w) - (CX + lblEdge) : (CX - lblEdge) - x;
    const float gap = 5.f * UI_SF;
    char worst[12];
    {   // the pattern with each 'D' made the font's widest digit
        const char wd = widestDigit(FONT_XL);
        size_t i = 0;
        for (; worstNum[i] && i < sizeof(worst) - 1; i++)
            worst[i] = worstNum[i] == 'D' ? wd : worstNum[i];
        worst[i] = '\0';
    }
    const float worstXL = textW(worst, FONT_XL);
    if (worstXL + textW(" ", FONT_XL)
            + fmaxf(textW(wordA, FONT_XL), textW(wordB, FONT_XL)) <= room) {
        ctext(b, x, y, w, FONT_XL, col, al, txt);
        return;
    }
    const lv_font_t *wf =
        worstXL + gap + fmaxf(textW(wordA, FONT_MED), textW(wordB, FONT_MED)) <= room
        ? FONT_MED : FONT_SMALL;

    char num[12];
    size_t n = (size_t)(sp - txt);
    if (n > sizeof(num) - 1) n = sizeof(num) - 1;
    memcpy(num, txt, n); num[n] = '\0';
    const char *word = sp + 1;
    lv_point_t ns; lv_txt_get_size(&ns, num, FONT_XL, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_point_t ws; lv_txt_get_size(&ws, word, wf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    // Same baseline: a line's baseline lies line_height - base_line below its top.
    const float wy = y + (float)(lv_font_get_line_height(FONT_XL) - FONT_XL->base_line)
                       - (float)(lv_font_get_line_height(wf) - wf->base_line);
    if (right) {
        ctext(b, x, wy, w, wf, col, LV_TEXT_ALIGN_RIGHT, word);
        ctext(b, x, y, w - (float)ws.x - gap, FONT_XL, col, LV_TEXT_ALIGN_RIGHT, num);
    } else {
        ctext(b, x, y, w, FONT_XL, col, LV_TEXT_ALIGN_LEFT, num);
        ctext(b, x + (float)ns.x + gap, wy, w - (float)ns.x - gap, wf, col, LV_TEXT_ALIGN_LEFT, word);
    }
}

void WindScreen::drawCornerKpis(CdLvBatch &b, float stw, float twa, float awa, float tws) {
    char buf[16];
    const float TY  = 4.f * UI_SF,   TV  = 22.f * UI_SF;   // top label / value y
    const float BY  = 430.f * UI_SF, BV  = 446.f * UI_SF;  // bottom label / value y
    const float LX  = 6.f * UI_SF,   RX  = 474.f * UI_SF;
    const float TW  = 152.f * UI_SF;               // top blocks — wide (room above the circle)
    const float BWL = 96.f * UI_SF,  BWR = 126.f * UI_SF;  // bottom blocks flank the wider polar bar

    // ── Top-left: STW ─────────────────────────────────────────
    ctext(b, LX, TY, TW, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, "BSPD");
    if (isnan(stw)) snprintf(buf, sizeof(buf), "--");
    else            snprintf(buf, sizeof(buf), "%.1f kn", stw);
    drawTopKpiValue(b, false, LX, TV, TW, CLR_TEXT, buf, "DD.D", "kn", "kn");

    // ── Top-right: TWA abs ────────────────────────────────────
    ctext(b, RX - TW, TY, TW, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, "TWA");
    float absTwa = fabsf(twa);
    lv_color_t twaC = isnan(twa) ? CLR_TEXT                  // "--" has no side
                    : (twa >= 0.f) ? CLR_STARBOARD : CLR_PORT;
    if (isnan(twa)) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d\xc2\xb0 %s",   // "°" in UTF-8
                  (int)(absTwa + 0.5f), twa >= 0.f ? T(STR_WIND_STB) : T(STR_WIND_PT));
    drawTopKpiValue(b, true, RX - TW, TV, TW, twaC, buf, "1DD\xc2\xb0",
                    T(STR_WIND_STB), T(STR_WIND_PT));

    // ── Bottom-left: AWA ─────────────────────────────────────
    ctext(b, LX, BY, BWL, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, "AWA");
    if (isnan(awa)) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d\xc2\xb0", (int)(fabsf(awa) + 0.5f));
    ctext(b, LX, BV, BWL, FONT_XL, isnan(awa) ? CLR_TEXT : CLR_WIND,   // "--" neutral, like TWA
          LV_TEXT_ALIGN_LEFT, buf);

    // ── Bottom-right: TWS ────────────────────────────────────
    ctext(b, RX - BWR, BY, BWR, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, "TWS");
    if (isnan(tws)) snprintf(buf, sizeof(buf), "--");
    else            snprintf(buf, sizeof(buf), "%.1f kn", tws);
    ctext(b, RX - BWR, BV, BWR, FONT_XL, CLR_TEXT, LV_TEXT_ALIGN_RIGHT, buf);
}

// ── Layer 14: Trim advice below the circle ────────────────────────────────────

void WindScreen::drawTrimAdvice(CdLvBatch &b, float absTwa, float twa, float tws) {
    // Point-of-sail name, INSIDE the circle just below the boat (the centre is
    // free now that the big TWA readout was removed). The verbose trim hint is
    // dropped — the sail graphic itself shows the trim and the big circle leaves
    // no room outside it. No name without a true wind angle (see the caller).
    (void)twa; (void)tws;
    if (isnan(absTwa)) return;
    PoS         pos  = pointOfSail(absTwa);
    const char *name = posLabel(pos);
    ctext(b, CX - 95.f * UI_SF, CY + 29.f * BOAT_S, 190.f * UI_SF,
          FONT_MED, CLR_TEXT, LV_TEXT_ALIGN_CENTER, name);
}

// ── Reef badge (coloured pill above the bow, or in the cockpit) ──────────────
void WindScreen::drawReefBadge(CdLvBatch &b, int reefCount, bool aft) {
    if (reefCount <= 0) return;

    // Colour: 1=amber, 2=orange, 3=red
    lv_color_t col;
    const char *label;
    switch (reefCount) {
        case 1:  col = CLR_ORANGE; label = T(STR_WIND_REEF1); break;
        case 2:  col = CLR_ORANGE; label = T(STR_WIND_REEF2); break;
        default: col = CLR_RED;    label = T(STR_WIND_REEF3); break;
    }

    // Badge pill: centred above the bow, inside the circle (only when reefed).
    //
    // Not while a downwind sail is drawn: the kite's crest fills exactly that
    // place, up to the heading box, and an orange pill on the orange crest
    // read as a tab grafted onto the kite. Then it sits in the cockpit:
    // 8 units aft of the boat centre, just clear of the keel (down to 7.6),
    // its 4.7-unit height ending above the winches (14.7) and its half width
    // of 7.4 units inside the guy's lead to the twing (rounded corners
    // included, 0.7 px clear on the 4" board, 1.1 px on the 1024x600 ones).
    // The cockpit is free because a kite only flies with the true wind more
    // than 90 deg aft: unless the boat is faster than the wind, |AWA| is then
    // more than half of that, the boom eased 32 deg or more off the
    // centreline, and the mainsail bellies outwards from it. (Motoring
    // downwind faster than the wind brings the apparent wind ahead and the
    // boom onto the centreline - the badge then lies on it.) Without a kite
    // the boom can lie on the centreline there, so the bow stays the badge's
    // place.
    const float bw = 56.f * UI_SF, bh = 18.f * UI_SF;
    const float bx = CX - bw * 0.5f;
    const float by = aft ? CY + 8.f * BOAT_S : CY - 35.f * BOAT_S;

    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col; rd.bg_opa = 220;
    rd.radius   = UI_S(4);   rd.border_width = 0;
    cdLvRect(b,
        (lv_coord_t)bx, (lv_coord_t)by,
        (lv_coord_t)bw, (lv_coord_t)bh, &rd);

    ctext(b, bx, by + 3.f * UI_SF, bw,
          FONT_TINY, CLR_TEXT, LV_TEXT_ALIGN_CENTER, label);
}

// ── arcRing helper ────────────────────────────────────────────────────────────
// Draws a donut segment using overlapping short line-segments.
// startWind / sweep: wind-angle convention (0° = top, CW positive).

void WindScreen::arcRing(float innerR, float outerR,
                          float startWind, float sweep,
                          lv_color_t col, lv_opa_t opa) {
    if (sweep >= 359.f) {
        // Full ring. lv_canvas_draw_arc() would be correct but evaluates an
        // LVGL radius mask over the whole bounding box for a band a few pixels
        // wide; fillAnnulus() touches only the band. Same reasoning as spanH().
        fillAnnulus(CX, CY, innerR, outerR + 1.f, col, opa);
        return;
    }
    // Partial zone = a filled annular SECTOR polygon (outer arc forward, inner arc
    // back), built with the wind-angle convention (0=top, CW) via wp() — so the
    // zones sit exactly where they should. lv_canvas_draw_arc's partial-angle
    // handling was off; this is precise AND solid (no stripes).
    int N = (int)(sweep / 4.f); if (N < 2) N = 2; if (N > 64) N = 64;
    lv_point_t pts[132];
    int n = 0;
    for (int i = 0; i <= N; i++) pts[n++] = wp(CX, CY, outerR, startWind + sweep * (float)i / (float)N);
    for (int i = N; i >= 0; i--) pts[n++] = wp(CX, CY, innerR, startWind + sweep * (float)i / (float)N);
    fillPolygon(pts, n, col, opa);
    static int yc = 0;
    if (++yc % 4 == 0) renderYield();      // one yield offer per 4 zone sectors
}

void WindScreen::symArcRing(float innerR, float outerR,
                              float startWind, float sweep,
                              lv_color_t col, lv_opa_t opa) {
    arcRing(innerR, outerR, startWind, sweep, col, opa);        // Stb side
    arcRing(innerR, outerR, 360.f - (startWind + sweep), sweep, col, opa); // Bb side
}

// ── triPointer helper ─────────────────────────────────────────────────────────
// Triangle pointer along windAngle, tip at tipR, base at baseR with half-width hw.

void WindScreen::triPointer(float windAngle,
                              float tipR, float baseR, float hw,
                              lv_color_t col, lv_opa_t opa) {
    float a    = windAngle * DEG2RAD;
    float sinA = sinf(a), cosA = cosf(a);

    // Tip of triangle (closer to centre)
    float tipX = CX + tipR * sinA;
    float tipY = CY - tipR * cosA;

    // Base centre
    float bcX = CX + baseR * sinA;
    float bcY = CY - baseR * cosA;

    // Base left and right (perpendicular: (cosA, sinA) direction in screen space)
    float blX = bcX + hw * cosA;
    float blY = bcY + hw * sinA;
    float brX = bcX - hw * cosA;
    float brY = bcY - hw * sinA;

    fillTri(_canvas, tipX, tipY, blX, blY, brX, brY, col, opa);
    linePx(tipX, tipY, blX, blY, col, opa, 2);
    linePx(tipX, tipY, brX, brY, col, opa, 2);
    linePx(blX, blY, brX, brY, col, opa, 1);
}

// ── Point-of-sail logic ───────────────────────────────────────────────────────

WindScreen::PoS WindScreen::pointOfSail(float absTwa) {
    if (absTwa <  (float)appConfig.cfg.noGoAngle) return PoS::NoGo;
    if (absTwa <  45.f) return PoS::CloseHauled;
    if (absTwa <  65.f) return PoS::CloseReach;
    if (absTwa < 100.f) return PoS::BeamReach;
    if (absTwa < 150.f) return PoS::BroadReach;
    return PoS::Running;
}

const char *WindScreen::posLabel(PoS p) {
    switch (p) {
        case PoS::NoGo:        return T(STR_WIND_POS_NOGO);
        case PoS::CloseHauled: return T(STR_WIND_POS_CLOSE_HAULED);
        case PoS::CloseReach:  return T(STR_WIND_POS_CLOSE_REACH);
        case PoS::BeamReach:   return T(STR_WIND_POS_BEAM_REACH);
        case PoS::BroadReach:  return T(STR_WIND_POS_BROAD_REACH);
        case PoS::Running:     return T(STR_WIND_POS_RUNNING);
        default:               return "";
    }
}

float WindScreen::optUpwindTwa(float tws) {
    if (isnan(tws)) return 42.f;
    return (tws < 8.f) ? 40.f : (tws < 15.f) ? 44.f : 48.f;
}

float WindScreen::optDownwindTwa(float tws) {
    if (isnan(tws)) return 155.f;
    return (tws < 8.f) ? 152.f : (tws < 15.f) ? 157.f : 162.f;
}

