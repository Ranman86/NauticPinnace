#include "Theme.h"
#include "../config/Config.h"
#include "fonts/latin_suppl.h"   // umlaut fallback fonts (see linkFallbackFonts)
#include "fonts/ob_icons.h"      // OpenBridge icon trial: icon fonts (see linkFallbackFonts)
#include <Arduino.h>
#include <string.h>

// Active runtime theme. Filled by applyThemeFromConfig() early in setup(),
// before any screen is created in DisplayManager::activate().
UiTheme uiTheme;
UiSizes uiSz;
UiFonts uiFont;

// ── Umlaut support ───────────────────────────────────────────────────────────
// LVGL's built-in Montserrat fonts cover ASCII plus degree, bullet and the
// FontAwesome icons — but no umlauts, no sharp-s, no en dash. Those live in the
// small generated latin_suppl_* fonts and are chained on via lv_font_t.fallback.
//
// The built-ins are `const` and sit in flash, so their .fallback cannot be
// patched in place. Instead we keep one RAM copy per size (a lv_font_t is a
// handful of pointers) that shares the same glyph data but carries the fallback
// link, and hand those out instead. ~14 x sizeof(lv_font_t) of RAM in total.
static struct FontPair { int sz; const lv_font_t *base; const lv_font_t *suppl; }
    s_fontPairs[] = {
        // Rows must match the LV_FONT_MONTSERRAT_XX enables in lv_conf.h -
        // a row whose font is compiled out is an undefined-reference link
        // error. The 7B drops the never-configured sizes (see lv_conf.h);
        // montserratBySize() snaps requests to the nearest remaining row.
#if LV_FONT_MONTSERRAT_10
        {10,&lv_font_montserrat_10,&latin_suppl_10},
#endif
        {12,&lv_font_montserrat_12,&latin_suppl_12},
        {14,&lv_font_montserrat_14,&latin_suppl_14},{16,&lv_font_montserrat_16,&latin_suppl_16},
#if LV_FONT_MONTSERRAT_18
        {18,&lv_font_montserrat_18,&latin_suppl_18},
#endif
#if LV_FONT_MONTSERRAT_20
        {20,&lv_font_montserrat_20,&latin_suppl_20},
#endif
#if LV_FONT_MONTSERRAT_22
        {22,&lv_font_montserrat_22,&latin_suppl_22},
#endif
        {24,&lv_font_montserrat_24,&latin_suppl_24},
#if LV_FONT_MONTSERRAT_28
        {28,&lv_font_montserrat_28,&latin_suppl_28},
#endif
        {32,&lv_font_montserrat_32,&latin_suppl_32},
#if LV_FONT_MONTSERRAT_36
        {36,&lv_font_montserrat_36,&latin_suppl_36},
#endif
        {40,&lv_font_montserrat_40,&latin_suppl_40},
#if LV_FONT_MONTSERRAT_44
        {44,&lv_font_montserrat_44,&latin_suppl_44},
#endif
        {48,&lv_font_montserrat_48,&latin_suppl_48},
    };
static constexpr int FONT_PAIR_N = sizeof(s_fontPairs) / sizeof(s_fontPairs[0]);
static lv_font_t s_fontWithFallback[FONT_PAIR_N];
static bool      s_fontsLinked = false;

// OpenBridge icon trial: the OpenBridge icons sit in the Private Use Area of their
// own fonts, one per pixel size a font role uses on this board. Each chain becomes
// Montserrat -> latin_suppl -> nearest icon font, so every label in every role can
// show them. latin_suppl is const as well, hence a second set of RAM copies (one
// lv_font_t each). Linked in BOTH icon sets: classic never asks for a PUA code
// point, so it draws exactly as before; the glyphs simply sit unused.
#define OB_FONT_ROW(px) { px, &ob_icons_##px },
static const struct { int sz; const lv_font_t *font; } s_obFonts[] = { OB_ICON_SIZES(OB_FONT_ROW) };
#undef OB_FONT_ROW
static lv_font_t s_supplWithFallback[FONT_PAIR_N];

// The largest icon font NOT larger than the Montserrat size in front of it. LVGL
// draws a fallback glyph on the MAIN font's baseline and the label clips to the
// main font's line box, so a larger icon font would lose its top rows (a role set
// to 10 px behind a 12 px icon font: 1-2 px cut off). The generator gives every
// board an icon font at its smallest Montserrat, so the fallback below never fires.
static const lv_font_t *nearestObFont(int sz) {
    const lv_font_t *best = nullptr;
    int bestSz = 0;
    for (const auto &f : s_obFonts)
        if (f.sz <= sz && f.sz > bestSz) { bestSz = f.sz; best = f.font; }
    return best ? best : s_obFonts[0].font;
}

static void linkFallbackFonts() {
    if (s_fontsLinked) return;
    for (int i = 0; i < FONT_PAIR_N; i++) {
        s_supplWithFallback[i]          = *s_fontPairs[i].suppl;              // OpenBridge icon trial
        s_supplWithFallback[i].fallback = nearestObFont(s_fontPairs[i].sz);
        s_fontWithFallback[i]           = *s_fontPairs[i].base;   // copy, then
        s_fontWithFallback[i].fallback  = &s_supplWithFallback[i]; // add the link
    }
    s_fontsLinked = true;
}

