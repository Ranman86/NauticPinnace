#pragma once
// ============================================================
// Configurable UI sizes (one set, shared by light & dark themes).
// X(field, default) — expanded by Theme.h (UiSizes struct), Config.h
// (ThemeSizes + defaults), Config.cpp (JSON) and Theme.cpp (apply).
// The UI_* macros in UiConfig.h redirect to uiSz.<field>.
//
// NOT included here (kept compile-time for safety): screen/canvas/buffer
// dimensions (UI_SCREEN_*, UI_AP_COMPASS_H), the float instrument
// geometry, and pure-logic thresholds
// (alarm temperatures, perf %, deviation thresholds).
// ============================================================
#define THEME_SIZE_FIELDS(X) \
    /* cards */ \
    X(cardRadius,        8) \
    X(cardPad,           8) \
    X(cardBorderW,       1) \
    /* nav overlay buttons — no radius field: radius MUST stay 0, otherwise
       LVGL allocates an AA mask buffer for the rounded corners (OOM risk,
       see comment in DisplayManager::makeFloatBtn). */ \
    X(navBtnBgOpa,       60) \
    X(navBtnBgOpaPress,  130) \
    X(navBtnArrowOpa,    160) \
    X(swipeThreshold,    70) \
    X(navFadeDelayMs,    4000) \
    X(navFadeOutMs,      800) \
    X(navFadeInMs,       300) \
    /* perf overlay */ \
    X(perfPad,           2) \
    X(perfBgOpa,         128) \
    /* engine gauge */ \
    X(engineArcSize,     340) \
    X(engineArcW,        26) \
    /* no engineCardW: the card width is computed from screen width and
       field count (EngineScreen), a fixed value would break the
       layout. */ \
    X(engineCardH,       72) \
    X(engineCardGap,     6) \
    /* speed screen */ \
    X(speedCardH,        100) \
    X(speedCardGap,      4) \
    X(speedBarH,         22) \
    X(speedBarRadius,    6) \
    /* rudder */ \
    X(rudderTrackW,      42) \
    X(rudderNeedleW,     3) \
    X(rudderPortOpa,     60) \
    X(rudderStbOpa,      60) \
    /* autopilot */ \
    X(apCardW,           140) \
    X(apCardH,           90) \
    X(apCardGap,         8) \
    X(apModeW,           140) \
    X(apModeH,           50) \
    X(apModeRadius,      6) \
    X(apDevBarH,         16) \
    /* demo banner */ \
    X(demoBannerH,       20)
