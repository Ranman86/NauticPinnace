// WindScreen.cpp – B&G-style circular wind-angle instrument for ESP32-S3
// Ported from BngRenderer.cs (SailTrimMonitor / SkiaSharp)
// LVGL 8 + dark theme (CLR_BG = 0x0A0A0F)

#include "WindScreen.h"
#include "RenderYield.h"
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

// ── Compile-time constants ────────────────────────────────────────────────────
static constexpr float DEG2RAD = (float)M_PI / 180.f;

// Zone colours (dark-theme adaptation of BngRenderer palette)
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

// cline() - der alte Umweg ueber lv_canvas_draw_line() - ist am 2026-08-30
// ENTFALLEN. Alle 28 Aufrufe laufen jetzt ueber WindScreen::linePx(), das
// direkt in den Canvas-Puffer schreibt. Achtung beim Nachziehen aus aelterem
// Code: die Argumentreihenfolge unterscheidet sich, linePx() nimmt die
// Deckkraft VOR der Strichbreite.

// Filled triangle by horizontal scanlines
static void fillTri(lv_obj_t *cv,
                    float ax, float ay, float bx, float by,
                    float cx, float cy, lv_color_t col, lv_opa_t opa = LV_OPA_75) {
    if (ay > by) { float t; t=ax;ax=bx;bx=t; t=ay;ay=by;by=t; }
    if (ay > cy) { float t; t=ax;ax=cx;cx=t; t=ay;ay=cy;cy=t; }
    if (by > cy) { float t; t=bx;bx=cx;cx=t; t=by;by=cy;cy=t; }
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = col; d.width = 1; d.opa = opa;
    float hFull = cy - ay;
    int row = 0;
    for (float y = ay; y <= cy; y += 1.f) {
        float xAC = (hFull > 0.f) ? ax + (y-ay)/hFull*(cx-ax) : ax;
        float xABBC;
        if (y <= by) {
            float h = by - ay;
            xABBC = (h > 0.f) ? ax + (y-ay)/h*(bx-ax) : ax;
        } else {
            float h = cy - by;
            xABBC = (h > 0.f) ? bx + (y-by)/h*(cx-bx) : bx;
        }
        float xL = (xAC < xABBC) ? xAC : xABBC;
        float xR = (xAC > xABBC) ? xAC : xABBC;
        lv_point_t pts[2] = {{ (lv_coord_t)xL, (lv_coord_t)y },
                              { (lv_coord_t)xR, (lv_coord_t)y }};
        lv_canvas_draw_line(cv, pts, 2, &d);
        // Offer a yield every 20 rows; renderYield decides whether this one has
        // earned a tick (RenderYield.h). The 4-inch board still sleeps 1 ms here.
        if (++row % 20 == 0) renderYield();
    }
}

static void ctext(lv_obj_t *cv, float x, float y, float maxW,
                  const lv_font_t *font, lv_color_t col,
                  lv_text_align_t align, const char *txt) {
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = font; d.color = col; d.align = align;
    lv_canvas_draw_text(cv, (lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)maxW, &d, txt);
}

// ── Filled-ellipse helper ─────────────────────────────────────────────────────
void WindScreen::fillEllipse(float cx, float cy, float rx, float ry,
                              lv_color_t col, lv_opa_t opa) {
    // Direct span writes, for the reason spelled out on spanH().
    int row = 0;
    for (float dy2 = -ry; dy2 <= ry; dy2 += 1.f) {
        float ratio = dy2 / ry;
        float hx = rx * sqrtf(1.f - ratio * ratio);
        spanH((int)(cy + dy2), (int)(cx - hx), (int)(cx + hx), col, opa);
        if (++row % 20 == 0) renderYield();
    }
}