// Nearest compiled Montserrat font to the requested size (see lv_conf.h enables).
const lv_font_t *montserratBySize(int sz) {
    linkFallbackFonts();
    int best = 3, bestd = 1 << 30;               // index 3 == 16 px default
    for (int i = 0; i < FONT_PAIR_N; i++) {
        int d = abs(s_fontPairs[i].sz - sz);
        if (d < bestd) { bestd = d; best = i; }
    }
    return &s_fontWithFallback[best];
}

// Large "hero" numeric fonts. 96/192 px use the custom generated fonts; any
// smaller request falls back to the nearest Montserrat (so the depth/grid-hero
// readouts can also be shrunk to a normal size from the WebUI).
const lv_font_t *bigFontBySize(int sz) {
    if (sz >= 144) return &depth_font_192;   // ~192 px custom numeric
    if (sz >= 72)  return &depth_font_96;    // ~96 px custom numeric
    return montserratBySize(sz);             // <=48 px -> Montserrat fallback
}

// ---- Runtime screen orientation --------------------------------------------
// Straight from LVGL: lv_disp_get_hor_res() already reports the ROTATED
// resolution (it swaps the driver's hor/ver for 90 and 270), so this stays
// correct no matter how the rotation was set.
lv_coord_t uiScreenW() { return lv_disp_get_hor_res(NULL); }
lv_coord_t uiScreenH() { return lv_disp_get_ver_res(NULL); }
bool       uiPortrait() { return uiScreenH() > uiScreenW(); }

void applyThemeFromConfig() {
    // Sizes (shared across variants). Configured on the 480 DESIGN grid and
    // scaled here once (UI_S; identity off the 7B) - same contract as the
    // fonts below, so the WebUI size controls keep meaning the same design.
    #define X(n,d) uiSz.n = UI_S(appConfig.cfg.themeSizes.n);
    THEME_SIZE_FIELDS(X)
    #undef X
    // ...but NOT every themeable "size" is a pixel count. Opacities (0-255)
    // and animation times (ms) live in the same list and must be taken raw -
    // the blanket UI_S above turned a 4000 ms fade delay into 5000 ms and an
    // opacity of 128 into 160. Restore those here rather than splitting the
    // X-macro list, which four files expand.
    #define RAW(n) uiSz.n = appConfig.cfg.themeSizes.n;
    RAW(navBtnBgOpa) RAW(navBtnBgOpaPress) RAW(navBtnArrowOpa)   // 0-255
    RAW(perfBgOpa) RAW(rudderPortOpa) RAW(rudderStbOpa)          // 0-255
    RAW(navFadeDelayMs) RAW(navFadeOutMs) RAW(navFadeInMs)       // milliseconds
    #undef RAW
    // Fonts (size per role -> nearest compiled Montserrat). The configured
    // sizes live on the 480 DESIGN grid; UI_S scales them to the current grid
    // (7B: x1.25 -> 12->14, 14->18, 16->20, 24->28, 32->40, 40->48; 48 stays
    // 48, the largest Montserrat). Identity on the 4" and 480 simulator, so
    // one stored config keeps meaning the same design on every board.
    #define X(r,d) uiFont.r = montserratBySize(UI_S(appConfig.cfg.themeFonts.r));
    THEME_FONT_FIELDS(X)
    #undef X
    // Large numeric fonts (96/192 px custom; <=48 px Montserrat fallback).
    // UI_S keeps 96/120 and 192/240 inside the same custom-font buckets.
    #define X(r,d) uiFont.r = bigFontBySize(UI_S(appConfig.cfg.themeFonts.r));
    THEME_BIGFONT_FIELDS(X)
    #undef X

    // Three variants; dark remains the fallback for unknown values.
    // (Previously a two-way selection – a saved "night" was thereby silently
    //  rendered as dark.)
    const ThemeColors *sel = &appConfig.cfg.themeDark;
    if      (strcmp(appConfig.cfg.themeActive, "light") == 0) sel = &appConfig.cfg.themeLight;
    else if (strcmp(appConfig.cfg.themeActive, "night") == 0) sel = &appConfig.cfg.themeNight;
    const ThemeColors &c = *sel;
    #define X(n,d,l) uiTheme.n = lv_color_hex(c.n);
    THEME_COLOR_FIELDS(X)
    #undef X
    Serial.printf("[theme] applied '%s'\n", appConfig.cfg.themeActive);
}

// See the long note on the declaration in Theme.h. Depth-first over the whole
// subtree; runs in microseconds and only when a screen is built or shown, so
// it is cheap enough to apply defensively rather than remembering to clear the
// flag at all 95 label creation sites.
void uiDisableLabelScroll(lv_obj_t *root) {
    if (!root) return;
    if (lv_obj_check_type(root, &lv_label_class))
        lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    const uint32_t n = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < n; i++) uiDisableLabelScroll(lv_obj_get_child(root, i));
}

const lv_font_t *uiFitValueFont(const lv_font_t *preferred, const char *sample,
                                lv_coord_t availW) {
    const lv_font_t *ladder[] = { FONT_HUGE, FONT_XXL, FONT_XL,
                                  FONT_LARGE, FONT_MED, FONT_SMALL };
    const int n = (int)(sizeof(ladder) / sizeof(ladder[0]));
    int i = 0;
    while (i < n - 1 && ladder[i] != preferred) i++;
    if (ladder[i] != preferred) i = 0;          // not on the ladder: start big
    const uint32_t len = (uint32_t)strlen(sample);
    for (; i < n; i++) {
        if (lv_txt_get_width(sample, len, ladder[i], 0, LV_TEXT_FLAG_NONE) <= availW)
            return ladder[i];
    }
    return ladder[n - 1];                       // nothing fits: smallest, clipped
}
