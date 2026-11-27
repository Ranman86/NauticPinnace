// AnchorScreen.cpp - anchor watch ("Ankerwache")
//
// Paint cost: a frame measured up to 47 ms on the 7B (with a long breadcrumb
// track) - and it was painted every frame, whether anything had changed or
// not, so LVGL re-rendered the whole canvas every time. What brings that down, each
// without changing a single pixel (the reasons are at the code): a frame whose
// inputs are unchanged is not painted at all (update()); the LVGL draws of a
// frame - up to 240 breadcrumbs, two rings, the glyph, the rode and a dozen
// texts - share one draw context instead of building one each (CdLvBatch), and
// the boat triangle is stored row by row straight into the buffer instead of
// one LVGL line per row (fillTri()).

#include "AnchorScreen.h"
#include "../../nmea/AnchorSyncNode.h"
#include "RenderYield.h"
#include "../Theme.h"
#include "../UiConfig.h"
#include "../CanvasDraw.h"
#include "../../nmea/DataModel.h"
#include "PaintPhase.h"
#include <math.h>
#include <string.h>

#ifdef SIMULATOR
void (*g_simAnchorTrack)(float *, float *, int, int &, bool &, float &, uint32_t &) = nullptr;
#endif

// ── Paint phases (measurement builds only, see PaintPhase.h) ──────────────────
// Eight phases in paint order: data lock + track sampler + frame key in
// update(); the clear; the two rings; the radius label and the north marker
// (mark); the breadcrumbs (trk); the anchor glyph, rode and boat (boat); the
// corner readouts and the status line, up to the one invalidate (txt); and
// lbl = every text draw of the frame added up, also counted in mark and txt.
// A frame whose key equals the painted one ends after pre, so the later
// phases print x<cnt>. aux dots = breadcrumbs drawn in a frame, calls = LVGL
// draws in it (the triangle rows are none any more); both window maxima.
#if PP_ENABLED
enum { AN_PH_PRE = 0, AN_PH_CLR, AN_PH_RING, AN_PH_MARK, AN_PH_TRK, AN_PH_BOAT,
       AN_PH_TXT, AN_PH_LBL, AN_PH_N };
enum { AN_AUX_DOTS = 0, AN_AUX_CALLS };
static PaintPhaseSet s_pp;
static PaintPhaseLap s_lblLap = { 0, 0 };   // all ctext() draws of one frame
static uint32_t      s_calls  = 0;          // LVGL draws of one frame

// Declared extern at the call site (main.cpp), like windPhaseTake().
void anchorPhaseReport() {
    static const char *const names[AN_PH_N] = {
        "pre", "clr", "ring", "mark", "trk", "boat", "txt", "lbl" };
    static const char *const aux[PP_MAX_AUX] = { "dots", "calls" };
    ppReport("anc", names, AN_PH_N, s_pp, aux);
}
#endif

// ── Local constants & helpers ────────────────────────────────────────────────
static constexpr float D2R = 0.017453292f;
static constexpr float R2D = 57.29578f;

static constexpr float CXC      = AnchorScreen::CS * 0.5f;   // canvas centre
static constexpr float CYC      = AnchorScreen::CS * 0.5f;
static constexpr float RING_PX  = 132.f * UI_SF;   // screen radius of the alarm circle
static constexpr float MAXBOAT  = 200.f * UI_SF;   // clamp boat marker inside the canvas

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// The LVGL draws of a frame all go through ONE draw context (CdLvBatch,
// CanvasDraw.h), which draw() opens after the clear: each cdLv* call below is
// the lv_canvas_draw_* it replaces, with the same descriptor and the same
// coordinates, but without the fake display that one built and tore down
// around every single draw, and without its invalidate of the whole canvas
// (draw() ends with one). Nothing that yields or touches an LVGL object may
// run while the batch is open.
static void cline(CdLvBatch &b, float x1, float y1, float x2, float y2,
                  lv_color_t col, lv_coord_t w = 1, lv_opa_t opa = LV_OPA_COVER) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = col; d.width = w; d.opa = opa;
    lv_point_t p[2] = {{(lv_coord_t)x1,(lv_coord_t)y1},{(lv_coord_t)x2,(lv_coord_t)y2}};
    cdLvLine(b, p, 2, &d);
    PP_ONLY(s_calls++);
}

