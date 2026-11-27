#pragma once
#include "../BaseScreen.h"
#include "../../config/Config.h"
#include "../UiConfig.h"
#include "../CanvasDraw.h"
#include "../../i18n/I18n.h"

// ============================================================
// WindScreen – circular wind-angle instrument.
//
// Ported from the author's own NauticPi wind renderer (then SailTrimMonitor; same author, MIT).
//
// Layout (480 × 430 design grid; scaled via UI_S/UI_SF to the panel):
//   Outer ring  : degree scale 000–330, coloured zone arcs
//   Inner circle: boat bird's-eye with wind-flow lines
//   Centre      : large TWA value + Stb/Bb side
//   Corners     : STW | TWA° | AWA° | TWS
//   Below       : Point-of-sail name + trim advice
//
// Zone colours (dark theme):
//   No-Go        0–30°  deep red
//   Am Wind     30–45°  green
//   Halb-am-Wind45–65°  lighter green
//   Halbwind    65–100° teal
//   Raumwind   100–150° amber
//   Vorwind    150–180° orange
// ============================================================
class WindScreen : public BaseScreen {
public:
    // Two variants share the entire outer ring (compass, wind rose, rudder arc,
    // pointers). Only the centre differs: SAILS = bird's-eye boat with sails;
    // ATTITUDE = aircraft-style artificial horizon (roll/pitch + RoT + wave).
    enum class CenterMode { SAILS, ATTITUDE };
    void setCenterMode(CenterMode m) { _centerMode = m; }

    const char *title() const override {
        return _centerMode == CenterMode::ATTITUDE ? T(STR_SCREEN_ATTITUDE)
                                                   : T(STR_SCREEN_WIND);
    }
    void create(lv_obj_t *parent) override;
    void update()                 override;
    // Back on screen: the rig starts from the current wind, not from the trim
    // and lee side it had when it was last painted - see resetRig().
    void onShow()                 override { resetRig(); }

private:
    CenterMode _centerMode = CenterMode::SAILS;

    // ── Canvas ───────────────────────────────────────────────
    static constexpr int   CW = SCREEN_W, CH = SCREEN_H;  // canvas size (full screen)

    // ── Instrument geometry (scaled from the NauticPi renderer 220px → 148px) ─
    static constexpr float CX = UI_WIND_CX, CY = UI_WIND_CY;
    static constexpr float R_OUTER    = UI_WIND_R_OUTER;   // outer bezel edge
    static constexpr float R_ZONE_O   = UI_WIND_R_ZONE_O;  // zone-arc outer radius
    static constexpr float R_ZONE_I   = UI_WIND_R_ZONE_I;  // zone-arc inner radius
    static constexpr float R_INNER    = UI_WIND_R_INNER;   // inner display circle
    static constexpr float BOAT_S     = UI_WIND_BOAT_SCALE; // boat scale (fills inner circle)
    static constexpr float R_TICK_O   = 202.f * UI_SF;  // tick outer end (= R_OUTER)
    static constexpr float R_TICK_MAJ = 191.f * UI_SF;  // major tick inner end
    static constexpr float R_TICK_MIN = 196.f * UI_SF;  // minor tick inner end
    static constexpr float R_LABEL    = 216.f * UI_SF;  // degree labels (outside ring)
    // ATTITUDE: radius of the sky/ground disc (clear of the compass rose).
    static constexpr float R_HORIZON  = 110.f * UI_SF;
    // drawInnerBg() may leave the disc to the sky/ground fill only while
    // horizonHole() can rebuild that fill's rows bit for bit: its row y
    // values must be whole numbers (CY - R_HORIZON integral) and every
    // product in R*R - dy*dy exact (CY and R_HORIZON multiples of 0.5, all
    // well below 2^11), so that no rounding - fused or not - can tell the
    // two computations apart; and the disc must lie at positive x, where
    // the casts truncate like floor. True on both grids (7B: CY 282.5,
    // R 137.5; 4": 226, 110); otherwise the inner fill stays whole.
    static constexpr bool  HORIZON_HOLE =
        (float)(int)(CY - R_HORIZON) == CY - R_HORIZON &&
        (float)(int)(2.f * CY) == 2.f * CY &&
        (float)(int)(2.f * R_HORIZON) == 2.f * R_HORIZON &&
        R_HORIZON < 1000.f && CY < 1000.f && CX - R_HORIZON >= 1.f;

