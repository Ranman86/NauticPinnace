// WindPlotScreen.cpp - wind rose of the last six minutes of TWD/TWS
//
// Paint cost: a frame measured up to 93 ms on the 7B - every frame, whether
// anything had changed or not, and LVGL then re-rendered the whole canvas.
// What brings that down, each without changing a single pixel (the reasons
// are at the code): a frame whose layout equals the painted one is not
// painted at all, and the stats line is written only when its text changes
// (update()); the 377 LVGL line, label and rect draws of a frame run in two
// draw contexts instead of 377 (CdLvBatch), each ring as one polyline whose
// sines and cosines are taken once for all four rings; the petals no longer
// invalidate the canvas one by one, and the histogram is built straight from
// the ring instead of from a 4.3 KB copy of it on the stack (paintRose(),
// buildLayout()).

#include "WindPlotScreen.h"
#include "RenderYield.h"
#include "../../PsramArena.h"
#include "../CanvasDraw.h"
#include "../../i18n/I18n.h"
#include "PaintPhase.h"
#include <math.h>
#include <string.h>

// ── Paint phases (measurement builds only, see PaintPhase.h) ──────────────────
// Eight phases: data lock + histogram + layout in update(), every frame; the
// canvas clear; the four rings; the eight spokes WITH their cardinal letters,
// and the letters alone (card, also inside spk - spk minus card is the
// spokes); the petals; the centre dot with the one invalidate (fin); the
// stats line, every frame. A frame whose layout equals the painted one ends
// after pre and lbl, so the canvas phases print x<cnt>, and a rose with fewer
// than two samples has no petal phase. aux pet = petals of a frame, rimax =
// its largest petal radius in px; both window maxima.
#if PP_ENABLED
enum { WP_PH_PRE = 0, WP_PH_CLR, WP_PH_RING, WP_PH_SPK, WP_PH_CARD,
       WP_PH_PET, WP_PH_FIN, WP_PH_LBL, WP_PH_N };
enum { WP_AUX_PET = 0, WP_AUX_RI };
static PaintPhaseSet s_pp;

// Declared extern at the call site (main.cpp), like windPhaseTake().
void wplotPhaseReport() {
    static const char *const names[WP_PH_N] = {
        "pre", "clr", "ring", "spk", "card", "petal", "fin", "lbl" };
    static const char *const aux[PP_MAX_AUX] = { "pet", "rimax" };
    ppReport("wpl", names, WP_PH_N, s_pp, aux);
}
#endif

void WindPlotScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H - NAV_BAR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, OPA_FULL, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(CS, CS);
    if (!_cbuf) _cbuf = (lv_color_t*)PsramArena::alloc(sz);   // reuse on live theme rebuild
    if (_cbuf) {
        // Buffer pre-zeroed by PsramArena::init() (before WiFi).
        _canvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_canvas, _cbuf, CS, CS, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(_canvas, LV_ALIGN_TOP_MID, 0, 0);
    } else {
        Serial.printf("[WindPlotScreen] canvas buf alloc FAILED (%u B)\n", (unsigned)sz);
        Serial.flush();
    }

    _statsLbl = lv_label_create(container);
    lv_label_set_text(_statsLbl, "");
    styleLabel(_statsLbl, FONT_SMALL, CLR_TEXT_DIM);
    lv_obj_align(_statsLbl, LV_ALIGN_BOTTOM_MID, 0, UI_S(-4));

    // New objects, possibly a new theme, font or language - and a reused
    // buffer that still holds the old picture: the next update() paints in
    // full, whatever the layout says.
    _painted = false;
}

