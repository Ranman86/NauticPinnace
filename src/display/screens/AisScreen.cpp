#include "AisScreen.h"
#include "RenderYield.h"
#include "../../PsramArena.h"
#include "../CanvasDraw.h"
#include "PaintPhase.h"
#include <math.h>
#include <string.h>

// ── Paint phases (measurement builds only, see PaintPhase.h) ──────────────────
// Seven phases in draw order: labels + data lock (+ the info box refresh, once
// a second; the purge moved to BusWatch) in update(), the layout pass (one
// data lock for own heading and targets, projection, CPA colour), canvas
// clear WITH the range rings folded in, the range labels,
// cross-hair + own ship, and the targets. The heading's data lock used to sit
// in own and the target snapshot, projection and colour in tgt; all of it is
// lay now. ring kept its name but holds only the labels since the rings moved
// into clr (they were 600 lv_canvas_draw_line chords there before), so ring
// is rlbl plus the loop; in an AIS_RINGS_REF build it holds the five
// cdRingAA rings too. rlbl is the five range-label draw_text calls alone,
// summed - it is INSIDE ring.
// A frame whose layout equals the painted one ends after lay, so clr..tgt
// print x<cnt>: frames minus that count is the number of skipped frames.
// aux n = targets actually drawn (past all three culls), sel = 1 when the
// selection circle was drawn; both are window maxima over painted frames.
#if PP_ENABLED
enum { AI_PH_PRE = 0, AI_PH_LAY, AI_PH_CLR, AI_PH_RING, AI_PH_RLBL, AI_PH_OWN,
       AI_PH_TGT, AI_PH_N };
enum { AI_AUX_N = 0, AI_AUX_SEL };
static PaintPhaseSet s_pp;

// Declared extern at the call site (main.cpp), like windPhaseTake().
void aisPhaseReport() {
    static const char *const names[AI_PH_N] = {
        "pre", "lay", "clr", "ring", "rlbl", "own", "tgt" };
    static const char *const aux[PP_MAX_AUX] = { "n", "sel" };
    ppReport("ais", names, AI_PH_N, s_pp, aux);
}
#endif

