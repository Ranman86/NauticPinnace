#include "AnchorScreen.h"
#include "../Theme.h"
#include "../UiConfig.h"
#include "../../nmea/DataModel.h"
#include <math.h>
#include <string.h>

// ── Local constants & helpers ────────────────────────────────────────────────
static constexpr float D2R = 0.017453292f;
static constexpr float R2D = 57.29578f;

static constexpr float CXC      = AnchorScreen::CS * 0.5f;   // canvas centre
static constexpr float CYC      = AnchorScreen::CS * 0.5f;
static constexpr float RING_PX  = 132.f;   // screen radius of the alarm circle
static constexpr float MAXBOAT  = 200.f;   // clamp boat marker inside the canvas

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void cline(lv_obj_t *cv, float x1, float y1, float x2, float y2,
                  lv_color_t col, lv_coord_t w = 1, lv_opa_t opa = LV_OPA_COVER) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = col; d.width = w; d.opa = opa;
    lv_point_t p[2] = {{(lv_coord_t)x1,(lv_coord_t)y1},{(lv_coord_t)x2,(lv_coord_t)y2}};
    lv_canvas_draw_line(cv, p, 2, &d);
}

static void ctext(lv_obj_t *cv, float x, float y, float w, const lv_font_t *font,
                  lv_color_t col, lv_text_align_t align, const char *txt) {
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = font; d.color = col; d.align = align;
    lv_canvas_draw_text(cv, (lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)w, &d, txt);
}

static void cring(lv_obj_t *cv, float r, lv_color_t col, lv_coord_t w, lv_opa_t opa) {
    lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
    a.color = col; a.width = w; a.opa = opa;
    lv_canvas_draw_arc(cv, (lv_coord_t)CXC, (lv_coord_t)CYC, (lv_coord_t)r, 0, 360, &a);
}

static void cdot(lv_obj_t *cv, float x, float y, float r, lv_color_t col, lv_opa_t opa = LV_OPA_COVER) {
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col; rd.bg_opa = opa; rd.radius = LV_RADIUS_CIRCLE; rd.border_width = 0;
    lv_canvas_draw_rect(cv, (lv_coord_t)(x - r), (lv_coord_t)(y - r),
                        (lv_coord_t)(2*r), (lv_coord_t)(2*r), &rd);
}

static void fillTri(lv_obj_t *cv, float ax,float ay,float bx,float by,float cx,float cy,
                    lv_color_t col, lv_opa_t opa = LV_OPA_COVER) {
    if (ay>by){float t;t=ax;ax=bx;bx=t;t=ay;ay=by;by=t;}
    if (ay>cy){float t;t=ax;ax=cx;cx=t;t=ay;ay=cy;cy=t;}
    if (by>cy){float t;t=bx;bx=cx;cx=t;t=by;by=cy;cy=t;}
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d); d.color=col; d.width=1; d.opa=opa;
    float hFull = cy - ay;
    for (float y=ay; y<=cy; y+=1.f) {
        float xAC = (hFull>0.f)? ax+(y-ay)/hFull*(cx-ax) : ax;
        float xO;
        if (y<=by){ float h=by-ay; xO=(h>0.f)? ax+(y-ay)/h*(bx-ax):ax; }
        else      { float h=cy-by; xO=(h>0.f)? bx+(y-by)/h*(cx-bx):bx; }
        float xL=(xAC<xO)?xAC:xO, xR=(xAC>xO)?xAC:xO;
        lv_point_t p[2]={{(lv_coord_t)xL,(lv_coord_t)y},{(lv_coord_t)xR,(lv_coord_t)y}};
        lv_canvas_draw_line(cv,p,2,&d);
    }
}

// Anchor glyph centred at (x,y): ring + stem + stock + two flukes.
static void drawAnchorGlyph(lv_obj_t *cv, float x, float y, lv_color_t col) {
    lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
    a.color = col; a.width = 2; a.opa = LV_OPA_COVER;
    lv_canvas_draw_arc(cv, (lv_coord_t)x, (lv_coord_t)(y-12), 4, 0, 360, &a);  // ring (top)
    cline(cv, x, y-8, x, y+12, col, 2);                  // stem
    cline(cv, x-9, y-3, x+9, y-3, col, 2);               // stock (crossbar)
    cline(cv, x, y+12, x-9, y+5, col, 2);                // left fluke
    cline(cv, x, y+12, x+9, y+5, col, 2);                // right fluke
}

