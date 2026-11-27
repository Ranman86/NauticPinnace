#include "ClockScreen.h"
#include "RenderYield.h"
#include "../../PsramArena.h"
#include "../Theme.h"
#include "../CanvasDraw.h"
#include "../../SunCalc.h"
#include "PaintPhase.h"
#if defined(BOARD_PANEL_1024X600)
#include "WorldMask600.h"   // 600x250 mask, same macro/function names
#else
#include "WorldMask.h"      // 480x200 mask
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>

// ── Paint phases (measurement builds only, see PaintPhase.h) ──────────────────
// lbl runs every frame (data snapshot, astronomy, all label texts). The map
// phases run with each map redraw (~8 s): per-pixel fill, graticule +
// terminator line, sun/boat/moon overlays. tdraw is the whole tide-curve
// redraw (~60 s), and teval the curve evaluation inside it (tideColumns()) -
// so (tdraw - teval) is the background/water pass, the anti-aliased curve
// and the two lv_canvas_draw_line calls for axis and "now" marker. mline
// holds two LVGL graticule lines and the anti-aliased terminator.
#if PP_ENABLED
enum { CL_PH_LBL = 0, CL_PH_MFILL, CL_PH_MLINE, CL_PH_MOVL, CL_PH_TEVAL,
       CL_PH_TDRAW, CL_PH_N };
static PaintPhaseSet s_pp;

// Declared extern at the call site (main.cpp), like windPhaseTake().
void clockPhaseReport() {
    static const char *const names[CL_PH_N] = {
        "lbl", "mfill", "mline", "movl", "teval", "tdraw" };
    ppReport("clk", names, CL_PH_N, s_pp, nullptr);
}
#endif

// Weekday names, index 0 = Sunday (matches the day-of-week arithmetic below).
static const StrId WDAY[7] = {
    STR_CLOCK_WDAY_SUN, STR_CLOCK_WDAY_MON, STR_CLOCK_WDAY_TUE,
    STR_CLOCK_WDAY_WED, STR_CLOCK_WDAY_THU, STR_CLOCK_WDAY_FRI,
    STR_CLOCK_WDAY_SAT
};
static constexpr float D2R = 0.0174532925f, R2D = 57.29578f;
static constexpr float TIDE_WIN = 13.f, TIDE_LEAD = 2.f;   // hours: shown window, "now" offset

// ── canvas helpers ───────────────────────────────────────────────────────────
static void mline(lv_obj_t *cv, float x1, float y1, float x2, float y2,
                  lv_color_t col, lv_coord_t w, lv_opa_t opa) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = col; d.width = w; d.opa = opa;
    lv_point_t p[2] = {{(lv_coord_t)x1,(lv_coord_t)y1},{(lv_coord_t)x2,(lv_coord_t)y2}};
    lv_canvas_draw_line(cv, p, 2, &d);
}
static void mdot(lv_obj_t *cv, float x, float y, float r, lv_color_t col) {
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col; rd.bg_opa = LV_OPA_COVER; rd.radius = LV_RADIUS_CIRCLE; rd.border_width = 0;
    lv_canvas_draw_rect(cv, (lv_coord_t)(x-r), (lv_coord_t)(y-r), (lv_coord_t)(2*r), (lv_coord_t)(2*r), &rd);
}

// One row of a 1-px horizontal line from (a, y) to (b, y), with the pixels
// lv_canvas_draw_line() gave it: draw_line_hor() (lv_draw_sw_line.c) ends one
// pixel short, x2 = max - 1, so the run is [min, max - 1], and a == b draws
// nothing at all. cdSpanH() clips to the canvas the way LVGL's clip area did.
static void lineRowH(const CdTarget &tg, int y, int a, int b, lv_color_t col) {
    if (a == b) return;
    if (b < a) { const int t = a; a = b; b = t; }
    cdSpanH(tg, y, a, b - 1, col, LV_OPA_COVER);
}

