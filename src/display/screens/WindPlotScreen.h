#pragma once
#include "../BaseScreen.h"
#include "../../i18n/I18n.h"

// Wind rose: historical TWD/TWS distribution as polar histogram
class WindPlotScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_WINDPLOT); }
    void create(lv_obj_t *parent) override;
    void update() override;

private:
    lv_obj_t  *_canvas   = nullptr;
    lv_color_t *_cbuf    = nullptr;
    lv_obj_t  *_statsLbl = nullptr;
    static constexpr int CS = UI_S(400);
    static constexpr int SECTORS = 36;     // 10 deg each

    // Everything the canvas depends on that can change without create()
    // running again, in the integers the raster uses: whether there is a rose
    // at all, and per sector the colour class and the radius of its petal.
    // The language is in it for the cardinal letters. Colours, fonts and sizes
    // only change through create(), which resets _painted. Zero-filled before
    // it is built and compared with memcmp(), so it has no padding: 38 bytes,
    // then the 16-bit radii on an even offset.
    struct RoseLayout {
        uint8_t empty;           // fewer than two samples: grid only, no dot
        uint8_t lang;            // i18nLang()
        uint8_t cls[SECTORS];    // 0..3 = green, yellow, orange, red; 0xFF = no petal
        int16_t ri[SECTORS];     // petal radius in px
    };
    RoseLayout _lay;             // the one on the canvas, valid with _painted
    bool       _painted = false;

    void buildLayout(RoseLayout &L, char *stats, size_t statsSz, bool &setStats);
    void paintRose(const RoseLayout &L);
};