static void ctext(CdLvBatch &b, float x, float y, float w, const lv_font_t *font,
                  lv_color_t col, lv_text_align_t align, const char *txt) {
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = font; d.color = col; d.align = align;
    PP_LAP_START(s_lblLap);
    cdLvText(b, (lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)w, &d, txt);
    PP_LAP_STOP(s_lblLap);
    PP_ONLY(s_calls++);
}

static void cring(CdLvBatch &b, float r, lv_color_t col, lv_coord_t w, lv_opa_t opa) {
    lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
    a.color = col; a.width = w; a.opa = opa;
    cdLvArc(b, (lv_coord_t)CXC, (lv_coord_t)CYC, (lv_coord_t)r, 0, 360, &a);
    PP_ONLY(s_calls++);
}

static void cdot(CdLvBatch &b, float x, float y, float r, lv_color_t col, lv_opa_t opa = LV_OPA_COVER) {
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col; rd.bg_opa = opa; rd.radius = LV_RADIUS_CIRCLE; rd.border_width = 0;
    cdLvRect(b, (lv_coord_t)(x - r), (lv_coord_t)(y - r),
             (lv_coord_t)(2*r), (lv_coord_t)(2*r), &rd);
    PP_ONLY(s_calls++);
}

// Filled triangle, one opaque row per scanline, stored straight into the
// canvas. Each row used to be an lv_canvas_draw_line of width 1 from
// (xL, y) to (xR, y), cast to lv_coord_t - a whole fake display per row. The
// float loop and the casts are the same; what changed is that the row's
// pixels are stored here, exactly the ones LVGL 8.4 filled for that line:
//   - equal end points draw nothing (lv_draw_sw_line() returns at once);
//   - otherwise draw_line_hor() fills [min, max - 1] of the row - the right
//     end point is left out - clipped to the canvas, and with no mask and
//     full opacity that blend is a plain store of the colour (fill_normal()
//     -> lv_color_fill()).
// So a row is [X0, X1 - 1] when X1 > X0, and nothing otherwise. X1 < X0
// cannot come up, so cdSpanH() never swaps the ends: xL is the smaller
// crossing and the cast never reverses an order, and with a NaN crossing
// both ends are that same NaN, i.e. X0 == X1. cdSpanH() drops a row off the
// canvas and clips the span to it, as LVGL's clip area did. Opaque only: the
// one caller draws the boat at full opacity. No LVGL call, so it may run
// inside the open batch.
static void fillTri(const CdTarget &tg, float ax,float ay,float bx,float by,float cx,float cy,
                    lv_color_t col) {
    if (ay>by){float t;t=ax;ax=bx;bx=t;t=ay;ay=by;by=t;}
    if (ay>cy){float t;t=ax;ax=cx;cx=t;t=ay;ay=cy;cy=t;}
    if (by>cy){float t;t=bx;bx=cx;cx=t;t=by;by=cy;cy=t;}
    float hFull = cy - ay;
    for (float y=ay; y<=cy; y+=1.f) {
        float xAC = (hFull>0.f)? ax+(y-ay)/hFull*(cx-ax) : ax;
        float xO;
        if (y<=by){ float h=by-ay; xO=(h>0.f)? ax+(y-ay)/h*(bx-ax):ax; }
        else      { float h=cy-by; xO=(h>0.f)? bx+(y-by)/h*(cx-bx):bx; }
        float xL=(xAC<xO)?xAC:xO, xR=(xAC>xO)?xAC:xO;
        const lv_coord_t X0 = (lv_coord_t)xL, X1 = (lv_coord_t)xR;
        if (X1 > X0) cdSpanH(tg, (lv_coord_t)y, X0, X1 - 1, col, LV_OPA_COVER);
    }
}