// Anti-aliased 1-px curve through (x, yAt[x]), x = 0..n-1, written straight
// into the buffer: the terminator and the tide curve, both one sample per
// pixel column: coverage = 1 - D, with D the real
// distance of the pixel centre to the polyline; y = 7 runs through the middle
// of row 7; every pixel is written at most once; a sample that is not finite
// (or beyond +-1e6) leaves out both segments that touch it. A general curve
// primitive costs a software float division per pixel and segment on the
// ESP32-S3; with width 1 only the two segments that meet at column c can
// reach a pixel of that column, and that pixel lies straight above or below
// their shared vertex. So the distance needs only multiply-adds with each
// segment's 1 / (1 + slope^2), taken once per column, and only the rows that
// can get a coverage above zero are visited.
static void columnCurveAA(const CdTarget &tg, const float *yAt, int n,
                          lv_color_t col, lv_opa_t opa) {
    if (!tg.buf || !yAt || n < 2 || opa == 0) return;
    const int cEnd = (n < tg.w) ? n : tg.w;                  // columns on the canvas
    auto usable  = [](float v) { return fabsf(v) < 1e6f; };   // false for NaN and inf
    auto clamp01 = [](float t) { return t < 0.f ? 0.f : (t > 1.f ? 1.f : t); };
    const float lim = (float)tg.h + 2.f;
    // Segment k runs from column k to k + 1 with slope s = yAt[k+1] - yAt[k]
    // and q = 1 / (1 + s^2). L = the segment left of column c, R = right of it.
    bool  hasL = false;
    float sL = 0.f, qL = 0.f;
    for (int c = 0; c < cEnd; c++) {
        const float yc   = yAt[c];
        const bool  hasR = c + 1 < n && usable(yc) && usable(yAt[c + 1]);
        float sR = 0.f, qR = 0.f;
        if (hasR) { sR = yAt[c + 1] - yc; qR = 1.f / (1.f + sR * sR); }
        if (hasL || hasR) {
            // A pixel can only be within 1 px of the curve if its row is
            // strictly within 1 of the y range the two segments span.
            float lo = yc, hi = yc;
            if (hasL) { const float e = yc - sL; if (e < lo) lo = e; if (e > hi) hi = e; }
            if (hasR) { const float e = yc + sR; if (e < lo) lo = e; if (e > hi) hi = e; }
            lo = lo < -2.f ? -2.f : (lo > lim ? lim : lo);
            hi = hi < -2.f ? -2.f : (hi > lim ? lim : hi);
            int ra = (int)floorf(lo - 1.f) + 1, rb = (int)ceilf(hi + 1.f) - 1;
            if (ra < 0) ra = 0;
            if (rb > tg.h - 1) rb = tg.h - 1;
            lv_color_t *p = tg.buf + (size_t)ra * (size_t)tg.w + (size_t)c;
            for (int r = ra; r <= rb; r++, p += tg.w) {
                // Pixel centre relative to the vertex (c, yc) is (0, dy); t is
                // the foot of its perpendicular along a segment, clamped to it.
                const float dy = (float)r - yc;
                float d2 = 2.f;                                  // out of reach
                if (hasR) {
                    const float t = clamp01(dy * sR * qR), ey = dy - t * sR;
                    d2 = t * t + ey * ey;
                }
                if (hasL) {
                    const float t = clamp01(-dy * sL * qL), ey = dy + t * sL;
                    const float e2 = t * t + ey * ey;
                    if (e2 < d2) d2 = e2;
                }
                if (!(d2 < 1.f)) continue;
                const lv_opa_t a = (lv_opa_t)((float)opa * (1.f - sqrtf(d2)));
                if (a == 0) continue;
                *p = (a >= LV_OPA_COVER) ? col : lv_color_mix(col, *p, a);
            }
        }
        hasL = hasR; sL = sR; qL = qR;
    }
}

// Day/night boundary row of one map column (sun north: the first night row;
// sun south: the first day row), as an integer that reproduces the old
// per-pixel float test exactly, so the fill compares two ints per pixel:
//   sun north (decDeg >= 0): night iff (float)y > yT  <=>  y >= floor(yT) + 1
//   sun south (decDeg <  0): night iff (float)y < yT  <=>  y <  ceil(yT)
// y is a whole number, so both are exact. Clamping to 0..mh changes no answer
// for the rows 0..mh-1. A NaN yT made the float test false on every row; the
// value returned for it does the same.
static int16_t nightRow(float yT, bool sunSouth, int mh) {
    if (yT != yT) return (int16_t)(sunSouth ? 0 : mh);
    float t = sunSouth ? ceilf(yT) : floorf(yT) + 1.f;
    if (t < 0.f)       t = 0.f;
    if (t > (float)mh) t = (float)mh;
    return (int16_t)t;
}

