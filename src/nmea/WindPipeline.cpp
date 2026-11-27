// ============================================================================
// WindPipeline.cpp - see WindPipeline.h. Caller holds data.lock().
// ============================================================================
#include "WindPipeline.h"
#include "DataModel.h"
#include "../config/Config.h"

namespace {

WindCalc::BoatInputs boatInputs() {
    WindCalc::BoatInputs b;
    b.stw = data.stw;  b.stwMs = data.lastStwUpdate;
    b.sog = data.sog;  b.sogMs = data.lastSogUpdate;
    b.hdg = data.hdg;  b.hdgMs = data.lastHdgUpdate;
    b.hdgMagnetic = data.hdgMagnetic;
    b.deviation   = data.hdgDeviation;
    b.variation   = data.variation;
    b.cog = data.cog;  b.cogMs = data.lastCogUpdate;
    b.cogMagnetic = data.cogMagnetic;
    return b;
}

void publish() {
    const WindCalc::Pipeline &p = data.windPipe;
    data.aws = p.aws;  data.awa = p.awa;
    data.tws = p.tws;  data.twa = p.twa;  data.twd = p.twd;
}

// The four time constants as they are configured right now (WindDampCfg.h
// has the keys); Config::fromJson has clamped them already.
inline WindCalc::WindDamping damping() {
    const AppConfig &c = appConfig.cfg;
    const WindCalc::WindDamping d = { c.calWindDampAppSpdS,  c.calWindDampAppDirS,
                                      c.calWindDampTrueSpdS, c.calWindDampTrueDirS };
    return d;
}
inline bool twdFromCog() { return appConfig.cfg.calTwdFromCog; }

} // namespace

void WindPipeline::apparent(float awsKn, float awaDeg, uint32_t nowMs) {
    if (data.windPipe.apparent(awsKn, awaDeg, boatInputs(), nowMs, damping(),
                               twdFromCog()))
        data.lastWindUpdate = nowMs;
    publish();
}

void WindPipeline::receivedTrue(uint8_t ref, float twsKn, float angleDeg, uint32_t nowMs) {
    if (data.windPipe.receivedTrue(ref, twsKn, angleDeg, boatInputs(), nowMs,
                                   damping(), twdFromCog()))
        data.lastWindUpdate = nowMs;
    publish();
}

void WindPipeline::tick(uint32_t nowMs) {
    data.windPipe.expire(nowMs);
    publish();
    // The input stamps are read with a signed age (WindCalc::fresh); clear
    // them once stale, or a log that died 24.8 days ago would count again.
    // Nothing but this pipeline reads them.
    WindCalc::dropStale(data.lastStwUpdate, nowMs, WindCalc::FRESH_MS);
    WindCalc::dropStale(data.lastSogUpdate, nowMs, WindCalc::FRESH_MS);
    WindCalc::dropStale(data.lastHdgUpdate, nowMs, WindCalc::FRESH_MS);
    WindCalc::dropStale(data.lastCogUpdate, nowMs, WindCalc::FRESH_MS);

    // Wind history, one sample per WIND_HIST_MS. The next slot is counted from
    // the previous SLOT, not from the tick that served it, so a display tick
    // that runs a little late does not stretch the six minutes; a tick more
    // than a whole period late (a long modal frame) starts the grid afresh
    // rather than firing a burst of identical catch-up samples.
    const uint32_t P = DataModel::WIND_HIST_MS;
    uint32_t &next = data.windHistNextMs;
    if (next && (int32_t)(nowMs - next) < 0) return;          // not due yet
    next = (next && (int32_t)(nowMs - next) < (int32_t)P) ? next + P : nowMs + P;
    // Only real samples: a gap without wind, boat speed or TWD must not fill
    // the rose with NaN entries that push six minutes of wind out.
    if (!isnan(data.twd) && !isnan(data.tws))
        data.pushWindSample(data.twd, data.tws, nowMs);
}
