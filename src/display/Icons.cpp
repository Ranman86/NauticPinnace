// ============================================================================
// Icons.cpp - see Icons.h. OpenBridge icon trial.
// ============================================================================
#include "Icons.h"
#include "../config/Config.h"
#include "fonts/ob_icons.h"
#include <lvgl.h>

namespace {

struct IconPair { const char *classic; const char *openbridge; };

// Indexed by NpIcon - the static_assert below keeps the two in step. Where the
// OpenBridge column repeats the classic glyph, the pack has nothing fitting and
// NauticPi keeps the old sign as well (see ICONS-TRIAL.md for the list).
// Unsized on purpose: with [NP_ICON_COUNT] a missing row would be zero-filled
// silently, and the size check below could never fail.
const IconPair kIcons[] = {
    /* WIND        */ { LV_SYMBOL_REFRESH,      OB_ICON_WIND },
    /* SPEED       */ { LV_SYMBOL_CHARGE,       OB_ICON_STW },
    /* DEPTH       */ { LV_SYMBOL_DOWN,         OB_ICON_DEPTH },
    /* ENGINE      */ { LV_SYMBOL_POWER,        OB_ICON_ENGINE },
    /* RUDDER      */ { LV_SYMBOL_SHUFFLE,      OB_ICON_RUDDER },
    /* AIS         */ { LV_SYMBOL_GPS,          OB_ICON_AIS },
    /* WINDPLOT    */ { LV_SYMBOL_LIST,         OB_ICON_TREND },
    /* AUTOPILOT   */ { LV_SYMBOL_PLAY,         OB_ICON_AUTOPILOT },
    /* MEDIA       */ { LV_SYMBOL_AUDIO,        OB_ICON_SOUND },
    /* ATTITUDE    */ { LV_SYMBOL_LOOP,         OB_ICON_ROLL },
    /* ANCHOR      */ { LV_SYMBOL_DOWNLOAD,     OB_ICON_ANCHORWATCH },
    /* TANK        */ { LV_SYMBOL_TINT,         OB_ICON_TANK },
    /* BATTERY     */ { LV_SYMBOL_BATTERY_FULL, OB_ICON_BATTERY },
    /* WEATHER     */ { LV_SYMBOL_EYE_OPEN,     OB_ICON_WEATHER },
    /* CLOCK       */ { LV_SYMBOL_BELL,         OB_ICON_TIME },
    /* VMG         */ { LV_SYMBOL_UP,           LV_SYMBOL_UP },        // no VMG icon
    /* ROUTE       */ { LV_SYMBOL_NEXT,         OB_ICON_WAYPOINT_NEXT },
    /* GRID        */ { LV_SYMBOL_KEYBOARD,     OB_ICON_TABLE },
    /* SETTINGS    */ { LV_SYMBOL_SETTINGS,     OB_ICON_SETTINGS },
    /* HOME        */ { LV_SYMBOL_HOME,         OB_ICON_HOME },
    /* PREV        */ { LV_SYMBOL_LEFT,         OB_ICON_CHEVRON_LEFT },
    /* NEXT        */ { LV_SYMBOL_RIGHT,        OB_ICON_CHEVRON_RIGHT },
    /* CLOSE       */ { LV_SYMBOL_CLOSE,        OB_ICON_CLOSE },
    /* WARNING     */ { LV_SYMBOL_WARNING,      OB_ICON_ALARM },
    /* DEMO        */ { LV_SYMBOL_WARNING,      OB_ICON_SIMULATION },
    /* MEDIA_PREV  */ { LV_SYMBOL_PREV,         OB_ICON_MEDIA_PREV },
    /* MEDIA_PLAY  */ { LV_SYMBOL_PLAY,         OB_ICON_MEDIA_PLAY },
    /* MEDIA_PAUSE */ { LV_SYMBOL_PAUSE,        OB_ICON_MEDIA_PAUSE },
    /* MEDIA_NEXT  */ { LV_SYMBOL_NEXT,         OB_ICON_MEDIA_NEXT },
    /* VOLUME      */ { LV_SYMBOL_VOLUME_MAX,   OB_ICON_SOUND },
    /* MUTE        */ { LV_SYMBOL_MUTE,         OB_ICON_SOUND_MUTED },
};
static_assert(sizeof(kIcons) / sizeof(kIcons[0]) == NP_ICON_COUNT,
              "kIcons must have one row per NpIcon");

}  // namespace

bool npIconsOpenBridge() {
    return appConfig.cfg.iconSet == ICON_SET_OPENBRIDGE;
}

const char *npSym(NpIcon id) {
    if (id >= NP_ICON_COUNT) return "";
    return npIconsOpenBridge() ? kIcons[id].openbridge : kIcons[id].classic;
}