// Label text and HIDDEN flag only on a change. lv_label_set_text() always
// invalidates the label and frees + re-allocates its text, same string or
// not, and lv_obj_clear_flag(HIDDEN) invalidates an already visible object -
// that re-blitted the whole tide image on every frame. update() runs at up to
// 10 Hz. Both compare against the object's own state, so there is no cache to
// go stale: a rebuild (resetForRebuild + create) brings new objects.
static void setText(lv_obj_t *l, const char *s) {
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, s) == 0) return;
    lv_label_set_text(l, s);
}
static void setHidden(lv_obj_t *o, bool hide) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hide) return;
    if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void ClockScreen::create(lv_obj_t *parent) {
    // The map draw copies the mask 1:1 — canvas and mask sizes must match.
    static_assert(MW == WORLD_MASK_W && MH == WORLD_MASK_H,
                  "map canvas size must equal the included world-mask size");
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(MW, MH);
    if (!_cbuf) _cbuf = (lv_color_t *)PsramArena::alloc(sz);
    if (_cbuf) {
        _canvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_canvas, _cbuf, MW, MH, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(_canvas, 0, 0);
        // Paint the background BEFORE anything is drawn. The PSRAM arena is
        // zero-filled at boot and 0x0000 is BLACK in RGB565, so an undrawn canvas
        // showed up as a black block. update() returns early when there is no
        // valid time (no PGN 126992), so drawMap()/drawTide() may never run.
        lv_canvas_fill_bg(_canvas, CLR_BG, LV_OPA_COVER);
    }

    _clock = lv_label_create(container);
    lv_label_set_text(_clock, "--:--:--");
    lv_obj_set_style_text_font(_clock, FONT_HUGE, 0);
    lv_obj_set_style_text_color(_clock, CLR_TEXT, 0);
    lv_obj_align(_clock, LV_ALIGN_TOP_MID, 0, MH - 2);   // tucked 2 px under the map edge

    _date = lv_label_create(container);
    lv_label_set_text(_date, "");
    lv_obj_set_style_text_font(_date, FONT_MED, 0);
    lv_obj_set_style_text_color(_date, CLR_TEXT_DIM, 0);
    lv_obj_align(_date, LV_ALIGN_TOP_MID, 0, UI_S(256));

    _sunLine = lv_label_create(container);
    lv_label_set_text(_sunLine, "");
    lv_obj_set_style_text_font(_sunLine, FONT_MED, 0);
    lv_obj_set_style_text_color(_sunLine, CLR_TEXT, 0);
    lv_obj_align(_sunLine, LV_ALIGN_TOP_MID, 0, UI_S(280));

    _moonLine = lv_label_create(container);
    lv_label_set_text(_moonLine, "");
    lv_obj_set_style_text_font(_moonLine, FONT_MED, 0);
    lv_obj_set_style_text_color(_moonLine, CLR_TEXT_DIM, 0);
    lv_obj_align(_moonLine, LV_ALIGN_TOP_MID, 0, UI_S(302));

    // ── Tide estimate (uncalibrated) ──
    size_t tsz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(TW, TH);
    if (!_tcbuf) _tcbuf = (lv_color_t *)PsramArena::alloc(tsz);
    if (_tcbuf) {
        _tideCanvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_tideCanvas, _tcbuf, TW, TH, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(_tideCanvas, LV_ALIGN_TOP_MID, 0, UI_S(326));
        lv_canvas_fill_bg(_tideCanvas, CLR_BG, LV_OPA_COVER);   // see _canvas above
        lv_obj_add_flag(_tideCanvas, LV_OBJ_FLAG_HIDDEN);       // shown only with a real curve
    }

    // Prominent next-tide headline (type + absolute local time).
    _tideBig = lv_label_create(container);
    lv_label_set_text(_tideBig, "");
    lv_obj_set_style_text_font(_tideBig, FONT_XL, 0);
    lv_obj_set_style_text_color(_tideBig, CLR_TEXT, 0);
    lv_obj_align(_tideBig, LV_ALIGN_TOP_MID, 0, UI_S(378));

    _tideLine = lv_label_create(container);
    lv_label_set_text(_tideLine, "");
    lv_obj_set_style_text_font(_tideLine, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_tideLine, CLR_TEXT_DIM, 0);
    lv_obj_align(_tideLine, LV_ALIGN_TOP_MID, 0, UI_S(418));

    _tideNote = lv_label_create(container);
    lv_label_set_text(_tideNote, T(STR_CLOCK_TIDE_EST_NOTE));
    lv_obj_set_style_text_font(_tideNote, FONT_TINY, 0);
    lv_obj_set_style_text_color(_tideNote, CLR_TEXT_DIM, 0);
    lv_obj_align(_tideNote, LV_ALIGN_TOP_MID, 0, UI_S(434));
}

static void hm(char *buf, size_t n, float hours) {
    if (isnan(hours)) { snprintf(buf, n, "--:--"); return; }
    int total = (int)(hours * 60.f + 0.5f);
    total = ((total % 1440) + 1440) % 1440;
    snprintf(buf, n, "%02d:%02d", total / 60, total % 60);
}

