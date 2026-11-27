#include "CanvasDraw.h"
#include "screens/RenderYield.h"   // renderYield() - the shared draw-loop yield
// CdLvBatch: lv_draw_sw_ctx_t and lv_draw_sw_init_ctx()/_deinit_ctx(), which
// lvgl.h does not pull in; lv_canvas.c includes the same header.
#include "src/draw/sw/lv_draw_sw.h"
#include <math.h>

// The one wall-clock timestamp behind renderYield(). It lives here rather than
// in the header so that every screen's fill loops and the scanline fill below
// share a single yield budget across the whole repaint - see RenderYield.h.
#if defined(BOARD_PANEL_1024X600)
uint32_t g_renderYieldLastMs = 0;
#endif

// ---- Filled annular sector ---------------------------------------------------
void cdFillRing(lv_obj_t *cv, float cx, float cy, float rInner, float rOuter,
                float a0Deg, float a1Deg, lv_color_t col) {
    if (!cv || rOuter <= 0.f || rOuter <= rInner) return;
    if (rInner < 0.f) rInner = 0.f;

    float span = a1Deg - a0Deg;
    if (span <= 0.f) return;
    if (span > 360.f) span = 360.f;

    // Normalise the start into [0,360); LVGL truncates to uint16_t (see header).
    float s = fmodf(a0Deg, 360.f);
    if (s < 0.f) s += 360.f;
    float e = s + span;                     // may exceed 360 → LVGL wraps it

    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = col;
    ad.opa   = LV_OPA_COVER;                // never translucent — see header
    lv_coord_t w = (lv_coord_t)(rOuter - rInner + 0.5f);
    if (w < 1) w = 1;
    ad.width = w;

    lv_canvas_draw_arc(cv, (lv_coord_t)(cx + 0.5f), (lv_coord_t)(cy + 0.5f),
                       (lv_coord_t)(rOuter + 0.5f),
                       (int32_t)(s + 0.5f), (int32_t)(e + 0.5f), &ad);
}


// ---- Filled polygon (even-odd scanline, direct buffer writes) ----------------
void cdFillPoly(lv_obj_t *cv, const lv_point_t *pts, int n, lv_color_t col) {
    if (!cv || !pts || n < 3) return;
    lv_img_dsc_t *img = lv_canvas_get_img(cv);
    if (!img || !img->data) return;
    lv_color_t *buf = (lv_color_t *)img->data;
    const int W = (int)img->header.w, H = (int)img->header.h;
    if (W <= 0 || H <= 0) return;

    int yMin = pts[0].y, yMax = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].y < yMin) yMin = pts[i].y;
        if (pts[i].y > yMax) yMax = pts[i].y;
    }
    if (yMin < 0) yMin = 0;
    if (yMax > H - 1) yMax = H - 1;

    static const int MAXX = 24;              // max edge crossings per scanline
    float xs[MAXX];

    for (int y = yMin; y <= yMax; y++) {
        int cnt = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            float y1 = (float)pts[j].y, y2 = (float)pts[i].y;
            // half-open test: a vertex counts once, so spans never double-open
            if ((y1 <= (float)y && y2 > (float)y) || (y2 <= (float)y && y1 > (float)y)) {
                float t = ((float)y - y1) / (y2 - y1);
                if (cnt < MAXX) xs[cnt++] = (float)pts[j].x + t * ((float)pts[i].x - (float)pts[j].x);
            }
        }
        for (int a = 1; a < cnt; a++) {      // insertion sort (cnt is tiny)
            float v = xs[a]; int b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = v;
        }
        for (int k = 0; k + 1 < cnt; k += 2) {
            int x0 = (int)(xs[k] + 0.5f), x1 = (int)(xs[k + 1] + 0.5f);
            if (x1 < 0 || x0 > W - 1) continue;
            if (x0 < 0) x0 = 0;
            if (x1 > W - 1) x1 = W - 1;
            lv_color_t *p = buf + (size_t)y * W + x0;
            for (int x = x0; x <= x1; x++) *p++ = col;
        }
        if ((y & 31) == 0) renderYield();   // yield: no yield point inside LVGL draws
    }
    lv_obj_invalidate(cv);
}

void cdFillTri(lv_obj_t *cv, lv_point_t a, lv_point_t b, lv_point_t c, lv_color_t col) {
    lv_point_t p[3] = { a, b, c };
    cdFillPoly(cv, p, 3, col);
}


// ============================================================
// Direct-buffer raster (moved from WindScreen)
//
// The seven functions after cdTargetOf() are WindScreen.cpp's former member
// functions fillEllipse, spanH, linePx, ringAA, discAA, fillAnnulus and
// fillPolygon, moved without changing their code. The only edits are the
// render target: _cbuf -> tg.buf, CW -> tg.w, CH -> tg.h, and the member calls
// spanH( -> cdSpanH(tg, . The parameter is tg rather than t because cdSpanH,
// cdLineAA and cdFillPolyOpa keep their local variables named t. Their
// comments came along; they are in English now and say tg.buf, tg.w and
// cdSpanH() where they used to say _cbuf, CW and spanH(). Since the move,
// cdRingAA has learnt the quarter evaluation, cdFillEllipse an optional hole
// and cdFillAnnulus a yield cadence - each without changing a pixel it
// writes (see their comments).
// ============================================================

CdTarget cdTargetOf(lv_obj_t *canvas) {
    CdTarget t = { nullptr, 0, 0 };
    if (!canvas) return t;
    lv_img_dsc_t *img = lv_canvas_get_img(canvas);
    if (!img || !img->data) return t;
    // The helpers index buf[y * w + x] with 2-byte pixels: only a plain
    // TRUE_COLOR canvas has that layout.
    if (img->header.cf != LV_IMG_CF_TRUE_COLOR) return t;
    const int w = (int)img->header.w, h = (int)img->header.h;
    if (w <= 0 || h <= 0) return t;
    t.buf = (lv_color_t *)img->data;
    t.w   = w;
    t.h   = h;
    return t;
}

// ── Filled-ellipse helper ─────────────────────────────────────────────────────
void cdFillEllipse(const CdTarget &tg, float cx, float cy, float rx, float ry,
                   lv_color_t col, lv_opa_t opa, CdRowHoleFn hole) {
    // Direct span writes, for the reason spelled out on cdSpanH().
    //
    // The span ends are computed exactly as before the hole existed - the
    // float expressions and their uses are untouched, so the compiler has no
    // new reason to fuse or reorder anything in them - and the hole only
    // splits the integer span afterwards. A hole that does not lie strictly
    // inside the span leaves the row whole, i.e. as without a hole.
    int row = 0;
    for (float dy2 = -ry; dy2 <= ry; dy2 += 1.f) {
        float ratio = dy2 / ry;
        float hx = rx * sqrtf(1.f - ratio * ratio);
        const int y = (int)(cy + dy2), x0 = (int)(cx - hx), x1 = (int)(cx + hx);
        int h0, h1;
        if (hole && hole(y, h0, h1) && x0 < h0 && h0 <= h1 && h1 < x1) {
            cdSpanH(tg, y, x0, h0 - 1, col, opa);
            cdSpanH(tg, y, h1 + 1, x1, col, opa);
        } else {
            cdSpanH(tg, y, x0, x1, col, opa);
        }
        if (++row % 20 == 0) renderYield();
    }
}

