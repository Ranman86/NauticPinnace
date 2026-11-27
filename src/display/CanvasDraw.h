#pragma once
#include <lvgl.h>

// ============================================================
// CanvasDraw – shared filled-shape and direct-buffer raster primitives for the
// canvas screens.
//
// Why this module exists: several screens used to fake a filled area by
// stroking dozens of thick 1° line segments in a loop. That renders as a comb
// with wedge-shaped gaps at the outer radius, blends its own alpha once per
// segment (so a "subtle tint" composites ~40× and turns opaque, with darker
// seams), and quantises the shape to whole degrees. These helpers draw the real
// thing in a single pass instead.
//
// ⚠ Never use lv_canvas_draw_polygon in this project. It is convex-only and
//   loops forever on a non-convex outline (PC: hang, ESP32: watchdog reset), its
//   mask buffer allocation is unchecked (NULL deref), it silently truncates
//   above 16 edges, and it segfaults in the PC simulator's LVGL build. Polygons
//   here are filled by scanline directly into the canvas buffer.
// ============================================================

// ---- Filled annular sector (ring band) --------------------------------------
// Solid, anti-aliased band between rInner and rOuter, from a0Deg to a1Deg.
//
// Angles use the LVGL convention: 0° = 3 o'clock, increasing CLOCKWISE
// (90° = bottom, 180° = left, 270° = top). Note the LVGL header comment claims
// otherwise — it is wrong; this matches the actual sw renderer.
//
// Angles may be passed negative or >360: they are normalised here. This is not
// cosmetic — lv_draw_arc takes uint16_t, so a raw negative angle wraps to
// (65536 - a) % 360 and lands 16° off (−30° would draw at 346°, not 330°).
//
// Always drawn at full opacity by design: LVGL's arc renderer issues
// overlapping quarter passes, so a translucent arc double-blends into a visible
// seam, and any opa < COVER also forces a slower read-modify-write per pixel.
// For a tint, pre-mix the colour against its backdrop with cdMix() and pass that.
void cdFillRing(lv_obj_t *cv, float cx, float cy, float rInner, float rOuter,
                float a0Deg, float a1Deg, lv_color_t col);

// ---- Filled polygon / triangle (even-odd scanline) --------------------------
// Written straight into the canvas buffer: one lv_canvas_draw_line per row would
// re-run LVGL's fake-display init (a malloc/free pair) and an invalidate for
// every single row. Solid fill, no AA on the edges.
void cdFillPoly(lv_obj_t *cv, const lv_point_t *pts, int n, lv_color_t col);
void cdFillTri(lv_obj_t *cv, lv_point_t a, lv_point_t b, lv_point_t c, lv_color_t col);

// ---- Colour helper ----------------------------------------------------------
// Blend fg over bg. mix = 0..255 (255 = fully fg). Use to pre-flatten a tint so
// it can be drawn opaque — see the note on cdFillRing.
static inline lv_color_t cdMix(lv_color_t fg, lv_color_t bg, uint8_t mix) {
    return lv_color_mix(fg, bg, mix);
}

// ---- Direct-buffer raster (moved from WindScreen) ---------------------------
// Pixel writers that store straight into a canvas buffer: no LVGL draw call,
// so no fake display, no malloc/free and NO lv_obj_invalidate. The caller
// invalidates its canvas once after the whole paint, as
// WindScreen::drawInstrument() does.
//
// Moved unchanged from WindScreen, which keeps its old member names as inline
// forwarders (WindScreen.h). The comments on the definitions moved with the
// bodies into CanvasDraw.cpp. The ones on the declarations stayed on those
// forwarders in WindScreen.h: the device measurements behind cdLineAA
// (WindScreen::linePx) and cdRingAA (ringAA), and why cdDiscAA (discAA) is
// not built from cdRingAA.
//
// The helpers do NOT share one pixel convention, and that is kept as it was:
//   - cdLineAA with w <= 1.5 (Xiaolin Wu) treats integer coordinates as pixel
//     centres; its w > 1.5 branch, cdRingAA and cdDiscAA put the centre of
//     pixel x at x + 0.5.
//   - cdFillEllipse and cdFillPolyOpa truncate with (int); cdFillAnnulus uses
//     the + 0.5 centre in y only.
// cdLineAA's Wu branch is not clipped up front: it walks the whole segment and
// drops off-canvas pixels one by one. No helper checks for NaN input.

