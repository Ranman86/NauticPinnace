// WindScreen.cpp – B&G-style circular wind-angle instrument for ESP32-S3
// Ported from BngRenderer.cs (SailTrimMonitor / SkiaSharp)
// LVGL 8 + dark theme (CLR_BG = 0x0A0A0F)

#include "WindScreen.h"
#include "../../config/Config.h"
#include "../../PolarTable.h"
#include "../../i18n/I18n.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../PsramArena.h"

WindScreen windScreen;

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

static void cline(lv_obj_t *cv,
                  float x1, float y1, float x2, float y2,
                  lv_color_t col, lv_coord_t w = 1, lv_opa_t opa = LV_OPA_COVER) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = col; d.width = w; d.opa = opa;
    lv_point_t p[2] = {{ (lv_coord_t)x1, (lv_coord_t)y1 },
                        { (lv_coord_t)x2, (lv_coord_t)y2 }};
    lv_canvas_draw_line(cv, p, 2, &d);
}

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
        // Yield SPI0 every 50 rows so WiFi beacon ISR can complete its SPI0 transaction.
        if (++row % 20 == 0) vTaskDelay(pdMS_TO_TICKS(1));
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
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = col; d.width = 1; d.opa = opa;
    int row = 0;
    for (float dy2 = -ry; dy2 <= ry; dy2 += 1.f) {
        float ratio = dy2 / ry;
        float hx = rx * sqrtf(1.f - ratio * ratio);
        lv_point_t pts[2] = {
            { (lv_coord_t)(cx - hx), (lv_coord_t)(cy + dy2) },
            { (lv_coord_t)(cx + hx), (lv_coord_t)(cy + dy2) }
        };
        lv_canvas_draw_line(_canvas, pts, 2, &d);
        if (++row % 20 == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ── Scanline polygon fill (even-odd) ──────────────────────────────────────────
// Replacement for lv_canvas_draw_polygon (which segfaults in the PC simulator's
// LVGL build). Fills a simple polygon with horizontal lv_canvas_draw_line spans.
void WindScreen::fillPolygon(const lv_point_t *pts, int n, lv_color_t col, lv_opa_t opa) {
    if (n < 3) return;
    int ymin = pts[0].y, ymax = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].y < ymin) ymin = pts[i].y;
        if (pts[i].y > ymax) ymax = pts[i].y;
    }
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = col; d.width = 1; d.opa = opa;
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
            lv_point_t p[2] = {{ (lv_coord_t)(xs[i] + 0.5f),   (lv_coord_t)y },
                               { (lv_coord_t)(xs[i+1] + 0.5f), (lv_coord_t)y }};
            lv_canvas_draw_line(_canvas, p, 2, &d);
        }
        if (++row % 20 == 0) vTaskDelay(pdMS_TO_TICKS(1));
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
            cline(_canvas, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, edgeCol, edgeW);
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
    if (!_cbuf) _cbuf = (lv_color_t *)PsramArena::alloc(sz);   // reuse on live theme rebuild
    if (_cbuf) {
        // Buffer is already zeroed by PsramArena::init() (before WiFi starts).
        _canvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_canvas, _cbuf, CW, CH, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(_canvas, 0, 0);
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

    // ── Yield-safe background fill ────────────────────────────────────────────
    // Problem: lv_canvas_fill_bg() writes 480×430×2 = 825 KB to PSRAM in one tight
    // loop.  With WiFi active, OPI PSRAM bandwidth drops to ~460 KB/s because the
    // CPU's cache-line fills (SPI0) compete with the WiFi beacon DMA (also SPI0).
    // At 460 KB/s the fill takes ~1.8 s.  The WiFi beacon ISR fires during the fill,
    // needs SPI0, finds it occupied by the ongoing PSRAM fills, and spins.  After
    // 800 ms of spinning the Interrupt WDT fires → TG1WDT_SYS_RST.
    //
    // Fix: fill in 8-row chunks (480×8×2 = 7 680 B each), then vTaskDelay(1).
    // The 1 ms yield puts Core 1 to sleep, freeing SPI0.  Core 0's WiFi ISR
    // completes its transaction in < 1 ms.  The NEXT chunk then runs at full
    // PSRAM speed (~35 MB/s → 0.22 ms), so total fill time drops from ~1.8 s
    // to ~66 ms and no single ISR invocation stalls for more than ~0.22 ms.
    {
        const lv_color_t bgColor = CLR_BG;
        const size_t     rowPx   = (size_t)CW;
        const int        CHUNK   = 8;   // rows per yield window
        lv_color_t      *p       = _cbuf;
        for (int row = 0; row < CH; row += CHUNK) {
            int         rows = (CH - row < CHUNK) ? CH - row : CHUNK;
            lv_color_t *end  = p + (size_t)rows * rowPx;
            while (p < end) *p++ = bgColor;
            vTaskDelay(pdMS_TO_TICKS(1));  // release SPI0 for WiFi ISR
        }
        lv_obj_invalidate(_canvas);
    }

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
    // vTaskDelay(2) between each heavy layer: 2 RTOS ticks gives WiFi beacon ISR
    // a guaranteed SPI0-free window on both Core 0 and Core 1.
    drawBezel();
    vTaskDelay(pdMS_TO_TICKS(1));
    drawZoneRing(absTwa);
    vTaskDelay(pdMS_TO_TICKS(1));
    drawTicksAndLabels();
    vTaskDelay(pdMS_TO_TICKS(1));
    drawInnerBg();
    if (_centerMode == CenterMode::ATTITUDE) {
        drawCompassRose(hdg);       // keep the heading-up compass card
        vTaskDelay(pdMS_TO_TICKS(1));
        drawCenter_Attitude(roll, pitch, rot);   // artificial horizon replaces the boat
    } else {
        drawWindLines(appConfig.cfg.windLinesApparent ? nAwa : nTwa);   // flow lines: apparent or true
        drawCompassRose(hdg);       // heading-up compass card inside the inner circle
        vTaskDelay(pdMS_TO_TICKS(1));
        drawBoat(sail);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
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
    drawRudderArc(rudder);          // curved rudder-angle gauge below the rose, above the polar bar

    // ── Polar performance bar ─────────────────────────────────────────────
    // Vertical bar on the right side, between the ring and canvas edge.
    // Bar track: x=450..468, y=CY-80..CY+80 (height 160px)
    if (_centerMode != CenterMode::ATTITUDE) {
        // Horizontal performance bar along the bottom, between the AWA (left) and
        // TWS (right) corner fields. Left = 0 %, right = 120 %; fill grows right.
        const float bx0 = 108.f, bx1 = 342.f;     // ~30 % wider than before
        const float bw  = bx1 - bx0;
        const float by  = 456.f, bh = 20.f;

        // Dark track (fixed colour, so the white label reads on light AND dark theme)
        lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
        rd.bg_color = (uiTheme.windRingBg); rd.bg_opa = 235;
        rd.radius = 4; rd.border_width = 0;
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
            rd.bg_color = barCol; rd.bg_opa = 230; rd.radius = 4;
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
        lv_canvas_draw_text(_canvas, (lv_coord_t)bx0, (lv_coord_t)(by + 2), (lv_coord_t)bw, &td, pb);
    }

    lv_obj_invalidate(_canvas);
}

// ── Layer 1: Bezel background ─────────────────────────────────────────────────

void WindScreen::drawBezel() {
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = C_BEZEL; rd.bg_opa = LV_OPA_COVER;
    rd.radius   = LV_RADIUS_CIRCLE; rd.border_width = 0;
    lv_coord_t sz = (lv_coord_t)(R_OUTER * 2 + 2);
    lv_canvas_draw_rect(_canvas,
        (lv_coord_t)(CX - R_OUTER - 1), (lv_coord_t)(CY - R_OUTER - 1),
        sz, sz, &rd);
}

// ── Layer 2: Coloured zone arcs ───────────────────────────────────────────────

void WindScreen::drawZoneRing(float absTwa) {
    // Base ring (dark)
    arcRing(R_ZONE_I, R_ZONE_O, -180.f, 360.f, C_BASE, LV_OPA_COVER);

    // No-Go: configurable half-angle (WebUI), centred at 0° / top.
    float ng = (float)appConfig.cfg.noGoAngle;
    if (ng < 10.f) ng = 10.f;
    if (ng > 44.f) ng = 44.f;
    arcRing(R_ZONE_I, R_ZONE_O, -ng, 2.f * ng, C_NOGO, 220);

    // Symmetric zones (Stb + Bb mirrored)
    symArcRing(R_ZONE_I, R_ZONE_O, ng, 45.f - ng, C_CLOSE, 210);  // close-hauled ng–45°
    symArcRing(R_ZONE_I, R_ZONE_O, 45.f, 20.f, C_CLOSER, 195);  // close reach 45–65°
    symArcRing(R_ZONE_I, R_ZONE_O, 65.f, 35.f, C_BEAM,   190);  // beam reach  65–100°
    symArcRing(R_ZONE_I, R_ZONE_O, 100.f,50.f, C_BROAD,  190);  // broad reach 100–150°

    // Running: 150° to 210° (symmetric around 180°)
    arcRing(R_ZONE_I, R_ZONE_O, 150.f, 60.f, C_RUN, 200);

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
            // Centre text: use 26px wide box centred on label point
            ctext(_canvas, lx - 13, ly - 6, 26,
                  FONT_TINY, C_TICK_MJ, LV_TEXT_ALIGN_CENTER, lbl);
        }
    }
}