    // ── Sail state (computed once per frame) ──────────────────
    struct SailState {
        float   boomAngleDeg    = 5.f;   // boom off stern axis (leeward)
        float   headsailAngleDeg= 20.f;  // jib clew angle from forestay
        float   flutterDeg      = 0.f;   // luffing oscillation amplitude
        float   leewardSide     = -1.f;  // +1 = Stb-side, -1 = Bb-side  (matches ss)
        bool    luffing         = false;
        bool    running         = false;
        bool    useSpinnaker    = false;
        bool    useCodeZero     = false;
        int     reefCount       = 0;     // 0..3
        float   mainCamber      = 0.16f; // mainsail draft fraction (belly depth / leech len)
        float   jibCamber       = 0.14f; // headsail draft fraction
        // |AWA|, or |TWA| without an apparent angle, 180 without either: what
        // the downwind sail is trimmed to (SpiGeom::symRotationDeg /
        // genSheetDeg). Never NaN.
        float   awaAbs          = 180.f;
        // The downwind sail this frame was planned for. Read from the config
        // ONCE, in computeSailState(): the web server task can change
        // appConfig.cfg.spiType at any moment, and the window that decides
        // useSpinnaker and the picture drawSpinnaker() draws must come from
        // the same read.
        SpiType spiType         = SpiType::None;
    };

    lv_obj_t   *_canvas = nullptr;
    lv_color_t *_cbuf   = nullptr;   // PSRAM pixel buffer

    // Compass-card tap (north-up <-> course-up). The press point is kept so the
    // release can tell a tap from the horizontal drag that means "next screen":
    // that drag ends in an LV_EVENT_CLICKED on this canvas as well.
    lv_coord_t _pressX      = 0;
    lv_coord_t _pressY      = 0;
    bool       _pressOnCard = false;

    // Canvas touch handler: toggles the compass card's orientation when the tap
    // both starts and ends inside the card. Registered for PRESSED and CLICKED.
    static void onCanvasTouch(lv_event_t *e);

    // Smoothed (low-pass) sail trim, signed degrees (+ = clew to starboard).
    // Persisted across frames so the sails ease continuously and tacks swing
    // through the centreline instead of jumping.
    float _smBoomDeg = 0.f;
    float _smJibDeg  = 0.f;
    bool  _trimInit  = false;
    // Downwind sail, smoothed the same way: rotation of the symmetric kite
    // and sheeting angle of the gennaker, both signed (+ = to starboard).
    // Tracked on EVERY frame, drawn or not - see drawBoat().
    float _smSpiRot  = 0.f;
    float _smGenDeg  = 0.f;
    // A downwind sail is drawn on the current frame: set by drawBoat(), read
    // by drawReefBadge(), which then moves out of the kite's way, and by
    // drawCompassRose() for the letter halo.
    bool  _spiDrawn  = false;
    // The butterfly rig is drawn on the current frame: set by drawBoat(),
    // read by drawInstrument(), which prints the "Butterfly" label over the
    // compass letters.
    bool  _butterflyDrawn = false;

    // Lee side kept across frames (+1 / -1; 0 = none yet, or the wind was
    // lost). Per instance: on the 1024x600 boards the two WindScreen
    // instances share a canvas, not their state. See latchLeeSide().
    float _leeSide   = 0.f;

    // lv_tick_get() of the last painted frame. A longer gap than
    // RIG_STALE_MS (a modal overlay was open: no update(), no onShow())
    // resets the rig like onShow() does.
    uint32_t _lastPaintTick = 0;
    static constexpr uint32_t RIG_STALE_MS = 2000;
    // Forget the smoothed trims and the kept lee side: the next frame takes
    // both from the wind as it is (_trimInit = false, _leeSide = 0).
    void resetRig() { _trimInit = false; _leeSide = 0.f; }

    // ── Orchestration ─────────────────────────────────────────
    void drawInstrument(float twa, float awa, float tws, float stw, float hdg);
    SailState computeSailState(float absTwa, float twa, float awa, float tws, float stw) const;