void ClockScreen::update() {
    PP_BEGIN(s_pp);
    uint16_t days; double secOfDay; int16_t offMin; uint32_t lastUpd; bool valid;
    float lat, lon;
    // BSH tide-forecast snapshot.
    bool bshIsBsh; int bshCount; uint32_t bshLastMs;
    DataModel::TideExtreme bshFc[DataModel::MAX_TIDE_FC];
    char bshStation[40];
    float busLevel; bool busRising; uint32_t busLastMs; char busStation[24];
    {
        auto lk = data.lock();
        days = data.sysDays; secOfDay = data.sysSecOfDay; offMin = data.localOffsetMin;
        lastUpd = data.lastTimeUpdate; valid = data.timeValid;
        lat = data.lat; lon = data.lon;
        bshIsBsh = data.tideIsBsh; bshCount = data.tideFcCount; bshLastMs = data.lastTideFcMs;
        for (int i = 0; i < bshCount && i < DataModel::MAX_TIDE_FC; i++) bshFc[i] = data.tideFc[i];
        snprintf(bshStation, sizeof(bshStation), "%s", data.tideStation);
        busLevel = data.tideBusLevel; busRising = data.tideBusRising;
        busLastMs = data.lastTideBusUpdate;
        snprintf(busStation, sizeof(busStation), "%s", data.tideBusStation);
    }
    char b[72];

    if (!valid) {
        setText(_clock, "--:--:--");
        setText(_date, T(STR_CLOCK_NO_TIME));
        setText(_sunLine, ""); setText(_moonLine, "");
        setText(_tideBig, ""); setText(_tideLine, "");
        // Also drop the "rough estimate" footnote — with no time there is no
        // estimate either, and leaving it up implies data that is not there.
        setText(_tideNote, "");
        if (_tideCanvas) setHidden(_tideCanvas, true);
        PP_END(s_pp, CL_PH_LBL);
        PP_FRAME(s_pp);
        return;
    }

    // UTC now (ticked by elapsed wall time) + local.
    double utcSec = secOfDay + (double)(millis() - lastUpd) / 1000.0;
    long   utcDays = days;
    while (utcSec >= 86400.0) { utcSec -= 86400.0; utcDays++; }
    double localSec = utcSec + (double)offMin * 60.0;
    long   localDays = utcDays;
    while (localSec >= 86400.0) { localSec -= 86400.0; localDays++; }
    while (localSec < 0.0)      { localSec += 86400.0; localDays--; }

    snprintf(b, sizeof(b), "%02d:%02d:%02d", (int)(localSec/3600.0),
             ((int)(localSec/60.0))%60, ((int)localSec)%60);
    setText(_clock, b);
    int yy; unsigned mo, dd; scCivilFromDays(localDays, yy, mo, dd);
    int wd = (int)(((localDays % 7) + 4 + 7) % 7);
    snprintf(b, sizeof(b), "%s, %02u.%02u.%04d", T(WDAY[wd]), dd, mo, yy);
    setText(_date, b);

    // Sun rise/set for the line.
    bool gpsOk = !isnan(lat) && !isnan(lon);
    if (gpsOk) {
        float riseUTC, setUTC; int st;
        scSunTimesUTC(yy, mo, dd, lat, lon, riseUTC, setUTC, st);
        if (st == 0) {
            float rL = fmodf(riseUTC + offMin/60.f + 24.f, 24.f);
            float sL = fmodf(setUTC  + offMin/60.f + 24.f, 24.f);
            char rb[8], sb[8]; hm(rb, sizeof(rb), rL); hm(sb, sizeof(sb), sL);
            snprintf(b, sizeof(b), "%s  %s  -  %s", T(STR_CLOCK_SUN), rb, sb);
        } else snprintf(b, sizeof(b), "%s  %s", T(STR_CLOCK_SUN),
                        st > 0 ? T(STR_CLOCK_POLAR_DAY) : T(STR_CLOCK_POLAR_NIGHT));
        setText(_sunLine, b);
    } else {
        snprintf(b, sizeof(b), "%s  %s", T(STR_CLOCK_SUN), T(STR_CLOCK_NO_GPS));
        setText(_sunLine, b);
    }

    // Moon phase.
    float age, illum; bool wax;
    scMoonPhase(utcDays, utcSec, age, illum, wax);
    StrId pn;
    if      (age < 1.8f)  pn = STR_CLOCK_MOON_NEW;
    else if (age < 5.5f)  pn = STR_CLOCK_MOON_WAX_CRESC;
    else if (age < 9.2f)  pn = STR_CLOCK_MOON_FIRST_Q;
    else if (age < 12.9f) pn = STR_CLOCK_MOON_WAX_GIBB;
    else if (age < 16.6f) pn = STR_CLOCK_MOON_FULL;
    else if (age < 20.3f) pn = STR_CLOCK_MOON_WANE_GIBB;
    else if (age < 24.0f) pn = STR_CLOCK_MOON_LAST_Q;
    else if (age < 27.7f) pn = STR_CLOCK_MOON_WANE_CRESC;
    else                  pn = STR_CLOCK_MOON_NEW;
    snprintf(b, sizeof(b), "%s  %s  %d%%", T(STR_CLOCK_MOON), T(pn),
             (int)(illum * 100.f + 0.5f));
    setText(_moonLine, b);

    // Tide: prefer the official BSH forecast (when fresh), else the estimate.
    uint32_t nowUtc = (uint32_t)utcDays * 86400UL + (uint32_t)utcSec;
    bool bshFresh = bshIsBsh && bshCount > 0 &&
                    (millis() - bshLastMs < 12UL * 3600UL * 1000UL);
    int bshNext = -1;
    if (bshFresh)
        for (int i = 0; i < bshCount; i++)
            if (bshFc[i].unixUtc > nowUtc) { bshNext = i; break; }

    bool busFresh = !isnan(busLevel) && (millis() - busLastMs < 10UL * 60UL * 1000UL);

    if (busFresh) {
        // Live water level from a tide station on the NMEA 2000 bus (PGN 130320).
        snprintf(b, sizeof(b), "%s  %.2f m", T(STR_CLOCK_TIDE_LEVEL), busLevel);
        setText(_tideBig, b);
        snprintf(b, sizeof(b), "%s  -  %s",
                 busRising ? T(STR_CLOCK_TIDE_RISING) : T(STR_CLOCK_TIDE_FALLING),
                 busStation[0] ? busStation : T(STR_CLOCK_TIDE_STATION));
        setText(_tideLine, b);
        setText(_tideNote, T(STR_CLOCK_SRC_N2K));
    } else if (bshNext >= 0) {
        const DataModel::TideExtreme &e = bshFc[bshNext];
        uint32_t evLoc = e.unixUtc + (int32_t)offMin * 60;
        snprintf(b, sizeof(b), "%s  %02d:%02d  %.1f m",
                 T(e.isHigh ? STR_CLOCK_TIDE_HW : STR_CLOCK_TIDE_LW),
                 (int)((evLoc % 86400) / 3600), (int)((evLoc % 3600) / 60),
                 e.cmCD / 100.0f);
        setText(_tideBig, b);
        if (bshNext + 1 < bshCount) {
            const DataModel::TideExtreme &e2 = bshFc[bshNext + 1];
            uint32_t l2 = e2.unixUtc + (int32_t)offMin * 60;
            snprintf(b, sizeof(b), "%s  -  %s %s %02d:%02d", bshStation,
                     T(STR_CLOCK_TIDE_THEN),
                     T(e2.isHigh ? STR_CLOCK_TIDE_HW : STR_CLOCK_TIDE_LW),
                     (int)((l2 % 86400) / 3600), (int)((l2 % 3600) / 60));
        } else snprintf(b, sizeof(b), "%s", bshStation);
        setText(_tideLine, b);
        setText(_tideNote, T(STR_CLOCK_SRC_BSH));
    } else if (gpsOk) {
        float lvl, hToNext, springF; bool rising, nextHigh;
        scTideEstimate(utcDays, utcSec, lon, lvl, rising, hToNext, nextHigh, springF);
        int hh = (int)hToNext, mm = (int)((hToNext - hh) * 60.f + 0.5f);
        if (mm == 60) { mm = 0; hh++; }
        int neMin = ((int)(localSec / 60.0) + hh * 60 + mm) % 1440;
        snprintf(b, sizeof(b), "%s  %02d:%02d",
                 T(nextHigh ? STR_CLOCK_TIDE_HW : STR_CLOCK_TIDE_LW),
                 neMin / 60, neMin % 60);
        setText(_tideBig, b);
        StrId sp = (springF > 1.25f) ? STR_CLOCK_TIDE_SPRING
                 : (springF < 0.78f) ? STR_CLOCK_TIDE_NEAP : STR_CLOCK_TIDE_MEAN;
        snprintf(b, sizeof(b), "%s  -  in %d:%02d  -  %s",
                 rising ? T(STR_CLOCK_TIDE_RISING) : T(STR_CLOCK_TIDE_FALLING),
                 hh, mm, T(sp));
        setText(_tideLine, b);
        setText(_tideNote, T(STR_CLOCK_TIDE_EST_NOTE));
    } else {
        setText(_tideBig, T(STR_CLOCK_TIDE_NO_GPS));
        setText(_tideLine, "");
        setText(_tideNote, T(STR_CLOCK_TIDE_EST_NOTE));
    }

    // Redraw the world map every ~8 s (terminator moves slowly). The subsolar
    // point (terminator, sun dot) is needed only for that redraw, so the
    // double-precision scSubsolar() runs only then - still inside lbl, so the
    // paint phases keep covering the frame.
    uint32_t now = millis();
    const bool mapDue = _canvas && _cbuf && (_lastMapMs == 0 || now - _lastMapMs >= 8000);
    float decDeg = 0.f, sunLon = 0.f;
    if (mapDue) {
        int uy; unsigned umo, udd; scCivilFromDays(utcDays, uy, umo, udd);
        scSubsolar(uy, umo, udd, utcSec, decDeg, sunLon);
    }
    PP_END(s_pp, CL_PH_LBL);

    bool mapDrawn = false;
    if (mapDue) {
        _lastMapMs = now;
        drawMap(lat, lon, decDeg, sunLon, illum, wax, gpsOk);
        mapDrawn = true;
    }
    // ── Tide curve: only shown when it actually means something ──────────────
    // The curve is the LOCAL ESTIMATE (scTideEstimate, longitude-based), not BSH
    // and not the bus level. Two cases where it must not be on screen:
    //   * no GPS fix  -> drawTide() never ran, so the canvas still holds its
    //                    initial content. The PSRAM arena is zero-filled at boot
    //                    and 0x0000 is BLACK in RGB565 — that was the black bar.
    //   * a real source is driving the readout (bus PGN 130320 or the BSH
    //     forecast) -> an estimated curve underneath can contradict the measured
    //     values above it, which is worse than showing nothing.
    const bool showCurve = gpsOk && !busFresh && bshNext < 0;
    // Redraw the tide curve every ~60 s, but never in the same update() as the
    // map: an update() that ran both paid for both, and on entering the screen
    // (both timers expired) that sum was the screen's worst frame. A due curve
    // then waits for the next update(), at most ~100 ms later.
    const bool tideDue = _tideCanvas && _tcbuf && showCurve &&
                         (_lastTideMs == 0 || now - _lastTideMs >= 60000);
    if (tideDue && !mapDrawn) {
        _lastTideMs = now;
        if (drawTide(utcDays, utcSec, lon)) _tideDrawn = true;
    }
    // Visible only with a curve on it. Until the first one the canvas is the
    // plain background from create(), and with the redraw deferred, unhiding
    // it at once would flash an empty box. Likewise a hidden canvas is not
    // uncovered while its curve is overdue and waiting for that redraw. One
    // that is already on screen stays up while it waits - hiding it for one
    // frame would flicker.
    if (_tideCanvas) {
        const bool waiting = tideDue && mapDrawn;
        const bool hidden  = lv_obj_has_flag(_tideCanvas, LV_OBJ_FLAG_HIDDEN);
        setHidden(_tideCanvas, !(showCurve && _tideDrawn && !(hidden && waiting)));
    }
    PP_FRAME(s_pp);
}

