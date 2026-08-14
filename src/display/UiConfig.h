// ============================================================
// UiConfig.h  –  All visual parameters in one place
//
// Change positions, sizes, colours, font sizes here.
// No more searching through individual screen files.
//
// Colours as 0xRRGGBB (used with lv_color_hex())
// Sizes in pixels, positions relative to the screen origin.
// ============================================================
#pragma once

// ══════════════════════════════════════════════════════════════
// SCREEN STRUCTURE
// ══════════════════════════════════════════════════════════════
#define UI_SCREEN_W         480     // Screen width
#define UI_SCREEN_H         480     // Screen height

// ══════════════════════════════════════════════════════════════
// COLOUR PALETTE (dark marine theme)
// ══════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════
// NAVIGATION OVERLAYS (semi-transparent side arrows)
// Click zone: 1/6 width × 1/4 height, vertically centred
// ══════════════════════════════════════════════════════════════
#define UI_NAV_BTN_W     (UI_SCREEN_W / 6)    // 80px – click zone width
#define UI_NAV_BTN_H     (UI_SCREEN_H / 4)    // 120px – click zone height
#define UI_NAV_BTN_BG_OPA        60            // Background opacity (0=invisible, 255=full)
#define UI_NAV_BTN_BG_OPA_PRESS 130            // Opacity when pressed
#define UI_NAV_BTN_ARROW_OPA    160            // Arrow opacity
#define UI_SWIPE_THRESHOLD       70            // Minimum pixels for a swipe gesture
#define UI_NAV_FADE_DELAY_MS   4000            // ms of inactivity until arrows fade out
#define UI_NAV_FADE_OUT_MS      800            // Fade-out animation duration (ms)
#define UI_NAV_FADE_IN_MS       300            // Fade-in animation duration (ms)

// ══════════════════════════════════════════════════════════════
// CARD STYLE (applies to all info cards)
// ══════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════
// PERFORMANCE OVERLAY (FPS / CPU% top right)
// ══════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════
// DEPTH (Depth Screen)
// ══════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════
// ENGINE (Engine Screen)
// ══════════════════════════════════════════════════════════════
#define UI_ENGINE_ARC_SIZE   340     // RPM arc diameter (px)
#define UI_ENGINE_ARC_Y        4     // Y position of the arc from the top edge
#define UI_ENGINE_ARC_START  135     // Start angle (°)
#define UI_ENGINE_ARC_END    405     // End angle (°) → 270° sweep
#define UI_ENGINE_ARC_W       26     // Arc thickness (px)
#define UI_ENGINE_RPM_Y      140     // Y position of RPM number
#define UI_ENGINE_RPM_UNIT_Y 192     // Y position of "RPM" unit
#define UI_ENGINE_STATUS_Y   215     // Y position of status label
#define UI_ENGINE_CARD_H      72     // Info card height
#define UI_ENGINE_CARD_GAP     6     // Gap between cards
#define UI_ENGINE_ALARM_COOL  95     // Coolant alarm threshold (°C)
// Oil pressure alarm threshold in hPa. Previously 200 hPa = 0.2 bar — a healthy
// diesel idles at 1.5–2 bar, so on the real engine the alarm could NEVER
// trigger (and in demo mode it was on permanently, because bar values were
// written into an hPa field there). 800 hPa = 0.8 bar is below the idle
// pressure, but well above a real pressure loss.
// ⚠ Check against the D1-30 manual and adjust if necessary.
#define UI_ENGINE_ALARM_OIL  800     // Oil pressure alarm threshold (hPa, min)

// ══════════════════════════════════════════════════════════════
// SPEED (Speed & Polar Screen)
// ══════════════════════════════════════════════════════════════
#define UI_SPEED_CARD_H      100     // Data card height
#define UI_SPEED_CARD_GAP      4     // Gap between cards
#define UI_SPEED_BAR_H        22     // Performance bar height
#define UI_SPEED_BAR_RADIUS    6     // Performance bar radius
#define UI_SPEED_BAR_BOTTOM   -8     // Bar offset from the bottom edge
#define UI_SPEED_PERF_GREEN   95     // green from this % up
#define UI_SPEED_PERF_YELLOW  80     // yellow from this % up (red below)

