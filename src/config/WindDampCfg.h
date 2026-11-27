#pragma once
// ============================================================================
// WindDampCfg - the four wind damping settings as the config stores them:
// their limits and default, the clamping, and how a config document maps
// onto them, including the two settings they replaced.
//
//   JSON "cal" key        Config field            WindCalc::WindDamping  acts on
//   wind_damp_app_spd     calWindDampAppSpdS      appSpd                 AWS
//   wind_damp_app_dir     calWindDampAppDirS      appDir                 AWA
//   wind_damp_true_spd    calWindDampTrueSpdS     trueSpd                TWS
//   wind_damp_true_dir    calWindDampTrueDirS     trueDir                TWA, TWD
//
// Before the split there were two keys for both groups: wind_damp_spd (AWS
// and TWS) and wind_damp_dir (AWA, TWA and TWD). They are still READ - a
// stored config, an old export or an old web page may carry them - and never
// written again (Config::toJson writes the four above only).
//
// Free of Arduino and ArduinoJson: Config.cpp reads the JSON and hands the
// numbers to apply(), and the host tests compile this header as it is. The
// limits also reach the WebUI, through GET /api/info (WebConfig.cpp), so the
// form does not carry a second copy of them. Keep it C++11.
// ============================================================================
#include <math.h>
#include "../nmea/WindCalc.h"

namespace WindDampCfg {

// Time constants in seconds. The default is the same for all four, so an
// installation that never touches the setting damps as it always did.
constexpr float MIN_S     = 0.f;    // 0 = off
constexpr float MAX_S     = 30.f;
constexpr float DEFAULT_S = 2.f;

// One stored value: NaN (nothing usable) keeps `keep`, anything else is
// clamped into MIN_S..MAX_S rather than refused - a WebUI field cannot send
// anything else, a hand-edited import could, and a negative time constant
// must not reach the filter.
inline float clamp(float v, float keep) {
    if (v != v) return keep;                       // NaN; no isnan() macro games
    return v < MIN_S ? MIN_S : (v > MAX_S ? MAX_S : v);
}

// What one config document says about the damping: the value of each key,
// NaN where the key is missing or is not a number.
struct Keys {
    float appSpd  = NAN;    // wind_damp_app_spd
    float appDir  = NAN;    // wind_damp_app_dir
    float trueSpd = NAN;    // wind_damp_true_spd
    float trueDir = NAN;    // wind_damp_true_dir
    float oldSpd  = NAN;    // wind_damp_spd  (before the split: AWS and TWS)
    float oldDir  = NAN;    // wind_damp_dir  (before the split: AWA, TWA, TWD)
};

inline float orElse(float v, float fallback) { return (v != v) ? fallback : v; }

// The four settings after one config document - the stored file at boot, a
// WebUI patch, an import - has been read on top of cur:
//   * a new key wins wherever it carries a number;
//   * where it does not, the old key of the same kind stands in: the old
//     speed for AWS and TWS, the old direction for AWA and TWA/TWD - so a
//     config saved with the two settings damps exactly as before;
//   * neither: the value held so far, so a patch changes only what it names.
// Every result goes through clamp(), with the value held so far as `keep`.
inline WindCalc::WindDamping apply(const WindCalc::WindDamping &cur, const Keys &k) {
    WindCalc::WindDamping d;
    d.appSpd  = clamp(orElse(k.appSpd,  k.oldSpd), cur.appSpd);
    d.appDir  = clamp(orElse(k.appDir,  k.oldDir), cur.appDir);
    d.trueSpd = clamp(orElse(k.trueSpd, k.oldSpd), cur.trueSpd);
    d.trueDir = clamp(orElse(k.trueDir, k.oldDir), cur.trueDir);
    return d;
}

} // namespace WindDampCfg
