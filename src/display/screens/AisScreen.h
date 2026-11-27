#pragma once
#include "../BaseScreen.h"
#include "../../config/Config.h"
#include "../../i18n/I18n.h"

class AisScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_AIS); }
    void create(lv_obj_t *parent) override;
    void update() override;

private:
    lv_obj_t  *_canvas   = nullptr;
    lv_color_t *_cbuf    = nullptr;
    lv_obj_t  *_infoBox  = nullptr;
    lv_obj_t  *_infoLbl  = nullptr;
    lv_obj_t  *_scaleLbl = nullptr;
    lv_obj_t  *_countLbl = nullptr;
    lv_obj_t  *_noPosLbl = nullptr;

    int  _rangeNm  = 5;
    // Selected target, by MMSI rather than by table index: purgeAisTargets()
    // (BusWatch, once a second) compacts the table and findOrCreateAis()
    // gives a far target's slot to a nearer one, so a remembered index can come
    // to point at another vessel.
    bool     _selValid = false;
    uint32_t _selMmsi  = 0;
    // millis() of the last showTargetInfo(); update() refreshes the box every
    // INFO_REFRESH_MS while a target is selected.
    uint32_t _infoMs   = 0;
    static constexpr uint32_t INFO_REFRESH_MS = 1000;

    static constexpr int CS = UI_S(390);

    // ---- Range rings ---------------------------------------------------------
    // Five anti-aliased 1-px rings, centred on the cross-hair pixel (CS/2,
    // CS/2) and folded into the canvas clear: paintBackground() writes every
    // pixel once, the ring pixels already mixed against the background, and
    // reads nothing back. The pixels are exactly those of
    //   cdRingAA(tg, CS/2 + 0.5f, CS/2 + 0.5f, k * RING_STEP, 1.f, grid, RING_OPA)
    // on a cleared canvas, k = 1..5 (build with -DAIS_RINGS_REF to draw them
    // that way, for a pixel compare).
    //
    // With the centre on a pixel centre, dx and dy of every pixel are whole
    // numbers, so cdRingAA's coverage depends on s = dx*dx + dy*dy alone.
    // _ringTab holds it per ring for each s in [(r-1)^2, (r+1)^2] - outside
    // that range it is 0 - computed once with cdRingAA's own float
    // expressions. Painting a ring is then a table lookup per pixel instead of
    // a sqrtf (a call into libgcc on the ESP32-S3).
    static constexpr int RING_N    = 5;
    static constexpr int RING_STEP = (CS/2 - UI_S(10)) / RING_N;   // maxR / 5
    // Opacities chosen so the AA lines carry the same ink per unit of length
    // as the LVGL chords they replace, measured on both boards in all three
    // themes (to within 4 %). An LVGL 1-px skew line covers only ~0.9 px, so
    // the old OPA_50 rings carried about 45 % of a solid line, and the old
    // OPA_FULL selection circle 85-89 %.
    static constexpr int RING_OPA  = 115;   // range rings, of CLR_GRID_LINE
    static constexpr int SEL_OPA   = 222;   // selection circle, of CLR_ACCENT
    // One byte per s, 4r + 1 per ring, ring after ring.
    static constexpr int RING_TAB_N = 4 * RING_STEP * (RING_N * (RING_N + 1) / 2) + RING_N;
    // The fold writes the rings of a row side by side; that needs them more
    // than their +-1 px reach apart, and cdRingAA's own band to stay split.
    static_assert(RING_STEP >= 4, "range rings too close for the folded clear");
    uint8_t _ringTab[RING_TAB_N];
    bool    _ringTabOk = false;
    void buildRingTable();

    // ---- Radar layout --------------------------------------------------------
    // drawRadar() works in two passes. The layout pass reduces the data to
    // what the canvas will show - integer canvas geometry, colours, names,
    // the selection - and the raster pass paints from that alone. When the
    // layout equals the one already on the canvas, the frame is skipped: no
    // clear, no draw call, no invalidate.
    //
    // A target as painted. Compared with memcmp, so it must not have padding
    // (the static_assert below) and is memset to zero before it is filled:
    // the fields a kind does not use stay 0.
    enum { LT_DOT = 0, LT_TRI, LT_TRI_VEC };
    struct LayTarget {
        lv_point_t tri[3];     // COG triangle (LT_TRI, LT_TRI_VEC)
        lv_point_t vecEnd;     // end of the 6-min COG vector (LT_TRI_VEC)
        lv_coord_t x, y;       // position on the canvas
        lv_color_t col;        // colour of its CPA class
        uint8_t    kind;       // LT_DOT: no COG, a dot instead of a triangle
        uint8_t    sel;        // 1 = draw the selection circle
        char       name[16];   // up to 15 chars of the name, NUL-padded; "" = none
    };
    static_assert(sizeof(LayTarget) == 4 * sizeof(lv_point_t) + 2 * sizeof(lv_coord_t)
                                       + sizeof(lv_color_t) + 2 + 16,
                  "LayTarget is compared with memcmp: it must not have padding");

    // The rest of the frame, compared field by field (sameFrame). Unit
    // (language), font and theme colours change only through create(), which
    // forces a repaint by itself; they are in here anyway, so that the layout
    // alone decides what is painted.
    struct LayFrame {
        int              rangeNm;
        const char      *unit;       // T(STR_UNIT_NM) of the range labels
        const lv_font_t *font;       // FONT_TINY: range labels and names
        lv_color_t       bg, grid, dim, text, accent;
        bool             hdgValid;   // own COG/HDG known
        lv_point_t       hdgEnd;     // end of the own heading vector
        bool             courseUp;   // the view turned to the own course
        float            rotDeg;     // what is up on the canvas, 0 = north
        lv_point_t       north;      // where the "N" goes (courseUp)
    };

    struct Layout {
        LayFrame  f;
        int       n;                        // targets in t[], in draw order
        LayTarget t[DataModel::MAX_AIS];
    };
    Layout _lay = {};            // the frame on the canvas, once _layPainted
    int    _layKnown = 0;        // targets with a position, in the table state of _lay.n
    bool   _layPainted = false;  // false until painted after create()
    float  _courseDeg  = 0.f;    // course up: the course the view is turned to
    bool   _courseSet  = false;
    bool   _courseCog  = false;  // course up: the COG is the source, not the heading

    void drawRadar();
    bool layoutRadar();          // fills _lay; true when it differs from the canvas
    void paintRadar();           // raster pass, from _lay
    void paintBackground(const Layout &L);   // clear + range rings, write-only
    static bool sameFrame(const LayFrame &a, const LayFrame &b);
    void showTargetInfo(uint32_t mmsi);
    static void onCanvasClick(lv_event_t *e);
};