// ── Horizontal span ───────────────────────────────────────────────────────────
// One horizontal run of pixels, written straight into the canvas buffer.
//
// WHY NOT lv_canvas_draw_line: because that is not a line drawing routine, it is
// a whole display. Every call runs init_fake_disp(), which zeroes an lv_disp_t
// AND an lv_disp_drv_t, then lv_mem_alloc()s an lv_draw_sw_ctx_t, initialises
// the software draw context, draws, tears the context down again with
// lv_mem_free(), and finally calls lv_obj_invalidate(canvas) - which walks the
// parent chain and merges an area into the display's invalid-area list. That is
// a malloc/free pair, two struct memsets and a full invalidate per span. The
// polygon filler (cdFillPolyOpa below) emits one span per scanline, so a single
// zone sector was paying that toll a hundred times over, for spans that are
// often 40 px wide. Measured on the 5B: the five zone/boat/overlay phases
// together were 198 ms of a 261 ms repaint.
//
// The caller owns this buffer (tg.buf, LV_IMG_CF_TRUE_COLOR, stride tg.w), so a
// span is a pointer and a loop. WindScreen::drawInstrument() already ends with
// one lv_obj_invalidate(_canvas), which is all the invalidation that was ever
// needed - the per-span ones were pure waste.
void cdSpanH(const CdTarget &tg, int y, int x0, int x1, lv_color_t col, lv_opa_t opa) {
    if (!tg.buf) return;
    if (y < 0 || y >= tg.h) return;
    if (x1 < x0) { const int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= tg.w) return;
    if (x0 < 0)      x0 = 0;
    if (x1 >= tg.w)  x1 = tg.w - 1;

    lv_color_t *p   = tg.buf + (size_t)y * (size_t)tg.w + (size_t)x0;
    lv_color_t *end = tg.buf + (size_t)y * (size_t)tg.w + (size_t)x1;

    if (opa >= LV_OPA_COVER) {
#if LV_COLOR_DEPTH == 16
        // Two pixels per store once aligned; both halves of the word are the
        // same colour, so the byte order in the pair is irrelevant. This does
        // not buy bandwidth: measured on the device (August 2026), 32-bit
        // instead of 16-bit stores in the wind background fill gained 0 ms,
        // because the RGB panel streams 50.8 MB/s from the same PSRAM. Only
        // writing fewer pixels is faster.
        if ((((uintptr_t)p) & 3u) && p <= end) *p++ = col;
        uint32_t      *w    = (uint32_t *)p;
        const uint32_t two  = ((uint32_t)col.full << 16) | (uint32_t)col.full;
        const int      pair = (int)((end - p + 1) / 2);
        for (int i = 0; i < pair; i++) *w++ = two;
        p = (lv_color_t *)w;
#endif
        while (p <= end) *p++ = col;
    } else {
        while (p <= end) { *p = lv_color_mix(col, *p, opa); p++; }
    }
}

// Anti-aliased line, straight into tg.buf - the counterpart of cdSpanH() for
// strokes instead of areas.
//
// WHY: WindScreen's old cline() was lv_canvas_draw_line() and paid, per call,
// the same toll the comment on cdSpanH() describes (malloc/free, two memsets,
// a full lv_obj_invalidate). Measured on the device: the 13 flow lines in
// WindScreen::drawWindLines() took 50.9 ms that way - of 77 ms for the whole
// BOAT phase, so about 4 ms for ONE stroke at 40 % opacity.
//
// The method is Xiaolin Wu: for each step along the longer axis, the two
// neighbouring pixels on the shorter axis are tinted in proportion. Without
// that the lines would show a hard staircase - at 40 % opacity behind the
// boat, that stands out at once.
void cdLineAA(const CdTarget &tg, float x0, float y0, float x1, float y1,
              lv_color_t col, lv_opa_t opa, float w) {
    if (!tg.buf) return;

    auto pixel = [&](int x, int y, float f) {
        if (x < 0 || x >= tg.w || y < 0 || y >= tg.h) return;
        if (f <= 0.f) return;
        if (f > 1.f) f = 1.f;
        const lv_opa_t a = (lv_opa_t)((float)opa * f);
        if (a == 0) return;
        lv_color_t *p = tg.buf + (size_t)y * (size_t)tg.w + (size_t)x;
        *p = (a >= LV_OPA_COVER) ? col : lv_color_mix(col, *p, a);
    };

    // Wide strokes: coverage from the REAL distance of each pixel to the
    // segment, exactly as in cdRingAA().
    //
    // The first attempt worked differently and looked visibly worse: it
    // walked the line in 1-px steps and set a strip across it at each step,
    // taking the opacity from the INTEGER offset and rounding the target
    // pixels. That gave no smoothing across the line at all - only steps,
    // plus double writes at the rounding boundaries. The user spotted it as
    // "pixelated" at once. Rule: the opacity must come from the actual
    // distance of the pixel centre, never from the loop index.
    if (w > 1.5f) {
        const float dxl = x1 - x0, dyl = y1 - y0;
        const float len2 = dxl * dxl + dyl * dyl;
        if (len2 < 1e-6f) return;
        const float len = sqrtf(len2);
        const float h   = w * 0.5f;
        const float r   = h + 1.f;            // reach, including the soft edge

        int ya = (int)floorf(fminf(y0, y1) - r);
        int yb = (int)ceilf (fmaxf(y0, y1) + r);
        if (ya < 0) ya = 0;
        if (yb > tg.h - 1) yb = tg.h - 1;
        const float xlo = fminf(x0, x1) - r, xhi = fmaxf(x0, x1) + r;

        // Line equation A*x + B*y + C = 0 with |A,B| = len, so that
        // |A*x + B*y + C| is directly the distance times len.
        const float A = -dyl, B = dxl, C = -(A * x0 + B * y0);

        for (int y = ya; y <= yb; y++) {
            const float py = (float)y + 0.5f;
            float xa = xlo, xb = xhi;
            if (fabsf(A) > 1e-4f) {
                const float k1 = (-r * len - B * py - C) / A;
                const float k2 = ( r * len - B * py - C) / A;
                const float lo = fminf(k1, k2), hi = fmaxf(k1, k2);
                if (lo > xa) xa = lo;
                if (hi < xb) xb = hi;
            }
            int ix0 = (int)floorf(xa), ix1 = (int)ceilf(xb);
            if (ix1 < 0 || ix0 > tg.w - 1 || ix0 > ix1) continue;
            if (ix0 < 0) ix0 = 0;
            if (ix1 > tg.w - 1) ix1 = tg.w - 1;

            for (int x = ix0; x <= ix1; x++) {
                const float px = (float)x + 0.5f;
                float t = ((px - x0) * dxl + (py - y0) * dyl) / len2;
                if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
                const float cx = x0 + t * dxl, cy = y0 + t * dyl;
                const float ddx = px - cx, ddy = py - cy;
                const float d = sqrtf(ddx * ddx + ddy * ddy);
                float f = h + 0.5f - d;
                if (f <= 0.f) continue;
                if (f > 1.f) f = 1.f;
                pixel(x, y, f);
            }
        }
        return;
    }

    const bool steil = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steil) { float t; t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }

    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float grad = (dx == 0.f) ? 1.f : dy / dx;

    float y = y0 + grad * (roundf(x0) - x0);
    const int xEnd = (int)roundf(x1);
    for (int x = (int)roundf(x0); x <= xEnd; x++, y += grad) {
        const int   iy = (int)floorf(y);
        const float fr = y - (float)iy;
        if (steil) { pixel(iy, x, 1.f - fr); pixel(iy + 1, x, fr); }
        else       { pixel(x, iy, 1.f - fr); pixel(x, iy + 1, fr); }
    }
}