// Layout pass: the histogram and, per sector, the integers paintRose() draws
// its petal from - by exactly the expressions the one-pass drawWindRose()
// evaluated while it painted, so the key and the picture cannot disagree.
// Also the stats text; setStats = false leaves the label as it is (samples,
// but not one with a TWS).
void WindPlotScreen::buildLayout(RoseLayout &L, char *stats, size_t statsSz, bool &setStats) {
    memset(&L, 0, sizeof L);
    L.lang = (uint8_t)i18nLang();

    // Histogram under the lock, straight from the ring. It used to copy the
    // whole ring (4.3 KB) onto the loop task's stack first and count the copy:
    // the same samples in the same order, so the same sums - and nothing but
    // the histogram ever read the copy. One valid TWD/TWS sample per second
    // (WindPipeline::tick), so a full ring is the last six minutes with true
    // wind and the sample count in the stats line is seconds.
    int count = 0;
    float sectorSum[SECTORS] = {};
    int   sectorCnt[SECTORS] = {};
    float sumTws = 0, cntTws = 0;
    {
        auto lk = data.lock();
        const WindSample *hist = data.windHistory;
        // Null only with the arena AND the heap exhausted (initBuffers());
        // the memcpy of the copy went through a null pointer there.
        if (hist) {
            const int histIdx = data.windHistIdx;
            count = data.windHistFull ? DataModel::WIND_HIST : histIdx;
            if (count >= 2) {
                for (int i=0;i<count;i++) {
                    int idx = (histIdx - count + i + DataModel::WIND_HIST) % DataModel::WIND_HIST;
                    float twd = hist[idx].twd;
                    float tws = hist[idx].tws;
                    if (isnan(twd)) continue;
                    int sector = ((int)(twd / 10)) % SECTORS;
                    if (!isnan(tws)) { sectorSum[sector] += tws; sectorCnt[sector]++; sumTws+=tws; cntTws++; }
                    else               sectorCnt[sector]++;
                }
            }
        }
    }

    // An empty ring (fresh boot, demo switched off) also empties the stats
    // line; it used to keep the last figures standing over a blank rose.
    if (count < 2) {
        L.empty = 1;
        stats[0] = '\0';
        setStats = true;
        return;
    }

    const int maxR = CS/2 - UI_S(20);
    int maxCnt = 1;
    for (int s=0;s<SECTORS;s++) if (sectorCnt[s]>maxCnt) maxCnt=sectorCnt[s];

    for (int s=0;s<SECTORS;s++) {
        if (sectorCnt[s]==0) { L.cls[s] = 0xFF; continue; }
        float ratio = (float)sectorCnt[s] / maxCnt;
        int ri = (int)(ratio * (maxR - UI_S(15))) + UI_S(5);
        float avgTws = sectorCnt[s]>0 ? sectorSum[s]/sectorCnt[s] : 0;

        // Colour by wind speed: the class paintRose() picks the colour by.
        uint8_t cls;
        if      (avgTws < 8)  cls = 0;   // CLR_GREEN
        else if (avgTws < 14) cls = 1;   // CLR_YELLOW
        else if (avgTws < 20) cls = 2;   // CLR_ORANGE
        else                  cls = 3;   // CLR_RED
        L.cls[s] = cls;
        L.ri[s]  = (int16_t)ri;          // 6..212 on the 7B/5B, 5..170 on the 4"
    }

    // Stats
    setStats = cntTws > 0;
    if (setStats)
        snprintf(stats, statsSz, "%d %s  %s: %.1f kn",
                 count, T(STR_WPLOT_SAMPLES), T(STR_WPLOT_AVG_TWS), sumTws/cntTws);
}