void ClockScreen::drawMap(float lat, float lon, float decDeg, float sunLonDeg,
                          float moonIllum, bool moonWax, bool gpsOk) {
    PP_SKIP(s_pp);   // the gap since lbl is the redraw timer check, not the map
    // Palette from the theme roles. "d"/"n" here means the DAY and NIGHT side of
    // the terminator, not the display illumination. The night side is derived
    // from the day side (the ratio of the earlier fixed colours was around
    // 45 %), so only ONE colour has to be configured per element.
    // 45 % brightness via lv_color_mix instead of custom bit arithmetic — that
    // way nothing depends on the colour depth (RGB565 here, RGB888 conceivable
    // in the simulator).
    const uint8_t NIGHT_MIX = 115;                      // 115/255 ~ 45 %
    auto dim = [NIGHT_MIX](lv_color_t c) {
        return lv_color_mix(c, lv_color_black(), NIGHT_MIX);
    };
    const lv_color_t SEAd = (uiTheme.mapSea),   SEAn = dim(SEAd);
    const lv_color_t LNDd = (uiTheme.mapLand),  LNDn = dim(LNDd);
    const lv_color_t EDGd = (uiTheme.mapCoast), EDGn = dim(EDGd);

    // 1. Terminator latitude -> canvas-y per column (the day/night boundary),
    //    and from it the day/night boundary row of each column for the fill.
    //    The sun is on the horizon where tan(lat) * tan(dec) = -cos(H), so
    //    latT = atan(-cos H / tan dec), always within +-90 deg. It is taken
    //    as atan2 with the sign of tan(dec) moved into the y argument and
    //    x = |tan dec| >= 0: no division, and towards the equinox (tan dec
    //    -> 0, also -0) latT runs smoothly out to +-90 deg, i.e. the
    //    terminator turns into the two meridians H = +-90 deg. The former
    //    atan2f(-cos H, tan dec) took a NEGATIVE x whenever the sun was south
    //    (about 23 Sep to 20 Mar); atan2 then returned angles beyond +-90 deg
    //    and the map was inverted in large parts.
    //    Night is south of latT with the sun north and north of it with the
    //    sun south - the two rules in nightRow().
    const float decR = decDeg * D2R;
    const float tanAbs = fabsf(tanf(decR));   // loop-invariant, so computed once
    const bool  sunSouth = !(decDeg >= 0.f);
    // Draw-time scratch, CPU-only on the UI task - lives in the PSRAM arena
    // instead of a static DRAM array (1.9 KB of internal RAM freed).
    static float   *yT = (float *)PsramArena::alloc(MW * sizeof(float));
    static int16_t *nT = (int16_t *)PsramArena::alloc(MW * sizeof(int16_t));
    // There is deliberately NO internal-RAM fallback array here. It would have
    // to be reserved in .bss on every board and every boot (2.4 KB on the 600
    // grid, 1.9 KB on the 480 one) just to cover the one case where the arena
    // is already exhausted - i.e. exactly when internal RAM is scarcest. So we
    // simply skip the redraw instead. The caller only reaches drawMap() with a
    // valid arena-backed canvas (see the "_canvas && _cbuf" guard in update()),
    // so the worst outcome is a map whose terminator stops moving, never a
    // crash and never an empty screen.
    if (!yT || !nT) return;
    for (int x = 0; x < MW; x++) {
        float lonp = (float)x/MW*360.f - 180.f;
        float Hh   = (lonp - sunLonDeg) * D2R;
        float cH   = cosf(Hh);
        float latT = atan2f(sunSouth ? cH : -cH, tanAbs) * R2D;
        yT[x] = (90.f - latT)/180.f*MH;
        nT[x] = nightRow(yT[x], sunSouth, MH);
    }

    // 2. Per-pixel land/sea fill, day/night by terminator, coastline highlight.
    //    The mask is read a byte at a time, 8 pixels with the MSB leftmost (the
    //    layout worldMaskLand() reads). A land pixel is coast when any of its
    //    4 neighbours is sea, and a neighbour outside the mask counts as sea -
    //    the same rule as the old per-pixel worldMaskLand() calls, now as bit
    //    operations on the byte, its row neighbours and the bytes above/below.
    static_assert(WORLD_MASK_ROWBYTES * 8 == WORLD_MASK_W,
                  "the byte-wise coast test needs whole mask bytes per row");
    const lv_color_t pal[6] = { SEAd, SEAn, LNDd, LNDn, EDGd, EDGn };   // [kind*2 + night]
    for (int y = 0; y < MH; y++) {
        lv_color_t    *row = _cbuf + (size_t)y*MW;
        const uint8_t *m   = WORLD_MASK + (size_t)y*WORLD_MASK_ROWBYTES;
        const uint8_t *mu  = (y > 0)      ? m - WORLD_MASK_ROWBYTES : nullptr;
        const uint8_t *md  = (y < MH - 1) ? m + WORLD_MASK_ROWBYTES : nullptr;
        uint8_t prev = 0;                                   // byte left of m[i]
        for (int i = 0; i < WORLD_MASK_ROWBYTES; i++) {
            const uint8_t B    = m[i];
            const uint8_t next = (i + 1 < WORLD_MASK_ROWBYTES) ? m[i + 1] : 0;
            const uint8_t L = (uint8_t)((B >> 1) | (prev << 7));   // bit = pixel x-1
            const uint8_t R = (uint8_t)((B << 1) | (next >> 7));   // bit = pixel x+1
            const uint8_t U = mu ? mu[i] : 0, D = md ? md[i] : 0;
            const uint8_t C = B & (uint8_t)~(L & R & U & D);       // coast: land, a sea neighbour
            prev = B;
            for (int k = 0; k < 8; k++) {
                const int x = i*8 + k, s = 7 - k;
                const int kind  = ((B >> s) & 1) + ((C >> s) & 1);   // 0 sea, 1 land, 2 coast
                const int night = (y >= nT[x]) != sunSouth;
                row[x] = pal[kind*2 + night];
            }
        }
        if ((y & 15) == 0) renderYield();   // see RenderYield.h
    }
    PP_END(s_pp, CL_PH_MFILL);

    // 3. Faint graticule (equator + prime meridian) + terminator line.
    const lv_color_t GRID = (uiTheme.mapGrid);
    mline(_canvas, 0, MH/2, MW, MH/2, GRID, 1, LV_OPA_30);
    mline(_canvas, MW/2, 0, MW/2, MH, GRID, 1, LV_OPA_30);
    // The terminator as ONE anti-aliased curve through yT[], written straight
    // into the buffer (columnCurveAA) instead of one lv_canvas_draw_line per
    // column with truncated end points. Same colour and opacity as before. A
    // pixel's coverage comes from the real distance of its centre to the
    // curve, and each pixel is written once, so the joints are not blended
    // twice. y = 7 runs through the middle of row 7, so the line lies on the
    // fill's day/night edge within half a pixel (the old one sat on the row
    // above that edge). Near the equinox yT swings between ~0 and ~MH within
    // a column or two; the curve joins those columns with a near-vertical
    // stroke, so it stays connected.
    columnCurveAA(cdTarget(_cbuf, MW, MH), yT, MW, CLR_YELLOW, LV_OPA_60);
    PP_END(s_pp, CL_PH_MLINE);

    // 5. Subsolar point (sun).
    { float sx=(sunLonDeg+180.f)/360.f*MW, sy=(90.f-decDeg)/180.f*MH;
      mdot(_canvas, sx, sy, 4.f*UI_SF, CLR_YELLOW); }

    // 6. Boat position.
    if (gpsOk) {
        float bx=(lon+180.f)/360.f*MW, by=(90.f-lat)/180.f*MH;
        const float arm = 6.f*UI_SF;
        mline(_canvas, bx-arm, by, bx+arm, by, CLR_RED, 1, LV_OPA_COVER);
        mline(_canvas, bx, by-arm, bx, by+arm, CLR_RED, 1, LV_OPA_COVER);
        mdot(_canvas, bx, by, 3.f*UI_SF, CLR_RED);
    }

    // 7. Moon phase disc (top-right). One opaque row per scanline straight into
    //    the buffer (a dark full row, then the lit span), with the same pixels
    //    the former lv_canvas_draw_line per row produced - see lineRowH().
    {
        const float cx=MW-UI_S(34), cy=UI_S(30), r=UI_S(20);
        const lv_color_t LIT=(uiTheme.text), DRK=(uiTheme.surface);
        const CdTarget tg = cdTarget(_cbuf, MW, MH);
        for (float dy=-r; dy<=r; dy+=1.f) {
            float hx=r*sqrtf(1.f-(dy/r)*(dy/r));
            float tx=hx*cosf(3.14159265f*moonIllum);          // terminator x at this row
            float litL, litR;
            if (moonWax) { litL=cx+tx; litR=cx+hx; } else { litL=cx-hx; litR=cx-tx; }
            const int y = (lv_coord_t)(cy+dy);
            lineRowH(tg, y, (lv_coord_t)(cx-hx), (lv_coord_t)(cx+hx), DRK);
            if (litR>litL) lineRowH(tg, y, (lv_coord_t)litL, (lv_coord_t)litR, LIT);
        }
    }

    lv_obj_invalidate(_canvas);
    PP_END(s_pp, CL_PH_MOVL);
}

