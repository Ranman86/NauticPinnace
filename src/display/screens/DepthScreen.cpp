// DepthScreen.cpp – Marine depth sounder with a large depth readout
//
// Text rendering strategy:
//   The depth value + unit ("12.4 m") is drawn directly with FONT_DEPTH, a real
//   96 px antialiased LVGL font (digits, '.', '-', space, m/f/t only) generated
//   by tools/gen_depth_font.py → src/display/fonts/depth_font_96.c.
//   This replaces the old 48 px + pixel-doubling trick (which looked blocky).
//
// Paint cost: a frame with the echogram measured 141-167 ms on the 7B, one
// without it 48-57 ms - every frame, whether anything had changed or not.
// What brings that down, each without changing a single pixel (the reasons
// are at the code): a frame whose inputs are unchanged is not painted at all
// (update()); the full-canvas clear is gone, the header and water fills store
// every pixel once before the texts and marks blend onto them (update(),
// drawTopSection()); the water colour is computed once per row and the seabed
// colour without a float division (drawWaterSection(), bedMix()); the 57 LVGL
// line and label draws of a frame run in two draw contexts instead of 57
// (CdLvBatch), and the canvas is invalidated once per frame instead of about
// 60 times.

#include "DepthScreen.h"
#include "../Icons.h"   // OpenBridge icon trial
#include "RenderYield.h"
#include "../../PsramArena.h"
#include "../../config/Config.h"
#include "../UiConfig.h"
#include "../CanvasDraw.h"
#include "PaintPhase.h"
#include <string.h>
#include <math.h>

static constexpr float FT_PER_M = 3.28084f;

// ── Paint phases (measurement builds only, see PaintPhase.h) ──────────────────
// Eight phases in draw order: data lock + depth text + alarm label + frame key
// in update(); the clear of the water rows, which only a frame WITHOUT an
// echogram has; the header fill; caption + depth value; the echogram fill
// (profile, then every water/echo/seabed pixel); scale marks with their
// labels; the NOW marker; the current-depth guide line, which ends with the
// one invalidate. A frame whose key equals the painted one ends after pre, a
// frame without an echogram after htxt, so the later phases print x<cnt>, and
// clr runs only in frames without an echogram. aux bed = seabed pixels of a
// frame, fb = how many of them took bedMix()'s float fallback; both window
// maxima over painted frames, counted outside the timed phases.
#if PP_ENABLED
enum { DP_PH_PRE = 0, DP_PH_CLR, DP_PH_HDR, DP_PH_HTXT, DP_PH_WFILL,
       DP_PH_SCALE, DP_PH_NOW, DP_PH_GUIDE, DP_PH_N };
enum { DP_AUX_BED = 0, DP_AUX_FB };
static PaintPhaseSet s_pp;

// Declared extern at the call site (main.cpp), like windPhaseTake().
void depthPhaseReport() {
    static const char *const names[DP_PH_N] = {
        "pre", "clr", "hdr", "htxt", "wfill", "scale", "now", "guide" };
    static const char *const aux[PP_MAX_AUX] = { "bed", "fb" };
    ppReport("dep", names, DP_PH_N, s_pp, aux);
}
#endif

// Canvas geometry: the header with the readout above row TOP_Y, the echogram
// (water section) of WATER_H rows below it.
static constexpr int TOP_Y   = UI_S(160);
static constexpr int WATER_H = (SCREEN_H - NAV_BAR_H) - TOP_Y;

static void ctext(CdLvBatch &b, int x, int y, int mw,
                  const lv_font_t *f, lv_color_t col, const char *txt,
                  lv_text_align_t a = LV_TEXT_ALIGN_LEFT)
{
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = f; d.color = col; d.opa = LV_OPA_COVER; d.align = a;
    cdLvText(b, (lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)mw, &d, txt);
}