void WindPlotScreen::paintRose(const RoseLayout &L) {
    // Chunked fill so there is somewhere to yield mid-clear; renderYield()
    // decides how many of those offers are worth a tick (see RenderYield.h).
    // The invalidate that followed it is gone: the one at the end of the
    // paint covers the whole canvas, and LVGL cannot render it in between.
    {
        const lv_color_t bgColor = CLR_BG;
        lv_color_t *p = _cbuf;
        for (int row = 0; row < CS; row += 8) {
            int rows = (CS - row < 8) ? CS - row : 8;
            lv_color_t *rowEnd = p + (size_t)rows * CS;
            while (p < rowEnd) *p++ = bgColor;
            renderYield();
        }
    }
    PP_END(s_pp, WP_PH_CLR);

    int cx = CS/2, cy = CS/2;
    int maxR = CS/2 - UI_S(20);

    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);

    // Rings, spokes and cardinal letters through one LVGL draw context: the
    // same draws in the same order with the same descriptors, but one fake
    // display instead of 376 (see CdLvBatch in CanvasDraw.h). No yield and no
    // lv_obj_* call may come while it is open - the petals, which yield, come
    // after it.
    {
        CdLvBatch b(_canvas, false);

        // Background circles: 90 chords of 4 deg per ring. Each chord used to
        // be an lv_canvas_draw_line of its own, from the point at angle a to
        // the point at a+4. Each ring is now ONE 91-point polyline, which
        // cdLvLine() draws as exactly those 90 segments in the same order
        // (point k to point k+1, as lv_canvas_draw_line does). The points are
        // the same: a chord's end, (a+4)*DEG_TO_RAD with a+4 a whole number,
        // is the next chord's start by the same expression. The sine and
        // cosine of the 91 angles are now taken once for all four rings, by
        // the same expressions, and every point is still cx + ri*sin and
        // cy - ri*cos - the same shape, so the compiler fuses (or does not
        // fuse) the multiply-add exactly as before.
        float rs[91], rc[91];
        for (int k = 0; k <= 90; k++) {
            int a = k*4;
            float a1 = a*DEG_TO_RAD;
            rs[k] = sinf(a1);
            rc[k] = cosf(a1);
        }
        lv_point_t ringPts[91];
        ld.color = CLR_GRID_LINE; ld.width = 1; ld.opa = OPA_50;
        for (int ri : {maxR/4, maxR/2, 3*maxR/4, maxR}) {
            for (int k = 0; k <= 90; k++)
                ringPts[k] = {(lv_coord_t)(cx+ri*rs[k]),(lv_coord_t)(cy-ri*rc[k])};
            cdLvLine(b, ringPts, 91, &ld);
        }
        PP_END(s_pp, WP_PH_RING);

        // Spoke lines (every 45 deg). Two-point lines each, NOT one polyline
        // through the centre: going back over a spoke would blend it twice.
        ld.color = CLR_BORDER; ld.width = 1;
        // Cardinal points: German Ost = "O", English East = "E" (see strings_screens_c.inc)
        const char *cards[] = { T(STR_WPLOT_C_N),  T(STR_WPLOT_C_NE), T(STR_WPLOT_C_E),  T(STR_WPLOT_C_SE),
                                T(STR_WPLOT_C_S),  T(STR_WPLOT_C_SW), T(STR_WPLOT_C_W),  T(STR_WPLOT_C_NW) };
        td.font = FONT_SMALL; td.opa = OPA_FULL;
        PP_LAP_DECL(cardLap);
        for (int i=0;i<8;i++) {
            float ar = i*45*DEG_TO_RAD;
            lv_point_t p1={(lv_coord_t)cx,(lv_coord_t)cy};
            lv_point_t p2={(lv_coord_t)(cx+(int)(maxR*sinf(ar))),(lv_coord_t)(cy-(int)(maxR*cosf(ar)))};
            lv_point_t _l[2]={p1,p2}; cdLvLine(b,_l,2,&ld);
            // Cardinal labels. The 7B needs a wider box: the font pass bumps
            // FONT_SMALL 14 -> 18 and the intercardinals ("NW"/"SO") no longer
            // fit 20 px - they wrapped onto two lines. The 4" keeps its released
            // 16 px box and offset untouched (pixel identity).
#if defined(BOARD_PANEL_1024X600)
            // 40 px: "NW" is the widest pair (two wide glyphs) and needs ~36 px
            // at FONT_SMALL 18 - 34 was one wrap short of it. Centred on the
            // spoke: left-aligned from 18 px left of it, a single letter sat
            // off its spoke and the "W" began 5 px left of the canvas, which
            // cut its first stroke off.
            const int cardBoxW = 40, cardBoxOffX = -20;
            td.align = LV_TEXT_ALIGN_CENTER;
#else
            const int cardBoxW = 16, cardBoxOffX = -6;
#endif
            int lx=(int)(cx+(maxR+UI_S(10))*sinf(ar))+cardBoxOffX;
            int ly=(int)(cy-(maxR+UI_S(10))*cosf(ar))+UI_S(-6);
            td.color = CLR_TEXT_DIM;
            PP_LAP_START(cardLap);
            cdLvText(b, (lv_coord_t)lx, (lv_coord_t)ly, cardBoxW, &td, cards[i]);
            PP_LAP_STOP(cardLap);
        }
        PP_ADD(s_pp, WP_PH_CARD, cardLap.us);
        PP_END(s_pp, WP_PH_SPK);
    }

    // Fewer than two samples: the grid alone, no petals and no dot.
    if (L.empty) {
        lv_obj_invalidate(_canvas);
        PP_END(s_pp, WP_PH_FIN);
        return;
    }

    // Draw petals
    const CdTarget tg = cdTargetOf(_canvas);
    PP_ONLY(int ppPet = 0, ppRi = 0);
    for (int s=0;s<SECTORS;s++) {
        if (L.cls[s] == 0xFF) continue;
        const int ri = L.ri[s];

        // Colour by wind speed (the class comes from buildLayout())
        lv_color_t col;
        if      (L.cls[s] == 0) col = CLR_GREEN;
        else if (L.cls[s] == 1) col = CLR_YELLOW;
        else if (L.cls[s] == 2) col = CLR_ORANGE;
        else                    col = CLR_RED;

        // Petal = one filled sector polygon (centre + the 10° rim arc). The old
        // version drew a fan of 10 radial strokes, which combed apart near the
        // rim (2 px strokes ~3.7 px apart) and piled its alpha up at the centre.
        // Pre-mixed against the background so it keeps the translucent look.
        {
            // Spans a hair over 10° so neighbouring petals overlap: sharing an
            // exact edge leaves a 1 px hairline seam between them.
            lv_point_t poly[13];
            int n = 0;
            poly[n++] = {(lv_coord_t)cx, (lv_coord_t)cy};
            for (int da = 0; da <= 10; da++) {
                float a = (s * 10 - 5.4f + da * 1.08f) * DEG_TO_RAD;
                poly[n++] = {(lv_coord_t)(cx + ri * sinf(a)),
                             (lv_coord_t)(cy - ri * cosf(a))};
            }
            // cdFillPolyOpa, opaque, rather than cdFillPoly: the same
            // scanline fill to the pixel - every edge is still taken from its
            // lower-numbered vertex (the closing one from the last), so the
            // crossings come out of the same expressions, only collected in
            // another order before the sort; the spans round the same way and
            // are stored opaque. What it leaves out is the invalidate of the
            // whole canvas after every petal. It yields every 32 rows, as
            // cdFillPoly did on every 32nd row.
            cdFillPolyOpa(tg, poly, n, cdMix(col, CLR_BG, OPA_75), LV_OPA_COVER, 32);
        }
        PP_ONLY(ppPet++; if (ri > ppRi) ppRi = ri);
    }
    PP_AUX(s_pp, WP_AUX_PET, ppPet);
    PP_AUX(s_pp, WP_AUX_RI, ppRi);
    PP_END(s_pp, WP_PH_PET);

    // Centre dot
    rd.bg_color = CLR_TEXT; rd.bg_opa = OPA_FULL; rd.radius = LV_RADIUS_CIRCLE;
    {
        CdLvBatch b(_canvas, false);
        cdLvRect(b, (lv_coord_t)(cx-UI_S(4)), (lv_coord_t)(cy-UI_S(4)), UI_S(8), UI_S(8), &rd);
    }

    lv_obj_invalidate(_canvas);
    PP_END(s_pp, WP_PH_FIN);
}