// ── Lifecycle ────────────────────────────────────────────────────────────────
void AnchorScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(CS, CS);
    if (!_cbuf) _cbuf = (lv_color_t *)PsramArena::alloc(sz);   // reuse on theme rebuild
    if (_cbuf) {
        _canvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_canvas, _cbuf, CS, CS, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(_canvas, (SCREEN_W - CS) / 2, 2);
    }

    // ── On-screen controls (bottom band, clear of the edge nav arrows) ───────
    auto mkBtn = [&](lv_obj_t *&out, const char *txt, int x, int w, lv_color_t bg,
                     lv_color_t fg, lv_event_cb_t cb) -> lv_obj_t * {
        lv_obj_t *b = lv_btn_create(container);
        lv_obj_set_size(b, w, 46);
        lv_obj_set_pos(b, x, 428);
        lv_obj_set_style_bg_color(b, bg, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, FONT_SMALL, 0);
        lv_obj_set_style_text_color(l, fg, 0);
        lv_obj_center(l);
        out = b;
        return l;
    };
    mkBtn(_btnSet,   T(STR_ANCH_SET_BTN), 64, 150, CLR_ACCENT, CLR_ON_ACCENT, cbSet);
    // Was U+2212 (−, "minus sign") which the font does not contain, so the button
    // showed an empty box. LV_SYMBOL_* glyphs ARE in the font; use the matching pair.
    mkBtn(_btnMinus, LV_SYMBOL_MINUS, 222, 46, CLR_SURFACE, CLR_TEXT, cbMinus);
    mkBtn(_btnPlus,  LV_SYMBOL_PLUS,  274, 46, CLR_SURFACE, CLR_TEXT, cbPlus);
    _btnAlarmLbl =
    mkBtn(_btnAlarm, "Alarm",        330, 110, CLR_SURFACE, CLR_TEXT, cbAlarm);
}

void AnchorScreen::resetForRebuild() {
    _canvas = nullptr;
    _btnSet = _btnMinus = _btnPlus = _btnAlarm = _btnAlarmLbl = nullptr;
    // _cbuf intentionally kept (PsramArena buffer reused; never freed).
}

void AnchorScreen::onShow() {
    refreshAlarmBtn();
}

void AnchorScreen::refreshAlarmBtn() {
    if (!_btnAlarm || !_btnAlarmLbl) return;
    bool on = appConfig.cfg.anchorAlarmOn;
    lv_obj_set_style_bg_color(_btnAlarm, on ? CLR_GREEN : CLR_SURFACE, 0);
    lv_label_set_text(_btnAlarmLbl, on ? T(STR_ANCH_ALARM_ON) : T(STR_ANCH_ALARM_OFF));
    lv_obj_set_style_text_color(_btnAlarmLbl, on ? CLR_ON_ACCENT : CLR_TEXT, 0);
}

// ── Per-frame update ─────────────────────────────────────────────────────────
void AnchorScreen::update() {
    if (!_canvas || !_cbuf) return;

    float curLat, curLon, cog; uint32_t gpsAge;
    {
        auto lk = data.lock();
        curLat = data.lat; curLon = data.lon; cog = data.cog;
        gpsAge = millis() - data.lastGpsUpdate;
    }

    // Sample the breadcrumb track (~ every 3 s) while anchored with a valid fix.
    if (appConfig.cfg.anchorSet && !isnan(curLat) && !isnan(curLon) &&
        !isnan(appConfig.cfg.anchorLat)) {
        uint32_t now = millis();
        if (now - _lastTrkMs >= 3000) {
            _lastTrkMs = now;
            float n = (curLat - appConfig.cfg.anchorLat) * 60.f * 1852.f;
            float e = (curLon - appConfig.cfg.anchorLon) * 60.f * 1852.f *
                      cosf(appConfig.cfg.anchorLat * D2R);
            _trkN[_trkIdx] = n; _trkE[_trkIdx] = e;
            _trkIdx = (_trkIdx + 1) % TRACK_N;
            if (_trkIdx == 0) _trkFull = true;
            float d = sqrtf(n*n + e*e);
            if (d > _maxDist) _maxDist = d;
        }
    }
    (void)cog; (void)gpsAge;
    draw();
}

