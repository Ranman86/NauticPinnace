#pragma once
// ============================================================================
// Icons - one symbol per MEANING, in two sets. OpenBridge icon trial.
//
// Every place that draws a symbol asks npSym(NP_ICON_...) instead of naming an
// LV_SYMBOL_* directly. The config switch ui.icons picks the set:
//
//   "classic"     the Font Awesome glyphs built into LVGL's Montserrat - the
//                 firmware draws exactly what it drew before the trial, byte
//                 for byte the same strings.
//   "openbridge"  the maritime icons of the OpenBridge Icon Pack (CC BY 4.0),
//                 the same icon for the same meaning as NauticPi uses. They
//                 live in the Private Use Area of the generated ob_icons_<px>
//                 fonts, which Theme.cpp chains behind every Montserrat size,
//                 so any label in any font role can show them.
//
// WHEN A SWITCH TAKES EFFECT. Saving ui.icons in the web configuration triggers
// the same live rebuild as a theme or language change: the instrument screens,
// the sidebar, the floating buttons and the demo banner are relabelled at once;
// the home launcher and the settings overlay are built on every open anyway.
//
// Removing the trial: see ICONS-TRIAL.md. Every call site carries the comment
// "OpenBridge icon trial".
// ============================================================================
#include <stdint.h>

enum NpIcon : uint8_t {
    // home launcher tiles, one per screen
    NP_ICON_WIND,
    NP_ICON_SPEED,
    NP_ICON_DEPTH,
    NP_ICON_ENGINE,
    NP_ICON_RUDDER,
    NP_ICON_AIS,
    NP_ICON_WINDPLOT,
    NP_ICON_AUTOPILOT,
    NP_ICON_MEDIA,
    NP_ICON_ATTITUDE,
    NP_ICON_ANCHOR,
    NP_ICON_TANK,
    NP_ICON_BATTERY,
    NP_ICON_WEATHER,
    NP_ICON_CLOCK,
    NP_ICON_VMG,            // stays Font Awesome in both sets: no VMG icon in the pack
    NP_ICON_ROUTE,
    NP_ICON_GRID,           // data grids (the default tile)
    // chrome
    NP_ICON_SETTINGS,
    NP_ICON_HOME,
    NP_ICON_PREV,
    NP_ICON_NEXT,
    NP_ICON_CLOSE,
    NP_ICON_WARNING,        // anchor drag, shallow water, over-revving
    NP_ICON_DEMO,           // demo banner
    // media screen
    NP_ICON_MEDIA_PREV,
    NP_ICON_MEDIA_PLAY,
    NP_ICON_MEDIA_PAUSE,
    NP_ICON_MEDIA_NEXT,
    NP_ICON_VOLUME,
    NP_ICON_MUTE,
    NP_ICON_COUNT
};

// The UTF-8 string to put into a label for this icon in the active set.
const char *npSym(NpIcon id);

// True when ui.icons is "openbridge".
bool npIconsOpenBridge();