// ── Layer 4: Inner circle background ─────────────────────────────────────────

void WindScreen::drawInnerBg() {
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = C_INNER; rd.bg_opa = LV_OPA_COVER;
    rd.radius   = LV_RADIUS_CIRCLE; rd.border_width = 0;
    lv_coord_t sz = (lv_coord_t)(R_INNER * 2);
    lv_canvas_draw_rect(_canvas,
        (lv_coord_t)(CX - R_INNER), (lv_coord_t)(CY - R_INNER),
        sz, sz, &rd);
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
    const float Rin  = R_INNER - 3.f;
    const float step = 24.f;
    for (float off = -(Rin - 4.f); off <= (Rin - 4.f); off += step) {
        float half = sqrtf(Rin * Rin - off * off);   // chord half-length inside the circle
        float x1 = CX - half * sinA + off * pX;
        float y1 = CY + half * cosA + off * pY;
        float x2 = CX + half * sinA + off * pX;
        float y2 = CY - half * cosA + off * pY;
        cline(_canvas, x1, y1, x2, y2, C_WIND_LN, 1, LV_OPA_40);
    }
}

// ── Layer 6: Boat bird's-eye ─────────────────────────────────────────────────
//
//   Coordinate origin: CX, CY  (= 240, 198).
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
        cline(_canvas, hp[i].x, hp[i].y, hp[(i+1)%36].x, hp[(i+1)%36].y, C_HULL, 2);


    // ── Forestay (bow → mast, on the centreline = the jib luff) ────────────
    lv_color_t cRig = (uiTheme.windRigging);
    cline(_canvas, bowX, bowY, mastX, mastY, cRig, 1, LV_OPA_60);

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
        cline(_canvas, mastX, mastY, clewJX, clewJY, cRig, 2, LV_OPA_70);   // whisker pole
        ctext(_canvas, CX - 36.f, CY - 35.f * BS, 72.f,
              FONT_TINY, CLR_ACCENT, LV_TEXT_ALIGN_CENTER, "Butterfly");
    } else if (sail.useSpinnaker) {
        drawSpinnaker(sail);
    } else if (sail.useCodeZero) {
        drawCodeZero(sail);
    } else if (!sail.running) {
        drawSailFoil(bowX, bowY, clewJX, clewJY, jss, jCam, sailCol, sailOpa, leechCol, 2);
        cline(_canvas, clewJX, clewJY, CX + jss * 9.f * BS, CY + 4.f * BS, CLR_GREEN, 1, LV_OPA_50); // jib sheet
    }

    // ── MAINSAIL at the MAST (centre) ──────────────────────────────────────
    drawSailFoil(mastX, mastY, clewMX, clewMY, bss, mCam, sailCol, sailOpa, leechCol, 2);
    cline(_canvas, mastX, mastY, clewMX, clewMY, cRig, 2);   // boom spar (on top)

    // ── Mast: a single point at the ship's centre (NOT a fore-aft line) ────
    { lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
      rd.bg_color = cRig; rd.bg_opa = LV_OPA_COVER; rd.radius = LV_RADIUS_CIRCLE; rd.border_width = 0;
      lv_canvas_draw_rect(_canvas, (lv_coord_t)(mastX - 3), (lv_coord_t)(mastY - 3), 6, 6, &rd); }

    // ── Rudder ─────────────────────────────────────────────────────────────
    {
        auto lk = data.lock();
        float ra = data.rudderAngle;
        drawRudder(isnan(ra) ? 0.f : ra, ss);
    }
}

