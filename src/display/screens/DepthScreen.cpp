// DepthScreen.cpp – Marine depth sounder with a large depth readout
//
// Text rendering strategy:
//   The depth value + unit ("12.4 m") is drawn directly with FONT_DEPTH, a real
//   96 px antialiased LVGL font (digits, '.', '-', space, m/f/t only) generated
//   by tools/gen_depth_font.py → src/display/fonts/depth_font_96.c.
//   This replaces the old 48 px + pixel-doubling trick (which looked blocky).

#include "DepthScreen.h"
#include "../../config/Config.h"
#include "../UiConfig.h"
#include <string.h>
#include <math.h>

static constexpr float FT_PER_M = 3.28084f;


static void ctext(lv_obj_t *cv, int x, int y, int mw,
                  const lv_font_t *f, lv_color_t col, const char *txt,
                  lv_text_align_t a = LV_TEXT_ALIGN_LEFT)
{
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = f; d.color = col; d.opa = LV_OPA_COVER; d.align = a;
    lv_canvas_draw_text(cv, (lv_coord_t)x, (lv_coord_t)y,
                            (lv_coord_t)mw, &d, txt);
}

// ── create ─────────────────────────────────────────────────────────────────────

void DepthScreen::create(lv_obj_t *parent)
{
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H - NAV_BAR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, OPA_FULL, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Main full-screen canvas
    const int H = SCREEN_H - NAV_BAR_H;
    if (!_cbuf) _cbuf = (lv_color_t *)PsramArena::alloc(   // reuse on live theme rebuild
                LV_CANVAS_BUF_SIZE_TRUE_COLOR(CW, H));
    if (_cbuf) {
        _canvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_canvas, _cbuf, CW, H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(_canvas, 0, 0);
    }

    // Alarm overlay (LVGL label, always on top of canvas)
    _lblAlarm = lv_label_create(container);
    lv_label_set_text(_lblAlarm, "");
    lv_obj_set_style_text_font(_lblAlarm, FONT_MED, 0);
    lv_obj_set_style_text_color(_lblAlarm, CLR_RED, 0);
    lv_obj_align(_lblAlarm, LV_ALIGN_TOP_MID, 0, 136);
}

// ── drawTopSection ─────────────────────────────────────────────────────────────
// Renders the depth value + unit at 96 px directly with FONT_DEPTH.

void DepthScreen::drawTopSection(const char *depthStr, const char *unitStr,
                                  bool alarm)
{
    if (!_cbuf || !_canvas) return;

    const int W = CW, TOP = 160;
    const lv_color_t BG = (uiTheme.depthBg);

    // ── Fill top header area ───────────────────────────────────────────────
    for (int r = 0; r < TOP; r++) {
        lv_color_t *row = _cbuf + r * W;
        for (int x = 0; x < W; x++) row[x] = BG;
    }
    // Accent separator
    {
        lv_color_t acc = (uiTheme.depthGrad);
        lv_color_t *row = _cbuf + (TOP - 2) * W;
        for (int x = 0; x < W; x++) { row[x] = acc; row[W+x] = acc; }
    }

    // ── "DEPTH" caption on main canvas ────────────────────────────────────
    ctext(_canvas, 0, 10, W, FONT_SMALL,
          (uiTheme.depthHdr), T(STR_DEPTH_CAPTION),
          LV_TEXT_ALIGN_CENTER);

    // ── Depth value: "12.4 m" drawn at 96 px, centred under the caption ────
    char combined[24];
    snprintf(combined, sizeof(combined), "%s %s", depthStr, unitStr);

    lv_color_t textCol = alarm ? CLR_RED : (uiTheme.depthVal);
    // FONT_DEPTH (Montserrat-Medium 96 px): line_height ≈ 118, caps ~70 px.
    // y=36 keeps the digits clear of the caption (~y28) and visually centred in
    // the 160 px header without clipping the baseline against the separator.
    ctext(_canvas, 0, 36, W, FONT_DEPTH, textCol,
          combined, LV_TEXT_ALIGN_CENTER);

    lv_obj_invalidate(_canvas);
}

// ── drawWaterSection ───────────────────────────────────────────────────────────