// ── Tide curve: canvas y per column ───────────────────────────────────────────
// The level is a slow cosine, springFactor * cos(2H), and 2H advances only
// ~0.012 rad per column (600 grid; ~0.015 on the 480 one). So scTideLevel()
// runs at every TIDE_STEP-th column and at the last one, and the columns in
// between are interpolated linearly. That alone could still move a column by
// 1 px, because y truncates with (int): an interpolated value just below an
// integer may belong just above it. So a column is also evaluated exactly
// wherever its interpolated pixel value lies within TIDE_GUARD of an integer.
// Everywhere else the interpolation error - at most h^2/8 * max|f''| over the
// h = 8 columns: 0.026 px on the 600 grid, 0.032 px on the 480 one - cannot
// reach an integer, so (int) gives the y the exact value gives. The result is
// the same yCol as evaluating every column, from about a quarter of the calls.
// The float line centre yLine is taken from the same value; there the
// interpolation error (< 0.04 px) simply stays in the drawing.
static constexpr int   TIDE_STEP  = 8;
static constexpr float TIDE_GUARD = 1.f / 16.f;   // px; about twice that bound

// Centre of the anti-aliased tide line in a column whose pixel value is
// v = level / 1.5 * amp (canvas y = yMid - v; columnCurveAA's y = 7 runs
// through the middle of row 7). The water fill starts at yCol = yMid - (int)v,
// and (int) truncates toward zero: the band -1 < v < 1 around the mean level
// is two rows deep, every other band one. The line follows the smooth v but
// is pulled half a row towards the mean level (inside that two-row band by
// 0.5 * v), so it never lies more than half a pixel from yCol, the top water
// row the former 1-px line was drawn on. It therefore meets the water edge in
// every column; the plain yMid - v would leave a step of water above the line
// below the mean level. Continuous in v; only its slope halves inside the
// two-row band.
static inline float tideLineY(int yMid, float v) {
    const float pull = (v > 1.f) ? 0.5f : (v < -1.f) ? -0.5f : 0.5f * v;
    return (float)yMid - v + pull;
}