void AisScreen::create(lv_obj_t *parent) {
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
        lv_obj_set_pos(_canvas, (SCREEN_W-CS)/2, 0);
        lv_obj_add_event_cb(_canvas, onCanvasClick, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);
        // Geometry only (no colours), so once is enough - also across rebuilds.
        if (!_ringTabOk) buildRingTable();
    } else {
        Serial.printf("[AisScreen] canvas buf alloc FAILED (%u B)\n", (unsigned)sz);
        Serial.flush();
    }
    // A rebuild (theme, fonts, language) brings a new canvas object and new
    // colours: the next update() paints in full, whatever the layout says.
    _layPainted = false;
    // Scale and count labels
    _scaleLbl = lv_label_create(container);
    char sl[12]; snprintf(sl, sizeof(sl), "5%s", T(STR_UNIT_NM));
    lv_label_set_text(_scaleLbl, sl);
    styleLabel(_scaleLbl, FONT_SMALL, CLR_TEXT_DIM);
    lv_obj_set_pos(_scaleLbl, UI_S(4), UI_S(4));

    _countLbl = lv_label_create(container);
    {
        char cb[16]; snprintf(cb, sizeof(cb), "0 %s", T(STR_AIS_TARGETS));
        lv_label_set_text(_countLbl, cb);
    }
    styleLabel(_countLbl, FONT_SMALL, CLR_TEXT_DIM);
    lv_obj_align(_countLbl, LV_ALIGN_TOP_RIGHT, UI_S(-4), UI_S(4));

    // Shown by update() while there is no own position: nothing can be placed
    // on the radar then, while the count above goes on counting what is on the
    // air - "3 targets" over an empty scope looked like a fault. In the colours
    // of the bus strip (DisplayManager::styleBusBanner()), below the own ship.
    _noPosLbl = lv_label_create(container);
    lv_label_set_text(_noPosLbl, T(STR_AIS_NO_OWN_POS));
    styleLabel(_noPosLbl, FONT_SMALL,
               lv_color_brightness(CLR_BG) < lv_color_brightness(CLR_TEXT) ? CLR_BG : CLR_TEXT);
    lv_obj_set_style_bg_color(_noPosLbl, CLR_YELLOW, 0);
    lv_obj_set_style_bg_opa(_noPosLbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(_noPosLbl, UI_S(8), 0);
    lv_obj_set_style_pad_ver(_noPosLbl, UI_S(3), 0);
    lv_obj_set_style_radius(_noPosLbl, UI_S(4), 0);
    lv_obj_align(_noPosLbl, LV_ALIGN_TOP_MID, 0, CS / 2 + UI_S(48));
    lv_obj_add_flag(_noPosLbl, LV_OBJ_FLAG_HIDDEN);

    // Info box (shown on target tap)
    _infoBox = lv_obj_create(container);
    lv_obj_set_size(_infoBox, SCREEN_W - UI_S(8), UI_S(80));
    lv_obj_align(_infoBox, LV_ALIGN_BOTTOM_MID, 0, UI_S(-2));
    styleCard(_infoBox);
    lv_obj_add_flag(_infoBox, LV_OBJ_FLAG_HIDDEN);

    _infoLbl = lv_label_create(_infoBox);
    lv_label_set_text(_infoLbl, "");
    styleLabel(_infoLbl, FONT_SMALL, CLR_TEXT);
    lv_obj_align(_infoLbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_long_mode(_infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_infoLbl, SCREEN_W - UI_S(24));
}

void AisScreen::drawRadar() {
    if (!_canvas || !_cbuf) return;
    const bool changed = layoutRadar();
    PP_END(s_pp, AI_PH_LAY);
    // Same layout as the painted one: the canvas already shows this frame.
    // Return before the clear, so there is nothing for LVGL to re-render
    // either. update() runs up to 10 times a second and AIS reports come
    // seconds apart, so most frames end here.
    if (!changed) return;
    paintRadar();
    _layPainted = true;
}

bool AisScreen::sameFrame(const LayFrame &a, const LayFrame &b) {
    return a.rangeNm == b.rangeNm && a.unit == b.unit && a.font == b.font
        && a.bg.full == b.bg.full && a.grid.full == b.grid.full
        && a.dim.full == b.dim.full && a.text.full == b.text.full
        && a.accent.full == b.accent.full
        && a.hdgValid == b.hdgValid
        && a.hdgEnd.x == b.hdgEnd.x && a.hdgEnd.y == b.hdgEnd.y
        && a.courseUp == b.courseUp && a.rotDeg == b.rotDeg
        && a.north.x == b.north.x && a.north.y == b.north.y;
}

// A target's CPA/TCPA is current only while own position, SOG and COG are
// known. DataModel::calcCpa() now clears them itself when one is missing,
// and BusWatch when own position or motion goes stale - this is the same rule
// at the point of drawing, so a CPA computed a moment before the own GPS went
// quiet is not shown against a position that no longer exists. Without these
// four the CPA is taken as unknown: the target is drawn green, as for any
// unknown CPA, and the info box shows "--". Call with data.lock() held.
static bool ownCpaInputs(const DataModel &d) {
    return !isnan(d.lat) && !isnan(d.lon) && !isnan(d.sog) && !isnan(d.cog);
}

// What course up turns the view to. The COG while the boat moves, with the same
// two limits as the wind (WindCalc.h): a boat doing 1 kn reads 0.991 and 1.011 by
// turns, and one limit would flip the view between COG and heading with every fix.
// Slower than that the COG is position noise, at rest it wanders round the compass,
// so the heading then. Without a compass the view holds the last course (held,
// heldDeg) for as long as the GPS reports; NaN means none, and the view stays north up.
static float pickCourse(float cog, float hdg, float sog, bool &useCog, bool held, float heldDeg) {
    useCog = !isnan(cog) && sog >= (useCog ? WindCalc::COG_OFF_SOG_KN : WindCalc::COG_MIN_SOG_KN);
    const float c = useCog ? cog : hdg;
    return (isnan(c) && held && !isnan(cog)) ? heldDeg : c;
}

// Canvas position of a target, or false when it is not on the radar. One function
// for the layout and the tap: the tap once skipped the range test, and a tap on
// empty water selected a vessel just beyond the outer ring.
static bool projectTarget(const AisTarget &t, float ownLat, float ownLon, float rotC, float rotS,
                          float nm2px, int rangeNm, int cs, int &tx, int &ty) {
    const float avgLat = (ownLat + t.lat) / 2.0f * DEG_TO_RAD;
    const float dx = lonDelta(t.lon, ownLon) * 60.0f * cosf(avgLat);  // nm east
    const float dy = (t.lat - ownLat) * 60.0f;                        // nm north
    if (sqrtf(dx * dx + dy * dy) > rangeNm * 1.1f) return false;
    tx = cs / 2 + (int)((dx * rotC - dy * rotS) * nm2px);
    ty = cs / 2 - (int)((dx * rotS + dy * rotC) * nm2px);
    return tx >= 0 && tx < cs && ty >= 0 && ty < cs;
}

// Layout pass: everything paintRadar() needs, in integer canvas coordinates.
// The geometry is computed with exactly the expressions the single-pass
// drawRadar() used, so the raster pass issues the same draw calls with the
// same arguments. Written into _lay in place, comparing as it goes - one
// layout buffer is enough, because a frame that differs is always painted.
bool AisScreen::layoutRadar() {
    int cx = CS/2, cy = CS/2;
    int maxR = CS/2 - UI_S(10);
    float nm2px = (float)maxR / _rangeNm;

    LayFrame f;
    f.rangeNm = _rangeNm;
    f.unit    = T(STR_UNIT_NM);
    f.font    = FONT_TINY;
    f.bg      = CLR_BG;
    f.grid    = CLR_GRID_LINE;
    f.dim     = CLR_TEXT_DIM;
    f.text    = CLR_TEXT;
    f.accent  = CLR_ACCENT;

    // Colour by CPA/TCPA. Thresholds come from the config (WebUI: AIS
    // editor, "Alarm CPA/TCPA") instead of being hardcoded; the yellow
    // pre-warning tier is 2x the red alarm tier - with the defaults
    // (0.5 nm / 10 min) that reproduces the old fixed 0.5/10 and 1.0/20.
    const float cpaAl  = appConfig.cfg.aisCpaAlarm;
    const float tcpaAl = appConfig.cfg.aisTcpaAlarm;

    bool chg = !_layPainted;
    int n = 0;
    float ownHdg;
    f.courseUp = false;
    f.rotDeg   = 0.f;
    f.north.x  = 0; f.north.y = 0;
    // One lock for the own heading and the targets (it used to be two), and
    // the targets are projected straight out of the table under it: that
    // replaces the 3.8 KB snapshot that lived on the loop stack.
    {
        auto lk = data.lock();
        // True, like the positions: a magnetic reading on a true radar is off by the
        // variation. Without a variation it stays as it came.
        float hdgT = data.hdg, cogT = data.cog;
        if (data.hdgMagnetic && !isnan(hdgT)) {
            if (!isnan(data.hdgDeviation)) hdgT += data.hdgDeviation;
            if (!isnan(data.variation))    hdgT += data.variation;
            hdgT = WindCalc::norm360(hdgT);
        }
        if (data.cogMagnetic && !isnan(cogT) && !isnan(data.variation))
            cogT = WindCalc::norm360(cogT + data.variation);
        ownHdg = isnan(cogT) ? hdgT : cogT;
        // The view follows in whole degrees and from a 2-degree change on: every turn
        // repaints the canvas, so GPS jitter must not. With no course the view stays
        // north up, and the label says so.
        const float course = pickCourse(cogT, hdgT, data.sog, _courseCog, _courseSet, _courseDeg);
        if (!appConfig.cfg.aisNorthUp && !isnan(course)) {
            float off = fabsf(course - _courseDeg);
            if (off > 180.f) off = 360.f - off;
            if (!_courseSet || off >= 2.f) { _courseDeg = roundf(course); _courseSet = true; }
            f.courseUp = true;
            f.rotDeg   = _courseDeg;
        } else {
            _courseSet = false;
        }
        const float rotC = cosf(f.rotDeg * DEG_TO_RAD), rotS = sinf(f.rotDeg * DEG_TO_RAD);
        const int cnt = data.aisCount;
        const float ownLat = data.lat, ownLon = data.lon;
        const bool  cpaOk  = ownCpaInputs(data);
        _layKnown = 0;
        for (int i = 0; i < cnt; i++) {
            const AisTarget &t = data.aisTargets[i];
            if (isnan(t.lat)||isnan(t.lon)) continue;
            _layKnown++;
            if (isnan(ownLat)||isnan(ownLon)) continue;

            int tx, ty;
            if (!projectTarget(t, ownLat, ownLon, rotC, rotS, nm2px, _rangeNm, CS, tx, ty)) continue;

            LayTarget e;
            memset(&e, 0, sizeof e);     // the fields this kind does not use stay 0
            e.x = (lv_coord_t)tx;
            e.y = (lv_coord_t)ty;

            lv_color_t col = CLR_GREEN;
            const float cpa  = cpaOk ? t.cpa  : NAN;   // see ownCpaInputs()
            const float tcpa = cpaOk ? t.tcpa : NAN;
            if (!isnan(cpa) && !isnan(tcpa)) {
                if (cpa < cpaAl && tcpa > 0 && tcpa < tcpaAl)          col = CLR_RED;
                else if (cpa < 2.f * cpaAl && tcpa < 2.f * tcpaAl)     col = CLR_YELLOW;
            }
            e.col = col;

            // Triangle pointing in COG direction, with the 6-min COG vector
            // (0.1 * sog nm) when the SOG is known; without a COG a dot.
            if (!isnan(t.cog)) {
                float cr = (t.cog - f.rotDeg)*DEG_TO_RAD;
                lv_point_t tri[3] = {
                    {(lv_coord_t)(tx+(int)(UI_S(10)*sinf(cr))),    (lv_coord_t)(ty-(int)(UI_S(10)*cosf(cr)))},
                    {(lv_coord_t)(tx+(int)(UI_S(6)*sinf(cr+2.3f))),(lv_coord_t)(ty-(int)(UI_S(6)*cosf(cr+2.3f)))},
                    {(lv_coord_t)(tx+(int)(UI_S(6)*sinf(cr-2.3f))),(lv_coord_t)(ty-(int)(UI_S(6)*cosf(cr-2.3f)))}
                };
                e.tri[0] = tri[0]; e.tri[1] = tri[1]; e.tri[2] = tri[2];
                e.kind = LT_TRI;
                if (!isnan(t.sog)) {
                    float vecNm = t.sog * 6.0f / 60.0f;
                    e.vecEnd.x = (lv_coord_t)(tx+(int)(vecNm*nm2px*sinf(cr)));
                    e.vecEnd.y = (lv_coord_t)(ty-(int)(vecNm*nm2px*cosf(cr)));
                    e.kind = LT_TRI_VEC;
                }
            } else {
                e.kind = LT_DOT;
            }

            // Name label, up to 15 chars (paintRadar() fits it to the room
            // beside the target). Copied up to the NUL only, so the rest stays 0
            // from the memset - what follows the NUL in the table is not drawn
            // and must not count as a change. Trailing blanks of a padded AIS
            // name go too, or "WAPPEN VON" would be cut as "WAPPEN ".
            // Without a name yet, the callsign: a class B vessel's two static
            // halves come separately, and part B (callsign) may be first.
            const char *label = t.name[0] ? t.name : t.callsign;
            int len = 0;
            while (len < (int)sizeof(e.name) - 1 && label[len]) { e.name[len] = label[len]; len++; }
            while (len > 0 && e.name[len - 1] == ' ') e.name[--len] = 0;

            e.sel = (_selValid && t.mmsi == _selMmsi) ? 1 : 0;

            if (n >= _lay.n || memcmp(&_lay.t[n], &e, sizeof e) != 0) chg = true;
            memcpy(&_lay.t[n], &e, sizeof e);
            n++;
        }
    }

    // Heading vector
    f.hdgValid = !isnan(ownHdg);
    f.hdgEnd.x = 0; f.hdgEnd.y = 0;
    if (f.hdgValid) {
        float ar = (ownHdg - f.rotDeg)*DEG_TO_RAD;
        f.hdgEnd.x = (lv_coord_t)(cx+(int)(UI_S(40)*sinf(ar)));
        f.hdgEnd.y = (lv_coord_t)(cy-(int)(UI_S(40)*cosf(ar)));
    }
    if (f.courseUp) {
        const float nr = -f.rotDeg*DEG_TO_RAD;
        f.north.x = (lv_coord_t)(cx+(int)((maxR - UI_S(12))*sinf(nr)));
        f.north.y = (lv_coord_t)(cy-(int)((maxR - UI_S(12))*cosf(nr)));
    }

    if (n != _lay.n || !sameFrame(f, _lay.f)) chg = true;
    _lay.f = f;
    _lay.n = n;
    return chg;
}

// cdRingAA's coverage for every s = dx*dx + dy*dy a ring pixel can have, see
// AisScreen.h. The float expressions are cdRingAA's own (w = 1, the centre on
// a pixel centre, so its dx and dy are exact integers and dx*dx + dy*dy is
// exactly (float)s), which is what makes the folded clear give the same
// pixels as cdRingAA. If cdRingAA's coverage changes, change this with it.
void AisScreen::buildRingTable() {
    const float w = 1.f, h = w * 0.5f;
    int o = 0;
    for (int ring = 1; ring <= RING_N; ring++) {
        const int   ri = ring * RING_STEP;
        const float r  = (float)ri;
        for (int s = (ri - 1) * (ri - 1); s <= (ri + 1) * (ri + 1); s++) {
            const float d = sqrtf((float)s);
            float f = h + 0.5f - fabsf(d - r);   // 1 in the middle, 0 outside
            lv_opa_t a = 0;
            if (f > 0.f) {
                if (f > 1.f) f = 1.f;
                a = (lv_opa_t)((float)RING_OPA * f);
            }
            _ringTab[o++] = a;
        }
    }
    _ringTabOk = true;
}

// A target's name, shortened until it fits `room` px in `font`: whole if it
// fits, else the longest start that fits with ".." behind it, "" if not even
// that does. `out` holds the 15 chars of a layout name plus "..".
static void fitName(char (&out)[18], const char *name, const lv_font_t *font,
                    lv_coord_t room) {
    out[0] = 0;
    if (room <= 0) return;
    int len = (int)strnlen(name, 15);
    memcpy(out, name, len);
    out[len] = 0;
    if (lv_txt_get_width(out, len, font, 0, LV_TEXT_FLAG_NONE) <= room) return;
    while (len > 0) {
        len--;
        while (len > 0 && out[len - 1] == ' ') len--;     // no "WAPPEN .."
        out[len] = '.'; out[len + 1] = '.'; out[len + 2] = 0;
        if (len > 0 && lv_txt_get_width(out, len + 2, font, 0, LV_TEXT_FLAG_NONE) <= room)
            return;
    }
    out[0] = 0;
}

// One row of paintBackground(): background, the ring runs in ascending x,
// background to the end. A function of its own (noinline: it has one call
// site) so that its store loops get registers to themselves: inlined into
// the row bookkeeping, -Os spilled the background colour and reloaded it for
// every pixel. The background loop is the old clear's: one store per pixel.
namespace {
struct AisRingRun { int x0, x1, base; };         // tab index = base + j*j, j = x - c
}
__attribute__((noinline))
static void aisRingRow(lv_color_t *row, int w, int c, const AisRingRun *run, int n,
                       const uint8_t *tab, const lv_color_t *mix, lv_color_t bg) {
    int x = 0;                                   // next pixel to write
    for (int i = 0; i < n; i++) {
        const int x0 = run[i].x0 < x ? x : run[i].x0;
        const int x1 = run[i].x1 < w ? run[i].x1 : w - 1;
        if (x1 < x0) continue;
        for (lv_color_t *p = row + x, *q = row + x0; p < q; ) *p++ = bg;
        lv_color_t *p = row + x0;
        const int b = run[i].base;
        for (int j = x0 - c; j <= x1 - c; j++) *p++ = mix[tab[b + j * j]];
        x = x1 + 1;
    }
    for (lv_color_t *p = row + x, *q = row + w; p < q; ) *p++ = bg;
}

// The canvas clear with the five range rings folded in. Row by row, every
// pixel is written exactly once and nothing is read back: background up to
// the next ring run, the run from _ringTab (each coverage mixed against the
// background once per paint), background again. Same pixels as a clear
// followed by cdRingAA for each ring, see AisScreen.h.
//
// Per row and ring, the run holds every |dx| = j with
// (r-1)^2 <= j*j + dy*dy <= (r+1)^2, i.e. all pixels with any coverage plus a
// few with none. Its bounds lo..hi follow the rows with a few integer steps
// each, no square root. A row that crosses a ring's hole gets two runs (left,
// right), one near the top or bottom of the ring a single run through the
// middle. Left runs go from the outermost ring in, right runs from the
// innermost out, so x only grows; the writer still clamps to the canvas and
// never steps back. What this costs is, as before, the CS * CS stores of the
// clear; the ring runs add about 9k (7B) / 7k (4") table lookups.
void AisScreen::paintBackground(const Layout &L) {
    if (!_ringTabOk) buildRingTable();
    const lv_color_t bg = L.f.bg;
    lv_color_t mix[RING_OPA + 1];
    mix[0] = bg;                                 // no coverage: cdRingAA leaves bg
    for (int a = 1; a <= RING_OPA; a++)
        mix[a] = (a >= LV_OPA_COVER) ? L.f.grid : lv_color_mix(L.f.grid, bg, (lv_opa_t)a);

    const uint8_t *const tab = _ringTab;
    const int c = CS/2;                          // centre pixel, both axes
    int jLo[RING_N] = {}, jHi[RING_N] = {};      // per ring, carried row to row
    for (int y = 0; y < CS; y++) {
        const int dy = y - c, dd = dy * dy;
        bool on[RING_N];
        int  base[RING_N];
        int  o = 0;                              // offset of ring k in tab
        for (int k = 0; k < RING_N; o += 4 * (k + 1) * RING_STEP + 1, k++) {
            const int ri = (k + 1) * RING_STEP;
            const int sLo = (ri - 1) * (ri - 1), sHi = (ri + 1) * (ri + 1);
            base[k] = o - sLo + dd;
            on[k]   = false;
            if (dd > sHi) continue;              // row above or below this ring
            int &lo = jLo[k], &hi = jHi[k];
            while ((hi + 1) * (hi + 1) + dd <= sHi) hi++;
            while (hi * hi + dd > sHi) hi--;
            while (lo > 0 && (lo - 1) * (lo - 1) + dd >= sLo) lo--;
            while (lo * lo + dd < sLo) lo++;
            on[k] = lo <= hi;
        }
        AisRingRun run[2 * RING_N];
        int n = 0;
        for (int k = RING_N - 1; k >= 0; k--)   // left halves, outermost first
            if (on[k]) run[n++] = AisRingRun{ c - jHi[k], jLo[k] ? c - jLo[k] : c + jHi[k], base[k] };
        for (int k = 0; k < RING_N; k++)        // right halves, innermost first
            if (on[k] && jLo[k]) run[n++] = AisRingRun{ c + jLo[k], c + jHi[k], base[k] };
        aisRingRow(_cbuf + (size_t)y * CS, CS, c, run, n, tab, mix, bg);

        // Somewhere to yield every 8 rows, as the plain clear had; renderYield()
        // decides how many of those offers are worth a tick (see RenderYield.h).
        if ((y & 7) == 7 || y == CS - 1) renderYield();
    }
}

// Raster pass: paints _lay. The same draw calls, in the same order and with
// the same arguments as the single-pass drawRadar() before it, except:
//   - the outermost range label starts lower (see lblMinY);
//   - the range rings are anti-aliased circles centred on the cross-hair
//     pixel, folded into the clear (paintBackground), instead of 120
//     lv_canvas_draw_line chords each, which sat about half a pixel up and
//     to the left of it;
//   - the selection circle is a cdRingAA ring instead of 36 chords.
void AisScreen::paintRadar() {
    const Layout &L = _lay;
    const CdTarget tg = cdTargetOf(_canvas);
#if defined(AIS_RINGS_REF)
    // Reference build for the pixel compare: the plain clear, the rings come
    // with cdRingAA in the ring loop below.
    for (int row = 0; row < CS; row += 8) {
        lv_color_t *p = _cbuf + (size_t)row * CS;
        lv_color_t *rowEnd = _cbuf + (size_t)((CS - row < 8) ? CS : row + 8) * CS;
        while (p < rowEnd) *p++ = L.f.bg;
        renderYield();
    }
#else
    paintBackground(L);
#endif
    lv_obj_invalidate(_canvas);
    PP_END(s_pp, AI_PH_CLR);

    int cx = CS/2, cy = CS/2;
    int maxR = CS/2 - UI_S(10);

    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);
    td.font = L.f.font; td.opa = OPA_75;

    // A range label sits UI_S(14) above its ring. For the outermost ring that
    // is y = cy - ri - UI_S(14) = -4 on every board, which cut off the top
    // row of its digits. LVGL leaves line_height - base_line - box_h - ofs_y
    // empty rows above a digit (3 in Montserrat 12, 14 and 16), so a label
    // may start that far above the canvas and still show in full. With the
    // default fonts the outermost one moves down 1 px; the others are far
    // below the limit.
    lv_coord_t lblMinY = 0;
    {
        lv_font_glyph_dsc_t g;
        if (td.font && lv_font_get_glyph_dsc(td.font, &g, '0', '\0')) {
            int above = td.font->line_height - td.font->base_line - g.box_h - g.ofs_y;
            if (above > 0) lblMinY = (lv_coord_t)-above;
        }
    }

    // Range labels (the rings themselves are already on the canvas, from
    // paintBackground)
    PP_LAP_DECL(rlblLap);
    for (int ring = 1; ring <= RING_N; ring++) {
        int ri = ring * RING_STEP;               // = ring * (maxR / 5)
#if defined(AIS_RINGS_REF)
        cdRingAA(tg, cx + 0.5f, cy + 0.5f, (float)ri, 1.f, L.f.grid, (lv_opa_t)RING_OPA);
#endif
        // Range label
        float labelNm = (float)ring * L.f.rangeNm / 5.0f;
        char rlbl[12]; snprintf(rlbl, sizeof(rlbl), "%.1f%s", labelNm, L.f.unit);
        td.color = L.f.grid;
        lv_coord_t ly = (lv_coord_t)(cy-ri-UI_S(14));
        if (ly < lblMinY) ly = lblMinY;
        PP_LAP_START(rlblLap);
        lv_canvas_draw_text(_canvas, (lv_coord_t)(cx+UI_S(4)), ly, UI_S(60), &td, rlbl);
        PP_LAP_STOP(rlblLap);
    }
    PP_ADD(s_pp, AI_PH_RLBL, rlblLap.us);
    PP_END(s_pp, AI_PH_RING);

    // Cross-hair lines
    ld.color = L.f.dim; ld.width = 1; ld.opa = OPA_50;
    for (int a : {0, 90, 180, 270}) {
        float ar = a*DEG_TO_RAD;
        lv_point_t p1={(lv_coord_t)cx,(lv_coord_t)cy};
        lv_point_t p2={(lv_coord_t)(cx+(int)(maxR*sinf(ar))),
                       (lv_coord_t)(cy-(int)(maxR*cosf(ar)))};
        lv_point_t _l[2]={p1,p2}; lv_canvas_draw_line(_canvas,_l,2,&ld);
    }

    // Own ship dot
    rd.bg_color = L.f.text; rd.bg_opa = OPA_FULL; rd.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(_canvas,
        (lv_coord_t)(cx-UI_S(5)), (lv_coord_t)(cy-UI_S(5)), UI_S(10), UI_S(10), &rd);

    // Heading vector
    if (L.f.hdgValid) {
        ld.color = L.f.text; ld.width = 2; ld.opa = OPA_FULL;
        lv_point_t p1={(lv_coord_t)cx,(lv_coord_t)cy};
        lv_point_t _l[2]={p1,L.f.hdgEnd}; lv_canvas_draw_line(_canvas,_l,2,&ld);
    }

    // Where north is, when it is not up (as on the anchor screen).
    if (L.f.courseUp) {
        td.color = L.f.text; td.font = FONT_SMALL; td.align = LV_TEXT_ALIGN_CENTER;
        lv_canvas_draw_text(_canvas, (lv_coord_t)(L.f.north.x - UI_S(8)), (lv_coord_t)(L.f.north.y - UI_S(8)),
                            UI_S(16), &td, "N");
        td.font = L.f.font; td.align = LV_TEXT_ALIGN_LEFT;
    }
    PP_END(s_pp, AI_PH_OWN);

    // AIS targets
    for (int i = 0; i < L.n; i++) {
        const LayTarget &e = L.t[i];
        const int tx = e.x, ty = e.y;
        const lv_color_t col = e.col;

        if (e.kind != LT_DOT) {
            // Solid target symbol (was a hollow 2 px wireframe, which was hard to
            // pick out against the radar rings at the CPA warning colours).
            cdFillTri(_canvas, e.tri[0], e.tri[1], e.tri[2], col);
            ld.color = col; ld.width = 2; ld.opa = OPA_FULL;
            { lv_point_t _l[2]={e.tri[0],e.tri[1]}; lv_canvas_draw_line(_canvas,_l,2,&ld); }
            { lv_point_t _l[2]={e.tri[1],e.tri[2]}; lv_canvas_draw_line(_canvas,_l,2,&ld); }
            { lv_point_t _l[2]={e.tri[2],e.tri[0]}; lv_canvas_draw_line(_canvas,_l,2,&ld); }

            // COG vector
            if (e.kind == LT_TRI_VEC) {
                lv_point_t vp1={(lv_coord_t)tx,(lv_coord_t)ty};
                ld.width = 1; ld.opa = OPA_50;
                lv_point_t _l[2]={vp1,e.vecEnd}; lv_canvas_draw_line(_canvas,_l,2,&ld);
            }
        } else {
            rd.bg_color = col; rd.radius = LV_RADIUS_CIRCLE; rd.bg_opa = OPA_FULL;
            lv_canvas_draw_rect(_canvas,
                (lv_coord_t)(tx-UI_S(4)), (lv_coord_t)(ty-UI_S(4)), UI_S(8), UI_S(8), &rd);
        }

        // Name label: as much of the name as fits beside the target - at most
        // UI_S(120), never past the canvas edge - cut with ".." where it does
        // not fit. It used to be the first 7 chars in a 60 px box, and a wide
        // name wrapped inside a word ("NORDWI" / "N"): lv_draw_label wraps at
        // max_w unless LV_TEXT_FLAG_EXPAND says otherwise.
        if (e.name[0]) {
            const lv_coord_t lx   = (lv_coord_t)(tx + UI_S(6));
            const lv_coord_t room = LV_MIN((lv_coord_t)UI_S(120), (lv_coord_t)(CS - 1 - lx));
            char nm[18];
            static_assert(sizeof nm == sizeof e.name + 2, "fitName: name plus \"..\"");
            fitName(nm, e.name, L.f.font, room);
            if (nm[0]) {
                td.color = col; td.font = L.f.font; td.flag = LV_TEXT_FLAG_EXPAND;
                lv_canvas_draw_text(_canvas, lx, (lv_coord_t)(ty-UI_S(6)), room, &td, nm);
                td.flag = LV_TEXT_FLAG_NONE;
            }
        }

        // Highlight selected: an anti-aliased ring (it was 36 chords at
        // OPA_FULL, see SEL_OPA). Centred on (tx, ty) with pixel x spanning
        // [x, x+1), which is the middle of the dot; the chords' ink sat
        // 0.2-0.3 px up and left of it. The half-pixel radius puts the 1-px
        // line on whole pixel rows/columns at N/E/S/W - with an integer
        // radius around a pixel-edge centre those four points would smear
        // over two half-strength pixels.
        if (e.sel) {
            PP_AUX(s_pp, AI_AUX_SEL, 1);
            cdRingAA(tg, (float)tx, (float)ty, (float)UI_S(14) + 0.5f, 1.f, L.f.accent, (lv_opa_t)SEL_OPA);
        }
    }
    lv_obj_invalidate(_canvas);
    PP_AUX(s_pp, AI_AUX_N, L.n);
    PP_END(s_pp, AI_PH_TGT);
}