// Anti-aliased circular ring. It replaces a full lv_canvas_draw_arc(0..360),
// which evaluates an LVGL radius mask over the whole bounding box; what that
// cost on the device is noted on WindScreen::ringAA() in WindScreen.h.
//
// Only the band itself is touched: per row, the one or two x ranges in which
// |distance to the centre - r| <= h are computed. Within them the distance
// gives the opacity directly, so the edges fade out softly instead of
// stepping - for a 2-px ring on a 600-px circle that is exactly the visible
// difference.
//
// Evaluated a quarter at a time. Every visited pixel costs a sqrtf() and the
// two border rings of the wind screen visit about 15,600 of them a frame
// (17 ms measured on the 7B, the per-pixel float work, not PSRAM, being the
// bulk of it). With 2*cx and 2*cy whole numbers - the centre on a pixel
// centre or a pixel corner, true for every caller - the pixel x mirrors to
// mx - x (mx = 2cx - 1) and the row y to my - y with dx and dy EXACTLY
// negated, so the coverage of the up to four mirror images is the same
// number: it is computed once and each image blended against its own pixel.
// Bit for bit what the plain loop gave, on any compiler:
//   - dx and dy are multiples of 0.5 below 1024 (guarded), so dx*dx, dy*dy
//     and their sum are exact - fused or not, the sum is the same - and
//     sqrtf() is correctly rounded; d - r, fabsf, the clamp and the opacity
//     product contain no multiply-add that could be contracted;
//   - every image is written only where the plain loop visits it: the same
//     per-row ranges (the same expressions, clipped the same way) are worked
//     out for both rows of a pair and each image is tested against its own
//     row's ranges - as often as it lies in them, so even overlapping halves
//     blend twice as before. The ranges also have half a pixel to spare: they
//     reach out to rOut + 1 and in to rIn - 1, so the pixel at either end of
//     a range has a centre at least rOut + 0.5 out resp. at most rIn - 0.5
//     in, i.e. coverage 0, and a range end that a rounding moves by a pixel
//     only adds or drops such a pixel;
//   - the rows and pixels are just visited in a different order, and no
//     pixel's result depends on another's.
// Checked against the plain loop in a PC harness over the real callers and
// 6,000 random rings (on and off the grid, clipped, translucent); the grid
// cases also with one of the two compiled with fused multiply-add and the
// other without.
// Anything else (an off-grid centre, a huge ring) takes the plain loop.
void cdRingAA(const CdTarget &tg, float cx, float cy, float r, float w,
              lv_color_t col, lv_opa_t opa) {
    if (!tg.buf || r <= 0.f || w <= 0.f) return;
    const float h    = w * 0.5f;
    const float rOut = r + h, rIn = r - h;
    const int   y0   = (int)floorf(cy - rOut - 1.f);
    const int   y1   = (int)ceilf (cy + rOut + 1.f);

    // Row y as the plain loop sets it up: dy, and the one or two x ranges it
    // visits, clipped to the target. Returns how many ranges are left; 0 =
    // the row is not visited at all.
    auto rowRanges = [&](int y, float &dy, int xa[2], int xb[2]) -> int {
        if (y < 0 || y >= tg.h) return 0;
        dy = (float)y + 0.5f - cy;
        const float ay = fabsf(dy);
        if (ay > rOut + 1.f) return 0;

        // Outer edge of the row; inside, the band splits into two halves as
        // soon as the row crosses the hole.
        const float xo = sqrtf(fmaxf(0.f, (rOut + 1.f) * (rOut + 1.f) - dy * dy));
        const bool  gespalten = ay < (rIn - 1.f);
        const float xi = gespalten
                       ? sqrtf(fmaxf(0.f, (rIn - 1.f) * (rIn - 1.f) - dy * dy))
                       : 0.f;
        int ra[2], rb[2], n;
        if (gespalten) {
            ra[0] = (int)floorf(cx - xo); rb[0] = (int)ceilf(cx - xi);
            ra[1] = (int)floorf(cx + xi); rb[1] = (int)ceilf(cx + xo); n = 2;
        } else {
            ra[0] = (int)floorf(cx - xo); rb[0] = (int)ceilf(cx + xo); n = 1;
        }
        int m = 0;
        for (int k = 0; k < n; k++) {
            int a = ra[k], b = rb[k];
            if (b < 0 || a >= tg.w) continue;
            if (a < 0) a = 0;
            if (b >= tg.w) b = tg.w - 1;
            xa[m] = a; xb[m] = b; m++;
        }
        return m;
    };
    // Opacity of the pixel centre at (dx, dy); 0 = not written.
    auto alpha = [&](float dx, float dy) -> lv_opa_t {
        const float d = sqrtf(dx * dx + dy * dy);
        float f = h + 0.5f - fabsf(d - r);   // 1 in the middle, 0 outside
        if (f <= 0.f) return 0;
        if (f > 1.f) f = 1.f;
        return (lv_opa_t)((float)opa * f);
    };
    // Blend pixel (x, y) once for every range of its row that holds x.
    auto put = [&](int x, int y, int n, const int xa[2], const int xb[2], lv_opa_t a) {
        lv_color_t *p = tg.buf + (size_t)y * (size_t)tg.w + (size_t)x;
        for (int k = 0; k < n; k++)
            if (x >= xa[k] && x <= xb[k])
                *p = (a >= LV_OPA_COVER) ? col : lv_color_mix(col, *p, a);
    };

    const float mxf = 2.f * cx - 1.f, myf = 2.f * cy - 1.f;
    const bool  mirror = fabsf(cx) < 4096.f && fabsf(cy) < 4096.f && rOut + 2.f < 1024.f &&
                         (float)(int)mxf == mxf && (float)(int)myf == myf;
    if (!mirror) {
        for (int y = y0; y <= y1; y++) {
            float dy; int xa[2], xb[2];
            const int n = rowRanges(y, dy, xa, xb);
            for (int k = 0; k < n; k++)
                for (int x = xa[k]; x <= xb[k]; x++) {
                    const lv_opa_t a = alpha((float)x + 0.5f - cx, dy);
                    if (a == 0) continue;
                    lv_color_t *p = tg.buf + (size_t)y * (size_t)tg.w + (size_t)x;
                    *p = (a >= LV_OPA_COVER) ? col : lv_color_mix(col, *p, a);
                }
        }
        return;
    }

    const int mx = (int)mxf, my = (int)myf;
    const int halfX = (mx >= 0) ? mx / 2 : -((1 - mx) / 2);   // floor(mx / 2)
    // Row pairs (y, my - y), the upper one first. Either may lie outside the
    // rows the plain loop visits, so the pairs start where either one could.
    const int yTop = (y0 < my - y1) ? y0 : my - y1;
    for (int y = yTop; 2 * y <= my; y++) {
        const int yb = my - y;
        float dyA = 0.f, dyB = 0.f;
        int axa[2], axb[2], bxa[2], bxb[2];
        const int na = (y >= y0 && y <= y1) ? rowRanges(y, dyA, axa, axb) : 0;
        const int nb = (yb != y && yb >= y0 && yb <= y1) ? rowRanges(yb, dyB, bxa, bxb) : 0;
        if (na == 0 && nb == 0) continue;
        const float dy = na ? dyA : dyB;           // dyB == -dyA: the same coverage

        // Left-half columns whose pixel or mirror image some range holds:
        // up to 8 intervals, sorted and merged.
        int ia[8], ib[8], ni = 0;
        auto add = [&](int a, int b) {
            if (b > halfX) b = halfX;
            if (a > b) return;
            int k = ni++;
            while (k > 0 && ia[k - 1] > a) { ia[k] = ia[k - 1]; ib[k] = ib[k - 1]; k--; }
            ia[k] = a; ib[k] = b;
        };
        for (int k = 0; k < na; k++) { add(axa[k], axb[k]); add(mx - axb[k], mx - axa[k]); }
        for (int k = 0; k < nb; k++) { add(bxa[k], bxb[k]); add(mx - bxb[k], mx - bxa[k]); }
        // Each column once: an interval starts at its own start or past the
        // last column already done, whichever is later.
        int next = (ni > 0) ? ia[0] : 0;
        for (int k = 0; k < ni; k++) {
            for (int x = (ia[k] > next) ? ia[k] : next; x <= ib[k]; x++) {
                const lv_opa_t a = alpha((float)x + 0.5f - cx, dy);
                if (a == 0) continue;
                const int xm = mx - x;
                if (na) {
                    put(x, y, na, axa, axb, a);
                    if (xm != x) put(xm, y, na, axa, axb, a);
                }
                if (nb) {
                    put(x, yb, nb, bxa, bxb, a);
                    if (xm != x) put(xm, yb, nb, bxa, bxb, a);
                }
            }
            if (ib[k] + 1 > next) next = ib[k] + 1;
        }
    }
}