// ── Scanline polygon fill (even-odd) ──────────────────────────────────────────
// Replacement for lv_canvas_draw_polygon (which segfaults in the PC simulator's
// LVGL build). Fills a simple polygon with horizontal lv_canvas_draw_line spans.
// One horizontal run of pixels, written straight into the canvas buffer.
//
// WHY NOT lv_canvas_draw_line: because that is not a line drawing routine, it is
// a whole display. Every call runs init_fake_disp(), which zeroes an lv_disp_t
// AND an lv_disp_drv_t, then lv_mem_alloc()s an lv_draw_sw_ctx_t, initialises
// the software draw context, draws, tears the context down again with
// lv_mem_free(), and finally calls lv_obj_invalidate(canvas) - which walks the
// parent chain and merges an area into the display's invalid-area list. That is
// a malloc/free pair, two struct memsets and a full invalidate per span. The
// polygon filler below emits one span per scanline, so a single zone sector was
// paying that toll a hundred times over, for spans that are often 40 px wide.
// Measured on the 5B: the five zone/boat/overlay phases together were 198 ms of
// a 261 ms repaint.
//
// We own this buffer (_cbuf, LV_IMG_CF_TRUE_COLOR, stride CW), so a span is a
// pointer and a loop. drawInstrument() already ends with one
// lv_obj_invalidate(_canvas), which is all the invalidation that was ever
// needed - the per-span ones were pure waste.
void WindScreen::spanH(int y, int x0, int x1, lv_color_t col, lv_opa_t opa) {
    if (!_cbuf) return;
    if (y < 0 || y >= CH) return;
    if (x1 < x0) { const int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= CW) return;
    if (x0 < 0)      x0 = 0;
    if (x1 >= CW)    x1 = CW - 1;

    lv_color_t *p   = _cbuf + (size_t)y * (size_t)CW + (size_t)x0;
    lv_color_t *end = _cbuf + (size_t)y * (size_t)CW + (size_t)x1;

    if (opa >= LV_OPA_COVER) {
#if LV_COLOR_DEPTH == 16
        // Two pixels per store once aligned: RGB565 written through a 16-bit
        // pointer runs at roughly a third of what this PSRAM can absorb, and
        // both halves of the word are the same colour so the byte order in the
        // pair is irrelevant.
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

// Kantengeglaettete 1-px-Linie, direkt in _cbuf - das Gegenstueck zu spanH()
// fuer Striche statt Flaechen.
//
// WARUM: cline() ist lv_canvas_draw_line() und zahlt pro Aufruf denselben Zoll,
// den der Kommentar bei spanH() beschreibt (malloc/free, zwei memsets, ein
// voller lv_obj_invalidate). Am Geraet gemessen: die 13 Stroemungslinien in
// drawWindLines() brauchten so 50,9 ms - von 77 ms der ganzen BOAT-Phase, also
// rund 4 ms fuer EINEN Strich von 40 % Deckkraft.
//
// Verfahren ist Xiaolin Wu: pro Schritt entlang der laengeren Achse werden die
// beiden benachbarten Pixel der kuerzeren Achse anteilig eingefaerbt. Ohne das
// haetten die Linien eine harte Treppe - bei 40 % Deckkraft hinter dem Boot
// faellt so etwas sofort auf.
void WindScreen::linePx(float x0, float y0, float x1, float y1,
                        lv_color_t col, lv_opa_t opa, float w) {
    if (!_cbuf) return;

    auto pixel = [&](int x, int y, float f) {
        if (x < 0 || x >= CW || y < 0 || y >= CH) return;
        if (f <= 0.f) return;
        if (f > 1.f) f = 1.f;
        const lv_opa_t a = (lv_opa_t)((float)opa * f);
        if (a == 0) return;
        lv_color_t *p = _cbuf + (size_t)y * (size_t)CW + (size_t)x;
        *p = (a >= LV_OPA_COVER) ? col : lv_color_mix(col, *p, a);
    };

    // Breite Striche: Deckung aus dem ECHTEN Abstand jedes Pixels zur Strecke,
    // genau wie in ringAA().
    //
    // Der erste Versuch lief anders und war sichtbar schlechter: er ging in
    // 1-px-Schritten die Linie entlang und setzte je Schritt einen Streifen
    // quer dazu, wobei die Deckkraft aus dem GANZZAHLIGEN Versatz kam und die
    // Zielpixel gerundet wurden. Damit gab es quer zur Linie ueberhaupt keine
    // Glaettung - nur Stufen, dazu Doppelschreiber an den Rundungsgrenzen.
    // Der Nutzer hat es sofort als "pixelig" erkannt. Merke: die Deckkraft muss
    // aus dem tatsaechlichen Abstand des Pixelmittelpunkts kommen, nie aus dem
    // Schleifenindex.
    if (w > 1.5f) {
        const float dxl = x1 - x0, dyl = y1 - y0;
        const float len2 = dxl * dxl + dyl * dyl;
        if (len2 < 1e-6f) return;
        const float len = sqrtf(len2);
        const float h   = w * 0.5f;
        const float r   = h + 1.f;            // Einflussradius inkl. weicher Kante

        int ya = (int)floorf(fminf(y0, y1) - r);
        int yb = (int)ceilf (fmaxf(y0, y1) + r);
        if (ya < 0) ya = 0;
        if (yb > CH - 1) yb = CH - 1;
        const float xlo = fminf(x0, x1) - r, xhi = fmaxf(x0, x1) + r;

        // Geradengleichung A*x + B*y + C = 0 mit |A,B| = len, damit
        // |A*x + B*y + C| unmittelbar der Abstand mal len ist.
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
            if (ix1 < 0 || ix0 > CW - 1 || ix0 > ix1) continue;
            if (ix0 < 0) ix0 = 0;
            if (ix1 > CW - 1) ix1 = CW - 1;

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

// Kantengeglaetteter Kreisring - siehe Begruendung im Header.
//
// Beruehrt wird nur das Band selbst: je Zeile werden die ein bis zwei
// x-Bereiche berechnet, in denen |Abstand zum Mittelpunkt - r| <= h liegt.
// Innerhalb davon ergibt der Abstand unmittelbar die Deckkraft, so dass die
// Kanten weich auslaufen statt zu treppen - bei einem 2 px breiten Ring auf
// einem 600 px grossen Kreis ist genau das der sichtbare Unterschied.
void WindScreen::ringAA(float cx, float cy, float r, float w,
                        lv_color_t col, lv_opa_t opa) {
    if (!_cbuf || r <= 0.f || w <= 0.f) return;
    const float h    = w * 0.5f;
    const float rOut = r + h, rIn = r - h;
    const int   y0   = (int)floorf(cy - rOut - 1.f);
    const int   y1   = (int)ceilf (cy + rOut + 1.f);

    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= CH) continue;
        const float dy = (float)y + 0.5f - cy;
        const float ay = fabsf(dy);
        if (ay > rOut + 1.f) continue;

        // Aeusserer Rand der Zeile; innen faellt das Band in zwei Haelften,
        // sobald die Zeile das Loch schneidet.
        const float xo = sqrtf(fmaxf(0.f, (rOut + 1.f) * (rOut + 1.f) - dy * dy));
        const bool  gespalten = ay < (rIn - 1.f);
        const float xi = gespalten
                       ? sqrtf(fmaxf(0.f, (rIn - 1.f) * (rIn - 1.f) - dy * dy))
                       : 0.f;

        auto bogen = [&](int xa, int xb) {
            if (xb < 0 || xa >= CW) return;
            if (xa < 0) xa = 0;
            if (xb >= CW) xb = CW - 1;
            lv_color_t *row = _cbuf + (size_t)y * (size_t)CW;
            for (int x = xa; x <= xb; x++) {
                const float dx = (float)x + 0.5f - cx;
                const float d  = sqrtf(dx * dx + dy * dy);
                float f = h + 0.5f - fabsf(d - r);   // 1 in der Mitte, 0 aussen
                if (f <= 0.f) continue;
                if (f > 1.f) f = 1.f;
                const lv_opa_t a = (lv_opa_t)((float)opa * f);
                if (a == 0) continue;
                lv_color_t *p = row + x;
                *p = (a >= LV_OPA_COVER) ? col : lv_color_mix(col, *p, a);
            }
        };

        if (gespalten) {
            bogen((int)floorf(cx - xo), (int)ceilf(cx - xi));
            bogen((int)floorf(cx + xi), (int)ceilf(cx + xo));
        } else {
            bogen((int)floorf(cx - xo), (int)ceilf(cx + xo));
        }
    }
}

// Gefuellte Scheibe mit weicher Kante - siehe Begruendung im Header.
void WindScreen::discAA(float cx, float cy, float r, lv_color_t col, lv_opa_t opa) {
    if (!_cbuf || r <= 0.f) return;
    int ya = (int)floorf(cy - r - 1.f);
    int yb = (int)ceilf (cy + r + 1.f);
    if (ya < 0) ya = 0;
    if (yb > CH - 1) yb = CH - 1;

    for (int y = ya; y <= yb; y++) {
        const float dy   = (float)y + 0.5f - cy;
        const float halb = sqrtf(fmaxf(0.f, (r + 1.f) * (r + 1.f) - dy * dy));
        int xa = (int)floorf(cx - halb), xb = (int)ceilf(cx + halb);
        if (xb < 0 || xa > CW - 1) continue;
        if (xa < 0) xa = 0;
        if (xb > CW - 1) xb = CW - 1;
        lv_color_t *row = _cbuf + (size_t)y * (size_t)CW;
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
void WindScreen::fillAnnulus(float cx, float cy, float rInner, float rOuter,
                             lv_color_t col, lv_opa_t opa) {
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
            spanH(y, (int)(cx - ho), (int)(cx - hi), col, opa);
            spanH(y, (int)(cx + hi), (int)(cx + ho), col, opa);
        } else {
            spanH(y, (int)(cx - ho), (int)(cx + ho), col, opa);
        }
        if (++row % 40 == 0) renderYield();
    }
}

void WindScreen::fillPolygon(const lv_point_t *pts, int n, lv_color_t col, lv_opa_t opa) {
    if (n < 3) return;
    int ymin = pts[0].y, ymax = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].y < ymin) ymin = pts[i].y;
        if (pts[i].y > ymax) ymax = pts[i].y;
    }
    if (ymin < 0)   ymin = 0;
    if (ymax >= CH) ymax = CH - 1;
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
            spanH(y, (int)(xs[i] + 0.5f), (int)(xs[i + 1] + 0.5f), col, opa);
        }
        if (++row % 20 == 0) renderYield();
    }
}

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