void ClockScreen::tideColumns(int16_t *yCol, float *yLine, long utcDays, double utcSec,
                              float lon) const {
    const int   yMid = TH / 2;
    const float amp  = (float)(TH / 2) - 5.f*UI_SF;
    // The clamped level at column x, computed as the former per-column loop did.
    auto level = [&](int x) -> float {
        float hrs = (float)x / TW * TIDE_WIN - TIDE_LEAD;
        double sec = utcSec + (double)hrs * 3600.0; long dy = utcDays;
        while (sec >= 86400.0) { sec -= 86400.0; dy++; }
        while (sec <  0.0)     { sec += 86400.0; dy--; }
        float lvl = scTideLevel(dy, sec, lon);
        if (lvl >  1.5f) lvl =  1.5f; if (lvl < -1.5f) lvl = -1.5f;
        return lvl;
    };
    // Both outputs of one column from the same v, so the line can never stray
    // from the fill row of its column (yCol is the same integer as before).
    auto put = [&](int x, float v) {
        yCol[x]  = (int16_t)(yMid - (int)v);
        yLine[x] = tideLineY(yMid, v);
    };
    float lvA = level(0);
    put(0, lvA / 1.5f * amp);
    for (int a = 0; a < TW - 1; ) {
        const int   b   = (a + TIDE_STEP < TW - 1) ? a + TIDE_STEP : TW - 1;
        const float lvB = level(b);
        put(b, lvB / 1.5f * amp);
        for (int x = a + 1; x < b; x++) {
            const float t = (float)(x - a) / (float)(b - a);
            float v = (lvA + (lvB - lvA) * t) / 1.5f * amp;
            const float f = v - floorf(v);                   // distance above an integer
            if (f < TIDE_GUARD || f > 1.f - TIDE_GUARD) v = level(x) / 1.5f * amp;
            put(x, v);
        }
        lvA = lvB;
        a = b;
    }
}