// ── Seabed fade, exact in integers ────────────────────────────────────────────
// Below the echo of a column (echo row yp) the seabed fades from depthBed into
// depthBg. Its mix factor used to be computed for every seabed pixel as
//     float gf = (float)(y - yp) / (H - yp + 1);
//     if (gf > 1.f) gf = 1.f;
//     m = (uint8_t)(255.f * (1.f - gf));
// - a float division per pixel, which the ESP32-S3 does in software
// (__divsf3), and the bulk of the echogram's cost on the 7B.
//
// Exactly, 255 * (1 - (y-yp)/D) with D = H+1-yp is num/D with
// num = 255 * (H+1-y): num is the same along a whole row, D down a whole
// column. The float chain is off from num/D by less than 5e-5 (three
// roundings of at most 2^-24, scaled by 255), while a num/D that is not a
// whole number lies at least 1/D >= 1/(H+1) away from both neighbouring whole
// numbers. So wherever D does not divide num the truncated float IS
// floor(num/D), and bedMix() returns that from one multiply-high with the
// column's reciprocal bedRcp(D) = ceil(2^32/D):
//     q = (num * ceil(2^32/D)) >> 32 = floor(num/D),
// exact while num * D < 2^32 (static_assert below). Where D divides num the
// float result can land just below the whole number and truncate to one less
// - it does in about two of three such pairs (1158 of 1756 at H = 400, 939 of
// 1424 at H = 320) - so there the original float expression runs, unchanged:
// clamp included, although gf < 1 always holds here (y - yp < D). Such pairs
// are about 2 % of all seabed (yp, y) pairs.
//
// Checked exhaustively on the PC for H = 400 and 320 (7B/5B and 4") and every
// H up to 600: every (yp, y) pair of the seabed, against the original in IEEE
// single precision and against the original expression compiled natively.
static_assert(255ull * (WATER_H + 1) * (WATER_H + 1) < (1ull << 32),
              "bedMix(): num * D must stay below 2^32");

// ceil(2^32 / D) for D >= 2 (D = H+1-yp >= 2, as yp <= H-1).
static inline uint32_t bedRcp(uint32_t D) { return 0xFFFFFFFFu / D + 1u; }

static inline uint8_t bedMix(int y, int yp, int H, uint32_t num, uint32_t rcp)
{
    const uint32_t D = (uint32_t)(H + 1 - yp);
    const uint32_t q = (uint32_t)(((uint64_t)num * rcp) >> 32);
    if (num - q * D != 0) return (uint8_t)q;
    float gf = (float)(y - yp) / (H - yp + 1);
    if (gf > 1.f) gf = 1.f;
    return (uint8_t)(255.f * (1.f - gf));
}

// The 256 colours lv_color_mix(depthBed, depthBg, m) can take, so a seabed
// pixel is one table load of the very colour it always was. Rebuilt whenever
// either colour differs from the pair it was built for - in practice once
// after create(), as the theme only changes through it.
void DepthScreen::bedPalette()
{
    const lv_color_t bed = uiTheme.depthBed, bg = uiTheme.depthBg;
    if (_palOk && _palBed.full == bed.full && _palBg.full == bg.full) return;
    for (int m = 0; m < 256; m++) _bedPal[m] = lv_color_mix(bed, bg, (uint8_t)m);
    _palBed = bed;
    _palBg  = bg;
    _palOk  = true;
}

// The echogram's depth scale, from the samples in the ring.
static float depthScaleMax(const float *hist, int histIdx, bool histFull)
{
    const int TOTAL = DataModel::DEPTH_HIST;
    int count = histFull ? TOTAL : histIdx;

    float maxM = 2.0f;
    for (int i = 0; i < count; i++) {
        int bi = histFull ? (histIdx + i) % TOTAL : i;
        if (hist[bi] > maxM) maxM = hist[bi];
    }
    maxM *= 1.20f;
    if (maxM < 2.0f) maxM = 2.0f;
    return maxM;
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

    // Echogram scratch: the profile row and the seabed reciprocal of every
    // column plus the seabed palette, one PSRAM arena block instead of a
    // static DRAM array (1.9 KB of internal RAM freed by the profile alone).
    // No internal-RAM fallback on purpose: it would sit in .bss on every
    // board and every boot (5.3 KB on the 600 grid, 4.4 KB on the 480 one) to
    // cover the single case where the arena is already exhausted - i.e.
    // exactly when internal RAM is scarcest. update() skips the echogram
    // instead: the header with the depth value is still drawn, so the worst
    // outcome is a missing echogram below the readout, not a crash.
    if (!_profY) {
        uint8_t *blk = (uint8_t *)PsramArena::alloc(
            CW * sizeof(int) + CW * sizeof(uint32_t) + 256 * sizeof(lv_color_t));
        if (blk) {
            _profY  = (int *)blk;
            _rcp    = (uint32_t *)(blk + CW * sizeof(int));
            _bedPal = (lv_color_t *)(blk + CW * (sizeof(int) + sizeof(uint32_t)));
        }
    }

    // Alarm overlay (LVGL label, always on top of canvas)
    _lblAlarm = lv_label_create(container);
    lv_label_set_text(_lblAlarm, "");
    lv_obj_set_style_text_font(_lblAlarm, FONT_MED, 0);
    lv_obj_set_style_text_color(_lblAlarm, CLR_RED, 0);
    lv_obj_align(_lblAlarm, LV_ALIGN_TOP_MID, 0, UI_S(136));

    // New objects, possibly a new theme, font or language: the next update()
    // paints the canvas whatever its key, with a fresh seabed palette.
    _painted = false;
    _palOk   = false;
}