// ── computeSailState ──────────────────────────────────────────────────────────
WindScreen::SailState WindScreen::computeSailState(float absTwa, float twa,
                                                    float awa, float tws, float stw) const {
    SailState s;
    s.leewardSide   = (twa >= 0.f) ? -1.f : 1.f;   // wind from stbd → lee = port
    s.luffing       = (absTwa < 28.f);             // head-to-wind: sails flap
    s.running       = (absTwa > 150.f);

    // ── Optimal trim = ease each sail to a constant angle of attack to the
    //    APPARENT wind. CONTINUOUS in AWA (no buckets → no jumping). The boom
    //    swings from ~4° (close-hauled, sheeted hard) to ~86° (running). ──────
    float absAwa = isnan(awa) ? 0.f : fabsf(awa);
    float boom = absAwa - 13.f;                 // ~13° angle of attack for the main
    if (boom <  4.f) boom =  4.f;
    if (boom > 86.f) boom = 86.f;
    s.boomAngleDeg     = boom;
    s.headsailAngleDeg = boom * 0.82f;          // jib leads inboard of the main
    s.flutterDeg       = 0.f;                   // (luffing flap handled in drawBoat)

    // ── Sail inventory ────────────────────────────────────────
    bool hasTws = !isnan(tws);
    s.useSpinnaker = appConfig.cfg.allowSpinnaker && !s.luffing && hasTws &&
                     ((absTwa > 100.f && absTwa < 150.f && tws < 25.f) ||
                      (absTwa >= 150.f && tws < 20.f));
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
    s.mainCamber = camberForTwa(absTwa);
    s.jibCamber  = s.mainCamber * 0.85f;   // headsails trim a touch flatter

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

void WindScreen::drawInstrument(float twa, float awa, float tws, float stw, float hdg) {
    if (!_canvas || !_cbuf) return;
    // Phase stopwatch. Each WS_PHASE_END() closes one phase and opens the next,
    // so the five phases tile the whole function with no gaps and no overlap;
    // on the 4" board every one of them expands to nothing. See the block at
    // the top of this file.
    WS_PHASE_BEGIN();

    // ── Chunked background fill ───────────────────────────────────────────────
    // The fill is broken into 8-row chunks so there is a place to yield at all:
    // lv_canvas_fill_bg() would write the whole canvas (480×430×2 = 825 KB on the
    // 4-inch grid, more here) in one uninterruptible loop.
    //
    // On the 4-inch board every chunk sleeps a tick, for the SPI0 reason spelled
    // out in RenderYield.h. On these boards that reason does not apply and the
    // sleeps were the single largest item in the frame: 600/8 = 75 chunks, one
    // tick each, ~75 ms of doing nothing. renderYield() keeps a yield here but
    // takes it at most every 20 ms, so the fill now costs about what the pixel
    // writes cost (~0.22 ms per chunk at full PSRAM speed) plus 2-3 ticks.
    {
        // Two pixels per store: a 16-bit store loop measured 18 MB/s here
        // against the ~49 MB/s this PSRAM answers a word-wide fill with.
        // NUR AUSSERHALB DER INSTRUMENTENSCHEIBE loeschen.
        //
        // drawBezel() fuellt unmittelbar danach die Scheibe bis R_OUTER+1 -
        // alles, was hier innerhalb davon geschrieben wird, ist bereits eine
        // Bilddauer spaeter wieder weg. Gerechnet: die Scheibe deckt
        // pi*253,5^2 = 201.850 der 360.000 Pixel ab, also 56 % der Leinwand.
        // Genau dieser Anteil war jede Bilddauer umsonst geschriebener
        // Speicher - und Speicherbandbreite ist auf diesem Board der Engpass,
        // nicht Rechenzeit.
        //
        // Die Ecken werden weiterhin voll geloescht; dort liegen die
        // Eckwerte, die Kursanzeige und der Polarbalken, und dort darf nichts
        // stehenbleiben.
        const lv_color_t bgColor = CLR_BG;
        const int        CHUNK   = 8;   // rows per yield window
        const float      rKeep   = R_OUTER + 1.f;   // exakt was drawBezel fuellt
        for (int row = 0; row < CH; row += CHUNK) {
            const int rows = (CH - row < CHUNK) ? CH - row : CHUNK;
            for (int y = row; y < row + rows; y++) {
                const float dy = (float)y + 0.5f - CY;
                if (fabsf(dy) >= rKeep) {           // Zeile beruehrt die Scheibe nicht
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

    // Normalise TWA to –180 … +180 (negative = port, positive = starboard)
    float nTwa = isnan(twa) ? 0.f : twa;
    while (nTwa >  180.f) nTwa -= 360.f;
    while (nTwa < -180.f) nTwa += 360.f;
    float absTwa = fabsf(nTwa);

    float nAwa = isnan(awa) ? 0.f : awa;

    float rudder, roll, pitch, rot, waveH, waveT;
    {
        auto lk = data.lock();
        rudder = data.rudderAngle;
        roll   = data.roll;  pitch = data.pitch;  rot = data.rateOfTurn;
        waveH  = data.waveHeight;  waveT = data.wavePeriod;
    }

    // Sail state computed once, shared by all boat sub-layers
    SailState sail = computeSailState(absTwa, nTwa, nAwa, tws, stw);

    // ── Draw order: back → front ──────────────────────────────────────────────
    // A yield offered between the heavy layers, for the same reason the fill
    // loops offer one. On the 4-inch board each of these is a 1 ms sleep; here
    // renderYield() collapses them into whatever the 20 ms budget allows, which
    // for a whole instrument repaint is one or two of the six.
    // Aufgeschluesselt gemessen (7B, 2026-08-30): bezel 23,1 ms, zoneRing
    // 36,0 ms, die beiden renderYield() zusammen 1,5 ms. Die Pausen sind also
    // vernachlaessigbar - was diese Phase kostet, ist echte Zeichenarbeit.
    drawBezel();
    renderYield();
    drawZoneRing(absTwa);
    renderYield();
    WS_PHASE_END(WS_PH_ZONE);   // bezel + the coloured zone sectors
    drawTicksAndLabels();
    renderYield();
    drawInnerBg();
    WS_PHASE_END(WS_PH_TICK);   // degree ticks/labels + inner circle
    if (_centerMode == CenterMode::ATTITUDE) {
        drawCompassRose(hdg);       // keep the heading-up compass card
        renderYield();
        drawCenter_Attitude(roll, pitch, rot);   // artificial horizon replaces the boat
    } else {
        drawWindLines(appConfig.cfg.windLinesApparent ? nAwa : nTwa);   // flow lines: apparent or true
        drawCompassRose(hdg);       // heading-up compass card inside the inner circle
        renderYield();
        drawBoat(sail);
    }
    renderYield();
    // Centre of the instrument: flow lines + compass rose + boat with sails, or
    // the artificial horizon in ATTITUDE mode.
    WS_PHASE_END(WS_PH_BOAT);

    // The two wind history curves, on top of the boat but under the pointers
    // and the value boxes: they are context, not something to read a number
    // off, and must never hide the live indicators.
    drawWindTrace(nTwa, nAwa);
    WS_PHASE_END(WS_PH_TRACE);
    drawInnerBorder();
    drawOuterBorder();
    if (!isnan(tws)) drawVmgMarkers(tws, absTwa);
    if (!isnan(twa)) drawTwaPointer(nTwa);
    if (!isnan(awa)) drawAwaPointer(nAwa);   // pointers on TOP of the VMG markers (no overpaint)
    if (_centerMode == CenterMode::ATTITUDE) {
        drawAttitudeKpis(roll, pitch, waveH, waveT);
    } else {
        drawCornerKpis(stw, nTwa, nAwa, tws);
    }
    drawHeadingBelow(hdg);
    if (_centerMode != CenterMode::ATTITUDE) {
        drawTrimAdvice(absTwa, nTwa, tws);
        drawReefBadge(sail.reefCount);
    }
    // Aufgeschluesselt gemessen (7B, 2026-08-30, vor der ringAA-Umstellung):
    // Raender 25,2 ms, Zeiger 9,8, KPIs 12,1, Ruderbogen 4,9. Die beiden
    // Randkreise waren also die Haelfte dieser Phase - deshalb ringAA().
    drawRudderArc(rudder);          // curved rudder-angle gauge below the rose, above the polar bar

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
        lv_canvas_draw_rect(_canvas, (lv_coord_t)bx0, (lv_coord_t)by,
                            (lv_coord_t)bw, (lv_coord_t)bh, &rd);

        // Filled portion (0..perf..120 %)
        float polar = windPolarSpeed(absTwa, tws);
        char pb[16];
        if (!isnan(stw) && !isnan(polar) && polar > 0.1f) {
            float perf = stw / polar;
            if (perf > 1.2f) perf = 1.2f;
            float fillW = perf / 1.2f * bw;
            lv_color_t barCol = (perf >= 0.95f) ? CLR_GREEN :
                                 (perf >= 0.75f) ? CLR_ORANGE : CLR_RED;
            rd.bg_color = barCol; rd.bg_opa = 230; rd.radius = UI_S(4);
            lv_canvas_draw_rect(_canvas, (lv_coord_t)bx0, (lv_coord_t)by,
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
        lv_canvas_draw_line(_canvas, pl, 2, &ld);

        // Label INSIDE the bar (single line, white = high contrast over fill + track)
        lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);
        td.font = FONT_SMALL; td.color = CLR_ON_ACCENT; td.opa = OPA_FULL;
        td.align = LV_TEXT_ALIGN_CENTER;
        lv_canvas_draw_text(_canvas, (lv_coord_t)bx0, (lv_coord_t)(by + 2.f * UI_SF), (lv_coord_t)bw, &td, pb);
    }
    // Everything stacked on top: borders, VMG markers, both pointers, corner
    // KPIs, heading box, trim advice, reef badge, rudder arc and polar bar.
    WS_PHASE_END(WS_PH_OVL);

    lv_obj_invalidate(_canvas);
}

// ── Layer 1: Bezel background ─────────────────────────────────────────────────

void WindScreen::drawBezel() {
    // A radius-CIRCLE lv_canvas_draw_rect() this size is an LVGL radius mask
    // evaluated over the full 600x600 bounding box; fillEllipse() writes the
    // same disc as spans. Same reasoning as spanH().
    //
    // RING statt Scheibe: die Mitte bis R_INNER faerbt drawInnerBg() gleich
    // danach ohnehin um. Das waren pi*185^2 = 107.500 von 201.850 Pixeln, also
    // gut die Haelfte dieser Fuellung, jede Bilddauer umsonst. Ein Pixel
    // Ueberlappung nach innen, damit an der Naht nichts durchblitzt.
    fillAnnulus(CX, CY, R_INNER - 1.f, R_OUTER + 1.f, C_BEZEL, LV_OPA_COVER);
}

// ── Layer 2: Coloured zone arcs ───────────────────────────────────────────────

void WindScreen::drawZoneRing(float absTwa) {
    // Base ring (dark)
    //
    // Die sechs Zonenfarben unten decken zusammen exakt 360 Grad ab
    // (2ng + 2(45-ng) + 2*20 + 2*35 + 2*50 + 60), dieser Grundring wird also
    // vollstaendig uebermalt. Gemessen kostet er 9,3 ms von 38 ms der Phase.
    // Er bleibt trotzdem: er ist die Versicherung gegen Haarlinien an den
    // Sektorgrenzen, wo die Polygon-Naeherung Luecken lassen kann. Auf einem
    // Instrument sind 9 ms dafuer gut angelegt - wer sie sparen will, muss
    // vorher am echten Panel pruefen, ob die Naht sichtbar wird.
    arcRing(R_ZONE_I, R_ZONE_O, -180.f, 360.f, C_BASE, LV_OPA_COVER);

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

    // Highlight current zone with a bright outer stroke
    PoS pos = pointOfSail(absTwa);
    lv_color_t hi = CLR_TEXT;
    float hiW = 4.f;
    switch (pos) {
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

    // Fine 5° subdivision ticks across the coloured zone band.
    lv_draw_line_dsc_t sd; lv_draw_line_dsc_init(&sd);
    sd.color = lv_color_hex(0x000000); sd.width = 1; sd.opa = LV_OPA_30;
    for (int w = 0; w < 360; w += 5) {
        lv_point_t p1 = wp(CX, CY, R_ZONE_I, (float)w);
        lv_point_t p2 = wp(CX, CY, R_ZONE_O, (float)w);
        lv_point_t pp[2] = { p1, p2 };
        lv_canvas_draw_line(_canvas, pp, 2, &sd);
    }
}

// ── Layer 3: Degree ticks + labels ───────────────────────────────────────────

void WindScreen::drawTicksAndLabels() {
    for (int w = 0; w < 360; w += 10) {
        bool major = (w % 30) == 0;
        float iR   = major ? R_TICK_MAJ : R_TICK_MIN;
        lv_point_t p1 = wp(CX, CY, iR,      (float)w);
        lv_point_t p2 = wp(CX, CY, R_TICK_O, (float)w);
        lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
        ld.color = major ? C_TICK_MJ : C_TICK_MN;
        ld.width = major ? 2 : 1;
        ld.opa   = LV_OPA_COVER;
        { lv_point_t _dl[2]={p1,p2}; lv_canvas_draw_line(_canvas,_dl,2,&ld); }

        if (major && (w < 150 || w > 210)) {   // hide 150/180/210 — rudder gauge sits there
            char lbl[4];
            snprintf(lbl, sizeof(lbl), "%03d", w);
            float a  = w * DEG2RAD;
            float lx = CX + R_LABEL * sinf(a);
            float ly = CY - R_LABEL * cosf(a);
            // Centre text: use a 26-design-px wide box centred on label point
            ctext(_canvas, lx - 13.f * UI_SF, ly - 6.f * UI_SF, 26.f * UI_SF,
                  FONT_TINY, C_TICK_MJ, LV_TEXT_ALIGN_CENTER, lbl);
        }
    }
}

// ── Layer 4: Inner circle background ─────────────────────────────────────────

void WindScreen::drawInnerBg() {
    fillEllipse(CX, CY, R_INNER, R_INNER, C_INNER, LV_OPA_COVER);   // see drawBezel()
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
//     keel → hull fill → hull outline → forestay → spinnaker or headsail
//     → mast → boom → mainsail → traveller → rudder → deck hardware → sheet lines

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
    if (!_trimInit) { _smBoomDeg = tgtBoom; _smJibDeg = tgtJib; _trimInit = true; }
    else {
        const float k = 0.18f;                       // ~0.5 s @ 10 Hz → smooth, no jumps
        _smBoomDeg += (tgtBoom - _smBoomDeg) * k;
        _smJibDeg  += (tgtJib  - _smJibDeg ) * k;
    }

    // ── Hull ───────────────────────────────────────────────────────────────
    // ── Hull — smooth deck-plan outline (mirrored half-beam stations) ──────
    drawKeel();
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
        ctext(_canvas, CX - 36.f * UI_SF, CY - 35.f * BS, 72.f * UI_SF,
              FONT_TINY, CLR_ACCENT, LV_TEXT_ALIGN_CENTER, "Butterfly");
    } else if (sail.useSpinnaker) {
        drawSpinnaker(sail);
    } else if (sail.useCodeZero) {
        drawCodeZero(sail);
    } else if (!sail.running) {
        drawSailFoil(bowX, bowY, clewJX, clewJY, jss, jCam, sailCol, sailOpa, leechCol, 2);
        linePx(clewJX, clewJY, CX + jss * 9.f * BS, CY + 4.f * BS, CLR_GREEN, LV_OPA_50, 1); // jib sheet
    }

    // ── MAINSAIL at the MAST (centre) ──────────────────────────────────────
    drawSailFoil(mastX, mastY, clewMX, clewMY, bss, mCam, sailCol, sailOpa, leechCol, 2);
    linePx(mastX, mastY, clewMX, clewMY, cRig, LV_OPA_COVER, 2);   // boom spar (on top)

    // ── Mast: a single point at the ship's centre (NOT a fore-aft line) ────
    { lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
      rd.bg_color = cRig; rd.bg_opa = LV_OPA_COVER; rd.radius = LV_RADIUS_CIRCLE; rd.border_width = 0;
      lv_canvas_draw_rect(_canvas, (lv_coord_t)(mastX - UI_S(3)), (lv_coord_t)(mastY - UI_S(3)), UI_S(6), UI_S(6), &rd); }

    // ── Rudder ─────────────────────────────────────────────────────────────
    {
        auto lk = data.lock();
        const float ra = data.rudderAngle;
        drawRudder(isnan(ra) ? 0.f : ra);
    }
}

// Ruderblatt am Heck: nur Blatt und Drehpunkt, keine Gradzahl - die stand
// doppelt im Bild, den Wert zeigt der Bogen unten (drawRudderArc). Das Blatt
// bleibt als schnell erfassbare Richtungsanzeige und ist gegenueber der ersten
// Fassung groesser (Halblaenge 4 -> 6, Strich 2 -> 3).
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

// ── Keel (dark oval behind hull) ─────────────────────────────────────────────
void WindScreen::drawKeel() {
    const float BS = BOAT_S;
    fillEllipse(CX, CY + 3.3f * BS, 1.8f * BS, 4.3f * BS, (uiTheme.windMast), LV_OPA_COVER);
    linePx(CX, CY - 1.f * BS, CX, CY + 7.f * BS, (uiTheme.windMast), LV_OPA_COVER, 3);
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

// ── Spinnaker ─────────────────────────────────────────────────────────────────
//   BoatPainter: cx2=40*leeSign, cy2=-50, rx=56, ry=46, colour (215,75,35,210)
//   Hals (tack):  (0, BowDotY=-88) → (cx2-52*leeSign, cy2)
//   Schot (sheet):(0, 22)          → (cx2+46*leeSign, cy2+40)
void WindScreen::drawSpinnaker(const SailState &sail) {
    const float BS  = BOAT_S;
    const float ss  = sail.leewardSide;
    const float ocx = CX + ss * 13.3f * BS;
    const float ocy = CY - 16.6f * BS;

    lv_color_t col = CLR_ORANGE;
    fillEllipse(ocx, ocy, 18.6f * BS, 15.3f * BS, col, 210);
    for (float a = 0.f; a < 360.f; a += 6.f) {
        float a1 = a * DEG2RAD, a2 = (a + 6.f) * DEG2RAD;
        linePx(ocx + 18.6f * BS * sinf(a1), ocy - 15.3f * BS * cosf(a1), ocx + 18.6f * BS * sinf(a2), ocy - 15.3f * BS * cosf(a2), CLR_ORANGE, 200, 1);
    }

    float halsX = CX - ss * 4.0f * BS;
    float halsY = ocy;
    linePx(CX, CY - 29.f * BS, halsX, halsY, (uiTheme.windRigging), LV_OPA_70, 1);

    float schotX = CX + ss * 28.6f * BS;
    float schotY = CY - 3.3f * BS;
    linePx(CX, CY + 7.3f * BS, schotX, schotY, CLR_GREEN, LV_OPA_60, 1);
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
    ctext(_canvas, lbx - 8.f * UI_SF, lby - 5.f * UI_SF, 16.f * UI_SF, FONT_TINY, gold, LV_TEXT_ALIGN_CENTER, "C0");

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
    ctext(_canvas, lx - 7.f * UI_SF, ly - 8.f * UI_SF, 14.f * UI_SF,
          FONT_SMALL, CLR_ON_ACCENT, LV_TEXT_ALIGN_CENTER, "T");
}

// ── Layer 10: AWA pointer (red, inside zone ring, tip at inner circle edge) ──

void WindScreen::drawAwaPointer(float awaDeg) {
    triPointer(awaDeg, R_INNER + 1.f, R_ZONE_O - 2.f, 12.f * UI_SF, C_AWA_PTR);  // same size as TWA, inner band
    float a  = awaDeg * DEG2RAD;
    float lx = CX + (R_ZONE_O - 12.f * UI_SF) * sinf(a);   // on the pointer body (centroid), not at the tip
    float ly = CY - (R_ZONE_O - 12.f * UI_SF) * cosf(a);
    ctext(_canvas, lx - 7.f * UI_SF, ly - 7.f * UI_SF, 14.f * UI_SF,
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
void WindScreen::drawCenter_Attitude(float roll, float pitch, float rot) {
    const float R   = 110.f * UI_SF;     // horizon-disc radius (clear of the compass rose)
    const float PPD = 3.4f * UI_SF;      // screen px per degree of pitch
    if (isnan(roll))  roll  = 0.f;
    if (isnan(pitch)) pitch = 0.f;
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
    lv_draw_line_dsc_t ds; lv_draw_line_dsc_init(&ds); ds.width = 1; ds.opa = LV_OPA_COVER;
    int rowc = 0;
    for (float y = CY - R; y <= CY + R; y += 1.f) {
        float dyc = y - CY;
        float h2  = R * R - dyc * dyc;
        if (h2 <= 0.f) continue;
        float hx  = sqrtf(h2);
        float xL  = CX - hx, xR = CX + hx;
        // ground where  d(x,y) = (y-horY)*cphi - (x-CX)*sphi  > 0  (sky otherwise)
        if (fabsf(sphi) < 1e-4f) {
            bool ground = ((y - horY) * cphi) > 0.f;
            ds.color = ground ? GROUND : SKY;
            lv_point_t p[2] = {{(lv_coord_t)xL,(lv_coord_t)y},{(lv_coord_t)xR,(lv_coord_t)y}};
            lv_canvas_draw_line(_canvas, p, 2, &ds);
        } else {
            float xs = CX + ((y - horY) * cphi) / sphi;   // x where d == 0
            if (xs < xL) xs = xL; else if (xs > xR) xs = xR;
            // Colour each segment by the sign of d at its midpoint (robust even
            // when the true split lies outside the row and xs was clamped).
            if (xs > xL + 0.5f) {
                float mid = 0.5f * (xL + xs);
                ds.color = ((y - horY) * cphi - (mid - CX) * sphi > 0.f) ? GROUND : SKY;
                lv_point_t p[2]={{(lv_coord_t)xL,(lv_coord_t)y},{(lv_coord_t)xs,(lv_coord_t)y}};
                lv_canvas_draw_line(_canvas, p, 2, &ds);
            }
            if (xR > xs + 0.5f) {
                float mid = 0.5f * (xs + xR);
                ds.color = ((y - horY) * cphi - (mid - CX) * sphi > 0.f) ? GROUND : SKY;
                lv_point_t p[2]={{(lv_coord_t)xs,(lv_coord_t)y},{(lv_coord_t)xR,(lv_coord_t)y}};
                lv_canvas_draw_line(_canvas, p, 2, &ds);
            }
        }
        if (++rowc % 20 == 0) renderYield();
    }

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

    // ── Pitch ladder (rungs at ±10/±20°, rotated by roll, clipped) ───────────
    const float ux = sphi, uy = -cphi;            // unit "up" (sky) normal
    const int   pv[4]    = { 10, 20, -10, -20 };
    const float halfL[4] = { 22.f * UI_SF, 15.f * UI_SF, 22.f * UI_SF, 15.f * UI_SF };
    char lbl[4];
    for (int i = 0; i < 4; i++) {
        float off = pv[i] * PPD;
        float mx = CX + off * ux, my = horY + off * uy;        // rung centre
        if ((mx-CX)*(mx-CX)+(my-CY)*(my-CY) > (R-6.f*UI_SF)*(R-6.f*UI_SF)) continue;
        float hl = halfL[i];
        float ax = mx - hl*cphi, ay = my - hl*sphi;
        float bx = mx + hl*cphi, by = my + hl*sphi;
        linePx(ax, ay, bx, by, LINEC, LV_OPA_80, 1);
        snprintf(lbl, sizeof(lbl), "%d", pv[i] < 0 ? -pv[i] : pv[i]);
        ctext(_canvas, bx + 2.f * UI_SF, by - 6.f * UI_SF, 18.f * UI_SF, FONT_TINY, LINEC, LV_TEXT_ALIGN_LEFT, lbl);
    }

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
    {
        float a = fmaxf(-60.f, fminf(60.f, roll));
        lv_point_t tip = wp(CX, CY, rB - 12.f * UI_SF, a);
        lv_point_t bl  = wp(CX, CY, rB - 22.f * UI_SF, a - 4.f);
        lv_point_t br  = wp(CX, CY, rB - 22.f * UI_SF, a + 4.f);
        fillTri(_canvas, tip.x, tip.y, bl.x, bl.y, br.x, br.y, CLR_YELLOW, LV_OPA_COVER);
    }

    // ── Fixed aircraft reference (does NOT rotate) ───────────────────────────
    linePx(CX - 38.f * UI_SF, CY, CX - 14.f * UI_SF, CY, CLR_YELLOW, LV_OPA_COVER, 3);
    linePx(CX - 14.f * UI_SF, CY, CX - 14.f * UI_SF, CY + 7.f * UI_SF, CLR_YELLOW, LV_OPA_COVER, 3);
    linePx(CX + 38.f * UI_SF, CY, CX + 14.f * UI_SF, CY, CLR_YELLOW, LV_OPA_COVER, 3);
    linePx(CX + 14.f * UI_SF, CY, CX + 14.f * UI_SF, CY + 7.f * UI_SF, CLR_YELLOW, LV_OPA_COVER, 3);
    { lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
      rd.bg_color = CLR_YELLOW; rd.bg_opa = LV_OPA_COVER; rd.radius = UI_S(2); rd.border_width = 0;
      lv_canvas_draw_rect(_canvas, (lv_coord_t)(CX-UI_S(3)), (lv_coord_t)(CY-UI_S(3)), UI_S(6), UI_S(6), &rd); }

    // ── Rate-of-turn strip (below the aircraft symbol) ───────────────────────
    {
        const float yb = CY + 62.f * UI_SF, FS = 60.f, halfW = 46.f * UI_SF;
        linePx(CX - halfW, yb, CX + halfW, yb, LINEC, LV_OPA_70, 1);
        linePx(CX, yb - 5.f * UI_SF, CX, yb + 5.f * UI_SF, LINEC, LV_OPA_70, 1);   // centre mark
        float r  = isnan(rot) ? 0.f : fmaxf(-FS, fminf(FS, rot));
        float xr = CX + (r / FS) * halfW;
        lv_color_t rc = (fabsf(rot) > 30.f) ? CLR_ORANGE : CLR_GREEN;
        fillTri(_canvas, xr, yb - 8.f * UI_SF, xr - 5.f * UI_SF, yb, xr + 5.f * UI_SF, yb, rc, LV_OPA_COVER);
        char rb[20];
        if (isnan(rot)) snprintf(rb, sizeof(rb), "ROT --");
        else snprintf(rb, sizeof(rb), "ROT %+d\xc2\xb0/min", (int)(rot + (rot>=0?0.5f:-0.5f)));
        ctext(_canvas, CX - 60.f * UI_SF, yb + 6.f * UI_SF, 120.f * UI_SF, FONT_TINY, CLR_TEXT, LV_TEXT_ALIGN_CENTER, rb);
    }
}

// Corner KPIs for the ATTITUDE screen: Roll / Pitch / wave height / wave period.
void WindScreen::drawAttitudeKpis(float roll, float pitch, float waveH, float waveT) {
    char buf[20];
    const float TY = 4.f * UI_SF, TV = 22.f * UI_SF, BY = 430.f * UI_SF, BV = 446.f * UI_SF;
    const float LX = 6.f * UI_SF, RX = 474.f * UI_SF, TW = 152.f * UI_SF,
                BWL = 120.f * UI_SF, BWR = 132.f * UI_SF;

    // Top-left: Roll
    ctext(_canvas, LX, TY, TW, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, T(STR_WIND_KPI_ROLL));
    if (isnan(roll)) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d\xc2\xb0 %s", (int)(fabsf(roll)+0.5f),
                  roll >= 0.f ? T(STR_WIND_STB) : T(STR_WIND_PT));
    ctext(_canvas, LX, TV, TW, FONT_XL,
          isnan(roll) ? CLR_TEXT : (roll >= 0.f ? CLR_STARBOARD : CLR_PORT),
          LV_TEXT_ALIGN_LEFT, buf);

    // Top-right: Pitch (bow up / stern up)
    ctext(_canvas, RX - TW, TY, TW, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, T(STR_WIND_KPI_PITCH));
    if (isnan(pitch)) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d\xc2\xb0 %s", (int)(fabsf(pitch)+0.5f),
                  pitch >= 0.f ? T(STR_WIND_BOW) : T(STR_WIND_STERN));
    ctext(_canvas, RX - TW, TV, TW, FONT_XL, CLR_TEXT, LV_TEXT_ALIGN_RIGHT, buf);

    // Bottom-left: estimated wave height
    ctext(_canvas, LX, BY, BWL, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, T(STR_WIND_KPI_WAVE));
    if (isnan(waveH)) snprintf(buf, sizeof(buf), "--");
    else              snprintf(buf, sizeof(buf), "%.1f m", waveH);
    ctext(_canvas, LX, BV, BWL, FONT_XL, CLR_WIND, LV_TEXT_ALIGN_LEFT, buf);

    // Bottom-right: estimated wave period
    ctext(_canvas, RX - BWR, BY, BWR, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, T(STR_WIND_KPI_PERIOD));
    if (isnan(waveT)) snprintf(buf, sizeof(buf), "--");
    else              snprintf(buf, sizeof(buf), "%.1f s", waveT);
    ctext(_canvas, RX - BWR, BV, BWR, FONT_XL, CLR_TEXT, LV_TEXT_ALIGN_RIGHT, buf);
}

// ── Layer 13: Corner KPIs ─────────────────────────────────────────────────────
//
//   Top-left : STW        Top-right : TWA
//   Bot-left : AWA        Bot-right : TWS

// ── "Steuerkurs" line (directly from DrawHeadingBelow in BngRenderer) ────────

void WindScreen::drawHeadingBelow(float hdg) {
    // Lubber heading readout at the very top of the compass card (always "up").
    char buf[8];
    if (isnan(hdg)) snprintf(buf, sizeof(buf), "---");
    else            snprintf(buf, sizeof(buf), "%03d", (int)(hdg + 0.5f) % 360);
    const float w = 48.f * UI_SF, h = 21.f * UI_SF;
    const float bx = CX - w * 0.5f, by = CY - R_INNER + 1.f;
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = (uiTheme.windDepthBg); rd.bg_opa = 235;
    rd.radius = UI_S(5); rd.border_width = 1; rd.border_color = (uiTheme.windMast);
    lv_canvas_draw_rect(_canvas, (lv_coord_t)bx, (lv_coord_t)by, (lv_coord_t)w, (lv_coord_t)h, &rd);
    ctext(_canvas, bx, by + 3.f * UI_SF, w, FONT_SMALL, CLR_TEXT, LV_TEXT_ALIGN_CENTER, buf);
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
void WindScreen::drawCompassRose(float hdg) {
    const bool haveHdg = !isnan(hdg);
    const bool northUp = appConfig.cfg.compassNorthUp;
    // Course-up without a compass fix has nothing to rotate by, so it falls back
    // to the north-up picture — which is exactly what the old "isnan → 0" did.
    const float rot = (northUp || !haveHdg) ? 0.f : hdg;
    const float rt0 = R_INNER -  8.f * UI_SF;   // minor tick inner end (short → gap to labels)
    const float rtM = R_INNER - 11.f * UI_SF;   // major tick inner end
    const float rt1 = R_INNER -  2.f;   // tick outer end (-2 hugs the unscaled 3px border)
    const float rl  = R_INNER - 21.f * UI_SF;   // cardinal labels (clear gap from the ticks)
    lv_color_t cTick = (uiTheme.windTickMin);
    lv_color_t cMaj  = (uiTheme.windTickMaj);
    for (int b = 0; b < 360; b += 10) {
        float a = (float)b - rot;       // bearing b → screen angle (b − rot)
        bool major = (b % 30) == 0;
        lv_point_t p1 = wp(CX, CY, major ? rtM : rt0, a);
        lv_point_t p2 = wp(CX, CY, rt1, a);
        linePx(p1.x, p1.y, p2.x, p2.y, major ? cMaj : cTick, LV_OPA_70, major ? 2 : 1);
    }
    // Cardinal letters: German Ost = "O", English East = "E" (not static — T() is
    // language-dependent and the language can change at runtime).
    const char *card[4] = { T(STR_WIND_CARD_N), T(STR_WIND_CARD_E),
                            T(STR_WIND_CARD_S), T(STR_WIND_CARD_W) };
    for (int i = 0; i < 4; i++) {
        float a = (float)(i * 90) - rot;
        float lx = CX + rl * sinf(a * DEG2RAD);
        float ly = CY - rl * cosf(a * DEG2RAD);
        lv_color_t cc = (i == 0) ? CLR_RED : cMaj;   // North in red
        ctext(_canvas, lx - 8.f * UI_SF, ly - 8.f * UI_SF, 16.f * UI_SF, FONT_SMALL, cc, LV_TEXT_ALIGN_CENTER, card[i]);
    }
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
void WindScreen::drawRudderArc(float rudderDeg) {
    if (isnan(rudderDeg)) rudderDeg = 0.f;
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
    lv_canvas_draw_arc(_canvas, (lv_coord_t)CX, (lv_coord_t)CY, (lv_coord_t)rM,
                       (int)(180.f - HALF - 90.f), (int)(180.f + HALF - 90.f), &ad);
    float r = rudderDeg; if (r > 35.f) r = 35.f; if (r < -35.f) r = -35.f;
    float sp = (fabsf(r) / 35.f) * HALF;
    if (sp > 0.5f) {
        if (r > 0.f) { ad.color = CLR_GREEN;     // Stbd: wind 180−sp..180 → LVGL 90−sp..90
            lv_canvas_draw_arc(_canvas,(lv_coord_t)CX,(lv_coord_t)CY,(lv_coord_t)rM,(int)(90.f-sp),90,&ad); }
        else { ad.color = CLR_RED;               // Port: wind 180..180+sp → LVGL 90..90+sp
            lv_canvas_draw_arc(_canvas,(lv_coord_t)CX,(lv_coord_t)CY,(lv_coord_t)rM,90,(int)(90.f+sp),&ad); }
    }
    { lv_point_t p1 = wp(CX, CY, rI - 2.f, 180.f), p2 = wp(CX, CY, rO + 2.f, 180.f);  // amidships tick
      linePx(p1.x, p1.y, p2.x, p2.y, CLR_TEXT, LV_OPA_COVER, 2); }
    lv_point_t sp2 = wp(CX, CY, rM, 180.f - HALF + 4.f);   // stbd end (right)
    ctext(_canvas, sp2.x - 13.f * UI_SF, sp2.y - 7.f * UI_SF, 26.f * UI_SF, FONT_TINY, CLR_GREEN, LV_TEXT_ALIGN_CENTER, T(STR_WIND_STB));
    lv_point_t bp2 = wp(CX, CY, rM, 180.f + HALF - 4.f);   // port end (left)
    ctext(_canvas, bp2.x - 13.f * UI_SF, bp2.y - 7.f * UI_SF, 26.f * UI_SF, FONT_TINY, CLR_RED, LV_TEXT_ALIGN_CENTER, T(STR_WIND_PT));
    char buf[10]; snprintf(buf, sizeof(buf), "%+d\xc2\xb0", (int)(rudderDeg + (rudderDeg >= 0.f ? 0.5f : -0.5f)));
    lv_point_t vp = wp(CX, CY, rM, 180.f);
    ctext(_canvas, vp.x - 26.f * UI_SF, vp.y - 15.f * UI_SF, 52.f * UI_SF, FONT_SMALL, CLR_ON_ACCENT, LV_TEXT_ALIGN_CENTER, buf);
}

void WindScreen::drawCornerKpis(float stw, float twa, float awa, float tws) {
    char buf[16];
    const float TY  = 4.f * UI_SF,   TV  = 22.f * UI_SF;   // top label / value y
    const float BY  = 430.f * UI_SF, BV  = 446.f * UI_SF;  // bottom label / value y
    const float LX  = 6.f * UI_SF,   RX  = 474.f * UI_SF;
    const float TW  = 152.f * UI_SF;               // top blocks — wide (room above the circle)
    const float BWL = 96.f * UI_SF,  BWR = 126.f * UI_SF;  // bottom blocks flank the wider polar bar

    // ── Top-left: STW ─────────────────────────────────────────
    ctext(_canvas, LX, TY, TW, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, "BSPD");
    if (isnan(stw)) snprintf(buf, sizeof(buf), "--");
    else            snprintf(buf, sizeof(buf), "%.1f kn", stw);
    ctext(_canvas, LX, TV, TW, FONT_XL, CLR_TEXT, LV_TEXT_ALIGN_LEFT, buf);

    // ── Top-right: TWA abs ────────────────────────────────────
    ctext(_canvas, RX - TW, TY, TW, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, "TWA");
    float absTwa = fabsf(twa);
    lv_color_t twaC = (twa >= 0.f) ? CLR_STARBOARD : CLR_PORT;
    if (isnan(twa)) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d\xc2\xb0 %s",   // "°" in UTF-8
                  (int)(absTwa + 0.5f), twa >= 0.f ? T(STR_WIND_STB) : T(STR_WIND_PT));
    ctext(_canvas, RX - TW, TV, TW, FONT_XL, twaC, LV_TEXT_ALIGN_RIGHT, buf);

    // ── Bottom-left: AWA ─────────────────────────────────────
    ctext(_canvas, LX, BY, BWL, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, "AWA");
    if (isnan(awa)) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d\xc2\xb0", (int)(fabsf(awa) + 0.5f));
    ctext(_canvas, LX, BV, BWL, FONT_XL, CLR_WIND, LV_TEXT_ALIGN_LEFT, buf);

    // ── Bottom-right: TWS ────────────────────────────────────
    ctext(_canvas, RX - BWR, BY, BWR, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT, "TWS");
    if (isnan(tws)) snprintf(buf, sizeof(buf), "--");
    else            snprintf(buf, sizeof(buf), "%.1f kn", tws);
    ctext(_canvas, RX - BWR, BV, BWR, FONT_XL, CLR_TEXT, LV_TEXT_ALIGN_RIGHT, buf);
}

// ── Layer 14: Trim advice below the circle ────────────────────────────────────

void WindScreen::drawTrimAdvice(float absTwa, float twa, float tws) {
    // Point-of-sail name, INSIDE the circle just below the boat (the centre is
    // free now that the big TWA readout was removed). The verbose trim hint is
    // dropped — the sail graphic itself shows the trim and the big circle leaves
    // no room outside it.
    (void)twa; (void)tws;
    PoS         pos  = pointOfSail(absTwa);
    const char *name = posLabel(pos);
    ctext(_canvas, CX - 95.f * UI_SF, CY + 29.f * BOAT_S, 190.f * UI_SF,
          FONT_MED, CLR_TEXT, LV_TEXT_ALIGN_CENTER, name);
}

// ── Reef badge (below trim-advice, coloured pill) ────────────────────────────
void WindScreen::drawReefBadge(int reefCount) {
    if (reefCount <= 0) return;

    // Colour: 1=amber, 2=orange, 3=red
    lv_color_t col;
    const char *label;
    switch (reefCount) {
        case 1:  col = CLR_ORANGE; label = T(STR_WIND_REEF1); break;
        case 2:  col = CLR_ORANGE; label = T(STR_WIND_REEF2); break;
        default: col = CLR_RED;    label = T(STR_WIND_REEF3); break;
    }

    // Badge pill: centred above the bow, inside the circle (only when reefed)
    const float bw = 56.f * UI_SF, bh = 18.f * UI_SF;
    const float bx = CX - bw * 0.5f;
    const float by = CY - 35.f * BOAT_S;

    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col; rd.bg_opa = 220;
    rd.radius   = UI_S(4);   rd.border_width = 0;
    lv_canvas_draw_rect(_canvas,
        (lv_coord_t)bx, (lv_coord_t)by,
        (lv_coord_t)bw, (lv_coord_t)bh, &rd);

    ctext(_canvas, bx, by + 3.f * UI_SF, bw,
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

// ── dashedRadial helper ───────────────────────────────────────────────────────

void WindScreen::dashedRadial(float windAngle, float r1, float r2, lv_color_t col) {
    float a    = windAngle * DEG2RAD;
    float sinA = sinf(a), cosA = cosf(a);

    const float dash = 5.f * UI_SF, gap = 4.f * UI_SF;
    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    ld.color = col; ld.width = 2; ld.opa = 200;

    for (float r = r1; r < r2; r += dash + gap) {
        float rEnd = (r + dash < r2) ? (r + dash) : r2;
        lv_point_t p1 = { (lv_coord_t)(CX + r    * sinA), (lv_coord_t)(CY - r    * cosA) };
        lv_point_t p2 = { (lv_coord_t)(CX + rEnd  * sinA), (lv_coord_t)(CY - rEnd  * cosA) };
        { lv_point_t _dl[2]={p1,p2}; lv_canvas_draw_line(_canvas,_dl,2,&ld); }
    }
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