// Render target: an LV_IMG_CF_TRUE_COLOR buffer of w x h pixels whose row
// stride is w, which holds for every canvas in this project. With
// buf == nullptr nothing is written; cdFillEllipse, cdFillAnnulus and
// cdFillPolyOpa still run their row loops and their renderYield() calls then.
struct CdTarget {
    lv_color_t *buf;   // pixel (0,0)
    int         w, h;  // canvas size: clip rectangle, and w is the row stride
};
static inline CdTarget cdTarget(lv_color_t *buf, int w, int h) {
    CdTarget t = { buf, w, h };
    return t;
}
// Target of an existing canvas, read through lv_canvas_get_img().
// {nullptr, 0, 0} for a null canvas, a canvas without a buffer, or one that
// is not LV_IMG_CF_TRUE_COLOR.
CdTarget cdTargetOf(lv_obj_t *canvas);

// One horizontal run [x0, x1], inclusive (swapped if x1 < x0), clipped to the
// target. opa >= LV_OPA_COVER stores (with 16-bit colour two pixels per
// 32-bit store once aligned); below that, lv_color_mix per pixel. No yield.
void cdSpanH(const CdTarget &tg, int y, int x0, int x1, lv_color_t col, lv_opa_t opa);

// Anti-aliased line. w <= 1.5: Xiaolin Wu, always about 1 px, whatever w is.
// w > 1.5: a round-capped stroke of width w, coverage from the real distance
// of each pixel centre; a zero-length one draws nothing. No yield.
// NOTE the order: opa BEFORE w - the reverse of the cline()/mline() wrappers
// in the other screens. A swapped call compiles without a warning.
void cdLineAA(const CdTarget &tg, float x0, float y0, float x1, float y1,
              lv_color_t col, lv_opa_t opa, float w = 1.f);

// Anti-aliased full ring of width w centred on radius r. Only the band
// itself is visited, and with the centre on the half-pixel grid (2*cx and
// 2*cy whole) only a quarter of it is evaluated: the coverage is exactly
// four-fold symmetric there (see the definition). No yield.
// AisScreen::buildRingTable() copies its coverage formula for the folded
// range rings: change both together and re-run the AIS_RINGS_REF pixel
// compare.
void cdRingAA(const CdTarget &tg, float cx, float cy, float r, float w,
              lv_color_t col, lv_opa_t opa);

// Filled disc with an anti-aliased edge. No yield.
void cdDiscAA(const CdTarget &tg, float cx, float cy, float r,
              lv_color_t col, lv_opa_t opa);

// Filled ellipse, NOT anti-aliased, one span per row. renderYield() every 20
// rows.
//
// hole (optional): asked once per row y; returning true with x0 <= x1 leaves
// the pixels x0..x1 of that row unwritten - for a caller that paints them
// opaque right afterwards anyway. Only a hole strictly inside the row's span
// is cut out; any other leaves the row whole. The span ends are the same
// with and without a hole.
typedef bool (*CdRowHoleFn)(int y, int &x0, int &x1);
void cdFillEllipse(const CdTarget &tg, float cx, float cy, float rx, float ry,
                   lv_color_t col, lv_opa_t opa = LV_OPA_COVER, CdRowHoleFn hole = nullptr);

// Filled ring between rInner and rOuter, NOT anti-aliased. renderYield()
// every yieldRows drawn rows (0 = never). The default 40 suits a band of a
// few dozen pixels; a ring only a few pixels wide writes far less per row and
// can take a longer cadence - on the 4" board every yield is a 1 ms sleep.
void cdFillAnnulus(const CdTarget &tg, float cx, float cy, float rInner, float rOuter,
                   lv_color_t col, lv_opa_t opa = LV_OPA_COVER, int yieldRows = 40);

// Even-odd scanline fill of a simple polygon with integer vertices, NOT
// anti-aliased, at most 24 edge crossings per row. yieldRows: offer
// renderYield() every that many rows; 0 = never. Unlike cdFillPoly() it takes
// an opacity, has its own yield cadence and does not invalidate - keep the two
// apart, merging them would change the other screens' timing.
void cdFillPolyOpa(const CdTarget &tg, const lv_point_t *pts, int n,
                   lv_color_t col, lv_opa_t opa, int yieldRows = 20);

// ---- Anti-aliased areas --------------------------------------------
// Written for the Rudder band and needle. Same rules as the helpers above:
// straight into tg.buf, no invalidate, no yield.
//
// One rule for all of them: the opacity of an edge pixel comes from the REAL
// distance of its centre to the geometry, never from a loop index. The two
// area fills evaluate only the pixels within about 1 px of an edge one by
// one; inside, whole runs go out through cdSpanH(), so a large area costs a
// few float operations per row, not per pixel. Edge pixels blend with
// lv_color_mix at (lv_opa_t)(opa * coverage), truncated, skipped at 0 - as
// in cdRingAA.
//
// cdSectorAA and cdFillConvexAA reject non-finite input (no hang, no write)
// and write every pixel at most once per call, so a translucent shape never
// double-blends where its pieces meet.
//
// Pixel convention: cdSectorAA and cdFillConvexAA put the centre of pixel x
// at x + 0.5 (as cdRingAA and cdDiscAA do): to centre a shape on pixel
// column 300, pass 300.5.