// Filled disc with a soft edge, for small dots whose outline is almost all
// edge. Not built from cdRingAA(): that takes its coverage from the distance
// to the nominal radius, so the middle of a disc would come out half
// transparent (see also WindScreen::discAA() in WindScreen.h).
void cdDiscAA(const CdTarget &tg, float cx, float cy, float r, lv_color_t col, lv_opa_t opa) {
    if (!tg.buf || r <= 0.f) return;
    int ya = (int)floorf(cy - r - 1.f);
    int yb = (int)ceilf (cy + r + 1.f);
    if (ya < 0) ya = 0;
    if (yb > tg.h - 1) yb = tg.h - 1;

    for (int y = ya; y <= yb; y++) {
        const float dy   = (float)y + 0.5f - cy;
        const float halb = sqrtf(fmaxf(0.f, (r + 1.f) * (r + 1.f) - dy * dy));
        int xa = (int)floorf(cx - halb), xb = (int)ceilf(cx + halb);
        if (xb < 0 || xa > tg.w - 1) continue;
        if (xa < 0) xa = 0;
        if (xb > tg.w - 1) xb = tg.w - 1;
        lv_color_t *row = tg.buf + (size_t)y * (size_t)tg.w;
        for (int x = xa; x <= xb; x++) {
            const float dx = (float)x + 0.5f - cx;
            float f = r + 0.5f - sqrtf(dx * dx + dy * dy);
            if (f <= 0.f) continue;
            if (f > 1.f) f = 1.f;
            const lv_opa_t a = (lv_opa_t)((float)opa * f);
            if (a == 0) continue;
            lv_color_t *p = row + x;
            *p = (a >= LV_OPA_COVER) ? col : lv_color_mix(col, *p, a);
        }
    }
}

// Filled ring, scanline by scanline. Rows that pass beside the hole get one
// span; rows that cross it get two, one per side.
void cdFillAnnulus(const CdTarget &tg, float cx, float cy, float rInner, float rOuter,
                   lv_color_t col, lv_opa_t opa, int yieldRows) {
    if (rOuter <= 0.f) return;
    if (rInner < 0.f) rInner = 0.f;
    const int y0 = (int)(cy - rOuter), y1 = (int)(cy + rOuter);
    int row = 0;
    for (int y = y0; y <= y1; y++) {
        const float dy = (float)y + 0.5f - cy;
        const float o2 = rOuter * rOuter - dy * dy;
        if (o2 <= 0.f) continue;
        const float ho = sqrtf(o2);
        const float i2 = rInner * rInner - dy * dy;
        if (i2 > 0.f) {
            const float hi = sqrtf(i2);
            cdSpanH(tg, y, (int)(cx - ho), (int)(cx - hi), col, opa);
            cdSpanH(tg, y, (int)(cx + hi), (int)(cx + ho), col, opa);
        } else {
            cdSpanH(tg, y, (int)(cx - ho), (int)(cx + ho), col, opa);
        }
        if (yieldRows > 0 && ++row % yieldRows == 0) renderYield();
    }
}

// ── Scanline polygon fill (even-odd) ──────────────────────────────────────────
// Replacement for lv_canvas_draw_polygon (which segfaults in the PC simulator's
// LVGL build). Fills a simple polygon with one cdSpanH() run per pair of edge
// crossings on each scanline.
void cdFillPolyOpa(const CdTarget &tg, const lv_point_t *pts, int n, lv_color_t col, lv_opa_t opa,
                   int yieldRows) {
    if (n < 3) return;
    int ymin = pts[0].y, ymax = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].y < ymin) ymin = pts[i].y;
        if (pts[i].y > ymax) ymax = pts[i].y;
    }
    if (ymin < 0)     ymin = 0;
    if (ymax >= tg.h) ymax = tg.h - 1;
    int row = 0;
    for (int y = ymin; y <= ymax; y++) {
        float xs[24]; int c = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            float y0 = pts[i].y, y1 = pts[j].y;
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                float t = (float)(y - y0) / (y1 - y0);
                if (c < 24) xs[c++] = pts[i].x + t * (pts[j].x - pts[i].x);
            }
        }
        for (int i = 1; i < c; i++) {            // insertion sort the crossings
            float v = xs[i]; int k = i - 1;
            while (k >= 0 && xs[k] > v) { xs[k+1] = xs[k]; k--; }
            xs[k+1] = v;
        }
        for (int i = 0; i + 1 < c; i += 2) {     // fill span pairs
            cdSpanH(tg, y, (int)(xs[i] + 0.5f), (int)(xs[i + 1] + 0.5f), col, opa);
        }
        if (yieldRows > 0 && ++row % yieldRows == 0) renderYield();
    }
}