// lv_label_set_text() frees, reallocates and re-measures the text and
// invalidates the label even when the text is the same - every frame for the
// scale and count labels, every second for the info box. Only touch it when
// the text really changes.
static void setLabelIfChanged(lv_obj_t *lbl, const char *txt) {
    if (!lbl) return;
    const char *cur = lv_label_get_text(lbl);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(lbl, txt);
}

// The same for the HIDDEN flag: lv_obj_clear_flag(HIDDEN) invalidates the box
// (and marks the layout dirty) even when it is already shown, which the
// once-a-second refresh would otherwise do for nothing.
static void setHidden(lv_obj_t *o, bool hide) {
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hide) return;
    if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// "--" for a value that is not known, else the number. Never a 0 for it: the
// box printed "SOG:0.0kn COG:0°" for a target that had sent neither.
static void fmtOrDash(char (&out)[12], const char *fmt, float v) {
    if (isnan(v)) snprintf(out, sizeof out, "--");
    else          snprintf(out, sizeof out, fmt, v);
}

// Fills the info box from the live table: on the tap that selects the target,
// and from update() about once a second while it is selected. It used to be
// filled on the tap only, a snapshot that never changed and outlived its
// target. Now BusWatch drops a target after 5 minutes without a report, and
// every target at once when the bus goes silent; the next refresh then finds
// it gone and takes the box and the selection down with it.
void AisScreen::showTargetInfo(uint32_t mmsi) {
    AisTarget t;
    bool found = false, cpaOk = false;
    {
        auto lk = data.lock();
        cpaOk = ownCpaInputs(data);
        for (int i = 0; i < data.aisCount; i++)
            if (data.aisTargets[i].mmsi == mmsi) { t = data.aisTargets[i]; found = true; break; }
    }
    _infoMs = millis();
    // Gone between the tap and this lookup (a full table recycled its oldest
    // slot), or purged since: drop the selection too, or its circle would come
    // back without an info box if the vessel reappears.
    if (!found) { _selValid = false; setHidden(_infoBox, true); return; }

    char buf[200];
    char sogStr[12], cogStr[12], cpaStr[12], tcpaStr[12];
    // The callsign behind the name when both are known; alone when there is
    // no name yet (see the radar label above).
    char who[40];
    if (t.name[0] && t.callsign[0]) snprintf(who, sizeof(who), "%s (%s)", t.name, t.callsign);
    else snprintf(who, sizeof(who), "%s", t.name[0] ? t.name : t.callsign[0] ? t.callsign
                                                                               : T(STR_AIS_UNKNOWN));
    fmtOrDash(sogStr,  "%.1f", t.sog);
    fmtOrDash(cogStr,  "%.0f", t.cog);
    fmtOrDash(cpaStr,  "%.2f", cpaOk ? t.cpa  : NAN);   // see ownCpaInputs()
    fmtOrDash(tcpaStr, "%.1f", cpaOk ? t.tcpa : NAN);

    snprintf(buf, sizeof(buf),
        "%s  MMSI:%lu\nSOG:%skn COG:%s°  CPA:%snm TCPA:%smin",
        who,
        (unsigned long)t.mmsi,
        sogStr, cogStr, cpaStr, tcpaStr);
    // Text and flag on a change only: a refresh of an unchanged box (a target
    // at anchor, between two of its reports) costs no redraw.
    setLabelIfChanged(_infoLbl, buf);
    setHidden(_infoBox, false);
}