// Annular sector between rIn and rOut, from a0Deg clockwise to a1Deg.
//
// Angles use the COMPASS (= rudder) convention, NOT the LVGL one of
// cdFillRing: 0 deg = straight up from the centre, increasing clockwise on
// screen (90 = right, 180 = down, -90 = left). A rudder angle is therefore
// passed as it is: port negative, starboard positive, e.g. the port half of a
// +-40 deg gauge is (-40, 0). LVGL angle = compass angle - 90.
// a1Deg - a0Deg is the span: <= 0 draws nothing, >= 360 a full ring. The
// angles are exact floats (no whole-degree rounding as in cdFillRing) and may
// lie outside 0..360. rIn <= 0 gives a pie slice. The centre may lie far
// outside the target; only the rows the sector touches are visited.
//
// Coverage = radial x angular. Radial from |p - c| against rIn and rOut;
// angular from the distance to the two edge rays. Where both edges of a pair
// fall into one pixel (a band or sector thinner than ~1 px) their coverages
// are combined as c0 + c1 - 1, so a thin sliver fades out instead of staying
// at half strength. Within ~1 px of the centre the angular part is only
// approximate.
//
// Known backdrops make the call write-only (no buffer read), as the Rudder
// band wants it:
//   bgOut  - colour beyond the two arcs (inside rIn, outside rOut). Without
//            bgSide it is also taken beyond the two angular edges, i.e. the
//            sector is drawn on a plain bgOut area.
//   bgSide - colour beyond the two angular edges WITHIN the band, i.e. the
//            sector is drawn over a band of the same radii that holds bgSide.
//            Edge pixels then become mix(mix(col, bgSide, angular), bgOut,
//            radial): the exact partition blend, no double blend.
// nullptr reads the pixel instead. bgSide assumes bgOut is given as well:
// without bgOut the arcs blend over the band's own rim pixels. Example, a
// gauge band with its active sector, every write a plain store - exact only
// while the 0° seam lies on a pixel boundary (integer cx); with a
// pixel-centred pivot the seam column is shared by two sectors and bgSide
// can name only one of them:
//   cdSectorAA(tg, cx, cy, rI, rO, -40, 0,  portTint, LV_OPA_COVER, &bg);
//   cdSectorAA(tg, cx, cy, rI, rO,   0, 40, stbdTint, LV_OPA_COVER, &bg, &portTint);
//   cdSectorAA(tg, cx, cy, rI, rO,   0, a,  active,   LV_OPA_COVER, &bg, &stbdTint);
void cdSectorAA(const CdTarget &tg, float cx, float cy, float rIn, float rOut,
                float a0Deg, float a1Deg, lv_color_t col, lv_opa_t opa,
                const lv_color_t *bgOut = nullptr, const lv_color_t *bgSide = nullptr);

// Filled convex polygon with float vertices xy[0..2n-1] = x0,y0, x1,y1, ...,
// 3 <= n <= CD_CONVEX_MAX, either winding. Coverage = clamp(d + 0.5 + grow,
// 0, 1) with d the real signed distance of the pixel centre to the outline
// (positive inside). grow > 0 widens the shape by that many px with rounded
// corners: grow = 1 gives the weight a solid triangle plus a 2-px outline
// stroke has, without the stroke. A non-convex outline is not supported: it
// draws the intersection of its edge half-planes (safe, but not its shape).
// The edge table lives on the stack (36 B per vertex), hence the small limit.
static const int CD_CONVEX_MAX = 8;
void cdFillConvexAA(const CdTarget &tg, const float *xy, int n,
                    lv_color_t col, lv_opa_t opa, float grow = 0.f);