// ============================================================
// Anti-aliased areas
//
// The area primitives work in two passes per row. First a few float bounds
// split the row into runs: pixels that are certainly fully covered (written
// as one cdSpanH() run), certainly empty (skipped) and the rest. Only "the
// rest" - the pixels within about 1 px of an edge - get the per-pixel
// coverage. The bounds carry a safety margin (CD_EPS) on the conservative
// side, so the split never changes a pixel: the result is the same as
// evaluating the coverage at every pixel of the target (checked so in a
// PC harness on real LVGL).
// ============================================================

static const float CD_EPS    = 0.01f;     // px, margin on every run bound
static const float CD_BIG    = 1e30f;     // "unbounded" in an interval
static const float CD_DEG2RAD = 0.017453292519943295f;

static inline float cdClamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// Finite and of a sane size. Also false for NaN, where every compare fails.
static inline bool cdOk(float v) { return fabsf(v) < 1e6f; }

// A closed interval on one row: X offsets, or pixel-centre x positions.
struct CdIv { float lo, hi; };

// a AND b, both sorted and disjoint; out holds up to na + nb - 1 intervals.
static int cdIvAnd(const CdIv *a, int na, const CdIv *b, int nb, CdIv *out) {
    int n = 0, i = 0, j = 0;
    while (i < na && j < nb) {
        const float lo = fmaxf(a[i].lo, b[j].lo), hi = fminf(a[i].hi, b[j].hi);
        if (lo <= hi) { out[n].lo = lo; out[n].hi = hi; n++; }
        if (a[i].hi < b[j].hi) i++; else j++;
    }
    return n;
}

// Everything except iv: 0, 1 or 2 intervals.
static int cdIvNot(const CdIv &iv, CdIv *out) {
    if (iv.lo > iv.hi) { out[0].lo = -CD_BIG; out[0].hi = CD_BIG; return 1; }
    int n = 0;
    if (iv.lo > -CD_BIG) { out[n].lo = -CD_BIG; out[n].hi = iv.lo; n++; }
    if (iv.hi <  CD_BIG) { out[n].lo = iv.hi;   out[n].hi = CD_BIG; n++; }
    return n;
}

// Narrow iv to the X where nx * X + ny * Y + c >= k (one edge half-plane).
static inline void cdHalfPlane(CdIv &iv, float nx, float ny, float c, float Y, float k) {
    const float rhs = k - ny * Y - c;
    if (nx > 0.f)        iv.lo = fmaxf(iv.lo, rhs / nx);
    else if (nx < 0.f)   iv.hi = fminf(iv.hi, rhs / nx);
    else if (rhs > 0.f)  { iv.lo = CD_BIG; iv.hi = -CD_BIG; }   // never on this row
}

// Pixel range of pixel-centre positions [lo, hi]: grown (the possibly
// touched pixels) or shrunk (the certainly covered ones), clipped to
// [0, w-1]. False if empty.
static bool cdPixRange(float lo, float hi, bool grow, int w, int &xa, int &xb) {
    const float lim = (float)w + 2.f;                  // keeps the (int) casts safe
    lo = fmaxf(-2.f, fminf(lim, lo - 0.5f));
    hi = fmaxf(-2.f, fminf(lim, hi - 0.5f));
    if (grow) { xa = (int)floorf(lo - 1e-3f); xb = (int)ceilf (hi + 1e-3f); }
    else      { xa = (int)ceilf (lo + 1e-3f); xb = (int)floorf(hi - 1e-3f); }
    if (xa < 0) xa = 0;
    if (xb > w - 1) xb = w - 1;
    return xa <= xb;
}

// Pixel rows that a shape spanning yMin..yMax (pixel-centre convention) can
// touch with an edge fade of `reach` px. False if none.
static bool cdRowRange(float yMin, float yMax, float reach, int h, int &ya, int &yb) {
    const float lim = (float)h + 2.f;
    yMin = fmaxf(-2.f, fminf(lim, yMin - reach - 1.f));
    yMax = fmaxf(-2.f, fminf(lim, yMax + reach + 1.f));
    ya = (int)floorf(yMin);
    yb = (int)ceilf(yMax);
    if (ya < 0) ya = 0;
    if (yb > h - 1) yb = h - 1;
    return ya <= yb;
}

// One row: P = pixel ranges that may be touched, S = pixel ranges that are
// certainly fully covered (S lies within P). Runs of S go to solid(), every
// other pixel of P to edge().
template <typename Solid, typename Edge>
static void cdRowRuns(int w, const CdIv *p, int np, const CdIv *s, int ns,
                      float off, Solid solid, Edge edge) {
    int pa[4] = { 0 }, pb[4] = { 0 }, sa[4] = { 0 }, sb[4] = { 0 }, nP = 0, nS = 0;
    for (int i = 0; i < np; i++) {
        int xa, xb;
        if (!cdPixRange(p[i].lo + off, p[i].hi + off, true, w, xa, xb)) continue;
        if (nP > 0 && xa <= pb[nP - 1] + 1) {          // grown ranges may touch
            if (xb > pb[nP - 1]) pb[nP - 1] = xb;
        } else { pa[nP] = xa; pb[nP] = xb; nP++; }
    }
    for (int i = 0; i < ns; i++) {
        int xa, xb;
        if (!cdPixRange(s[i].lo + off, s[i].hi + off, false, w, xa, xb)) continue;
        sa[nS] = xa; sb[nS] = xb; nS++;
    }
    int k = 0;
    for (int i = 0; i < nP; i++) {
        int x = pa[i];
        while (x <= pb[i]) {
            while (k < nS && sb[k] < x) k++;
            if (k < nS && sa[k] <= x) {
                const int e = sb[k] < pb[i] ? sb[k] : pb[i];
                solid(x, e);
                x = e + 1;
            } else {
                const int e = (k < nS && sa[k] <= pb[i]) ? sa[k] - 1 : pb[i];
                for (; x <= e; x++) edge(x);
            }
        }
    }
}

// ---- Annular sector ------------------------------------------------------------
struct CdSector {
    float rIn, rOut;
    bool  ring;               // span >= 360: no angular edges
    bool  flip;               // span > 180: evaluate the complement, invert
    float u0x, u0y, u1x, u1y; // edge rays of the evaluated (convex) wedge
    float n0x, n0y, n1x, n1y; // their inward unit normals
};

