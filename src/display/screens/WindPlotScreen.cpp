#include "WindPlotScreen.h"
#include "../../PsramArena.h"
#include "../CanvasDraw.h"
#include "../../i18n/I18n.h"
#include <math.h>
#include <string.h>

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
    lv_obj_align(_statsLbl, LV_ALIGN_BOTTOM_MID, 0, -4);
}

void WindPlotScreen::drawWindRose() {
    if (!_canvas || !_cbuf) return;
    // Yield-safe fill: prevents IWDT from WiFi-ISR SPI0 starvation (see WindScreen.cpp)
    {
        const lv_color_t bgColor = CLR_BG;
        lv_color_t *p = _cbuf;
        for (int row = 0; row < CS; row += 8) {
            int rows = (CS - row < 8) ? CS - row : 8;
            lv_color_t *rowEnd = p + (size_t)rows * CS;
            while (p < rowEnd) *p++ = bgColor;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        lv_obj_invalidate(_canvas);
    }

    // Copy history under lock
    WindSample hist[DataModel::WIND_HIST];
    int histIdx, count;
    bool full;
    {
        auto lk = data.lock();
        histIdx = data.windHistIdx;
        full    = data.windHistFull;
        count   = full ? DataModel::WIND_HIST : histIdx;
        memcpy(hist, data.windHistory, sizeof(WindSample)*DataModel::WIND_HIST);
    }

    int cx = CS/2, cy = CS/2;
    int maxR = CS/2 - 20;

    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);

    // Background circles
    ld.color = CLR_GRID_LINE; ld.width = 1; ld.opa = OPA_50;
    for (int ri : {maxR/4, maxR/2, 3*maxR/4, maxR}) {
        for (int a=0;a<360;a+=4) {
            float a1=a*DEG_TO_RAD, a2=(a+4)*DEG_TO_RAD;
            lv_point_t p1={(lv_coord_t)(cx+ri*sinf(a1)),(lv_coord_t)(cy-ri*cosf(a1))};
            lv_point_t p2={(lv_coord_t)(cx+ri*sinf(a2)),(lv_coord_t)(cy-ri*cosf(a2))};
            lv_point_t _l[2]={p1,p2}; lv_canvas_draw_line(_canvas,_l,2,&ld);
        }
    }

    // Spoke lines (every 45 deg)
    ld.color = CLR_BORDER; ld.width = 1;
    // Cardinal points: German Ost = "O", English East = "E" (see strings_screens_c.inc)
    const char *cards[] = { T(STR_WPLOT_C_N),  T(STR_WPLOT_C_NE), T(STR_WPLOT_C_E),  T(STR_WPLOT_C_SE),
                            T(STR_WPLOT_C_S),  T(STR_WPLOT_C_SW), T(STR_WPLOT_C_W),  T(STR_WPLOT_C_NW) };
    td.font = FONT_SMALL; td.opa = OPA_FULL;
    for (int i=0;i<8;i++) {
        float ar = i*45*DEG_TO_RAD;
        lv_point_t p1={(lv_coord_t)cx,(lv_coord_t)cy};
        lv_point_t p2={(lv_coord_t)(cx+(int)(maxR*sinf(ar))),(lv_coord_t)(cy-(int)(maxR*cosf(ar)))};
        lv_point_t _l[2]={p1,p2}; lv_canvas_draw_line(_canvas,_l,2,&ld);
        // Cardinal labels
        int lx=(int)(cx+(maxR+10)*sinf(ar))-6;
        int ly=(int)(cy-(maxR+10)*cosf(ar))-6;
        td.color = CLR_TEXT_DIM;
        lv_canvas_draw_text(_canvas, (lv_coord_t)lx, (lv_coord_t)ly, 16, &td, cards[i]);
    }

    if (count < 2) { lv_obj_invalidate(_canvas); return; }

    // Build 36-sector histogram (10 deg each)
    static const int SECTORS = 36;
    float sectorSum[SECTORS] = {};
    int   sectorCnt[SECTORS] = {};
    float sumTws = 0, cntTws = 0;

    for (int i=0;i<count;i++) {
        int idx = (histIdx - count + i + DataModel::WIND_HIST) % DataModel::WIND_HIST;
        float twd = hist[idx].twd;
        float tws = hist[idx].tws;
        if (isnan(twd)) continue;
        int sector = ((int)(twd / 10)) % SECTORS;
        if (!isnan(tws)) { sectorSum[sector] += tws; sectorCnt[sector]++; sumTws+=tws; cntTws++; }
        else               sectorCnt[sector]++;
    }

    int maxCnt = 1;
    for (int s=0;s<SECTORS;s++) if (sectorCnt[s]>maxCnt) maxCnt=sectorCnt[s];

    // Draw petals
    for (int s=0;s<SECTORS;s++) {
        if (sectorCnt[s]==0) continue;
        float ratio = (float)sectorCnt[s] / maxCnt;
        int ri = (int)(ratio * (maxR - 15)) + 5;
        float avgTws = sectorCnt[s]>0 ? sectorSum[s]/sectorCnt[s] : 0;

        // Colour by wind speed
        lv_color_t col;
        if      (avgTws < 8)  col = CLR_GREEN;
        else if (avgTws < 14) col = CLR_YELLOW;
        else if (avgTws < 20) col = CLR_ORANGE;
        else                  col = CLR_RED;

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
            cdFillPoly(_canvas, poly, n, cdMix(col, CLR_BG, OPA_75));
        }
    }

    // Centre dot
    rd.bg_color = CLR_TEXT; rd.bg_opa = OPA_FULL; rd.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(_canvas,
        (lv_coord_t)(cx-4), (lv_coord_t)(cy-4), 8, 8, &rd);

    lv_obj_invalidate(_canvas);

    // Stats
    if (cntTws > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d %s  %s: %.1f kn",
                 count, T(STR_WPLOT_SAMPLES), T(STR_WPLOT_AVG_TWS), sumTws/cntTws);
        lv_label_set_text(_statsLbl, buf);
    }
}

void WindPlotScreen::update() {
    drawWindRose();
}