// ── drawTopSection ─────────────────────────────────────────────────────────────
// Renders the depth value + unit at 96 px directly with FONT_DEPTH.

void DepthScreen::drawTopSection(const char *depthStr, const char *unitStr,
                                  bool alarm)
{
    if (!_cbuf || !_canvas) { PP_SKIP(s_pp); return; }

    const int W = CW, TOP = TOP_Y;
    const lv_color_t BG  = (uiTheme.depthBg);
    const lv_color_t acc = (uiTheme.depthGrad);

    // ── Fill top header area ───────────────────────────────────────────────
    // Every header pixel is stored before anything blends onto it, and only
    // once: the background down to row TOP-3, then the two rows of the accent
    // separator (which used to be painted over the background).
    for (int r = 0; r < TOP; r++) {
        const lv_color_t c = (r < TOP - 2) ? BG : acc;
        lv_color_t *row = _cbuf + r * W;
        for (int x = 0; x < W; x++) row[x] = c;
    }
    PP_END(s_pp, DP_PH_HDR);

    // Both texts through one LVGL draw context; update() invalidates the
    // canvas once when the whole frame is painted.
    {
        CdLvBatch b(_canvas, false);

        // ── "DEPTH" caption on main canvas ────────────────────────────────
        ctext(b, 0, UI_S(10), W, FONT_SMALL,
              (uiTheme.depthHdr), T(STR_DEPTH_CAPTION),
              LV_TEXT_ALIGN_CENTER);

        // ── Depth value: "12.4 m" drawn at 96 px, centred under the caption ─
        char combined[24];
        snprintf(combined, sizeof(combined), "%s %s", depthStr, unitStr);

        lv_color_t textCol = alarm ? CLR_RED : (uiTheme.depthVal);
        // FONT_DEPTH (Montserrat-Medium 96 px): line_height ≈ 118, caps ~70 px.
        // Base-480 coords (scaled via UI_S): y=36 keeps the digits clear of the
        // caption (~y28) and visually centred in the 160 px (base) header without
        // clipping the baseline against the separator.
        ctext(b, 0, UI_S(36), W, FONT_DEPTH, textCol,
              combined, LV_TEXT_ALIGN_CENTER);
    }
    PP_END(s_pp, DP_PH_HTXT);
}

// ── drawWaterSection ───────────────────────────────────────────────────────────
// maxM and guideY come from update(), which keys the frame on them.

void DepthScreen::drawWaterSection(const float *hist, int histIdx,
                                    bool histFull, float maxM, int guideY)
{
    if (!_canvas || !_cbuf || !_profY) { PP_SKIP(s_pp); return; }

    const int W = CW;
    const int H = WATER_H;
    const int TOTAL = DataModel::DEPTH_HIST;
    int count = histFull ? TOTAL : histIdx;

    int      *profileY = _profY;
    uint32_t *rcp      = _rcp;
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
        rcp[x] = bedRcp((uint32_t)(H + 1 - yp));
    }
    bedPalette();

    // Every pixel of rows TOP_Y.. is stored here before anything blends onto
    // it - which is why update() no longer clears the canvas first.
    const lv_color_t grad = uiTheme.depthGrad, wbg = uiTheme.depthBg;
    const lv_color_t echo = uiTheme.depthEcho;
    const lv_color_t *pal = _bedPal;
    lv_color_t *base = _cbuf + (size_t)TOP_Y * W;
    for (int y = 0; y < H; y++) {
        float yFrac = (float)y / H;
        // Water column: depthGrad at the top, fading into depthBg towards
        // the bottom. Previously this was a hard-wired blue gradient that
        // stayed the same in EVERY lighting mode - i.e. blue instead of red
        // in night mode. Its colour depends on the row alone: the same
        // expression as ever, evaluated once per row instead of per pixel.
        const lv_color_t wcol = lv_color_mix(grad, wbg, (uint8_t)(255.f * (1.f - yFrac)));
        // Seabed: depthBed directly below the echo, fading into depthBg
        // towards the bottom (previously fixed brown to black) - see bedMix().
        const uint32_t num = 255u * (uint32_t)(H + 1 - y);
        lv_color_t *row = base + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            int yp = profileY[x];
            lv_color_t c;
            if (y < yp - 1) {
                c = wcol;
            } else if (y <= yp + 1) {
                c = echo;
            } else {
                c = pal[bedMix(y, yp, H, num, rcp[x])];
            }
            row[x] = c;
        }
        if ((y & 7) == 7) renderYield();   // see RenderYield.h
    }
    PP_END(s_pp, DP_PH_WFILL);
