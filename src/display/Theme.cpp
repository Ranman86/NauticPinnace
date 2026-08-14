#include "Theme.h"
#include "../config/Config.h"
#include "fonts/latin_suppl.h"   // umlaut fallback fonts (see linkFallbackFonts)
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
        {10,&lv_font_montserrat_10,&latin_suppl_10},{12,&lv_font_montserrat_12,&latin_suppl_12},
        {14,&lv_font_montserrat_14,&latin_suppl_14},{16,&lv_font_montserrat_16,&latin_suppl_16},
        {18,&lv_font_montserrat_18,&latin_suppl_18},{20,&lv_font_montserrat_20,&latin_suppl_20},
        {22,&lv_font_montserrat_22,&latin_suppl_22},{24,&lv_font_montserrat_24,&latin_suppl_24},
        {28,&lv_font_montserrat_28,&latin_suppl_28},{32,&lv_font_montserrat_32,&latin_suppl_32},
        {36,&lv_font_montserrat_36,&latin_suppl_36},{40,&lv_font_montserrat_40,&latin_suppl_40},
        {44,&lv_font_montserrat_44,&latin_suppl_44},{48,&lv_font_montserrat_48,&latin_suppl_48},
    };
static constexpr int FONT_PAIR_N = sizeof(s_fontPairs) / sizeof(s_fontPairs[0]);
static lv_font_t s_fontWithFallback[FONT_PAIR_N];
static bool      s_fontsLinked = false;

static void linkFallbackFonts() {
    if (s_fontsLinked) return;
    for (int i = 0; i < FONT_PAIR_N; i++) {
        s_fontWithFallback[i]          = *s_fontPairs[i].base;   // copy, then
        s_fontWithFallback[i].fallback = s_fontPairs[i].suppl;   // add the link
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

void applyThemeFromConfig() {
    // Sizes (shared across variants).
    #define X(n,d) uiSz.n = appConfig.cfg.themeSizes.n;
    THEME_SIZE_FIELDS(X)
    #undef X
    // Fonts (size per role -> nearest compiled Montserrat).
    #define X(r,d) uiFont.r = montserratBySize(appConfig.cfg.themeFonts.r);
    THEME_FONT_FIELDS(X)
    #undef X
    // Large numeric fonts (96/192 px custom; <=48 px Montserrat fallback).
    #define X(r,d) uiFont.r = bigFontBySize(appConfig.cfg.themeFonts.r);
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
