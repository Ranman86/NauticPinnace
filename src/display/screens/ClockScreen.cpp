#include "ClockScreen.h"
#include "../Theme.h"
#include "../../SunCalc.h"
#include "WorldMask.h"
#include <math.h>
#include <stdio.h>

// Weekday names, index 0 = Sunday (matches the day-of-week arithmetic below).
static const StrId WDAY[7] = {
    STR_CLOCK_WDAY_SUN, STR_CLOCK_WDAY_MON, STR_CLOCK_WDAY_TUE,
    STR_CLOCK_WDAY_WED, STR_CLOCK_WDAY_THU, STR_CLOCK_WDAY_FRI,
    STR_CLOCK_WDAY_SAT
};
static constexpr float D2R = 0.0174532925f, R2D = 57.29578f;

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

void ClockScreen::create(lv_obj_t *parent) {
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
    lv_obj_align(_clock, LV_ALIGN_TOP_MID, 0, 198);

    _date = lv_label_create(container);
    lv_label_set_text(_date, "");
    lv_obj_set_style_text_font(_date, FONT_MED, 0);
    lv_obj_set_style_text_color(_date, CLR_TEXT_DIM, 0);
    lv_obj_align(_date, LV_ALIGN_TOP_MID, 0, 256);

    _sunLine = lv_label_create(container);
    lv_label_set_text(_sunLine, "");
    lv_obj_set_style_text_font(_sunLine, FONT_MED, 0);
    lv_obj_set_style_text_color(_sunLine, CLR_TEXT, 0);
    lv_obj_align(_sunLine, LV_ALIGN_TOP_MID, 0, 280);

    _moonLine = lv_label_create(container);
    lv_label_set_text(_moonLine, "");
    lv_obj_set_style_text_font(_moonLine, FONT_MED, 0);
    lv_obj_set_style_text_color(_moonLine, CLR_TEXT_DIM, 0);
    lv_obj_align(_moonLine, LV_ALIGN_TOP_MID, 0, 302);

    // ── Tide estimate (uncalibrated) ──
    size_t tsz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(TW, TH);
    if (!_tcbuf) _tcbuf = (lv_color_t *)PsramArena::alloc(tsz);
    if (_tcbuf) {
        _tideCanvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_tideCanvas, _tcbuf, TW, TH, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(_tideCanvas, LV_ALIGN_TOP_MID, 0, 326);
        lv_canvas_fill_bg(_tideCanvas, CLR_BG, LV_OPA_COVER);   // see _canvas above
        lv_obj_add_flag(_tideCanvas, LV_OBJ_FLAG_HIDDEN);       // shown only with a real curve
    }

    // Prominent next-tide headline (type + absolute local time).
    _tideBig = lv_label_create(container);
    lv_label_set_text(_tideBig, "");
    lv_obj_set_style_text_font(_tideBig, FONT_XL, 0);
    lv_obj_set_style_text_color(_tideBig, CLR_TEXT, 0);
    lv_obj_align(_tideBig, LV_ALIGN_TOP_MID, 0, 378);

    _tideLine = lv_label_create(container);
    lv_label_set_text(_tideLine, "");
    lv_obj_set_style_text_font(_tideLine, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_tideLine, CLR_TEXT_DIM, 0);
    lv_obj_align(_tideLine, LV_ALIGN_TOP_MID, 0, 418);

    _tideNote = lv_label_create(container);
    lv_label_set_text(_tideNote, T(STR_CLOCK_TIDE_EST_NOTE));
    lv_obj_set_style_text_font(_tideNote, FONT_TINY, 0);
    lv_obj_set_style_text_color(_tideNote, CLR_TEXT_DIM, 0);
    lv_obj_align(_tideNote, LV_ALIGN_TOP_MID, 0, 434);
}

static void hm(char *buf, size_t n, float hours) {
    if (isnan(hours)) { snprintf(buf, n, "--:--"); return; }
    int total = (int)(hours * 60.f + 0.5f);
    total = ((total % 1440) + 1440) % 1440;
    snprintf(buf, n, "%02d:%02d", total / 60, total % 60);
}