    // ── Draw layers (bottom → top) ───────────────────────────
    void drawBezel();
    void drawZoneRing(float absTwa);
    void drawTicksAndLabels();
    // skipHorizon: leave the ATTITUDE sky/ground disc out (see the definition).
    void drawInnerBg(bool skipHorizon = false);
    // cdFillEllipse() hole for drawInnerBg(): the pixels of row y that the
    // sky/ground fill of drawCenter_Attitude() is certain to overwrite.
    static bool horizonHole(int y, int &x0, int &x1);
    void drawWindLines(float twaDeg);
    void drawBoat(const SailState &sail);
    void drawInnerBorder();
    void drawOuterBorder();
    void drawTwaPointer(float twaDeg);
    void drawAwaPointer(float awaDeg);
    void drawVmgMarkers(float tws, float absTwa);
    // ATTITUDE-mode centre: artificial horizon (roll/pitch), pitch ladder, bank
    // scale, fixed aircraft symbol, rate-of-turn indicator; + attitude KPIs.
    void drawCenter_Attitude(float roll, float pitch, float rot);
    // Its moving part - sky/ground disc, horizon line, pitch ladder - for a
    // known roll and pitch only (attitudeKnown() in WindScreen.cpp).
    void drawHorizonDisc(float roll, float pitch);
    // The overlay draws below take the paint's open LVGL draw context
    // (CdLvBatch, see ctext() in WindScreen.cpp): drawInstrument() opens it
    // once for the KPIs, the heading box, the trim advice, the reef badge,
    // the rudder arc and the polar bar.
    void drawAttitudeKpis(CdLvBatch &b, float roll, float pitch, float waveH, float waveT);
    void drawHeadingBelow(CdLvBatch &b, float hdg);
    // Compass card inside the inner circle (course-up or north-up), in two
    // layers: the ring (ticks, and the heading index in north-up mode) and the
    // cardinal letters. The Wind & Trim centre draws the ring under the boat
    // and the letters over it, so a sail never hides a letter; see
    // drawInstrument().
    enum CardLayer : uint8_t { CARD_RING = 1, CARD_LETTERS = 2, CARD_ALL = 3 };
    void drawCompassRose(float hdg, uint8_t layers = CARD_ALL);
    // A letter this close to the bow (screen angle, degrees) gets a halo in
    // the inner colour while a downwind sail is drawn. Measured in 2-D with
    // the drawn kite pixels (symmetric kite at any trim, gennaker at any
    // sheeting angle) against each letter's glyph box: the widest glyph, W,
    // is reached up to 49.75 deg off the bow with the default small font
    // (49.65 on the 1024x600 boards), the letters' text boxes up to 50.5 deg,
    // a W with the small font set one or two steps larger (16 or 18) up to
    // 53.4 deg. 55 covers all of that and still holds at most two letters.
    static constexpr float LETTER_HALO_DEG = 55.f;
    void drawRudderArc(CdLvBatch &b, float rudderDeg); // rudder-angle gauge on the bottom arc (Bb/Stb)
    void drawCornerKpis(CdLvBatch &b, float stw, float twa, float awa, float tws);
    // A top corner value (left or right of the rose), kept clear of the
    // degree label at 330 or 030 below it. worstNum is the pattern of the
    // widest number the field shows ('D' stands for the font's widest digit,
    // e.g. "1DD°"), wordA/wordB its two side words (or the unit twice); see
    // WindScreen.cpp.
    void drawTopKpiValue(CdLvBatch &b, bool right, float x, float y, float w,
                         lv_color_t col, const char *txt, const char *worstNum,
                         const char *wordA, const char *wordB);
    void drawTrimAdvice(CdLvBatch &b, float absTwa, float twa, float tws);
    // aft = a downwind sail fills the badge's usual place above the bow;
    // the badge then moves into the cockpit (see the definition).
    void drawReefBadge(CdLvBatch &b, int reefCount, bool aft);