void AisScreen::onCanvasClick(lv_event_t *e) {
    AisScreen *self = (AisScreen*)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p; lv_indev_get_point(indev, &p);

    int maxR = self->CS/2 - UI_S(10);
    float nm2px = (float)maxR / self->_rangeNm;

    // The tap arrives in SCREEN coordinates, so the canvas's own position on
    // the screen has to come off BOTH axes before comparing against a target's
    // canvas-relative position.
    //
    // This used to be derived as (SCREEN_W - CS) / 2 - the canvas's offset
    // inside its CONTAINER - which silently assumed the container sits at the
    // screen origin. On the 1024x600 boards it does not: the instrument block
    // starts at (88, 0) in landscape and (0, 88) in portrait. Every tap was
    // therefore evaluated 88 px away from where it landed, and with a pick
    // radius of UI_S(20) = 25 px no target could ever be selected under your
    // finger - you had to tap beside the ship. The 4-inch board's container IS
    // at (0, 0), which is why this survived unnoticed since the 600-grid port.
    //
    // lv_obj_get_coords() reports the absolute position, so this is correct on
    // every board and in every orientation. On the 4-inch it evaluates to
    // exactly the old numbers (x1 = 45 = (480-390)/2, y1 = 0).
    lv_area_t ca;
    lv_obj_get_coords(self->_canvas, &ca);

    float ownLat, ownLon;
    int best = -1; uint32_t bestMmsi = 0; float bestDist = UI_S(20);
    // As the frame on the canvas is turned.
    const float rotC = cosf(self->_lay.f.rotDeg*DEG_TO_RAD), rotS = sinf(self->_lay.f.rotDeg*DEG_TO_RAD);
    {
        auto lk = data.lock();
        ownLat=data.lat; ownLon=data.lon;
        for (int i=0;i<data.aisCount;i++) {
            AisTarget &t=data.aisTargets[i];
            if (isnan(t.lat)||isnan(t.lon)||isnan(ownLat)||isnan(ownLon)) continue;
            int tx, ty;
            if (!projectTarget(t, ownLat, ownLon, rotC, rotS, nm2px, self->_rangeNm, self->CS, tx, ty)) continue;
            // Convert screen click to canvas coordinates before comparing
            float d=sqrtf(powf(p.x - ca.x1 - tx, 2.f)+powf(p.y - ca.y1 - ty, 2.f));
            if (d<bestDist) { bestDist=d; best=i; bestMmsi=t.mmsi; }
        }
    }
    // A tap that hits no target, or the selected one again, clears the
    // selection.
    if (best < 0 || (self->_selValid && bestMmsi == self->_selMmsi)) {
        self->_selValid = false;
        lv_obj_add_flag(self->_infoBox, LV_OBJ_FLAG_HIDDEN);
    } else {
        self->_selValid = true;
        self->_selMmsi  = bestMmsi;
        self->showTargetInfo(bestMmsi);
    }
}