// ── Keel (dark oval behind hull) ─────────────────────────────────────────────
void WindScreen::drawKeel() {
    const float BS = BOAT_S;
    fillEllipse(CX, CY + 3.3f * BS, 1.8f * BS, 4.3f * BS, (uiTheme.windMast), LV_OPA_COVER);
    cline(_canvas, CX, CY - 1.f * BS, CX, CY + 7.f * BS, (uiTheme.windMast), 3);
}


// ── Rudder (cyan, pivots at scaled Cy=74 * 0.333 = CY+24.7) ─────────────────
void WindScreen::drawRudder(float rudderDeg, float /*ss*/) {
    const float BS   = BOAT_S;
    const float pX   = CX;
    const float pY   = CY + 24.7f * BS;
    const float half = 4.0f * BS;
    float ra = rudderDeg * DEG2RAD;
    float dx = half * sinf(ra);
    float dy = half * cosf(ra);

    lv_color_t col = (uiTheme.windFlow);
    cline(_canvas, pX - dx, pY - dy, pX + dx, pY + dy, col, 2);

    // Pivot dot
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col; rd.bg_opa = LV_OPA_COVER;
    rd.radius   = LV_RADIUS_CIRCLE; rd.border_width = 0;
    lv_canvas_draw_rect(_canvas,
        (lv_coord_t)(pX - 2), (lv_coord_t)(pY - 2), 4, 4, &rd);

    // Label only if significant deflection
    if (fabsf(rudderDeg) > 2.f) {
        char buf[8]; snprintf(buf, sizeof(buf), "%+.0f", rudderDeg);
        ctext(_canvas, pX - 14.f, pY + 5.f, 28.f,
              FONT_TINY, col, LV_TEXT_ALIGN_CENTER, buf);
    }
}