    // ── Boat sub-elements ─────────────────────────────────────
    // (No drawKeel() any more: the opaque hull fill hid every pixel of it,
    // see drawBoat().)
    // Rudder blade at the stern. The degree figure beside it is gone - it was
    // on screen twice, the arc below (drawRudderArc) shows the value. What
    // stays is the blade, as a direction indicator that reads at a glance.
    void drawRudder(float rudderDeg);
    // Downwind sail, as picked in the WebUI (AppConfig::spiType). Geometry in
    // SpiGeom.h; these do the pixel work only.
    void drawSpinnaker(const SailState &sail);     // dispatch by type
    void drawSpiGores(const SailState &sail);      // symmetric, six panels in two tones
    void drawSpiBalloon(const SailState &sail);    // symmetric, shaded bands, opaque
    void drawGennaker(const SailState &sail);      // asymmetric, tack at the stem tip
    // Sheet (clew -> quarter block -> winch) and, for the symmetric kite, the
    // guy (tack -> twing -> winch). Tack and clew in canvas pixels; the
    // blocks and winches come from SpiGeom.
    void spiLines(float tackX, float tackY, float clewX, float clewY,
                  float leeSide, bool guy);
    void drawCodeZero(const SailState &sail);
    void drawSheetLines(const SailState &sail,
                        float clewX, float clewY);

    // ── Low-level pixel helper ────────────────────────────────
    // The raster helpers below forward to CanvasDraw (cd*), which holds the
    // bodies and the comments on their definitions. The target is built on
    // every call from this instance's buffer and canvas size, so the canvas
    // both instances share on 1024x600 and a theme rebuild need nothing extra.
    CdTarget tgt() const { return cdTarget(_cbuf, CW, CH); }

    void fillEllipse(float cx, float cy, float rx, float ry,
                     lv_color_t col, lv_opa_t opa = LV_OPA_COVER, CdRowHoleFn hole = nullptr) {
        cdFillEllipse(tgt(), cx, cy, rx, ry, col, opa, hole);
    }

    // One horizontal run of pixels written straight into _cbuf, clipped to the
    // canvas. See the comment on cdSpanH() in CanvasDraw.cpp for why this does
    // not go through lv_canvas_draw_line.
    void spanH(int y, int x0, int x1, lv_color_t col, lv_opa_t opa) {
        cdSpanH(tgt(), y, x0, x1, col, opa);
    }

    // Counterpart of spanH() for 1-px lines, straight into _cbuf.
    // Measured on the 7B: through cline() (= lv_canvas_draw_line) the 13 flow
    // lines cost 50.9 ms of the 77 ms BOAT phase, about 4 ms PER LINE - the
    // same malloc/free-plus-invalidate toll that the comment on cdSpanH()
    // describes for areas.
    // Anti-aliased after Xiaolin Wu: the lines sit at 40 % opacity behind the
    // boat, where a hard staircase would show at once.
    // w = stroke width. Up to 1.5 px the Wu variant runs (one pixel pair per
    // step); above that, a stroke w wide with soft edges, its coverage taken
    // from each pixel centre's distance to the segment - so 2-3 px hull and
    // rig lines stay smooth as well.
    void linePx(float x0, float y0, float x1, float y1,
                lv_color_t col, lv_opa_t opa, float w = 1.f) {
        cdLineAA(tgt(), x0, y0, x1, y1, col, opa, w);
    }

    // ---- Wind history traces ------------------------------------------------
    // The last ~60 s of apparent and true wind as two thin curves: angle around
    // the rose, wind speed as radius. Fed from DataModel::windTrace, which is
    // sampled on a fixed clock and stores BOAT-RELATIVE angles - see the long
    // note there for why a compass-referenced history cannot be used here.
    //
    // Drawn between the boat and the compass rose. The outer limit is NOT
    // R_INNER: the rose's N/O/S/W letters already occupy the band from about
    // R_INNER-29*UI_SF outwards, so a curve reaching R_INNER would run straight
    // through them.
    // Out to the inner rim, so the newest end sits directly under the arrow
    // it belongs to. It crosses the compass-rose letters on the way; that is
    // the accepted price for the curve visibly ARRIVING at its pointer.
    static constexpr float R_TRACE_MAX = R_INNER * 0.99f;
    // Half-width of the strip across the pointer axis. Deliberately small:
    // the trace is context under the live pointers, not a second gauge.
    static constexpr float W_TRACE     = R_INNER * 0.30f;
    void drawWindTrace(float nTwa, float nAwa);