#if PP_ENABLED
    // Seabed pixels and float fallbacks of this frame, from the profile -
    // outside the hot loop and outside every timed phase.
    {
        uint32_t bed = 0, fb = 0;
        for (int x = 0; x < W; x++) {
            const uint32_t D = (uint32_t)(H + 1 - profileY[x]);
            for (int y = profileY[x] + 2; y < H; y++) {
                bed++;
                if ((255u * (uint32_t)(H + 1 - y)) % D == 0) fb++;
            }
        }
        PP_AUX(s_pp, DP_AUX_BED, bed);
        PP_AUX(s_pp, DP_AUX_FB, fb);
        PP_SKIP(s_pp);
    }
#endif

    // Scale labels: printed again only when the scale changes.
    uint32_t maxBits;
    memcpy(&maxBits, &maxM, sizeof maxBits);
    if (!_scaleOk || maxBits != _scaleFor) {
        for (int i = 0; i <= 5; i++) {
            float frac = (float)i / 5;
            snprintf(_scaleTxt[i], sizeof(_scaleTxt[i]), "%.1f", frac * maxM);
        }
        _scaleFor = maxBits;
        _scaleOk  = true;
    }

    // Marks, NOW marker and guide line through one LVGL draw context, in the
    // order they were always drawn in (every one blends onto what is below).
    {
        CdLvBatch b(_canvas, false);

        // Scale marks
        lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);
        td.font = FONT_TINY; td.opa = LV_OPA_COVER;
        for (int i = 0; i <= 5; i++) {
            float frac = (float)i / 5;
            int yabs = TOP_Y + (int)(frac * (H - 1));
            lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
            ld.color = (uiTheme.depthGrid); ld.width = 1; ld.opa = 180;
            lv_point_t pl[2] = {{0,(lv_coord_t)yabs},{(lv_coord_t)UI_S(18),(lv_coord_t)yabs}};
            cdLvLine(b, pl, 2, &ld);
            td.color = (uiTheme.depthScale);
            cdLvText(b, UI_S(2), yabs - UI_S(7), UI_S(34), &td, _scaleTxt[i]);
        }
        PP_END(s_pp, DP_PH_SCALE);

        // NOW marker
        {
            lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
            ld.color = (uiTheme.depthNow); ld.width = 2; ld.opa = 150;
            lv_point_t pn[2] = {{(lv_coord_t)(W-UI_S(3)),(lv_coord_t)TOP_Y},
                                  {(lv_coord_t)(W-UI_S(3)),(lv_coord_t)(TOP_Y+H-1)}};
            cdLvLine(b, pn, 2, &ld);
            td.color = (uiTheme.depthNow);
            // The box must hold "NOW" and "JETZT" in FONT_TINY on one line, or
            // the label wraps into "NO" / "W". The 4" box was 26 px up to 1.3.0
            // - too narrow for either word (NOW alone is about 30 px). 7B: the
            // font grew 12 -> 14, hence the wider box there.
#if defined(BOARD_PANEL_1024X600)
            cdLvText(b, W-52, TOP_Y+UI_S(3), 48, &td, T(STR_DEPTH_NOW));
#else
            cdLvText(b, W-48, TOP_Y+UI_S(3), 44, &td, T(STR_DEPTH_NOW));
#endif
        }
        PP_END(s_pp, DP_PH_NOW);

        // Current depth guide line
        if (guideY >= 0) {
            int yc = guideY;
            lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
            ld.color = CLR_TEXT; ld.width = 1; ld.opa = 70;
            for (int xx = UI_S(24); xx < W - UI_S(30); xx += UI_S(10)) {
                lv_point_t pd[2] = {{(lv_coord_t)xx,(lv_coord_t)yc},
                                      {(lv_coord_t)(xx+UI_S(5)),(lv_coord_t)yc}};
                cdLvLine(b, pd, 2, &ld);
            }
        }
    }
}

// ── update ─────────────────────────────────────────────────────────────────────

