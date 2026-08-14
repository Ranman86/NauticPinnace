#pragma once
// ============================================================
// Single source of truth for every themeable colour.
// X(field, darkHex, lightHex) — expanded by Theme.h (UiTheme struct),
// Config.h (ThemeColors struct + dark defaults), Config.cpp (light defaults +
// JSON read/write) and Theme.cpp (apply). Add a colour here once and it flows
// everywhere; the WebUI editor lists keys separately (THEME_COLORS catalog).
// ============================================================
#define THEME_COLOR_FIELDS(X) \
    /* ---- core palette ---- */ \
    X(bg,             0x081521, 0xE7EEF6) \
    X(surface,        0x16304A, 0xFFFFFF) \
    X(border,         0x274A6E, 0xCDD9E6) \
    X(text,           0xEAF2F8, 0x16283A) \
    X(textDim,        0x8AA8C4, 0x5A7088) \
    X(accent,         0x16B8B8, 0x0F9A9A) \
    X(green,          0x2FBF71, 0x109A5A) \
    X(yellow,         0xF0A500, 0xB8791A) \
    X(red,            0xFF5A52, 0xC0392B) \
    X(orange,         0xFF8A3D, 0xD2691E) \
    X(port,           0xFF5A52, 0xC0392B) \
    X(starboard,      0x2FBF71, 0x109A5A) \
    X(wind,           0x16B8B8, 0x0F9A9A) \
    X(gridLine,       0x274A6E, 0xCDD9E6) \
    /* ---- overlays / banner ---- */ \
    X(navBtnBg,      0x000000, 0x000000) \
    X(perfText,      0x00FF00, 0x0A7A0A) \
    X(perfBg,        0x000000, 0xFFFFFF) \
    X(demoBanner,    0xCC1800, 0xCC1800) \
    X(demoText,      0xFFFFFF, 0xFFFFFF) \
    /* ---- depth screen ---- */ \
    X(depthBg,        0x081521, 0xE7EEF6) \
    X(depthGrad,      0x16304A, 0xC6D8E8) \
    X(depthHdr,      0x1E4060, 0x4A78A0) \
    X(depthVal,      0xD8F4FF, 0x0A3A5A) \
    X(depthEcho,      0x16B8B8, 0x0F9A9A) \
    X(depthGrid,      0x274A6E, 0xCDD9E6) \
    X(depthScale,    0x2E6080, 0x5888A8) \
    X(depthNow,      0xFF3010, 0xD32F2F) \
    /* ---- autopilot screen ---- */ \
    X(apCompassBg,    0x16304A, 0xE7EEF6) \
    X(apCompassMaj,  0x607080, 0x405868) \
    X(apCompassMin,   0x274A6E, 0xCDD9E6) \
    X(apTarget,       0x16B8B8, 0x0F9A9A) \
    X(apDevBar,       0x0C2136, 0xDFE7F0) \
    X(apModeActive,  0x0D3020, 0xC8E6D0) \
    X(apModeManual,  0x102030, 0xD0DAE4) \
    X(apModeStandby, 0x1A1208, 0xEDE4D0) \
    /* ---- wind screen ---- */ \
    X(windBezel,      0x0C2035, 0xDFE7F0) \
    X(windInner,      0x081521, 0xEEF2F6) \
    X(windRingBg,     0x16304A, 0xE0E8F0) \
    X(windDepthBg,    0x16304A, 0xE0E8F0) \
    X(windZoneNogo,  0x300808, 0xF0C8C8) \
    X(windZoneClose, 0x0A5820, 0xB8E0C0) \
    X(windZoneCloser,0x126830, 0xC8E8D0) \
    X(windZoneBeam,  0x085050, 0xBCE0E0) \
    X(windZoneBroad, 0x684408, 0xEAD8A8) \
    X(windZoneRun,   0x682008, 0xEFCBB0) \
    X(windTickMaj,   0x557088, 0x506880) \
    X(windTickMin,    0x274A6E, 0xB8C4D0) \
    X(windTwa,        0x16B8B8, 0x0F9A9A) \
    X(windAwa,        0xFF5A52, 0xC0392B) \
    X(windVmg,        0x2FBF71, 0x109A5A) \
    X(windHull,      0x7898B8, 0x4A6A8A) \
    X(windSail,      0xCCD8F0, 0x8090A8) \
    X(windMast,       0x274A6E, 0x8898A8) \
    X(windFlow,      0x103054, 0x6A90C0) \
    X(windRigging,   0x558098, 0x4A7088) \
    /* World map on the clock screen. The night side of the terminator is
       derived from these values (MAP_NIGHT_SCALE in ClockScreen.cpp), so only
       ONE colour per element has to be set instead of two. */ \
    X(mapSea,        0x15395A, 0xAFC9DF) \
    X(mapLand,       0x4F7A3D, 0x9EBE86) \
    X(mapCoast,      0x77AB55, 0x6E9455) \
    X(mapGrid,       0x2A4D6E, 0xB9CBDD) \
    /* Seabed below the echo-sounder profile (the water column itself runs from
       depthGrad at the top to depthBg at the bottom). */ \
    X(depthBed,      0x1A1008, 0xC8B48C) \
    /* Water area of the tide curve. Curve line and axis are derived from it
       and from gridLine respectively (see ClockScreen::drawTide). */ \
    X(tideWater,     0x1F6FB0, 0x7FB4DC) \
    /* Artificial horizon (vessel attitude). The disc is ~38,000 pixels —
       the largest single colour area in the whole device. The horizon line,
       the pitch ladder and the roll marks use the text colour. */ \
    X(attSky,        0x2C7BD6, 0x5C9FE0) \
    X(attGround,     0x8A5A2B, 0xA8763C) \
    /* Foreground on coloured surfaces: button labels on CLR_ACCENT, text over
       filled bars and pointers. Was written everywhere as lv_color_white()
       in the code — in night mode 20 small white light sources. */ \
    X(onAccent,      0xFFFFFF, 0xFFFFFF)