// ══════════════════════════════════════════════════════════════
// RUDDER (Rudder Screen)
// ══════════════════════════════════════════════════════════════
#define UI_RUDDER_PIVOT_OFFS 1.05f   // Pivot Y = CS * PIVOT_OFFS (below canvas)
#define UI_RUDDER_RADIUS     0.88f   // Arc radius = CS * RADIUS
#define UI_RUDDER_TRACK_W     42     // Arc thickness (px)
#define UI_RUDDER_MAX_ANG    40.0f   // Maximum rudder deflection (°)
#define UI_RUDDER_ANGLE_Y   -110     // Y offset of angle number from bottom edge
#define UI_RUDDER_DIR_Y      -60     // Y offset of direction text from bottom edge
#define UI_RUDDER_PORT_OPA    60     // Port zone tint opacity
#define UI_RUDDER_STB_OPA     60     // Starboard zone tint opacity

// ══════════════════════════════════════════════════════════════
// AUTOPILOT (Autopilot Screen)
// ══════════════════════════════════════════════════════════════
#define UI_AP_COMPASS_H      170     // Compass canvas height
#define UI_AP_CAPTION_Y      175     // "Set Heading" Label Y
#define UI_AP_TARGET_Y       196     // Target course number Y
#define UI_AP_MODE_W         140     // Mode box width
#define UI_AP_MODE_H          50     // Mode box height
#define UI_AP_MODE_X           8     // Mode box X (left)
#define UI_AP_MODE_Y         182     // Mode box Y
#define UI_AP_MODE_RADIUS      6     // Mode box corner radius
#define UI_AP_MODE_BORDER_W    2     // Mode box border width
// Cards + deviation bar fill the lower area: previously the content ended at
// y=381 and left 99 px of dead space down to the edge.
#define UI_AP_CARDS_Y        272     // Info cards Y position
#define UI_AP_CARD_W         140     // Info card width
#define UI_AP_CARD_H         118     // Info card height
#define UI_AP_CARD_GAP         8     // Info card gap
#define UI_AP_DEVBAR_Y       410     // Deviation indicator Y
#define UI_AP_DEVBAR_H        26     // Deviation indicator height
// Compass arc geometry. Must match the canvas height UI_AP_COMPASS_H (170):
// the old values (pivot 700 / R 600 / ±50°) let the arc run out of the canvas
// already at ±28° and placed the degree numbers exactly on the bottom edge –
// they were half cut off. Now flatter: at R=480, ±30° covers exactly half the
// width (480·sin30 = 240), and across the full width the arc drops only
// 64 px, so band, tick marks AND numbers stay inside the canvas.
#define UI_AP_SPAN_DEG       30.0f   // Compass arc visible range (±°)
#define UI_AP_PIVOT_Y       505.0f   // Compass arc pivot Y (below canvas)
#define UI_AP_R_OUTER       480.0f   // Compass arc outer radius
#define UI_AP_R_INNER       438.0f   // Compass arc inner radius
#define UI_AP_DEV_THRESH_HI  10.0f   // Course deviation: orange (°)
#define UI_AP_DEV_THRESH_LO   3.0f   // Course deviation: yellow → green (°)

// ══════════════════════════════════════════════════════════════
// WIND & TRIM (Wind Screen)
// ══════════════════════════════════════════════════════════════
#define UI_WIND_CX          240.0f   // Instrument centre X (centred; polar bar now at the bottom)
#define UI_WIND_CY          226.0f   // Instrument centre Y (scaled to the max, space at top/bottom used)
#define UI_WIND_R_OUTER     202.0f   // Outer rim radius (maximized)
#define UI_WIND_R_ZONE_O    183.0f   // Zone arc outer radius
#define UI_WIND_R_ZONE_I    155.0f   // Zone arc inner radius
#define UI_WIND_R_INNER     148.0f   // Inner circle radius
#define UI_WIND_BOAT_SCALE   3.8f    // Boat scaling (larger; up to just short of compass rose/heading)
// Wind zone colours