// Anchor glyph centred at (x,y): ring + stem + stock + two flukes.
static void drawAnchorGlyph(CdLvBatch &b, float x, float y, lv_color_t col) {
    lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
    a.color = col; a.width = 2; a.opa = LV_OPA_COVER;
    cdLvArc(b, (lv_coord_t)x, (lv_coord_t)(y - UI_S(12)), UI_S(4), 0, 360, &a);  // ring (top)
    PP_ONLY(s_calls++);
    cline(b, x, y - UI_S(8),  x, y + UI_S(12), col, 2);                          // stem
    cline(b, x - UI_S(9), y - UI_S(3), x + UI_S(9), y - UI_S(3), col, 2);        // stock (crossbar)
    cline(b, x, y + UI_S(12), x - UI_S(9), y + UI_S(5), col, 2);                 // left fluke
    cline(b, x, y + UI_S(12), x + UI_S(9), y + UI_S(5), col, 2);                 // right fluke
}

// ── Frame key ─────────────────────────────────────────────────────────────────
// A float's bit pattern, with every NaN folded into one: wherever a NaN can
// reach the key, any NaN draws the same (no boat course -> 0°, see draw()).
static uint32_t keyBits(float v) {
    if (isnan(v)) return 0x7FC00000u;   // itself a NaN pattern: no number has it
    uint32_t k;
    memcpy(&k, &v, sizeof k);
    return k;
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
        lv_obj_set_pos(_canvas, (SCREEN_W - CS) / 2, UI_S(2));
    }

    // Breadcrumb rings, same reuse idiom as _cbuf above: allocated once and
    // kept across theme rebuilds, so the swing track survives a live restyle
    // instead of being re-allocated (the arena never frees) and re-zeroed.
    // The arena hands back zeroed memory, which matches _trkIdx = 0 /
    // _trkFull = false - nothing is read until update() has written it.
    if (!_trkN) _trkN = (float *)PsramArena::alloc(TRACK_N * sizeof(float));
    if (!_trkE) _trkE = (float *)PsramArena::alloc(TRACK_N * sizeof(float));

    // New objects, possibly a new theme, font or language - and a reused
    // buffer that still holds the old picture: the next update() paints in
    // full, whatever the frame key says.
    _painted = false;

    // ── On-screen controls (bottom band, clear of the edge nav arrows) ───────
    auto mkBtn = [&](lv_obj_t *&out, const char *txt, int x, int w, lv_color_t bg,
                     lv_color_t fg, lv_event_cb_t cb) -> lv_obj_t * {
        lv_obj_t *b = lv_btn_create(container);
        lv_obj_set_size(b, w, UI_S(46));
        lv_obj_set_pos(b, x, SCREEN_H - UI_S(52));   // band bottom 6 px above the panel edge
        lv_obj_set_style_bg_color(b, bg, 0);
        lv_obj_set_style_radius(b, UI_S(8), 0);
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
    _btnSetLbl =
    mkBtn(_btnSet,   T(STR_ANCH_SET_BTN), UI_S(64), UI_S(150), CLR_ACCENT, CLR_ON_ACCENT, cbSet);
    refreshSetBtn();
    // Was U+2212 (−, "minus sign") which the font does not contain, so the button
    // showed an empty box. LV_SYMBOL_* glyphs ARE in the font; use the matching pair.
    mkBtn(_btnMinus, LV_SYMBOL_MINUS, UI_S(222), UI_S(46), CLR_SURFACE, CLR_TEXT, cbMinus);
    mkBtn(_btnPlus,  LV_SYMBOL_PLUS,  UI_S(274), UI_S(46), CLR_SURFACE, CLR_TEXT, cbPlus);
    _btnAlarmLbl =
    mkBtn(_btnAlarm, "Alarm",        UI_S(330), UI_S(110), CLR_SURFACE, CLR_TEXT, cbAlarm);
}