// Radial (fr) and angular (fa) coverage of the pixel centre at offset (X, Y)
// from the centre.
static void cdSectorCov(const CdSector &g, float X, float Y, float &fr, float &fa) {
    const float d  = sqrtf(X * X + Y * Y);
    const float cO = cdClamp01(g.rOut + 0.5f - d);
    const float cI = (g.rIn > 0.f) ? cdClamp01(d - g.rIn + 0.5f) : 1.f;
    fr = cdClamp01(cO + cI - 1.f);
    if (g.ring || fr <= 0.f) { fa = 1.f; return; }

    // s: signed distance to the edge's line (positive inside), t: position
    // along its ray. With t <= 0 the pixel lies behind the centre as seen
    // from that ray, so its nearest point on the ray is the centre itself.
    const float s0 = X * g.n0x + Y * g.n0y, s1 = X * g.n1x + Y * g.n1y;
    const float t0 = X * g.u0x + Y * g.u0y, t1 = X * g.u1x + Y * g.u1y;
    float f;
    if (t0 > 0.f && t1 > 0.f) {
        // In front of both rays the two outsides cannot overlap, so the
        // coverages add up: right at one edge, and right for a thin sliver.
        f = cdClamp01(0.5f + s0) + cdClamp01(0.5f + s1) - 1.f;
    } else {
        const float d0 = (t0 > 0.f) ? fabsf(s0) : d;
        const float d1 = (t1 > 0.f) ? fabsf(s1) : d;
        const float m  = fminf(d0, d1);
        f = 0.5f + ((s0 >= 0.f && s1 >= 0.f) ? m : -m);
    }
    f  = cdClamp01(f);
    fa = g.flip ? 1.f - f : f;
}

void cdSectorAA(const CdTarget &tg, float cx, float cy, float rIn, float rOut,
                float a0Deg, float a1Deg, lv_color_t col, lv_opa_t opa,
                const lv_color_t *bgOut, const lv_color_t *bgSide) {
    if (!tg.buf || opa == 0) return;
    if (!cdOk(cx) || !cdOk(cy) || !cdOk(rIn) || !cdOk(rOut) || !cdOk(a0Deg) || !cdOk(a1Deg)) return;
    if (rIn < 0.f) rIn = 0.f;
    if (!(rOut > rIn)) return;
    const float span = a1Deg - a0Deg;
    if (!(span > 0.f)) return;

    CdSector g;
    g.rIn = rIn; g.rOut = rOut;
    g.ring = span >= 360.f;
    g.flip = !g.ring && span > 180.f;
    // A reflex sector is the complement of the convex wedge a1 .. a0 + 360.
    const float b0 = (g.flip ? a1Deg : a0Deg) * CD_DEG2RAD;
    const float b1 = (g.flip ? a0Deg + 360.f : a1Deg) * CD_DEG2RAD;
    // Compass convention: direction (sin a, -cos a). The inward normal of
    // the start edge points clockwise, that of the end edge anticlockwise.
    g.u0x = sinf(b0);  g.u0y = -cosf(b0);  g.n0x =  cosf(b0);  g.n0y =  sinf(b0);
    g.u1x = sinf(b1);  g.u1y = -cosf(b1);  g.n1x = -cosf(b1);  g.n1y = -sinf(b1);

    // Rows: the sector's bounding box. Its top and bottom lie at corners or
    // where the outer arc passes 0 (up) or 180 (down) within the span.
    float yMin = cy - rOut, yMax = cy + rOut;
    if (!g.ring) {
        const float ca0 = cosf(a0Deg * CD_DEG2RAD), ca1 = cosf(a1Deg * CD_DEG2RAD);
        const float c4[4] = { cy - rIn * ca0, cy - rOut * ca0, cy - rIn * ca1, cy - rOut * ca1 };
        float lo = c4[0], hi = c4[0];
        for (int i = 1; i < 4; i++) { lo = fminf(lo, c4[i]); hi = fmaxf(hi, c4[i]); }
        float m = fmodf(0.f - a0Deg, 360.f);   if (m < 0.f) m += 360.f;
        if (m <= span) lo = yMin;
        m = fmodf(180.f - a0Deg, 360.f);       if (m < 0.f) m += 360.f;
        if (m <= span) hi = yMax;
        yMin = lo; yMax = hi;
    }
    int ya, yb;
    if (!cdRowRange(yMin, yMax, 0.5f, tg.h, ya, yb)) return;

    // Colour of a fully covered pixel: with a known backdrop it is fixed, so
    // the interior becomes a plain store even below LV_OPA_COVER.
    const lv_color_t *under = bgSide ? bgSide : bgOut;
    const bool       store = (opa >= LV_OPA_COVER) || under;
    const lv_color_t solidCol = (opa >= LV_OPA_COVER || !under) ? col : lv_color_mix(col, *under, opa);

    const float ro = rOut + 0.5f + CD_EPS;              // beyond: fr == 0
    const float rh = rIn  - 0.5f - CD_EPS;              // within: fr == 0
    const float rf = rOut - 0.5f - CD_EPS;              // within and ...
    const float rg = rIn  + 0.5f + CD_EPS;              // ... beyond: fr == 1

    for (int y = ya; y <= yb; y++) {
        const float Y = (float)y + 0.5f - cy;
        const float xo2 = ro * ro - Y * Y;
        if (xo2 <= 0.f) continue;
        const float xo = sqrtf(xo2);

        // Radial runs, as X = pixel centre - cx.
        CdIv pr[2], sr[2];
        int npr = 1, nsr = 0;
        const float xh2 = rh * rh - Y * Y;
        if (rh > 0.f && xh2 > 0.f) {
            const float xh = sqrtf(xh2);
            pr[0].lo = -xo; pr[0].hi = -xh; pr[1].lo = xh; pr[1].hi = xo; npr = 2;
        } else {
            pr[0].lo = -xo; pr[0].hi = xo;
        }
        const float xf2 = rf * rf - Y * Y;
        if (rf > 0.f && xf2 >= 0.f) {
            const float xf  = sqrtf(xf2);
            const float xg2 = rg * rg - Y * Y;
            if (rIn > 0.f && xg2 > 0.f) {
                const float xg = sqrtf(xg2);
                if (xg <= xf) {
                    sr[0].lo = -xf; sr[0].hi = -xg; sr[1].lo = xg; sr[1].hi = xf; nsr = 2;
                }
            } else {
                sr[0].lo = -xf; sr[0].hi = xf; nsr = 1;
            }
        }

        // Angular runs: where both edge distances are >= 0.5 the wedge
        // covers fully, where either is <= -0.5 not at all.
        CdIv pa[2], sa[2];
        int npa = 1, nsa = 1;
        if (g.ring) {
            pa[0].lo = sa[0].lo = -CD_BIG; pa[0].hi = sa[0].hi = CD_BIG;
        } else {
            CdIv full = { -CD_BIG, CD_BIG }, poss = { -CD_BIG, CD_BIG };
            cdHalfPlane(full, g.n0x, g.n0y, 0.f, Y,  0.5f + CD_EPS);
            cdHalfPlane(full, g.n1x, g.n1y, 0.f, Y,  0.5f + CD_EPS);
            cdHalfPlane(poss, g.n0x, g.n0y, 0.f, Y, -0.5f - CD_EPS);
            cdHalfPlane(poss, g.n1x, g.n1y, 0.f, Y, -0.5f - CD_EPS);
            if (!g.flip) {
                pa[0] = poss; sa[0] = full;
                if (poss.lo > poss.hi) continue;
                if (full.lo > full.hi) nsa = 0;
            } else {
                npa = cdIvNot(full, pa);
                nsa = cdIvNot(poss, sa);
                if (npa == 0) continue;
            }
        }

        CdIv p[3], s[3];
        const int np = cdIvAnd(pr, npr, pa, npa, p);
        const int ns = cdIvAnd(sr, nsr, sa, nsa, s);
        if (np == 0) continue;

        lv_color_t *row = tg.buf + (size_t)y * (size_t)tg.w;
        cdRowRuns(tg.w, p, np, s, ns, cx,
            [&](int xa, int xb) {
                if (store) cdSpanH(tg, y, xa, xb, solidCol, LV_OPA_COVER);
                else       cdSpanH(tg, y, xa, xb, col, opa);
            },
            [&](int x) {
                float fr, fa;
                cdSectorCov(g, (float)x + 0.5f - cx, Y, fr, fa);
                lv_color_t *px = row + x;
                if (!bgSide) {
                    const lv_opa_t a = (lv_opa_t)((float)opa * (fr * fa));
                    if (a == 0) return;
                    *px = (a >= LV_OPA_COVER) ? col : lv_color_mix(col, bgOut ? *bgOut : *px, a);
                } else {
                    // Angular blend within the band first, then the arcs.
                    const lv_opa_t aA = (lv_opa_t)((float)opa * fa);
                    const lv_opa_t aR = (lv_opa_t)(255.f * fr);
                    if (aA == 0 || aR == 0) return;    // the band/backdrop stays
                    const lv_color_t cb = (aA >= LV_OPA_COVER) ? col : lv_color_mix(col, *bgSide, aA);
                    *px = (aR >= LV_OPA_COVER) ? cb : lv_color_mix(cb, bgOut ? *bgOut : *px, aR);
                }
            });
    }
}