void AisScreen::update() {
    PP_BEGIN(s_pp);
    _rangeNm = appConfig.cfg.aisRange;
    char buf[16];
    // No purgeAisTargets() here any more: BusWatch sweeps the table once a
    // second from DisplayManager::update(), whichever screen is up, and
    // empties it when the bus goes silent. Purged only while this screen was
    // shown, the table - and the WebUI's aisCount - kept dead targets
    // everywhere else.
    int cnt; bool noPos;
    { auto lk=data.lock(); cnt=data.aisWithPosition(); noPos=isnan(data.lat)||isnan(data.lon); }
    setHidden(_noPosLbl, !noPos);
    // The info box follows its target (showTargetInfo()): at the rate BusWatch
    // sweeps, so a purged target takes its box down within a second.
    if (_selValid && (uint32_t)(millis() - _infoMs) >= INFO_REFRESH_MS)
        showTargetInfo(_selMmsi);
    PP_END(s_pp, AI_PH_PRE);
    // Without a canvas drawRadar() returns at once: only pre is charged. An
    // unchanged frame charges pre and lay.
    drawRadar();
    // Those on the radar of those known, when that is not all of them: "50" over a
    // radar showing 11 looked as if 39 had gone missing. Both figures from the one
    // table state layoutRadar() saw. Without a canvas there is no radar: the count.
    const bool radar = _canvas && _cbuf;
    const int shown = radar ? _lay.n : cnt, known = radar ? _layKnown : cnt;
    if (shown != known)
        snprintf(buf, sizeof(buf), "%d/%d %s", shown, known, T(STR_AIS_TARGETS));
    else
        snprintf(buf, sizeof(buf), "%d %s", known,
                 known != 1 ? T(STR_AIS_TARGETS) : T(STR_AIS_TARGET_ONE));
    setLabelIfChanged(_countLbl, buf);
    // The scale, and with course up asked for, which way the view really is: without
    // a course or a heading it stays north up.
    char sl[40];
    if (appConfig.cfg.aisNorthUp)
        snprintf(sl, sizeof(sl), "%d%s", _rangeNm, T(STR_UNIT_NM));
    else
        snprintf(sl, sizeof(sl), "%d%s\n%s", _rangeNm, T(STR_UNIT_NM),
                 _lay.f.courseUp ? T(STR_AIS_COURSE_UP) : T(STR_AIS_NORTH_UP));
    setLabelIfChanged(_scaleLbl, sl);
    PP_FRAME(s_pp);
}