// Tide-forcing curve: a ~13 h window (2 h past .. 11 h future) of the normalised
// astronomical tide level, water filled below the curve, "now" marked. Coarse —
// see the disclaimer label. Redrawn ~once a minute. Returns false when nothing
// was drawn (no scratch memory).
bool ClockScreen::drawTide(long utcDays, double utcSec, float lon) {
    PP_SKIP(s_pp);   // a skipped or early-out map redraw is not charged here
    const lv_color_t BG    = CLR_BG;
    // Water area from the theme; the curve line above it is lightened towards
    // the text colour (i.e. towards red in night mode, not light blue), the
    // axis uses the existing grid colour.
    const lv_color_t WATER = (uiTheme.tideWater);
    const lv_color_t LINEC = lv_color_mix((uiTheme.text), WATER, 128);
    const lv_color_t AXIS  = (uiTheme.gridLine);
    const int        yMid  = TH / 2;

    // Curve per column: the first water row (yCol) and the centre of the line
    // (yLine). Draw-time scratch in the PSRAM arena, like drawMap()'s yT and
    // for the same reason; without it the redraw is skipped.
    static int16_t *yCol  = (int16_t *)PsramArena::alloc(TW * sizeof(int16_t));
    static float   *yLine = (float *)PsramArena::alloc(TW * sizeof(float));
    if (!yCol || !yLine) return false;

    // 1. The curve.
    PP_LAP_DECL(tevalLap);
    PP_LAP_START(tevalLap);
    tideColumns(yCol, yLine, utcDays, utcSec, lon);
    PP_LAP_STOP(tevalLap);
    PP_ADD(s_pp, CL_PH_TEVAL, tevalLap.us);

    // 2. Background and water in one row-major pass, every pixel written once.
    //    These are the pixels the former BG clear plus one 1-px
    //    lv_canvas_draw_line (x,y)->(x,TH-1) per column produced: a vertical
    //    line ends one pixel short (draw_line_ver(), lv_draw_sw_line.c:
    //    y2 = max - 1), so water fills rows y..TH-2 and the bottom row stays BG;
    //    y == TH-1 drew nothing. Only a y below the canvas, which a level
    //    clamped to +-1.5 never gives, would have put water on the bottom row.
    for (int r = 0; r < TH; r++) {
        lv_color_t *row = _tcbuf + (size_t)r*TW;
        if (r < TH - 1) for (int x = 0; x < TW; x++) row[x] = (yCol[x] <= r) ? WATER : BG;
        else            for (int x = 0; x < TW; x++) row[x] = (yCol[x] >  r) ? WATER : BG;
    }
    renderYield();   // see RenderYield.h

    // 3. The curve: ONE anti-aliased line through yLine[], written straight
    //    into the buffer (columnCurveAA), in the colour and the 1-px weight of
    //    the former lv_canvas_draw_line per column pair. Each pixel's coverage
    //    comes from the real distance of its centre to the line, and each
    //    pixel is written once. yLine stays within half a pixel of the top
    //    water row, the row those segments covered (tideLineY()).
    columnCurveAA(cdTarget(_tcbuf, TW, TH), yLine, TW, LINEC, LV_OPA_COVER);
    renderYield();
    // Mean-level axis.
    { lv_draw_line_dsc_t a; lv_draw_line_dsc_init(&a); a.color=AXIS; a.width=1; a.opa=LV_OPA_40;
      lv_point_t p[2]={{0,(lv_coord_t)yMid},{(lv_coord_t)(TW-1),(lv_coord_t)yMid}}; lv_canvas_draw_line(_tideCanvas,p,2,&a); }
    // "Now" marker.
    { int xn=(int)(TIDE_LEAD/TIDE_WIN*TW); lv_draw_line_dsc_t n; lv_draw_line_dsc_init(&n); n.color=CLR_ACCENT; n.width=2; n.opa=LV_OPA_COVER;
      lv_point_t p[2]={{(lv_coord_t)xn,0},{(lv_coord_t)xn,(lv_coord_t)(TH-1)}}; lv_canvas_draw_line(_tideCanvas,p,2,&n); }
    lv_obj_invalidate(_tideCanvas);
    PP_END(s_pp, CL_PH_TDRAW);
    return true;
}

void ClockScreen::resetForRebuild() {
    _canvas = _tideCanvas = nullptr;
    _clock = _date = _sunLine = _moonLine = nullptr;
    _tideBig = _tideLine = _tideNote = nullptr;
    _lastMapMs = 0; _lastTideMs = 0;
    _tideDrawn = false;   // create() fills the tide canvas with plain BG again
}