// ── Sheet lines: jib clew → fairlead → winch ─────────────────────────────────
void WindScreen::drawSheetLines(const SailState &sail, float clewX, float clewY) {
    const float BS  = BOAT_S;
    const float ss  = sail.leewardSide;
    const float flX = CX + ss * 8.7f * BS,  flY = CY + 3.3f * BS;
    const float wX  = CX + ss * 7.0f * BS,  wY  = CY + 14.7f * BS;

    lv_color_t col = CLR_GREEN;   // green sheet
    cline(_canvas, clewX, clewY, flX, flY, col, 1, LV_OPA_60);
    cline(_canvas, flX,   flY,   wX,  wY,  col, 1, LV_OPA_50);
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
        cline(_canvas,
              ocx + 18.6f * BS * sinf(a1), ocy - 15.3f * BS * cosf(a1),
              ocx + 18.6f * BS * sinf(a2), ocy - 15.3f * BS * cosf(a2),
              CLR_ORANGE, 1, 200);
    }

    float halsX = CX - ss * 4.0f * BS;
    float halsY = ocy;
    cline(_canvas, CX, CY - 29.f * BS, halsX, halsY, (uiTheme.windRigging), 1, LV_OPA_70);

    float schotX = CX + ss * 28.6f * BS;
    float schotY = CY - 3.3f * BS;
    cline(_canvas, CX, CY + 7.3f * BS, schotX, schotY, CLR_GREEN, 1, LV_OPA_60);
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
    ctext(_canvas, lbx - 8.f, lby - 5.f, 16.f, FONT_TINY, gold, LV_TEXT_ALIGN_CENTER, "C0");

    drawSheetLines(sail, clewX, clewY);
}

// ── Layer 7+8: Circle borders ─────────────────────────────────────────────────

void WindScreen::drawInnerBorder() {
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.color = (uiTheme.windMast); d.width = 3; d.opa = LV_OPA_COVER;
    lv_canvas_draw_arc(_canvas,
        (lv_coord_t)CX, (lv_coord_t)CY, (lv_coord_t)R_INNER, 0, 360, &d);
}

void WindScreen::drawOuterBorder() {
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.color = (uiTheme.windHull); d.width = 2; d.opa = LV_OPA_COVER;
    lv_canvas_draw_arc(_canvas,
        (lv_coord_t)CX, (lv_coord_t)CY, (lv_coord_t)R_OUTER, 0, 360, &d);
}