// ══════════════════════════════════════════════════════════════
// DEMO BANNER
// ══════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════
// RUNTIME-SIZE REDIRECTS
// The cosmetic sizes above are overridden here to read from the runtime
// `uiSz` struct (WebUI-configurable). Structural/buffer dims and float
// geometry keep their literal values above. See theme_sizes.h.
// ══════════════════════════════════════════════════════════════
#include "Theme.h"
#undef  UI_NAV_BTN_BG_OPA
#define UI_NAV_BTN_BG_OPA       (uiSz.navBtnBgOpa)
#undef  UI_NAV_BTN_BG_OPA_PRESS
#define UI_NAV_BTN_BG_OPA_PRESS (uiSz.navBtnBgOpaPress)
#undef  UI_NAV_BTN_ARROW_OPA
#define UI_NAV_BTN_ARROW_OPA    (uiSz.navBtnArrowOpa)
#undef  UI_SWIPE_THRESHOLD
#define UI_SWIPE_THRESHOLD      (uiSz.swipeThreshold)
#undef  UI_NAV_FADE_DELAY_MS
#define UI_NAV_FADE_DELAY_MS    (uiSz.navFadeDelayMs)
#undef  UI_NAV_FADE_OUT_MS
#define UI_NAV_FADE_OUT_MS      (uiSz.navFadeOutMs)
#undef  UI_NAV_FADE_IN_MS
#define UI_NAV_FADE_IN_MS       (uiSz.navFadeInMs)
#undef  UI_ENGINE_ARC_SIZE
#define UI_ENGINE_ARC_SIZE      (uiSz.engineArcSize)
#undef  UI_ENGINE_ARC_W
#define UI_ENGINE_ARC_W         (uiSz.engineArcW)
#undef  UI_ENGINE_CARD_H
#define UI_ENGINE_CARD_H        (uiSz.engineCardH)
#undef  UI_ENGINE_CARD_GAP
#define UI_ENGINE_CARD_GAP      (uiSz.engineCardGap)
#undef  UI_SPEED_CARD_H
#define UI_SPEED_CARD_H         (uiSz.speedCardH)
#undef  UI_SPEED_CARD_GAP
#define UI_SPEED_CARD_GAP       (uiSz.speedCardGap)
#undef  UI_SPEED_BAR_H
#define UI_SPEED_BAR_H          (uiSz.speedBarH)
#undef  UI_SPEED_BAR_RADIUS
#define UI_SPEED_BAR_RADIUS     (uiSz.speedBarRadius)
#undef  UI_RUDDER_TRACK_W
#define UI_RUDDER_TRACK_W       (uiSz.rudderTrackW)
#undef  UI_RUDDER_PORT_OPA
#define UI_RUDDER_PORT_OPA      (uiSz.rudderPortOpa)
#undef  UI_RUDDER_STB_OPA
#define UI_RUDDER_STB_OPA       (uiSz.rudderStbOpa)
#undef  UI_AP_CARD_W
#define UI_AP_CARD_W            (uiSz.apCardW)
#undef  UI_AP_CARD_H
#define UI_AP_CARD_H            (uiSz.apCardH)
#undef  UI_AP_CARD_GAP
#define UI_AP_CARD_GAP          (uiSz.apCardGap)
#undef  UI_AP_MODE_W
#define UI_AP_MODE_W            (uiSz.apModeW)
#undef  UI_AP_MODE_H
#define UI_AP_MODE_H            (uiSz.apModeH)
#undef  UI_AP_MODE_RADIUS
#define UI_AP_MODE_RADIUS       (uiSz.apModeRadius)
#undef  UI_AP_DEVBAR_H
#define UI_AP_DEVBAR_H          (uiSz.apDevBarH)