void DepthScreen::update()
{
    PP_BEGIN(s_pp);
    float depth;
    float histBuf[DataModel::DEPTH_HIST];
    int   histIdx;
    bool  histFull;
    uint32_t histSeq;
    {
        auto lk = data.lock();
        depth    = data.depth;
        histIdx  = data.depthHistIdx;
        histFull = data.depthHistFull;
        histSeq  = data.depthHistSeq;
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

    // The alarm label is written only when its text changes: every
    // lv_label_set_text frees, allocates and re-measures, and invalidates the
    // label - it did so every frame, for an empty text too.
    char ab[40];
    const char *alarmTxt = "";
    if (alarm) {
        float ad = useImp ? appConfig.cfg.depthAlarm*FT_PER_M
                          : appConfig.cfg.depthAlarm;
        snprintf(ab, sizeof(ab), "%s %s < %.1f%s", npSym(NP_ICON_WARNING),   // OpenBridge icon trial
                 T(STR_DEPTH_SHALLOW), ad, unit);
        alarmTxt = ab;
    }
    if (strcmp(lv_label_get_text(_lblAlarm), alarmTxt) != 0)
        lv_label_set_text(_lblAlarm, alarmTxt);

    if (!_canvas || !_cbuf) { PP_END(s_pp, DP_PH_PRE); PP_FRAME(s_pp); return; }

    // What the canvas shows. The echogram needs two samples and the scratch
    // block (see create()). Its scale and the guide-line row are evaluated
    // here, by the expressions drawWaterSection() used to evaluate itself,
    // and handed to it - so the frame key and the picture cannot disagree.
    const bool water = (histIdx >= 2 || histFull) && _profY;
    float maxM   = 2.0f;
    int   guideY = -1;   // no guide line
    if (water) {
        maxM = depthScaleMax(histBuf, histIdx, histFull);
        float currentDepth = isnan(depth) ? 0.f : depth;
        if (currentDepth > 0.f && currentDepth <= maxM)
            guideY = TOP_Y + (int)(currentDepth / maxM * WATER_H);
    }

    // Skip a frame whose key equals the one on the canvas: no clear, no
    // paint, no invalidate. The buffer belongs to this screen alone, so that
    // frame is still in it - also after the screen was hidden - and LVGL goes
    // on showing it. The key holds everything the picture depends on that can
    // change without create() running again: the history ring (by its
    // sequence number - it decides whether there is an echogram, its scale
    // and its profile), the depth text, unit and alarm colour, and the guide
    // row. With real data that is every frame between two depth messages; in
    // demo mode about three frames in four.
    FrameKey key;
    key.histSeq = histSeq;
    key.guideY  = guideY;
    key.ft      = useImp;
    key.alarm   = alarm;
    strncpy(key.num, numStr, sizeof(key.num));
    PP_END(s_pp, DP_PH_PRE);
    if (_painted && key.histSeq == _paintedKey.histSeq && key.guideY == _paintedKey.guideY &&
        key.ft == _paintedKey.ft && key.alarm == _paintedKey.alarm &&
        strcmp(key.num, _paintedKey.num) == 0) {
        PP_FRAME(s_pp);
        return;
    }

    // No full-canvas clear any more: with the echogram, the header fill and
    // the water fill store every pixel of the canvas before anything blends
    // onto it. Without one, only the rows below the header need the
    // background - cleared BEFORE the header texts, as the full clear was, so
    // a configured depth font tall enough to reach below the header still
    // shows there. 8-row chunks, offering a yield after each;
    // renderYield() decides which of them cost a tick (see RenderYield.h).
    if (!water) {
        const int totalRows = SCREEN_H - NAV_BAR_H;
        lv_color_t bg = CLR_BG, *p = _cbuf + (size_t)TOP_Y * CW;
        for (int r = TOP_Y; r < totalRows; r += 8) {
            int rows = ((totalRows-r) < 8) ? (totalRows-r) : 8;
            lv_color_t *end = p + (size_t)rows * CW;
            while (p < end) *p++ = bg;
            renderYield();
        }
        PP_END(s_pp, DP_PH_CLR);
    }

    drawTopSection(numStr, unit, alarm);

    if (water)
        drawWaterSection(histBuf, histIdx, histFull, maxM, guideY);

    lv_obj_invalidate(_canvas);
    _painted    = true;
    _paintedKey = key;
    if (water) PP_END(s_pp, DP_PH_GUIDE);
    PP_FRAME(s_pp);
}