// ---- Convex polygon --------------------------------------------------------------
struct CdEdge {
    float x, y, dx, dy, len2;  // start point, edge vector, its squared length
    float nx, ny, c;           // inward unit normal: s = nx*px + ny*py + c
};

// Coverage of the pixel centre (px, py): clamp(sigma + 0.5 + grow) with sigma
// the real signed distance to the outline. Inside a convex polygon that is
// the nearest edge line; outside it is the nearest edge SEGMENT, and only
// edges the point lies beyond can hold that nearest point.
static float cdConvexCov(const CdEdge *e, int ne, float px, float py, float grow) {
    float s[CD_CONVEX_MAX];
    float mn = CD_BIG;
    bool  inside = true;
    for (int i = 0; i < ne; i++) {
        s[i] = e[i].nx * px + e[i].ny * py + e[i].c;
        if (s[i] < 0.f) inside = false;
        mn = fminf(mn, s[i]);
    }
    float sigma = mn;
    if (!inside) {
        float best = CD_BIG;
        for (int i = 0; i < ne; i++) {
            if (s[i] >= 0.f) continue;
            const float qx = px - e[i].x, qy = py - e[i].y;
            float t = (qx * e[i].dx + qy * e[i].dy) / e[i].len2;
            if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
            const float ddx = qx - t * e[i].dx, ddy = qy - t * e[i].dy;
            best = fminf(best, ddx * ddx + ddy * ddy);
        }
        sigma = -sqrtf(best);
    }
    return cdClamp01(sigma + 0.5f + grow);
}

void cdFillConvexAA(const CdTarget &tg, const float *xy, int n,
                    lv_color_t col, lv_opa_t opa, float grow) {
    if (!tg.buf || !xy || n < 3 || n > CD_CONVEX_MAX || opa == 0) return;
    if (!cdOk(grow)) return;
    for (int i = 0; i < 2 * n; i++) if (!cdOk(xy[i])) return;

    // Winding from the signed area; in y-down screen space a positive area
    // runs clockwise and has its inside to the right of each edge.
    float area2 = 0.f;
    for (int i = 0, j = n - 1; i < n; j = i++)
        area2 += xy[2 * j] * xy[2 * i + 1] - xy[2 * i] * xy[2 * j + 1];
    if (fabsf(area2) < 1e-6f) return;
    const float sgn = (area2 > 0.f) ? 1.f : -1.f;

    CdEdge e[CD_CONVEX_MAX];
    int ne = 0;
    float yMin = CD_BIG, yMax = -CD_BIG;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const float x0 = xy[2 * i], y0 = xy[2 * i + 1];
        const float dx = xy[2 * j] - x0, dy = xy[2 * j + 1] - y0;
        yMin = fminf(yMin, y0); yMax = fmaxf(yMax, y0);
        const float len2 = dx * dx + dy * dy;
        if (len2 < 1e-12f) continue;                   // repeated vertex
        const float len = sqrtf(len2);
        CdEdge &k = e[ne++];
        k.x = x0; k.y = y0; k.dx = dx; k.dy = dy; k.len2 = len2;
        k.nx = -dy / len * sgn;  k.ny = dx / len * sgn;
        k.c  = -(k.nx * x0 + k.ny * y0);
    }
    if (ne < 3) return;

    int ya, yb;
    if (!cdRowRange(yMin, yMax, 0.5f + fmaxf(grow, 0.f), tg.h, ya, yb)) return;
    const float kPoss = -(0.5f + grow) - CD_EPS;       // below: coverage 0
    const float kFull = fmaxf(0.f, 0.5f - grow) + CD_EPS; // above, inside: 1

    for (int y = ya; y <= yb; y++) {
        const float py = (float)y + 0.5f;
        CdIv poss = { -CD_BIG, CD_BIG }, full = { -CD_BIG, CD_BIG };
        for (int i = 0; i < ne; i++) {
            cdHalfPlane(poss, e[i].nx, e[i].ny, e[i].c, py, kPoss);
            cdHalfPlane(full, e[i].nx, e[i].ny, e[i].c, py, kFull);
        }
        if (poss.lo > poss.hi) continue;
        const int nsF = (full.lo <= full.hi) ? 1 : 0;

        lv_color_t *row = tg.buf + (size_t)y * (size_t)tg.w;
        cdRowRuns(tg.w, &poss, 1, &full, nsF, 0.f,
            [&](int xa, int xb) { cdSpanH(tg, y, xa, xb, col, opa); },
            [&](int x) {
                const float f = cdConvexCov(e, ne, (float)x + 0.5f, py, grow);
                const lv_opa_t a = (lv_opa_t)((float)opa * f);
                if (a == 0) return;
                lv_color_t *px = row + x;
                *px = (a >= LV_OPA_COVER) ? col : lv_color_mix(col, *px, a);
            });
    }
}

