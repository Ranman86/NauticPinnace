#pragma once
#include "../BaseScreen.h"
#include "../../config/Config.h"
#include "../UiConfig.h"
#include "../../i18n/I18n.h"

// ============================================================
// WindScreen – B&G-style circular wind-angle instrument.
//
// Ported from BngRenderer.cs (SailTrimMonitor project).
//
// Layout (480 × 430 working area):
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

private:
    CenterMode _centerMode = CenterMode::SAILS;

    // ── Canvas ───────────────────────────────────────────────
    static constexpr int   CW = 480, CH = 480;  // canvas size (full screen)

    // ── Instrument geometry (scaled from BngRenderer 220px → 148px) ─
    static constexpr float CX = UI_WIND_CX, CY = UI_WIND_CY;
    static constexpr float R_OUTER    = UI_WIND_R_OUTER;   // outer bezel edge
    static constexpr float R_ZONE_O   = UI_WIND_R_ZONE_O;  // zone-arc outer radius
    static constexpr float R_ZONE_I   = UI_WIND_R_ZONE_I;  // zone-arc inner radius
    static constexpr float R_INNER    = UI_WIND_R_INNER;   // inner display circle
    static constexpr float BOAT_S     = UI_WIND_BOAT_SCALE; // boat scale (fills inner circle)
    static constexpr float R_TICK_O   = 202.f;  // tick outer end (= R_OUTER)
    static constexpr float R_TICK_MAJ = 191.f;  // major tick inner end
    static constexpr float R_TICK_MIN = 196.f;  // minor tick inner end
    static constexpr float R_LABEL    = 216.f;  // degree labels (outside ring)

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
    };

    lv_obj_t   *_canvas = nullptr;
    lv_color_t *_cbuf   = nullptr;   // PSRAM pixel buffer

    // Smoothed (low-pass) sail trim, signed degrees (+ = clew to starboard).
    // Persisted across frames so the sails ease continuously and tacks swing
    // through the centreline instead of jumping.
    float _smBoomDeg = 0.f;
    float _smJibDeg  = 0.f;
    bool  _trimInit  = false;

    // ── Orchestration ─────────────────────────────────────────
    void drawInstrument(float twa, float awa, float tws, float stw, float hdg);
    SailState computeSailState(float absTwa, float twa, float awa, float tws, float stw) const;

    // ── Draw layers (bottom → top) ───────────────────────────
    void drawBezel();
    void drawZoneRing(float absTwa);
    void drawTicksAndLabels();
    void drawInnerBg();
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
    void drawAttitudeKpis(float roll, float pitch, float waveH, float waveT);
    void drawHeadingBelow(float hdg);
    void drawCompassRose(float hdg);     // heading-up compass card inside the inner circle
    void drawRudderArc(float rudderDeg); // rudder-angle gauge on the bottom arc (Bb/Stb)
    void drawCornerKpis(float stw, float twa, float awa, float tws);
    void drawTrimAdvice(float absTwa, float twa, float tws);
    void drawReefBadge(int reefCount);

    // ── Boat sub-elements ─────────────────────────────────────
    void drawKeel();
    void drawRudder(float rudderDeg, float ss);
void drawSpinnaker(const SailState &sail);
    void drawCodeZero(const SailState &sail);
    void drawSheetLines(const SailState &sail,
                        float clewX, float clewY);

    // ── Low-level pixel helper ────────────────────────────────
    void fillEllipse(float cx, float cy, float rx, float ry,
                     lv_color_t col, lv_opa_t opa = LV_OPA_COVER);

    // Scanline fill of a simple polygon (even-odd rule). Used instead of
    // lv_canvas_draw_polygon, which segfaults in the PC simulator's LVGL build;
    // this uses lv_canvas_draw_line spans (works on both device and simulator).
    void fillPolygon(const lv_point_t *pts, int n, lv_color_t col, lv_opa_t opa);

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

    // Dashed radial line (VMG marker)
    void dashedRadial(float windAngle, float r1, float r2, lv_color_t col);

    // ── Point-of-sail logic ───────────────────────────────────
    enum class PoS { NoGo, CloseHauled, CloseReach, BeamReach, BroadReach, Running };
    static PoS         pointOfSail(float absTwa);
    static const char *posLabel(PoS p);
    static float       optUpwindTwa(float tws);
    static float       optDownwindTwa(float tws);
};

extern WindScreen windScreen;