// ---- One LVGL draw context for a run of canvas draws (CdLvBatch) -----------
// LVGL's own line, label, rect and arc renderers, but with ONE fake display
// for the whole run instead of one per call.
//
// Why: every lv_canvas_draw_line/_text/_rect/_arc builds a complete fake
// display around a single draw - init_fake_disp() zeroes an lv_disp_t and an
// lv_disp_drv_t, lv_mem_alloc()s and initialises a software draw context,
// draws, frees it all again in deinit_fake_disp() and ends with an
// lv_obj_invalidate() of the whole canvas (see the note on cdSpanH() in
// CanvasDraw.cpp). Measured on the 7B that is about 0.1 ms per call whatever
// is drawn: the Depth echogram paid it 57 times a frame for 48 short lines and
// 9 labels. A batch pays it once.
//
// Why the pixels cannot change: cdLvBegin()/cdLvEnd() are a verbatim copy of
// lv_canvas.c's init_fake_disp()/deinit_fake_disp() of LVGL 8.4.0, together
// with the _lv_refr_set_disp_refreshing() save/restore every lv_canvas_draw_*
// does around its draw, and each cdLv* draw passes exactly the coordinates its
// lv_canvas_draw_* counterpart builds to the same lv_draw_* call. What a batch
// shares between draws is the draw context, and the software context carries
// no state from one draw into the next: every renderer that narrows
// draw_ctx->clip_area (arc quarters, image rows) restores it before it
// returns, and the blend caches are locals of the blend call. The dropped
// per-call invalidates never touched a pixel - a canvas is only re-read by
// LVGL's own refresh, which cannot run before the caller returns.
// Re-check the copy against lv_canvas.c whenever LVGL is upgraded.
//
// Rules while a batch is open:
//   - No lv_obj_* call, renderYield() or anything else that might reach
//     lv_timer_handler(): LVGL takes the fake display for the one it is
//     refreshing (_lv_refr_get_disp_refreshing()) until cdLvEnd().
//   - Batches nest only last-in first-out, and a batch is never copied or
//     moved: the fake display points into the object itself.
//   - cdLvBegin() fails - and every cdLv* draw is then a no-op - for a null
//     canvas, an indexed one (which lv_canvas_draw_* refuse as well) and when
//     lv_mem_alloc() fails (lv_canvas_draw_* would go on with a NULL context).
//
// cdLvEnd() closes the batch and, if any draw was issued, invalidates the
// canvas once - unless the batch was opened with invalidate = false because
// the caller invalidates the whole canvas itself at the end of its paint.
// The destructor calls cdLvEnd(), so a batch closes on every path out of its
// scope; calling cdLvEnd() again is harmless.
//
//   { CdLvBatch b(_canvas);
//     cdLvLine(b, pts, 2, &ld);                 // = lv_canvas_draw_line(...)
//     cdLvText(b, x, y, w, &td, "NOW");          // = lv_canvas_draw_text(...)
//   }                                            // one lv_obj_invalidate
struct CdLvBatch {
    explicit CdLvBatch(lv_obj_t *canvas, bool invalidate = true);
    ~CdLvBatch();
    CdLvBatch(const CdLvBatch &) = delete;
    CdLvBatch &operator=(const CdLvBatch &) = delete;

    bool open() const { return _open; }

    // Internal state of the fake display - for the cdLv* functions only.
    lv_obj_t     *_canvas;
    lv_disp_t     _disp;      // the fake display ...
    lv_disp_drv_t _drv;       // ... its driver ...
    lv_area_t     _clip;      // ... and its clip = buffer area: the whole canvas
    lv_disp_t    *_refrOri;   // the display LVGL was refreshing before
    lv_coord_t    _h;         // canvas height: the bottom of every text box
    uint8_t       _cf;        // canvas colour format (lv_img_cf_t)
    bool          _open, _inval, _drew;
};

// Opens the fake display (init_fake_disp). false: the batch stays closed.
// The constructor calls it; call it directly only to reopen a batch that
// cdLvEnd() closed.
bool cdLvBegin(CdLvBatch &b, lv_obj_t *canvas, bool invalidate = true);
// Closes it (deinit_fake_disp) and invalidates as described above.
void cdLvEnd(CdLvBatch &b);

// The draws, each equal to the lv_canvas_draw_* named on it.
// = lv_canvas_draw_line(canvas, pts, n, dsc): segment i runs pts[i]..pts[i+1].
void cdLvLine(CdLvBatch &b, const lv_point_t pts[], uint32_t n,
              const lv_draw_line_dsc_t *dsc);
// = lv_canvas_draw_text(canvas, x, y, maxW, dsc, txt): the label box is
// x..x+maxW-1 by y..canvas height-1.
void cdLvText(CdLvBatch &b, lv_coord_t x, lv_coord_t y, lv_coord_t maxW,
              const lv_draw_label_dsc_t *dsc, const char *txt);
// = lv_canvas_draw_rect(canvas, x, y, w, h, dsc).
void cdLvRect(CdLvBatch &b, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
              const lv_draw_rect_dsc_t *dsc);
// = lv_canvas_draw_arc(canvas, x, y, r, a0, a1, dsc), angles truncated to
// uint16_t as there (see the note on cdFillRing() above).
void cdLvArc(CdLvBatch &b, lv_coord_t x, lv_coord_t y, lv_coord_t r,
             int32_t a0, int32_t a1, const lv_draw_arc_dsc_t *dsc);
