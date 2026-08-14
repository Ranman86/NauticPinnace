#pragma once
// ============================================================
// Configurable font roles. X(role, defaultSizePx).
// Each role maps to a compiled Montserrat font chosen by size at boot
// (montserratBySize()). The FONT_* macros in Theme.h redirect to uiFont.<role>.
// The large numeric fonts FONT_DEPTH / FONT_DEPTH_XL are configured via the
// separate THEME_BIGFONT_FIELDS list below (mapped by bigFontBySize()).
// ============================================================
#define THEME_FONT_FIELDS(X) \
    X(tiny,  12) \
    X(small, 14) \
    X(med,   16) \
    X(large, 24) \
    X(xl,    32) \
    X(xxl,   40) \
    X(huge,  48)

// Large "hero" fonts: 96/192 px map to the custom numeric fonts (depth_font_96/
// _192); <=48 px falls back to Montserrat (mapped via bigFontBySize()).
//   depth    -> FONT_DEPTH    (Depth-screen readout)
//   gridHero -> FONT_DEPTH_XL (data-grid hero field)
#define THEME_BIGFONT_FIELDS(X) \
    X(depth,    96) \
    X(gridHero, 192)