void AnchorScreen::draw() {
    // Yield-safe background fill (8-row chunks → release SPI0 for the WiFi ISR).
    {
        const lv_color_t bg = CLR_BG;
        lv_color_t *p = _cbuf;
        for (int row = 0; row < CS; row += 8) {
            int rows = (CS - row < 8) ? CS - row : 8;
            lv_color_t *end = p + (size_t)rows * CS;
            while (p < end) *p++ = bg;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Snapshot anchor config + live position.
    bool  set    = appConfig.cfg.anchorSet;
    bool  alarmOn= appConfig.cfg.anchorAlarmOn;
    bool  northUp= appConfig.cfg.anchorNorthUp;
    float radius = appConfig.cfg.anchorRadius;
    float ancLat = appConfig.cfg.anchorLat, ancLon = appConfig.cfg.anchorLon;
    float curLat, curLon, cog, hdg; uint32_t gpsAge;
    {
        auto lk = data.lock();
        curLat = data.lat; curLon = data.lon; cog = data.cog; hdg = data.hdg;
        gpsAge = millis() - data.lastGpsUpdate;
    }

    bool gpsOk = !isnan(curLat) && !isnan(curLon) && gpsAge < 10000;

    float distM = NAN, brg = NAN, nM = 0, eM = 0;
    if (set && gpsOk && !isnan(ancLat)) {
        nM = (curLat - ancLat) * 60.f * 1852.f;
        eM = (curLon - ancLon) * 60.f * 1852.f * cosf(ancLat * D2R);
        distM = sqrtf(nM*nM + eM*eM);
        brg = atan2f(eM, nM) * R2D; if (brg < 0) brg += 360.f;
    }
    bool alarming = set && alarmOn && !isnan(distM) && distM > radius;
    float mPerPx = (radius > 1.f ? radius : 40.f) / RING_PX;

    // View rotation: North-up (0) or heading-up (rotate so the bow points up).
    float rotDeg = (northUp || isnan(hdg)) ? 0.f : hdg;
    float rcr = cosf(rotDeg * D2R), rsr = sinf(rotDeg * D2R);
    // Project a (East, North) metre offset into screen pixels with the rotation.
    auto projE = [&](float E, float N, float &sx, float &sy) {
        sx = CXC + (E * rcr - N * rsr) / mPerPx;
        sy = CYC - (E * rsr + N * rcr) / mPerPx;
    };

    // ── Range rings ──────────────────────────────────────────────────────────
    cring(_canvas, RING_PX * 0.5f, CLR_TEXT_DIM, 1, LV_OPA_40);   // inner half-radius
    cring(_canvas, RING_PX,        alarming ? CLR_RED : CLR_ORANGE,
          alarming ? 4 : 3, LV_OPA_COVER);                        // alarm circle
    {   // radius label on the alarm ring (bottom)
        char rb[16]; snprintf(rb, sizeof(rb), "%d m", (int)(radius + 0.5f));
        ctext(_canvas, CXC - 30, CYC + RING_PX + 2, 60, FONT_TINY,
              alarming ? CLR_RED : CLR_ORANGE, LV_TEXT_ALIGN_CENTER, rb);
    }

    // North marker — rotates with the view so it always points to true north.
    {
        float nx = CXC + (RING_PX + 22.f) * sinf(-rotDeg * D2R);
        float ny = CYC - (RING_PX + 22.f) * cosf(-rotDeg * D2R);
        ctext(_canvas, nx - 8, ny - 8, 16, FONT_SMALL, CLR_TEXT, LV_TEXT_ALIGN_CENTER, "N");
    }

    // ── Breadcrumb track ─────────────────────────────────────────────────────
    int n = _trkFull ? TRACK_N : _trkIdx;
    int start = _trkFull ? _trkIdx : 0;
    for (int k = 0; k < n; k++) {
        int i = (start + k) % TRACK_N;
        float x, y; projE(_trkE[i], _trkN[i], x, y);
        if (x > 4 && x < CS-4 && y > 4 && y < CS-4)
            cdot(_canvas, x, y, 1.5f, CLR_WIND, LV_OPA_50);
    }

    // ── Anchor at centre ─────────────────────────────────────────────────────
    drawAnchorGlyph(_canvas, CXC, CYC, set ? CLR_TEXT : CLR_TEXT_DIM);

    // ── Boat marker + rode line ──────────────────────────────────────────────
    if (set && gpsOk && !isnan(distM)) {
        float bx, by; projE(eM, nM, bx, by);
        // clamp inside the canvas; flag off-scale by colour
        float dx = bx - CXC, dy = by - CYC, dpx = sqrtf(dx*dx + dy*dy);
        if (dpx > MAXBOAT) { bx = CXC + dx / dpx * MAXBOAT; by = CYC + dy / dpx * MAXBOAT; }
        cline(_canvas, CXC, CYC, bx, by, CLR_TEXT_DIM, 1, LV_OPA_60);   // rode/scope
        // boat triangle pointing along COG, in the rotated view frame
        float h = (isnan(cog) ? 0.f : cog) - rotDeg;
        float ar = h * D2R;
        float tipx = bx + 11.f * sinf(ar),       tipy = by - 11.f * cosf(ar);
        float lar = (h + 140.f) * D2R, rar = (h - 140.f) * D2R;
        float lx = bx + 8.f * sinf(lar), ly = by - 8.f * cosf(lar);
        float rx = bx + 8.f * sinf(rar), ry = by - 8.f * cosf(rar);
        lv_color_t bc = alarming ? CLR_RED : CLR_GREEN;
        fillTri(_canvas, tipx, tipy, lx, ly, rx, ry, bc, LV_OPA_COVER);
    }

    // ── Corner readouts ──────────────────────────────────────────────────────
    char buf[24];
    // Top-left: distance from anchor (kept below the 20 px demo banner)
    ctext(_canvas, 8, 22, 120, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT,
          T(STR_ANCH_DISTANCE));
    if (isnan(distM)) snprintf(buf, sizeof(buf), "--");
    else              snprintf(buf, sizeof(buf), "%d m", (int)(distM + 0.5f));
    ctext(_canvas, 8, 38, 150, FONT_XL, alarming ? CLR_RED : CLR_TEXT, LV_TEXT_ALIGN_LEFT, buf);
    // Top-right: bearing
    ctext(_canvas, CS - 128, 22, 120, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT,
          T(STR_ANCH_BEARING));
    if (isnan(brg)) snprintf(buf, sizeof(buf), "--");
    else            snprintf(buf, sizeof(buf), "%03d\xc2\xb0", (int)(brg + 0.5f) % 360);
    ctext(_canvas, CS - 128, 38, 120, FONT_XL, CLR_TEXT, LV_TEXT_ALIGN_RIGHT, buf);
    // Bottom-left: max drift
    ctext(_canvas, 8, CS - 44, 120, FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, "MAX");
    if (_maxDist <= 0.f) snprintf(buf, sizeof(buf), "--");
    else                 snprintf(buf, sizeof(buf), "%d m", (int)(_maxDist + 0.5f));
    ctext(_canvas, 8, CS - 28, 120, FONT_SMALL, CLR_TEXT, LV_TEXT_ALIGN_LEFT, buf);

    // Centre status text when not armed / no fix / dragging.
    const char *msg = nullptr; lv_color_t mc = CLR_TEXT_DIM;
    if (!set)            { msg = T(STR_ANCH_NOT_SET); }
    else if (!gpsOk)     { msg = T(STR_ANCH_NO_GPS);          mc = CLR_ORANGE; }
    else if (alarming)   { msg = T(STR_ALARM_ANCHOR_DRAG);    mc = CLR_RED; }
    if (msg)
        ctext(_canvas, CXC - 110, CYC + RING_PX * 0.5f + 8, 220, FONT_SMALL,
              mc, LV_TEXT_ALIGN_CENTER, msg);

    lv_obj_invalidate(_canvas);
}

// ── Button callbacks ─────────────────────────────────────────────────────────
void AnchorScreen::cbSet(lv_event_t *e) {
    AnchorScreen *self = (AnchorScreen *)lv_event_get_user_data(e);
    float lat, lon; bool ok;
    {
        auto lk = data.lock();
        lat = data.lat; lon = data.lon;
        ok = !isnan(lat) && !isnan(lon) && (millis() - data.lastGpsUpdate) < 10000;
    }
    if (!ok) return;                       // no fix → ignore (status shown on screen)
    appConfig.cfg.anchorSet = true;
    appConfig.cfg.anchorLat = lat;
    appConfig.cfg.anchorLon = lon;
    appConfig.save();
    self->_trkIdx = 0; self->_trkFull = false; self->_maxDist = 0.f; self->_lastTrkMs = 0;
}

void AnchorScreen::cbMinus(lv_event_t *e) {
    (void)e;
    appConfig.cfg.anchorRadius = clampf(appConfig.cfg.anchorRadius - 5.f, 10.f, 200.f);
    appConfig.save();
}

void AnchorScreen::cbPlus(lv_event_t *e) {
    (void)e;
    appConfig.cfg.anchorRadius = clampf(appConfig.cfg.anchorRadius + 5.f, 10.f, 200.f);
    appConfig.save();
}

void AnchorScreen::cbAlarm(lv_event_t *e) {
    AnchorScreen *self = (AnchorScreen *)lv_event_get_user_data(e);
    appConfig.cfg.anchorAlarmOn = !appConfig.cfg.anchorAlarmOn;
    appConfig.save();
    self->refreshAlarmBtn();
}