// ── Layer 9: TWA pointer (blue triangle, tip points inward at zone outer) ────

void WindScreen::drawTwaPointer(float twaDeg) {
    triPointer(twaDeg, R_ZONE_O, R_OUTER + 11.f, 12.f, C_TWA_PTR);   // big arrow, outer band
    // "T" label on the pointer body
    float a  = twaDeg * DEG2RAD;
    float lx = CX + (R_OUTER + 1.f) * sinf(a);
    float ly = CY - (R_OUTER + 1.f) * cosf(a);
    ctext(_canvas, lx - 7.f, ly - 8.f, 14.f,
          FONT_SMALL, CLR_ON_ACCENT, LV_TEXT_ALIGN_CENTER, "T");
}

// ── Layer 10: AWA pointer (red, inside zone ring, tip at inner circle edge) ──

void WindScreen::drawAwaPointer(float awaDeg) {
    triPointer(awaDeg, R_INNER + 1.f, R_ZONE_O - 2.f, 12.f, C_AWA_PTR);  // same size as TWA, inner band
    float a  = awaDeg * DEG2RAD;
    float lx = CX + (R_ZONE_O - 12.f) * sinf(a);   // on the pointer body (centroid), not at the tip
    float ly = CY - (R_ZONE_O - 12.f) * cosf(a);
    ctext(_canvas, lx - 7.f, ly - 7.f, 14.f,
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
        arcRing(R_ZONE_I - 8.f, R_ZONE_O + 2.f,  up - 3.f,        6.f, C_VMG, LV_OPA_COVER);
        arcRing(R_ZONE_I - 8.f, R_ZONE_O + 2.f, -up + 360.f - 3.f, 6.f, C_VMG, LV_OPA_COVER);
        triPointer( up,         R_ZONE_O + 1.f, R_ZONE_O + 9.f, 5.f, C_VMG, LV_OPA_COVER);
        triPointer(-up + 360.f, R_ZONE_O + 1.f, R_ZONE_O + 9.f, 5.f, C_VMG, LV_OPA_COVER);
    }
    if (showDown) {
        arcRing(R_ZONE_I - 8.f, R_ZONE_O + 2.f,  down - 3.f,        6.f, C_VMG, LV_OPA_COVER);
        arcRing(R_ZONE_I - 8.f, R_ZONE_O + 2.f, -down + 360.f - 3.f, 6.f, C_VMG, LV_OPA_COVER);
        triPointer( down,        R_ZONE_O + 1.f, R_ZONE_O + 9.f, 5.f, C_VMG, LV_OPA_COVER);
        triPointer(-down + 360.f, R_ZONE_O + 1.f, R_ZONE_O + 9.f, 5.f, C_VMG, LV_OPA_COVER);
    }
}

// ── Layer 12: Centre TWA value ────────────────────────────────────────────────