void AnchorScreen::resetForRebuild() {
    _canvas = nullptr;
    _btnSet = _btnMinus = _btnPlus = _btnAlarm = _btnAlarmLbl = nullptr;
    _btnSetLbl = nullptr;
    // _cbuf and _trkN/_trkE intentionally kept (PsramArena buffers reused;
    // the arena never frees, and keeping the track preserves the swing history
    // across a live theme rebuild).
}

// Re-label the buttons when the state moved under them. Cheap when nothing
// changed, which is the normal case.
void AnchorScreen::syncButtons() {
    if (appConfig.cfg.anchorSet != _btnSetShown) {
        _btnSetShown = appConfig.cfg.anchorSet;
        refreshSetBtn();
    }
    if (appConfig.cfg.anchorAlarmOn != _btnAlarmShown) {
        _btnAlarmShown = appConfig.cfg.anchorAlarmOn;
        refreshAlarmBtn();
    }
}

void AnchorScreen::onShow() {
    _btnSetShown   = !appConfig.cfg.anchorSet;       // force both to redraw
    _btnAlarmShown = !appConfig.cfg.anchorAlarmOn;
    syncButtons();
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
    PP_BEGIN(s_pp);
    syncButtons();   // the bus may have set or lifted the anchor since the last frame
    // _trkN/_trkE joined this guard when they moved into the PSRAM arena: both
    // the sampler below and the breadcrumb loop in draw() index them without a
    // further check, so an exhausted arena must stop us here rather than
    // dereference a null pointer. draw() is only ever called from this
    // function, so this one guard covers it.
    if (!_canvas || !_cbuf || !_trkN || !_trkE) return;

    // One snapshot of the live data for the whole frame: the sampler, the
    // frame key and draw() all work from it (draw() used to take a second
    // lock of its own, and a third for the time offset).
    Frame f;
    float hdg; uint32_t gpsAge;
    {
        auto lk = data.lock();
        f.curLat = data.lat; f.curLon = data.lon; f.cog = data.cog; hdg = data.hdg;
        gpsAge = millis() - data.lastGpsUpdate;
        f.offMin = data.localOffsetMin;
    }
    const float curLat = f.curLat, curLon = f.curLon;

#ifdef SIMULATOR
    // Test harness (sim/sim_main.cpp, --set anchortrack): lays a fixed track
    // into the rings on the first frame with an anchor down and, unless it
    // runs "live", keeps _lastTrkMs current so that the sampler below never
    // takes a sample - the dots are then the same in every run. It writes the
    // same members the sampler does; anything added to the sampler's
    // bookkeeping belongs in simAnchorTrack() as well - except the track
    // generation, which is bumped here whenever the call changed the track.
    if (g_simAnchorTrack) {
        static float s_n[TRACK_N], s_e[TRACK_N];
        memcpy(s_n, _trkN, sizeof s_n);
        memcpy(s_e, _trkE, sizeof s_e);
        const int idx0 = _trkIdx; const bool full0 = _trkFull; const float max0 = _maxDist;
        g_simAnchorTrack(_trkN, _trkE, TRACK_N, _trkIdx, _trkFull, _maxDist, _lastTrkMs);
        if (memcmp(s_n, _trkN, sizeof s_n) != 0 || memcmp(s_e, _trkE, sizeof s_e) != 0 ||
            _trkIdx != idx0 || _trkFull != full0 || memcmp(&_maxDist, &max0, sizeof max0) != 0)
            _trkGen++;
    }
#endif

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
            _trkGen++;
        }
    }

    // Snapshot anchor config, read where draw() used to read it.
    f.set     = appConfig.cfg.anchorSet;
    f.alarmOn = appConfig.cfg.anchorAlarmOn;
    f.radius  = appConfig.cfg.anchorRadius;
    f.ancLat  = appConfig.cfg.anchorLat;
    f.ancLon  = appConfig.cfg.anchorLon;
    f.setUtc  = appConfig.cfg.anchorSetUtc;
    f.gpsOk   = !isnan(f.curLat) && !isnan(f.curLon) && gpsAge < 10000;
    // View rotation: North-up (0) or heading-up (rotate so the bow points up).
    f.rotDeg  = (appConfig.cfg.anchorNorthUp || isnan(hdg)) ? 0.f : hdg;

    // Skip a frame whose key equals the one on the canvas: no clear, no
    // paint, no invalidate. The buffer belongs to this screen alone, so that
    // frame is still in it - also after the screen was hidden - and LVGL goes
    // on showing it. Which inputs reach a pixel depends on the state, and
    // only those are keyed:
    //   always        - anchor set or not (glyph colour, status line, every
    //                   reading), the radius (label, scale), the rotation
    //                   (north marker, track, boat), the track (by its
    //                   generation) and the largest drift (MAX);
    //   anchor set    - whether the fix is good (the time-driven input: it
    //                   lapses 10 s after the last position, with nothing
    //                   else changing) and the "since" time;
    //   ... and a fix with the anchor position known - the boat: own and
    //                   anchor position, course, and whether the alarm is
    //                   armed (an armed one outside the circle turns it red).
    // With the anchor up only the radius, the rotation and the track count, so
    // a moving position repaints nothing (the heading still turns a
    // heading-up view).
    FrameKey k;
    memset(&k, 0, sizeof k);
    k.set     = f.set;
    k.lang    = (uint8_t)i18nLang();
    k.radius  = keyBits(f.radius);
    k.rotDeg  = keyBits(f.rotDeg);
    k.maxDist = keyBits(_maxDist);
    k.trkGen  = _trkGen;
    if (f.set) {
        k.gpsOk  = f.gpsOk;
        k.setUtc = f.setUtc;
        if (f.setUtc) k.offMin = f.offMin;
        if (f.gpsOk && !isnan(f.ancLat)) {
            k.alarmOn = f.alarmOn;
            k.curLat  = keyBits(f.curLat);
            k.curLon  = keyBits(f.curLon);
            k.ancLat  = keyBits(f.ancLat);
            k.ancLon  = keyBits(f.ancLon);
            k.cog     = keyBits(f.cog);
        }
    }
    PP_END(s_pp, AN_PH_PRE);
    if (_painted && memcmp(&k, &_paintedKey, sizeof k) == 0) { PP_FRAME(s_pp); return; }

    draw(f);
    _painted    = true;
    _paintedKey = k;
    PP_FRAME(s_pp);
}

