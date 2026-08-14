#include "CanvasDraw.h"
#include <Arduino.h>    // vTaskDelay / pdMS_TO_TICKS (sim: sim/arduino_stubs.h)
#include <math.h>

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
        if ((y & 31) == 0) vTaskDelay(pdMS_TO_TICKS(1));   // yield: no yield point inside LVGL draws
    }
    lv_obj_invalidate(cv);
}

void cdFillTri(lv_obj_t *cv, lv_point_t a, lv_point_t b, lv_point_t c, lv_color_t col) {
    lv_point_t p[3] = { a, b, c };
    cdFillPoly(cv, p, 3, col);
}