// ── ATTITUDE centre: aircraft-style artificial horizon ───────────────────────
// A sky/ground disc that rotates with roll and translates with pitch, plus a
// pitch ladder, a fixed aircraft reference, a bank scale + pointer and a
// rate-of-turn strip. Drawn with scanline spans + cline()/fillTri() only — no
// lv_canvas_draw_polygon (it segfaults in the PC simulator). The disc (R=110)
// sits inside the compass rose, so the outer ring stays fully visible.
void WindScreen::drawCenter_Attitude(float roll, float pitch, float rot) {
    const float R   = 110.f;     // horizon-disc radius (clear of the compass rose)
    const float PPD = 3.4f;      // screen px per degree of pitch
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
        if (++rowc % 20 == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }

    // ── Horizon line across the disc (intersect line with the circle) ────────
    {
        float b = pc * PPD;                       // vertical offset of P0 from centre
        float B = 2.f * b * sphi, Cq = b * b - R * R;
        float disc = B * B - 4.f * Cq;
        if (disc > 0.f) {
            float sd = sqrtf(disc);
            float t1 = (-B - sd) * 0.5f, t2 = (-B + sd) * 0.5f;
            cline(_canvas, CX + t1 * cphi, horY + t1 * sphi,
                           CX + t2 * cphi, horY + t2 * sphi, LINEC, 2);
        }
    }

    // ── Pitch ladder (rungs at ±10/±20°, rotated by roll, clipped) ───────────
    const float ux = sphi, uy = -cphi;            // unit "up" (sky) normal
    const int   pv[4]    = { 10, 20, -10, -20 };
    const float halfL[4] = { 22.f, 15.f, 22.f, 15.f };
    char lbl[4];
    for (int i = 0; i < 4; i++) {
        float off = pv[i] * PPD;
        float mx = CX + off * ux, my = horY + off * uy;        // rung centre
        if ((mx-CX)*(mx-CX)+(my-CY)*(my-CY) > (R-6.f)*(R-6.f)) continue;
        float hl = halfL[i];
        float ax = mx - hl*cphi, ay = my - hl*sphi;
        float bx = mx + hl*cphi, by = my + hl*sphi;
        cline(_canvas, ax, ay, bx, by, LINEC, 1, LV_OPA_80);
        snprintf(lbl, sizeof(lbl), "%d", pv[i] < 0 ? -pv[i] : pv[i]);
        ctext(_canvas, bx + 2.f, by - 6.f, 18.f, FONT_TINY, LINEC, LV_TEXT_ALIGN_LEFT, lbl);
    }

    // ── Bank scale (fixed) at the top + moving pointer at the current roll ───
    const float rB = 100.f;                       // scale radius (over the sky)
    const int   bt[5] = { 10, 20, 30, 45, 60 };
    { lv_point_t p1 = wp(CX,CY,rB,0.f), p2 = wp(CX,CY,rB-11.f,0.f);  // 0° major tick
      cline(_canvas, p1.x, p1.y, p2.x, p2.y, LINEC, 2, LV_OPA_70); }
    for (int i = 0; i < 5; i++) {
        for (int s = -1; s <= 1; s += 2) {
            float ang = (float)(s * bt[i]);
            bool maj = (bt[i] % 30) == 0;
            lv_point_t p1 = wp(CX, CY, rB, ang);
            lv_point_t p2 = wp(CX, CY, rB - (maj ? 11.f : 7.f), ang);
            cline(_canvas, p1.x, p1.y, p2.x, p2.y, LINEC, maj ? 2 : 1, LV_OPA_70);
        }
    }
    {
        float a = fmaxf(-60.f, fminf(60.f, roll));
        lv_point_t tip = wp(CX, CY, rB - 12.f, a);
        lv_point_t bl  = wp(CX, CY, rB - 22.f, a - 4.f);
        lv_point_t br  = wp(CX, CY, rB - 22.f, a + 4.f);
        fillTri(_canvas, tip.x, tip.y, bl.x, bl.y, br.x, br.y, CLR_YELLOW, LV_OPA_COVER);
    }

    // ── Fixed aircraft reference (does NOT rotate) ───────────────────────────
    cline(_canvas, CX - 38.f, CY, CX - 14.f, CY,        CLR_YELLOW, 3);
    cline(_canvas, CX - 14.f, CY, CX - 14.f, CY + 7.f,  CLR_YELLOW, 3);
    cline(_canvas, CX + 38.f, CY, CX + 14.f, CY,        CLR_YELLOW, 3);
    cline(_canvas, CX + 14.f, CY, CX + 14.f, CY + 7.f,  CLR_YELLOW, 3);
    { lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
      rd.bg_color = CLR_YELLOW; rd.bg_opa = LV_OPA_COVER; rd.radius = 2; rd.border_width = 0;
      lv_canvas_draw_rect(_canvas, (lv_coord_t)(CX-3), (lv_coord_t)(CY-3), 6, 6, &rd); }

    // ── Rate-of-turn strip (below the aircraft symbol) ───────────────────────
    {
        const float yb = CY + 62.f, FS = 60.f, halfW = 46.f;
        cline(_canvas, CX - halfW, yb, CX + halfW, yb, LINEC, 1, LV_OPA_70);
        cline(_canvas, CX, yb - 5.f, CX, yb + 5.f,     LINEC, 1, LV_OPA_70);   // centre mark
        float r  = isnan(rot) ? 0.f : fmaxf(-FS, fminf(FS, rot));
        float xr = CX + (r / FS) * halfW;
        lv_color_t rc = (fabsf(rot) > 30.f) ? CLR_ORANGE : CLR_GREEN;
        fillTri(_canvas, xr, yb - 8.f, xr - 5.f, yb, xr + 5.f, yb, rc, LV_OPA_COVER);
        char rb[20];
        if (isnan(rot)) snprintf(rb, sizeof(rb), "ROT --");
        else snprintf(rb, sizeof(rb), "ROT %+d\xc2\xb0/min", (int)(rot + (rot>=0?0.5f:-0.5f)));
        ctext(_canvas, CX - 60.f, yb + 6.f, 120.f, FONT_TINY, CLR_TEXT, LV_TEXT_ALIGN_CENTER, rb);
    }
}

