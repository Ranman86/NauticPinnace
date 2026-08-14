// strings_screens.inc — labels drawn by the individual screens in
// src/display/screens/*.cpp.
//
// Split into three fragments purely so they can be edited independently:
//   _a  AisScreen, AnchorScreen, AutopilotScreen, BatteryScreen, ClockScreen, DepthScreen
//   _b  EngineScreen, FusionScreen, GridScreen, RouteScreen, RudderScreen, SpeedScreen
//   _c  TankScreen, VmgScreen, WeatherScreen, WindPlotScreen, WindScreen, BootScreen
//
// One line per string:  X(KEY, "deutsch", "english")
// Rules:
//   - KEY is unique across ALL .inc files (they share one enum). Prefix each
//     key with its screen, e.g. WIND_, DEPTH_, ENG_, AIS_.
//   - Write real UTF-8 (Ö, ü, ß). Do NOT use \xNN escapes: C hex escapes are
//     greedy, so "Stra\xC3\x9Fe" silently swallows the 'e' into the escape.
//   - These are drawn on the 480x480 panel, so English must not be longer than
//     German where space is tight, or the label clips.
//   - Units (kn, m, V, A, hPa, °C) are NOT translated.
#include "strings_screens_a.inc"
#include "strings_screens_b.inc"
#include "strings_screens_c.inc"

#define I18N_STRINGS_SCREENS(X) \
    I18N_STRINGS_SCREENS_A(X)  \
    I18N_STRINGS_SCREENS_B(X)  \
    I18N_STRINGS_SCREENS_C(X)