void AnchorScreen::draw(const Frame &f) {
    // Background fill in 8-row chunks, offering a yield after each; renderYield()
    // decides how many of those offers are worth a tick (see RenderYield.h).
    {
        const lv_color_t bg = CLR_BG;
        lv_color_t *p = _cbuf;
        for (int row = 0; row < CS; row += 8) {
            int rows = (CS - row < 8) ? CS - row : 8;
            lv_color_t *end = p + (size_t)rows * CS;
            while (p < end) *p++ = bg;
            renderYield();
        }
    }
    PP_END(s_pp, AN_PH_CLR);

    // The frame's inputs, from update()'s snapshot.
    const bool  set     = f.set;
    const bool  alarmOn = f.alarmOn;
    const float radius  = f.radius;
    const float ancLat  = f.ancLat, ancLon = f.ancLon;
    const float curLat  = f.curLat, curLon = f.curLon, cog = f.cog;
    const bool  gpsOk   = f.gpsOk;
    const float rotDeg  = f.rotDeg;

    float distM = NAN, brg = NAN, nM = 0, eM = 0;
    if (set && gpsOk && !isnan(ancLat)) {
        nM = (curLat - ancLat) * 60.f * 1852.f;
        eM = (curLon - ancLon) * 60.f * 1852.f * cosf(ancLat * D2R);
        distM = sqrtf(nM*nM + eM*eM);
        brg = atan2f(eM, nM) * R2D; if (brg < 0) brg += 360.f;
    }
    bool alarming = set && alarmOn && !isnan(distM) && distM > radius;
    float mPerPx = (radius > 1.f ? radius : 40.f) / RING_PX;

    float rcr = cosf(rotDeg * D2R), rsr = sinf(rotDeg * D2R);
    // Project a (East, North) metre offset into screen pixels with the rotation.
    auto projE = [&](float E, float N, float &sx, float &sy) {
        sx = CXC + (E * rcr - N * rsr) / mPerPx;
        sy = CYC - (E * rsr + N * rcr) / mPerPx;
    };

    // Everything from here to the invalidate goes through one LVGL draw
    // context, in the order it was always drawn in (every draw blends onto
    // what is below it); the boat triangle is stored directly in between.
    const CdTarget tg = cdTargetOf(_canvas);
    {
        CdLvBatch b(_canvas, false);

        // ── Range rings ──────────────────────────────────────────────────────
        cring(b, RING_PX * 0.5f, CLR_TEXT_DIM, 1, LV_OPA_40);   // inner half-radius
        cring(b, RING_PX,        alarming ? CLR_RED : CLR_ORANGE,
              alarming ? UI_S(4) : 3, LV_OPA_COVER);            // alarm circle
        PP_END(s_pp, AN_PH_RING);
        {   // radius label on the alarm ring (bottom)
            char rb[16]; snprintf(rb, sizeof(rb), "%d m", (int)(radius + 0.5f));
            ctext(b, CXC - UI_S(30), CYC + RING_PX + 2, UI_S(60), FONT_TINY,
                  alarming ? CLR_RED : CLR_ORANGE, LV_TEXT_ALIGN_CENTER, rb);
        }

        // North marker — rotates with the view so it always points to true north.
        {
            float nx = CXC + (RING_PX + 22.f * UI_SF) * sinf(-rotDeg * D2R);
            float ny = CYC - (RING_PX + 22.f * UI_SF) * cosf(-rotDeg * D2R);
            ctext(b, nx - UI_S(8), ny - UI_S(8), UI_S(16), FONT_SMALL, CLR_TEXT, LV_TEXT_ALIGN_CENTER, "N");
        }
        PP_END(s_pp, AN_PH_MARK);

        // ── Breadcrumb track ─────────────────────────────────────────────────
        int n = _trkFull ? TRACK_N : _trkIdx;
        int start = _trkFull ? _trkIdx : 0;
        PP_ONLY(int dots = 0);
        for (int k = 0; k < n; k++) {
            int i = (start + k) % TRACK_N;
            float x, y; projE(_trkE[i], _trkN[i], x, y);
            if (x > UI_S(4) && x < CS - UI_S(4) && y > UI_S(4) && y < CS - UI_S(4)) {
                cdot(b, x, y, 1.5f * UI_SF, CLR_WIND, LV_OPA_50);
                PP_ONLY(dots++);
            }
        }
        PP_AUX(s_pp, AN_AUX_DOTS, dots);
        PP_END(s_pp, AN_PH_TRK);

        // ── Anchor at centre ─────────────────────────────────────────────────
        drawAnchorGlyph(b, CXC, CYC, set ? CLR_TEXT : CLR_TEXT_DIM);

        // ── Boat marker + rode line ──────────────────────────────────────────
        if (set && gpsOk && !isnan(distM)) {
            float bx, by; projE(eM, nM, bx, by);
            // clamp inside the canvas; flag off-scale by colour
            float dx = bx - CXC, dy = by - CYC, dpx = sqrtf(dx*dx + dy*dy);
            if (dpx > MAXBOAT) { bx = CXC + dx / dpx * MAXBOAT; by = CYC + dy / dpx * MAXBOAT; }
            cline(b, CXC, CYC, bx, by, CLR_TEXT_DIM, 1, LV_OPA_60);   // rode/scope
            // boat triangle pointing along COG, in the rotated view frame
            float h = (isnan(cog) ? 0.f : cog) - rotDeg;
            float ar = h * D2R;
            float tipx = bx + 11.f * UI_SF * sinf(ar),       tipy = by - 11.f * UI_SF * cosf(ar);
            float lar = (h + 140.f) * D2R, rar = (h - 140.f) * D2R;
            float lx = bx + 8.f * UI_SF * sinf(lar), ly = by - 8.f * UI_SF * cosf(lar);
            float rx = bx + 8.f * UI_SF * sinf(rar), ry = by - 8.f * UI_SF * cosf(rar);
            lv_color_t bc = alarming ? CLR_RED : CLR_GREEN;
            fillTri(tg, tipx, tipy, lx, ly, rx, ry, bc);
        }
        PP_END(s_pp, AN_PH_BOAT);

        // ── Corner readouts ──────────────────────────────────────────────────
        char buf[24];
        // Top-left: distance from anchor (kept below the demo banner)
        ctext(b, UI_S(8), UI_S(22), UI_S(120), FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT,
              T(STR_ANCH_DISTANCE));
        if (isnan(distM)) snprintf(buf, sizeof(buf), "--");
        else              snprintf(buf, sizeof(buf), "%d m", (int)(distM + 0.5f));
        ctext(b, UI_S(8), UI_S(38), UI_S(150), FONT_XL, alarming ? CLR_RED : CLR_TEXT, LV_TEXT_ALIGN_LEFT, buf);
        // Top-right: bearing
        ctext(b, CS - UI_S(128), UI_S(22), UI_S(120), FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT,
              T(STR_ANCH_BEARING));
        if (isnan(brg)) snprintf(buf, sizeof(buf), "--");
        else            snprintf(buf, sizeof(buf), "%03d\xc2\xb0", (int)(brg + 0.5f) % 360);
        ctext(b, CS - UI_S(128), UI_S(38), UI_S(120), FONT_XL, CLR_TEXT, LV_TEXT_ALIGN_RIGHT, buf);
        // Bottom-left: max drift
        ctext(b, UI_S(8), CS - UI_S(44), UI_S(120), FONT_SMALL, CLR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, "MAX");
        if (_maxDist <= 0.f) snprintf(buf, sizeof(buf), "--");
        else                 snprintf(buf, sizeof(buf), "%d m", (int)(_maxDist + 0.5f));
        ctext(b, UI_S(8), CS - UI_S(28), UI_S(120), FONT_SMALL, CLR_TEXT, LV_TEXT_ALIGN_LEFT, buf);
        // Bottom-right: when the anchor fell (setUtc, shown in local time). NOT
        // revUtc - that moves on every change and would show the time of the last
        // radius nudge instead of the moment the anchor went down. Blank when the
        // device had no clock at the time: a missing time beats an invented one.
        ctext(b, CS - UI_S(128), CS - UI_S(44), UI_S(120), FONT_SMALL, CLR_TEXT_DIM,
              LV_TEXT_ALIGN_RIGHT, T(STR_ANCH_SINCE));
        if (set && f.setUtc) {
            const uint32_t loc = f.setUtc + (int32_t)f.offMin * 60;
            const uint32_t sod = loc % 86400UL;
            snprintf(buf, sizeof(buf), "%02u:%02u",
                     (unsigned)(sod / 3600), (unsigned)((sod % 3600) / 60));
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        ctext(b, CS - UI_S(128), CS - UI_S(28), UI_S(120), FONT_SMALL, CLR_TEXT,
              LV_TEXT_ALIGN_RIGHT, buf);

        // Centre status text when not armed / no fix / dragging.
        const char *msg = nullptr; lv_color_t mc = CLR_TEXT_DIM;
        if (!set)            { msg = T(STR_ANCH_NOT_SET); }
        else if (!gpsOk)     { msg = T(STR_ANCH_NO_GPS);          mc = CLR_ORANGE; }
        else if (alarming)   { msg = T(STR_ALARM_ANCHOR_DRAG);    mc = CLR_RED; }
        if (msg)
            ctext(b, CXC - UI_S(110), CYC + RING_PX * 0.5f + UI_S(8), UI_S(220), FONT_SMALL,
                  mc, LV_TEXT_ALIGN_CENTER, msg);
    }

    lv_obj_invalidate(_canvas);
    PP_END(s_pp, AN_PH_TXT);
    PP_ADD(s_pp, AN_PH_LBL, s_lblLap.us);
    PP_ONLY(s_lblLap.us = 0);
    PP_AUX(s_pp, AN_AUX_CALLS, s_calls);
    PP_ONLY(s_calls = 0);
}

// ── Button callbacks ─────────────────────────────────────────────────────────
// Label and colour for the dual-purpose set/lift button.
void AnchorScreen::refreshSetBtn() {
    if (!_btnSet || !_btnSetLbl) return;
    const bool down = appConfig.cfg.anchorSet;
    lv_label_set_text(_btnSetLbl, T(down ? STR_ANCH_LIFT_BTN : STR_ANCH_SET_BTN));
    lv_obj_set_style_bg_color(_btnSet, down ? CLR_SURFACE : CLR_ACCENT, 0);
    lv_obj_set_style_text_color(_btnSetLbl, down ? CLR_TEXT : CLR_ON_ACCENT, 0);
}

void AnchorScreen::cbSet(lv_event_t *e) {
    AnchorScreen *self = (AnchorScreen *)lv_event_get_user_data(e);

    if (appConfig.cfg.anchorSet) {              // anchor is down → lift it
        appConfig.cfg.anchorSet     = false;
        appConfig.cfg.anchorLat     = NAN;
        appConfig.cfg.anchorLon     = NAN;
        appConfig.cfg.anchorSetUtc  = 0;
        appConfig.cfg.anchorAlarmOn = false;    // a lifted anchor cannot drag
        AnchorSyncNode::noteLocalChange();
        appConfig.save();
        self->_trkIdx = 0; self->_trkFull = false; self->_maxDist = 0.f; self->_lastTrkMs = 0;
        self->_trkGen++;
        self->refreshSetBtn();
        self->refreshAlarmBtn();
        return;
    }

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
    // When the anchor fell. Held separately from the revision stamp and shown
    // to the user; 0 when this device has no clock, which is honest - better a
    // missing time than an invented one.
    appConfig.cfg.anchorSetUtc = anchorSyncNowUtcOrZero();
    AnchorSyncNode::noteLocalChange();
    appConfig.save();
    self->_trkIdx = 0; self->_trkFull = false; self->_maxDist = 0.f; self->_lastTrkMs = 0;
    self->_trkGen++;
    self->refreshSetBtn();
}

// The radius buttons stamp a revision like everything else: pulling the swing
// circle IS a change, and ordering by setUtc alone would have swallowed it.
void AnchorScreen::cbMinus(lv_event_t *e) {
    (void)e;
    const float before = appConfig.cfg.anchorRadius;
    appConfig.cfg.anchorRadius = clampf(before - 5.f, 10.f, 200.f);
    if (appConfig.cfg.anchorRadius != before) AnchorSyncNode::noteLocalChange();
    appConfig.save();
}

void AnchorScreen::cbPlus(lv_event_t *e) {
    (void)e;
    const float before = appConfig.cfg.anchorRadius;
    appConfig.cfg.anchorRadius = clampf(before + 5.f, 10.f, 200.f);
    if (appConfig.cfg.anchorRadius != before) AnchorSyncNode::noteLocalChange();
    appConfig.save();
}

void AnchorScreen::cbAlarm(lv_event_t *e) {
    AnchorScreen *self = (AnchorScreen *)lv_event_get_user_data(e);
    appConfig.cfg.anchorAlarmOn = !appConfig.cfg.anchorAlarmOn;
    AnchorSyncNode::noteLocalChange();
    appConfig.save();
    self->refreshAlarmBtn();
}