// Corner KPIs for the ATTITUDE screen: Roll / Pitch / wave height / wave period.
void WindScreen::drawAttitudeKpis(float roll, float pitch, float waveH, float waveT) {
    char buf[20];
    const float TY = 4.f, TV = 22.f, BY = 430.f, BV = 446.f;
    const float LX = 6.f, RX = 474.f, TW = 152.f, BWL = 120.f, BWR = 132.f;

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
    const float w = 48.f, h = 21.f;
    const float bx = CX - w * 0.5f, by = CY - R_INNER + 1.f;
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = (uiTheme.windDepthBg); rd.bg_opa = 235;
    rd.radius = 5; rd.border_width = 1; rd.border_color = (uiTheme.windMast);
    lv_canvas_draw_rect(_canvas, (lv_coord_t)bx, (lv_coord_t)by, (lv_coord_t)w, (lv_coord_t)h, &rd);
    ctext(_canvas, bx, by + 3.f, w, FONT_SMALL, CLR_TEXT, LV_TEXT_ALIGN_CENTER, buf);
}

// Heading-up compass card inside the inner circle: ticks every 10° (major 30°)
// plus N/O/S/W, rotated so the current heading is always at the top.
void WindScreen::drawCompassRose(float hdg) {
    if (isnan(hdg)) hdg = 0.f;
    const float rt0 = R_INNER -  8.f;   // minor tick inner end (short → gap to labels)
    const float rtM = R_INNER - 11.f;   // major tick inner end
    const float rt1 = R_INNER -  2.f;   // tick outer end
    const float rl  = R_INNER - 21.f;   // cardinal labels (clear gap from the ticks)
    lv_color_t cTick = (uiTheme.windTickMin);
    lv_color_t cMaj  = (uiTheme.windTickMaj);
    for (int b = 0; b < 360; b += 10) {
        float a = (float)b - hdg;       // heading-up: bearing b → screen angle (b − hdg)
        bool major = (b % 30) == 0;
        lv_point_t p1 = wp(CX, CY, major ? rtM : rt0, a);
        lv_point_t p2 = wp(CX, CY, rt1, a);
        cline(_canvas, p1.x, p1.y, p2.x, p2.y, major ? cMaj : cTick, major ? 2 : 1, LV_OPA_70);
    }
    // Cardinal letters: German Ost = "O", English East = "E" (not static — T() is
    // language-dependent and the language can change at runtime).
    const char *card[4] = { T(STR_WIND_CARD_N), T(STR_WIND_CARD_E),
                            T(STR_WIND_CARD_S), T(STR_WIND_CARD_W) };
    for (int i = 0; i < 4; i++) {
        float a = (float)(i * 90) - hdg;
        float lx = CX + rl * sinf(a * DEG2RAD);
        float ly = CY - rl * cosf(a * DEG2RAD);
        lv_color_t cc = (i == 0) ? CLR_RED : cMaj;   // North in red
        ctext(_canvas, lx - 8.f, ly - 8.f, 16.f, FONT_SMALL, cc, LV_TEXT_ALIGN_CENTER, card[i]);
    }
}