void DepthScreen::drawWaterSection(const float *hist, int histIdx,
                                    bool histFull, float currentDepth)
{
    if (!_canvas || !_cbuf) return;

    const int W = CW, TOP_Y = 160;
    const int H = (SCREEN_H - NAV_BAR_H) - TOP_Y;
    const int TOTAL = DataModel::DEPTH_HIST;
    int count = histFull ? TOTAL : histIdx;

    float maxM = 2.0f;
    for (int i = 0; i < count; i++) {
        int bi = histFull ? (histIdx + i) % TOTAL : i;
        if (hist[bi] > maxM) maxM = hist[bi];
    }
    maxM *= 1.20f;
    if (maxM < 2.0f) maxM = 2.0f;

    static int profileY[480];
    for (int x = 0; x < W; x++) {
        float d = 0.f;
        if (count > 0) {
            int age = W - 1 - x;
            int off = age * count / W;
            if (off < count) {
                int pos = (histIdx - 1 - off + TOTAL * 4) % TOTAL;
                d = hist[pos];
                if (d < 0) d = 0;
            }
        }
        int yp = (int)(d / maxM * H);
        if (yp < 0) yp = 0;
        if (yp >= H) yp = H - 1;
        profileY[x] = yp;
    }

    lv_color_t *base = _cbuf + (size_t)TOP_Y * W;
    for (int y = 0; y < H; y++) {
        float yFrac = (float)y / H;
        lv_color_t *row = base + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            int yp = profileY[x];
            lv_color_t c;
            if (y < yp - 1) {
                // Water column: depthGrad at the top, fading into depthBg
                // towards the bottom. Previously this was a hard-wired blue
                // gradient that stayed the same in EVERY lighting mode - i.e.
                // blue instead of red in night mode.
                c = lv_color_mix((uiTheme.depthGrad), (uiTheme.depthBg),
                                 (uint8_t)(255.f * (1.f - yFrac)));
            } else if (y <= yp + 1) {
                c = (uiTheme.depthEcho);
            } else {
                // Seabed: depthBed directly below the echo, fading into depthBg
                // towards the bottom (previously fixed brown to black).
                float gf = (float)(y - yp) / (H - yp + 1);
                if (gf > 1.f) gf = 1.f;
                c = lv_color_mix((uiTheme.depthBed), (uiTheme.depthBg),
                                 (uint8_t)(255.f * (1.f - gf)));
            }
            row[x] = c;
        }
        if ((y & 7) == 7) vTaskDelay(pdMS_TO_TICKS(1));
    }
    lv_obj_invalidate(_canvas);

    // Scale marks
    lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);
    td.font = FONT_TINY; td.opa = LV_OPA_COVER;
    for (int i = 0; i <= 5; i++) {
        float frac = (float)i / 5;
        int yabs = TOP_Y + (int)(frac * (H - 1));
        lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
        ld.color = (uiTheme.depthGrid); ld.width = 1; ld.opa = 180;
        lv_point_t pl[2] = {{0,(lv_coord_t)yabs},{18,(lv_coord_t)yabs}};
        lv_canvas_draw_line(_canvas, pl, 2, &ld);
        char tbuf[10]; snprintf(tbuf, sizeof(tbuf), "%.1f", frac * maxM);
        td.color = (uiTheme.depthScale);
        lv_canvas_draw_text(_canvas, 2, yabs - 7, 34, &td, tbuf);
    }

    // NOW marker
    {
        lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
        ld.color = (uiTheme.depthNow); ld.width = 2; ld.opa = 150;
        lv_point_t pn[2] = {{(lv_coord_t)(W-3),(lv_coord_t)TOP_Y},
                              {(lv_coord_t)(W-3),(lv_coord_t)(TOP_Y+H-1)}};
        lv_canvas_draw_line(_canvas, pn, 2, &ld);
        td.color = (uiTheme.depthNow);
        lv_canvas_draw_text(_canvas, W-28, TOP_Y+3, 26, &td, T(STR_DEPTH_NOW));
    }

    // Current depth guide line
    if (currentDepth > 0.f && currentDepth <= maxM) {
        int yc = TOP_Y + (int)(currentDepth / maxM * H);
        lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
        ld.color = CLR_TEXT; ld.width = 1; ld.opa = 70;
        for (int xx = 24; xx < W - 30; xx += 10) {
            lv_point_t pd[2] = {{(lv_coord_t)xx,(lv_coord_t)yc},
                                  {(lv_coord_t)(xx+5),(lv_coord_t)yc}};
            lv_canvas_draw_line(_canvas, pd, 2, &ld);
        }
    }

    lv_obj_invalidate(_canvas);
}

// ── update ─────────────────────────────────────────────────────────────────────

void DepthScreen::update()
{
    float depth;
    float histBuf[DataModel::DEPTH_HIST];
    int   histIdx;
    bool  histFull;
    {
        auto lk = data.lock();
        depth    = data.depth;
        histIdx  = data.depthHistIdx;
        histFull = data.depthHistFull;
        memcpy(histBuf, data.depthHistory, sizeof(histBuf));
    }

    bool useImp = (strcmp(appConfig.cfg.depthUnit, "ft") == 0);
    float dv = (!isnan(depth) && useImp) ? depth * FT_PER_M : depth;
    const char *unit = useImp ? "ft" : "m";
    bool alarm = false;

    char numStr[12];
    if (isnan(depth)) snprintf(numStr, sizeof(numStr), "--.-");
    else {
        snprintf(numStr, sizeof(numStr), "%.1f", dv);
        float a = appConfig.cfg.depthAlarm;
        alarm = (a > 0 && depth < a);
    }

    if (alarm) {
        char ab[40];
        float ad = useImp ? appConfig.cfg.depthAlarm*FT_PER_M
                          : appConfig.cfg.depthAlarm;
        snprintf(ab, sizeof(ab), LV_SYMBOL_WARNING " %s < %.1f%s",
                 T(STR_DEPTH_SHALLOW), ad, unit);
        lv_label_set_text(_lblAlarm, ab);
    } else {
        lv_label_set_text(_lblAlarm, "");
    }

    if (!_canvas || !_cbuf) return;

    // Yield-safe full canvas clear
    {
        const int totalRows = SCREEN_H - NAV_BAR_H;
        lv_color_t bg = CLR_BG, *p = _cbuf;
        for (int r = 0; r < totalRows; r += 8) {
            int rows = ((totalRows-r) < 8) ? (totalRows-r) : 8;
            lv_color_t *end = p + (size_t)rows * CW;
            while (p < end) *p++ = bg;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        lv_obj_invalidate(_canvas);
    }

    drawTopSection(numStr, unit, alarm);

    if (histIdx >= 2 || histFull)
        drawWaterSection(histBuf, histIdx, histFull, isnan(depth) ? 0.f : depth);
}