// lv_label_set_text() frees, reallocates and re-measures the text and
// invalidates the label even when the text is the same. Only touch it when
// the text really changes.
static void setLabelIfChanged(lv_obj_t *lbl, const char *txt) {
    if (!lbl) return;
    const char *cur = lv_label_get_text(lbl);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(lbl, txt);
}

void WindPlotScreen::update() {
    PP_BEGIN(s_pp);
    if (!_canvas || !_cbuf) { PP_SKIP(s_pp); return; }

    RoseLayout L;
    char stats[64];
    bool setStats = false;
    buildLayout(L, stats, sizeof stats, setStats);
    PP_END(s_pp, WP_PH_PRE);

    // Skip a frame whose layout equals the one on the canvas: no clear, no
    // paint, no invalidate. The buffer belongs to this screen alone, so that
    // frame is still in it - also after the screen was hidden - and LVGL goes
    // on showing it. The layout holds every integer the petals are drawn from
    // and the language of the letters; the rings, spokes and dot are the same
    // in every frame. The ring gains a sample once a second at most, and in a
    // steady wind the one it drops often lands in the same sector, so most
    // frames end here.
    if (!_painted || memcmp(&L, &_lay, sizeof L) != 0) {
        paintRose(L);
        _lay     = L;
        _painted = true;
    }

    if (setStats) setLabelIfChanged(_statsLbl, stats);
    PP_END(s_pp, WP_PH_LBL);
    PP_FRAME(s_pp);
}