// ---- One LVGL draw context for a run of canvas draws --------------------------
// The bodies below follow lv_canvas.c of LVGL 8.4.0 line by line; the comments
// say where they part from it and why that cannot change a pixel. See the
// header for the rules a caller has to keep.

CdLvBatch::CdLvBatch(lv_obj_t *canvas, bool invalidate) {
    cdLvBegin(*this, canvas, invalidate);
}

CdLvBatch::~CdLvBatch() {
    cdLvEnd(*this);
}

bool cdLvBegin(CdLvBatch &b, lv_obj_t *canvas, bool invalidate) {
    b._canvas = canvas;
    b._open   = false;
    b._inval  = invalidate;
    b._drew   = false;
    if (!canvas) return false;

    lv_img_dsc_t *dsc = lv_canvas_get_img(canvas);
    // Every lv_canvas_draw_* returns at once (with a warning) for these.
    if (dsc->header.cf >= LV_IMG_CF_INDEXED_1BIT && dsc->header.cf <= LV_IMG_CF_INDEXED_8BIT)
        return false;

    // init_fake_disp(canvas, &fake_disp, &driver, &clip_area), with the three
    // stack objects of the lv_canvas_draw_* living in the batch instead.
    b._clip.x1 = 0;
    b._clip.x2 = dsc->header.w - 1;
    b._clip.y1 = 0;
    b._clip.y2 = dsc->header.h - 1;

    lv_memset_00(&b._disp, sizeof(lv_disp_t));
    b._disp.driver = &b._drv;

    lv_disp_drv_init(b._disp.driver);
    b._disp.driver->hor_res = dsc->header.w;
    b._disp.driver->ver_res = dsc->header.h;

    lv_draw_ctx_t *draw_ctx = (lv_draw_ctx_t *)lv_mem_alloc(sizeof(lv_draw_sw_ctx_t));
    LV_ASSERT_MALLOC(draw_ctx);
    // init_fake_disp() returns here as well, but its caller then draws with
    // the NULL context; a batch stays closed instead.
    if (draw_ctx == NULL) return false;
    lv_draw_sw_init_ctx(&b._drv, draw_ctx);
    b._disp.driver->draw_ctx = draw_ctx;
    draw_ctx->clip_area = &b._clip;
    draw_ctx->buf_area  = &b._clip;
    draw_ctx->buf       = (void *)dsc->data;

    lv_disp_drv_use_generic_set_px_cb(b._disp.driver, dsc->header.cf);
    if (LV_COLOR_SCREEN_TRANSP && dsc->header.cf != LV_IMG_CF_TRUE_COLOR_ALPHA) {
        b._drv.screen_transp = 0;
    }

    // What every lv_canvas_draw_* does right after init_fake_disp(); undone
    // in cdLvEnd() instead of after each draw.
    b._refrOri = _lv_refr_get_disp_refreshing();
    _lv_refr_set_disp_refreshing(&b._disp);

    b._h    = (lv_coord_t)dsc->header.h;
    b._cf   = (uint8_t)dsc->header.cf;
    b._open = true;
    return true;
}

void cdLvEnd(CdLvBatch &b) {
    if (!b._open) return;
    b._open = false;
    _lv_refr_set_disp_refreshing(b._refrOri);
    // deinit_fake_disp(canvas, &fake_disp)
    lv_draw_sw_deinit_ctx(b._disp.driver, b._disp.driver->draw_ctx);
    lv_mem_free(b._disp.driver->draw_ctx);
    // The one invalidate for the whole run, where each lv_canvas_draw_* ends
    // with its own.
    if (b._inval && b._drew) lv_obj_invalidate(b._canvas);
}

// lv_canvas_draw_line() and _rect() switch anti-aliasing off for one draw
// whose colour is the chroma key of a chroma-keyed canvas. Their fake display
// dies with that draw; the batch's lives on, so the flag goes back afterwards.
static inline bool cdLvChromaKey(const CdLvBatch &b, lv_color_t c) {
    const lv_color_t ctransp = LV_COLOR_CHROMA_KEY;
    return b._cf == LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED && c.full == ctransp.full;
}

void cdLvLine(CdLvBatch &b, const lv_point_t pts[], uint32_t n,
              const lv_draw_line_dsc_t *dsc) {
    if (!b._open) return;
    b._drew = true;
    // n < 2 draws nothing in lv_canvas_draw_line() either - except n == 0,
    // where its uint32_t loop bound wraps and runs off the array.
    if (n < 2) return;
    const uint32_t aa = b._drv.antialiasing;
    if (cdLvChromaKey(b, dsc->color)) b._drv.antialiasing = 0;
    for (uint32_t i = 0; i < n - 1; i++)
        lv_draw_line(b._drv.draw_ctx, dsc, &pts[i], &pts[i + 1]);
    b._drv.antialiasing = aa;
}

void cdLvText(CdLvBatch &b, lv_coord_t x, lv_coord_t y, lv_coord_t maxW,
              const lv_draw_label_dsc_t *dsc, const char *txt) {
    if (!b._open) return;
    b._drew = true;
    lv_area_t coords;
    coords.x1 = x;
    coords.y1 = y;
    coords.x2 = x + maxW - 1;
    coords.y2 = b._h - 1;
    lv_draw_label(b._drv.draw_ctx, dsc, &coords, txt, NULL);
}

void cdLvRect(CdLvBatch &b, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
              const lv_draw_rect_dsc_t *dsc) {
    if (!b._open) return;
    b._drew = true;
    const uint32_t aa = b._drv.antialiasing;
    if (cdLvChromaKey(b, dsc->bg_color)) b._drv.antialiasing = 0;
    lv_area_t coords;
    coords.x1 = x;
    coords.y1 = y;
    coords.x2 = x + w - 1;
    coords.y2 = y + h - 1;
    lv_draw_rect(b._drv.draw_ctx, dsc, &coords);
    b._drv.antialiasing = aa;
}

void cdLvArc(CdLvBatch &b, lv_coord_t x, lv_coord_t y, lv_coord_t r,
             int32_t a0, int32_t a1, const lv_draw_arc_dsc_t *dsc) {
#if LV_DRAW_COMPLEX
    if (!b._open) return;
    b._drew = true;
    lv_point_t p = {x, y};
    lv_draw_arc(b._drv.draw_ctx, dsc, &p, r, a0, a1);
#else
    // lv_canvas_draw_arc() draws nothing (and warns) without LV_DRAW_COMPLEX.
    LV_UNUSED(b); LV_UNUSED(x); LV_UNUSED(y); LV_UNUSED(r);
    LV_UNUSED(a0); LV_UNUSED(a1); LV_UNUSED(dsc);
#endif
}