void ClockScreen::update() {
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
        lv_label_set_text(_clock, "--:--:--");
        lv_label_set_text(_date, T(STR_CLOCK_NO_TIME));
        lv_label_set_text(_sunLine, ""); lv_label_set_text(_moonLine, "");
        lv_label_set_text(_tideBig, ""); lv_label_set_text(_tideLine, "");
        // Also drop the "rough estimate" footnote — with no time there is no
        // estimate either, and leaving it up implies data that is not there.
        lv_label_set_text(_tideNote, "");
        if (_tideCanvas) lv_obj_add_flag(_tideCanvas, LV_OBJ_FLAG_HIDDEN);
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
    lv_label_set_text(_clock, b);
    int yy; unsigned mo, dd; scCivilFromDays(localDays, yy, mo, dd);
    int wd = (int)(((localDays % 7) + 4 + 7) % 7);
    snprintf(b, sizeof(b), "%s, %02u.%02u.%04d", T(WDAY[wd]), dd, mo, yy);
    lv_label_set_text(_date, b);

    // Sun rise/set for the line.
    bool gpsOk = !isnan(lat) && !isnan(lon);
    int uy; unsigned umo, udd; scCivilFromDays(utcDays, uy, umo, udd);
    float decDeg = 0.f, sunLon = 0.f;
    scSubsolar(uy, umo, udd, utcSec, decDeg, sunLon);
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
        lv_label_set_text(_sunLine, b);
    } else {
        snprintf(b, sizeof(b), "%s  %s", T(STR_CLOCK_SUN), T(STR_CLOCK_NO_GPS));
        lv_label_set_text(_sunLine, b);
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
    lv_label_set_text(_moonLine, b);

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
        lv_label_set_text(_tideBig, b);
        snprintf(b, sizeof(b), "%s  -  %s",
                 busRising ? T(STR_CLOCK_TIDE_RISING) : T(STR_CLOCK_TIDE_FALLING),
                 busStation[0] ? busStation : T(STR_CLOCK_TIDE_STATION));
        lv_label_set_text(_tideLine, b);
        lv_label_set_text(_tideNote, T(STR_CLOCK_SRC_N2K));
    } else if (bshNext >= 0) {
        const DataModel::TideExtreme &e = bshFc[bshNext];
        uint32_t evLoc = e.unixUtc + (int32_t)offMin * 60;
        snprintf(b, sizeof(b), "%s  %02d:%02d  %.1f m",
                 T(e.isHigh ? STR_CLOCK_TIDE_HW : STR_CLOCK_TIDE_LW),
                 (int)((evLoc % 86400) / 3600), (int)((evLoc % 3600) / 60),
                 e.cmCD / 100.0f);
        lv_label_set_text(_tideBig, b);
        if (bshNext + 1 < bshCount) {
            const DataModel::TideExtreme &e2 = bshFc[bshNext + 1];
            uint32_t l2 = e2.unixUtc + (int32_t)offMin * 60;
            snprintf(b, sizeof(b), "%s  -  %s %s %02d:%02d", bshStation,
                     T(STR_CLOCK_TIDE_THEN),
                     T(e2.isHigh ? STR_CLOCK_TIDE_HW : STR_CLOCK_TIDE_LW),
                     (int)((l2 % 86400) / 3600), (int)((l2 % 3600) / 60));
        } else snprintf(b, sizeof(b), "%s", bshStation);
        lv_label_set_text(_tideLine, b);
        lv_label_set_text(_tideNote, T(STR_CLOCK_SRC_BSH));
    } else if (gpsOk) {
        float lvl, hToNext, springF; bool rising, nextHigh;
        scTideEstimate(utcDays, utcSec, lon, lvl, rising, hToNext, nextHigh, springF);
        int hh = (int)hToNext, mm = (int)((hToNext - hh) * 60.f + 0.5f);
        if (mm == 60) { mm = 0; hh++; }
        int neMin = ((int)(localSec / 60.0) + hh * 60 + mm) % 1440;
        snprintf(b, sizeof(b), "%s  %02d:%02d",
                 T(nextHigh ? STR_CLOCK_TIDE_HW : STR_CLOCK_TIDE_LW),
                 neMin / 60, neMin % 60);
        lv_label_set_text(_tideBig, b);
        StrId sp = (springF > 1.25f) ? STR_CLOCK_TIDE_SPRING
                 : (springF < 0.78f) ? STR_CLOCK_TIDE_NEAP : STR_CLOCK_TIDE_MEAN;
        snprintf(b, sizeof(b), "%s  -  in %d:%02d  -  %s",
                 rising ? T(STR_CLOCK_TIDE_RISING) : T(STR_CLOCK_TIDE_FALLING),
                 hh, mm, T(sp));
        lv_label_set_text(_tideLine, b);
        lv_label_set_text(_tideNote, T(STR_CLOCK_TIDE_EST_NOTE));
    } else {
        lv_label_set_text(_tideBig, T(STR_CLOCK_TIDE_NO_GPS));
        lv_label_set_text(_tideLine, "");
        lv_label_set_text(_tideNote, T(STR_CLOCK_TIDE_EST_NOTE));
    }

    // Redraw the world map every ~8 s (terminator moves slowly).
    uint32_t now = millis();
    if (_canvas && _cbuf && (_lastMapMs == 0 || now - _lastMapMs >= 8000)) {
        _lastMapMs = now;
        drawMap(lat, lon, decDeg, sunLon, illum, wax, gpsOk);
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
    if (_tideCanvas) {
        if (showCurve) lv_obj_clear_flag(_tideCanvas, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(_tideCanvas, LV_OBJ_FLAG_HIDDEN);
    }
    // Redraw the tide curve every ~60 s.
    if (_tideCanvas && _tcbuf && showCurve && (_lastTideMs == 0 || now - _lastTideMs >= 60000)) {
        _lastTideMs = now;
        drawTide(utcDays, utcSec, lon);
    }
}

void ClockScreen::drawMap(float lat, float lon, float decDeg, float sunLonDeg,
                          float moonIllum, bool moonWax, bool gpsOk) {
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

    // 1. Terminator latitude -> canvas-y per column (the day/night boundary).
    const float decR = decDeg * D2R;
    static float yT[MW];
    for (int x = 0; x < MW; x++) {
        float lonp = (float)x/MW*360.f - 180.f;
        float Hh   = (lonp - sunLonDeg) * D2R;
        float latT = atan2f(-cosf(Hh), tanf(decR)) * R2D;
        yT[x] = (90.f - latT)/180.f*MH;
    }

    // 2. Per-pixel land/sea fill, day/night by terminator, coastline highlight.
    for (int y = 0; y < MH; y++) {
        lv_color_t *row = _cbuf + (size_t)y*MW;
        for (int x = 0; x < MW; x++) {
            bool night = (decDeg >= 0.f) ? ((float)y > yT[x]) : ((float)y < yT[x]);
            if (worldMaskLand(x, y)) {
                bool coast = !worldMaskLand(x-1,y) || !worldMaskLand(x+1,y) ||
                             !worldMaskLand(x,y-1) || !worldMaskLand(x,y+1);
                row[x] = coast ? (night?EDGn:EDGd) : (night?LNDn:LNDd);
            } else {
                row[x] = night ? SEAn : SEAd;
            }
        }
        if ((y & 15) == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }

    // 3. Faint graticule (equator + prime meridian) + terminator line.
    const lv_color_t GRID = (uiTheme.mapGrid);
    mline(_canvas, 0, MH/2, MW, MH/2, GRID, 1, LV_OPA_30);
    mline(_canvas, MW/2, 0, MW/2, MH, GRID, 1, LV_OPA_30);
    { float prevY=-1; for (int x=0;x<MW;x++){ if(prevY>=0) mline(_canvas,x-1,prevY,x,yT[x],CLR_YELLOW,1,LV_OPA_60); prevY=yT[x]; } }

    // 5. Subsolar point (sun).
    { float sx=(sunLonDeg+180.f)/360.f*MW, sy=(90.f-decDeg)/180.f*MH;
      mdot(_canvas, sx, sy, 4, CLR_YELLOW); }

    // 6. Boat position.
    if (gpsOk) {
        float bx=(lon+180.f)/360.f*MW, by=(90.f-lat)/180.f*MH;
        mline(_canvas, bx-6, by, bx+6, by, CLR_RED, 1, LV_OPA_COVER);
        mline(_canvas, bx, by-6, bx, by+6, CLR_RED, 1, LV_OPA_COVER);
        mdot(_canvas, bx, by, 3, CLR_RED);
    }

    // 7. Moon phase disc (top-right).
    {
        const float cx=MW-34, cy=30, r=20;
        const lv_color_t LIT=(uiTheme.text), DRK=(uiTheme.surface);
        lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d); d.width=1; d.opa=LV_OPA_COVER;
        for (float dy=-r; dy<=r; dy+=1.f) {
            float hx=r*sqrtf(1.f-(dy/r)*(dy/r));
            float tx=hx*cosf(3.14159265f*moonIllum);          // terminator x at this row
            float litL, litR;
            if (moonWax) { litL=cx+tx; litR=cx+hx; } else { litL=cx-hx; litR=cx-tx; }
            // dark full row, then lit span
            d.color=DRK; { lv_point_t p[2]={{(lv_coord_t)(cx-hx),(lv_coord_t)(cy+dy)},{(lv_coord_t)(cx+hx),(lv_coord_t)(cy+dy)}}; lv_canvas_draw_line(_canvas,p,2,&d); }
            if (litR>litL){ d.color=LIT; lv_point_t p[2]={{(lv_coord_t)litL,(lv_coord_t)(cy+dy)},{(lv_coord_t)litR,(lv_coord_t)(cy+dy)}}; lv_canvas_draw_line(_canvas,p,2,&d); }
        }
    }

    lv_obj_invalidate(_canvas);
}

// Tide-forcing curve: a ~13 h window (2 h past .. 11 h future) of the normalised
// astronomical tide level, water filled below the curve, "now" marked. Coarse —
// see the disclaimer label. Redrawn ~once a minute.
void ClockScreen::drawTide(long utcDays, double utcSec, float lon) {
    const lv_color_t BG    = CLR_BG;
    // Water area from the theme; the curve line above it is lightened towards
    // the text colour (i.e. towards red in night mode, not light blue), the
    // axis uses the existing grid colour.
    const lv_color_t WATER = (uiTheme.tideWater);
    const lv_color_t LINEC = lv_color_mix((uiTheme.text), WATER, 128);
    const lv_color_t AXIS  = (uiTheme.gridLine);

    const float WIN = 13.f, LEAD = 2.f;          // hours: shown window, "now" offset
    const int   yMid = TH / 2;
    const float amp  = (float)(TH / 2) - 5.f;

    { lv_color_t *p=_tcbuf, *e=_tcbuf+(size_t)TW*TH; while(p<e)*p++=BG; }

    lv_draw_line_dsc_t wl; lv_draw_line_dsc_init(&wl); wl.width=1; wl.opa=LV_OPA_COVER;
    int prevY = -1;
    for (int x = 0; x < TW; x++) {
        float hrs = (float)x / TW * WIN - LEAD;
        double sec = utcSec + (double)hrs * 3600.0; long dy = utcDays;
        while (sec >= 86400.0) { sec -= 86400.0; dy++; }
        while (sec <  0.0)     { sec += 86400.0; dy--; }
        float lvl, h2, sf; bool ri, nh;
        scTideEstimate(dy, sec, lon, lvl, ri, h2, nh, sf);
        if (lvl >  1.5f) lvl =  1.5f; if (lvl < -1.5f) lvl = -1.5f;
        int y = yMid - (int)(lvl / 1.5f * amp);
        wl.color = WATER;
        { lv_point_t pc[2]={{(lv_coord_t)x,(lv_coord_t)y},{(lv_coord_t)x,(lv_coord_t)(TH-1)}}; lv_canvas_draw_line(_tideCanvas,pc,2,&wl); }
        if (prevY >= 0) { wl.color=LINEC; lv_point_t pl[2]={{(lv_coord_t)(x-1),(lv_coord_t)prevY},{(lv_coord_t)x,(lv_coord_t)y}}; lv_canvas_draw_line(_tideCanvas,pl,2,&wl); }
        prevY = y;
        if ((x & 127) == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }
    // Mean-level axis.
    { lv_draw_line_dsc_t a; lv_draw_line_dsc_init(&a); a.color=AXIS; a.width=1; a.opa=LV_OPA_40;
      lv_point_t p[2]={{0,(lv_coord_t)yMid},{(lv_coord_t)(TW-1),(lv_coord_t)yMid}}; lv_canvas_draw_line(_tideCanvas,p,2,&a); }
    // "Now" marker.
    { int xn=(int)(LEAD/WIN*TW); lv_draw_line_dsc_t n; lv_draw_line_dsc_init(&n); n.color=CLR_ACCENT; n.width=2; n.opa=LV_OPA_COVER;
      lv_point_t p[2]={{(lv_coord_t)xn,0},{(lv_coord_t)xn,(lv_coord_t)(TH-1)}}; lv_canvas_draw_line(_tideCanvas,p,2,&n); }
    lv_obj_invalidate(_tideCanvas);
}

void ClockScreen::resetForRebuild() {
    _canvas = _tideCanvas = nullptr;
    _clock = _date = _sunLine = _moonLine = nullptr;
    _tideBig = _tideLine = _tideNote = nullptr;
    _lastMapMs = 0; _lastTideMs = 0;
}