    // Anti-aliased circular ring of width w around radius r.
    // Replaces a full lv_canvas_draw_arc(0..360): that evaluates an LVGL
    // radius mask over the WHOLE 600x600 bounding box, for a band two to three
    // pixels wide. Measured on the device, the two border circles together
    // cost 25.2 ms that way - half the overlay phase.
    // This touches only the band itself (about 8500 pixels per ring) and
    // takes the edge opacity from the distance to the nominal radius. The two
    // rings then measured 17.0 ms: each visited pixel is a PSRAM read plus a
    // write, so this is cheaper, not free.
    void ringAA(float cx, float cy, float r, float w,
                lv_color_t col, lv_opa_t opa) {
        cdRingAA(tgt(), cx, cy, r, w, col, opa);
    }

    // Filled, anti-aliased disc. For small dots (pivots, markers) - a
    // staircase shows most there, because the outline is almost all edge.
    // Do NOT rebuild it from ringAA(): its coverage comes from the distance
    // to the nominal radius, so the middle of a disc would come out half
    // transparent.
    void discAA(float cx, float cy, float r, lv_color_t col, lv_opa_t opa) {
        cdDiscAA(tgt(), cx, cy, r, col, opa);
    }

    // Filled ring between rInner and rOuter, scanline by scanline through
    // spanH(). Replaces a full-circle lv_canvas_draw_arc(), which evaluates an
    // LVGL radius mask across the whole bounding box. yieldRows: see
    // cdFillAnnulus().
    void fillAnnulus(float cx, float cy, float rInner, float rOuter,
                     lv_color_t col, lv_opa_t opa = LV_OPA_COVER, int yieldRows = 40) {
        cdFillAnnulus(tgt(), cx, cy, rInner, rOuter, col, opa, yieldRows);
    }

    // Scanline fill of a simple polygon (even-odd rule). Used instead of
    // lv_canvas_draw_polygon, which segfaults in the PC simulator's LVGL build.
    // yieldRows: offer renderYield() every that many rows; 0 = never, for a
    // caller that fills several small polygons and yields once after them.
    void fillPolygon(const lv_point_t *pts, int n, lv_color_t col, lv_opa_t opa,
                     int yieldRows = 20) {
        cdFillPolyOpa(tgt(), pts, n, col, opa, yieldRows);
    }

    // Draw one sail as a cambered airfoil seen from above: a filled lens between
    // the straight luff(tack)->clew chord and a leeward-bulging quadratic Bézier
    // (the draft). The belly sits on the lee side; the chord direction is the
    // trim angle. One polygon pass + a leeward edge outline.
    void drawSailFoil(float tackX, float tackY, float clewX, float clewY,
                      float leeSign, float camberFrac,
                      lv_color_t col, lv_opa_t opa,
                      lv_color_t edgeCol, lv_coord_t edgeW);

    // ── Low-level drawing helpers ─────────────────────────────
    // Draw a filled donut segment using scan-line radial lines.
    // startWind / sweep in wind-angle convention: 0° = top, CW positive.
    void arcRing(float innerR, float outerR,
                 float startWind, float sweep,
                 lv_color_t col, lv_opa_t opa = LV_OPA_COVER);

    // Same arc, drawn symmetrically on Stb AND Bb side
    void symArcRing(float innerR, float outerR,
                    float startWind, float sweep,
                    lv_color_t col, lv_opa_t opa = LV_OPA_COVER);

    // Triangle pointer along a wind angle
    void triPointer(float windAngle,
                    float tipR, float baseR, float halfWidth,
                    lv_color_t col, lv_opa_t opa = LV_OPA_COVER);

    // ── Point-of-sail logic ───────────────────────────────────
    enum class PoS { NoGo, CloseHauled, CloseReach, BeamReach, BroadReach, Running };
    static PoS         pointOfSail(float absTwa);
    static const char *posLabel(PoS p);
    static float       optUpwindTwa(float tws);
    static float       optDownwindTwa(float tws);
};

extern WindScreen windScreen;