// Rudder-angle gauge on the bottom arc (150°..210°). Stbd (+) fills GREEN toward
// 150° (right), Port (−) fills RED toward 210° (left). ±35° rudder → ±30° arc.
void WindScreen::drawRudderArc(float rudderDeg) {
    if (isnan(rudderDeg)) rudderDeg = 0.f;
    // Curved gauge hugging the rose's outer edge (no gap) at the bottom, drawn with
    // lv_canvas_draw_arc → anti-aliased edges. ±30° of arc ≙ ±35° of rudder; Stbd
    // green (right/150°), Port red (left/210°). The 150/180/210 labels are hidden
    // here (drawTicksAndLabels) so the gauge sits flush on the rose.
    const float rI = R_OUTER + 8.f, rO = R_OUTER + 24.f, rM = (rI + rO) * 0.5f;   // moved out by ½ thickness
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
      cline(_canvas, p1.x, p1.y, p2.x, p2.y, CLR_TEXT, 2); }
    lv_point_t sp2 = wp(CX, CY, rM, 180.f - HALF + 4.f);   // stbd end (right)
    ctext(_canvas, sp2.x - 13.f, sp2.y - 7.f, 26.f, FONT_TINY, CLR_GREEN, LV_TEXT_ALIGN_CENTER, T(STR_WIND_STB));
    lv_point_t bp2 = wp(CX, CY, rM, 180.f + HALF - 4.f);   // port end (left)
    ctext(_canvas, bp2.x - 13.f, bp2.y - 7.f, 26.f, FONT_TINY, CLR_RED, LV_TEXT_ALIGN_CENTER, T(STR_WIND_PT));
    char buf[10]; snprintf(buf, sizeof(buf), "%+d\xc2\xb0", (int)(rudderDeg + (rudderDeg >= 0.f ? 0.5f : -0.5f)));
    lv_point_t vp = wp(CX, CY, rM, 180.f);
    ctext(_canvas, vp.x - 26.f, vp.y - 15.f, 52.f, FONT_SMALL, CLR_ON_ACCENT, LV_TEXT_ALIGN_CENTER, buf);
}

void WindScreen::drawCornerKpis(float stw, float twa, float awa, float tws) {
    char buf[16];
    const float TY  = 4.f,   TV  = 22.f;   // top label / value y
    const float BY  = 430.f, BV  = 446.f;  // bottom label / value y
    const float LX  = 6.f,   RX  = 474.f;
    const float TW  = 152.f;               // top blocks — wide (room above the circle)
    const float BWL = 96.f,  BWR = 126.f;  // bottom blocks flank the wider polar bar

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
    ctext(_canvas, CX - 95.f, CY + 29.f * BOAT_S, 190.f,
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
    const float bw = 56.f, bh = 18.f;
    const float bx = CX - bw * 0.5f;
    const float by = CY - 35.f * BOAT_S;

    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col; rd.bg_opa = 220;
    rd.radius   = 4;   rd.border_width = 0;
    lv_canvas_draw_rect(_canvas,
        (lv_coord_t)bx, (lv_coord_t)by,
        (lv_coord_t)bw, (lv_coord_t)bh, &rd);

    ctext(_canvas, bx, by + 3.f, bw,
          FONT_TINY, CLR_TEXT, LV_TEXT_ALIGN_CENTER, label);
}

// ── arcRing helper ────────────────────────────────────────────────────────────
// Draws a donut segment using overlapping short line-segments.
// startWind / sweep: wind-angle convention (0° = top, CW positive).

void WindScreen::arcRing(float innerR, float outerR,
                          float startWind, float sweep,
                          lv_color_t col, lv_opa_t opa) {
    if (sweep >= 359.f) {
        // Full ring → lv_canvas_draw_arc handles a complete circle cleanly.
        float midR = (innerR + outerR) / 2.f;
        lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
        d.color = col; d.width = (lv_coord_t)(outerR - innerR + 1.f); d.opa = opa;
        lv_canvas_draw_arc(_canvas, (lv_coord_t)CX, (lv_coord_t)CY, (lv_coord_t)midR, 0, 360, &d);
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
    if (++yc % 4 == 0) vTaskDelay(pdMS_TO_TICKS(1));          // yield SPI0 for WiFi beacon ISR
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
    cline(_canvas, tipX, tipY, blX, blY, col, 2, opa);
    cline(_canvas, tipX, tipY, brX, brY, col, 2, opa);
    cline(_canvas, blX,  blY,  brX, brY, col, 1, opa);
}

// ── dashedRadial helper ───────────────────────────────────────────────────────

void WindScreen::dashedRadial(float windAngle, float r1, float r2, lv_color_t col) {
    float a    = windAngle * DEG2RAD;
    float sinA = sinf(a), cosA = cosf(a);

    const float dash = 5.f, gap = 4.f;
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

