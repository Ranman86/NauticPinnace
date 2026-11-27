// ============================================================================
// WindCalc.cpp - see WindCalc.h for the conventions and the pipeline rules.
//
// Depends on nothing but the C maths library, so the host tests compile this
// very file.
// ============================================================================
#include "WindCalc.h"
#include <math.h>

namespace WindCalc {

static const float DEG2RAD = 0.017453292519943295f;
static const float RAD2DEG = 57.29577951308232f;

// isnan() is a macro in some C libraries and a function in others; a local
// helper keeps the spelling independent of which one the build pulled in.
static inline bool nanf_(float v) { return v != v; }

static inline float secondsBetween(uint32_t fromMs, uint32_t toMs) {
    return (float)(int32_t)(toMs - fromMs) / 1000.f;
}

float norm180(float deg) {
    if (nanf_(deg)) return NAN;
    deg = fmodf(deg, 360.f);                // (-360, 360)
    if (deg > 180.f)        deg -= 360.f;
    else if (deg <= -180.f) deg += 360.f;
    return deg;
}

float norm360(float deg) {
    if (nanf_(deg)) return NAN;
    deg = fmodf(deg, 360.f);                // (-360, 360)
    if (deg < 0.f) deg += 360.f;
    // -1e-6 + 360 rounds to exactly 360 in float, which is 0 on a compass.
    if (deg >= 360.f) deg -= 360.f;
    return deg;
}

void apparentToTrue(float aws, float awaDeg, float boatSpd, float prevTwaDeg,
                    float &tws, float &twaDeg) {
    if (nanf_(aws) || nanf_(awaDeg) || nanf_(boatSpd)) {
        tws = NAN; twaDeg = NAN; return;
    }
    const float a  = awaDeg * DEG2RAD;
    const float ux = aws * cosf(a) - boatSpd;   // forward component
    const float uy = aws * sinf(a);             // starboard component
    tws    = sqrtf(ux * ux + uy * uy);
    twaDeg = (tws < CALM_KN) ? prevTwaDeg : norm180(atan2f(uy, ux) * RAD2DEG);
}

void trueToApparent(float tws, float twaDeg, float boatSpd,
                    float &aws, float &awaDeg) {
    if (nanf_(tws) || nanf_(twaDeg) || nanf_(boatSpd)) {
        aws = NAN; awaDeg = NAN; return;
    }
    const float a  = twaDeg * DEG2RAD;
    const float ax = tws * cosf(a) + boatSpd;
    const float ay = tws * sinf(a);
    aws    = sqrtf(ax * ax + ay * ay);
    awaDeg = norm180(atan2f(ay, ax) * RAD2DEG);
}

float lowPassAlpha(float dtSec, float tauSec) {
    if (!(tauSec > 0.f)) return 1.f;        // also catches NaN
    if (!(dtSec  > 0.f)) return 0.f;
    return 1.f - expf(-dtSec / tauSec);
}

float ScalarLowPass::update(float x, float dtSec, float tauSec) {
    if (nanf_(x)) { reset(); return NAN; }
    if (!primed) { y = x; primed = true; return y; }
    const float a = lowPassAlpha(dtSec, tauSec);
    // Assigned rather than y += 1 * (x - y): that is not exactly x in float,
    // and "0 = off" has to mean off.
    if (a >= 1.f) y = x;
    else          y += a * (x - y);
    return y;
}

float AngleLowPass::update(float xDeg, float dtSec, float tauSec) {
    if (nanf_(xDeg)) { reset(); return NAN; }
    const float r  = xDeg * DEG2RAD;
    const float cx = cosf(r), sx = sinf(r);
    const float a  = primed ? lowPassAlpha(dtSec, tauSec) : 1.f;
    primed = true;
    if (a >= 1.f) {                         // first sample, or no damping
        c = cx; s = sx;
        y = norm180(xDeg);
        return y;
    }
    c += a * (cx - c);
    s += a * (sx - s);
    // The mean of two exactly opposite directions is the origin, which has no
    // angle. Keep the last output until the vector has a length again.
    if (c * c + s * s > 1e-12f) y = norm180(atan2f(s, c) * RAD2DEG);
    return y;
}

// ---- Pipeline ---------------------------------------------------------------

void Pipeline::reset() {
    clearApparent();
    clearTrue();
    refMs_[0] = refMs_[1] = refMs_[2] = 0;
    rxTwd_ = NAN;
    cogLatchMs_ = 0;
}

void Pipeline::clearApparent() {
    awsF_.reset(); awaF_.reset();
    aws = awa = NAN;
    appMs_ = 0;
}

// Leaves the COG latch alone: a wind sample with its speed or angle missing
// (apparent()) clears the true wind but says nothing about the course, and
// must not make the COG climb back to COG_MIN_SOG_KN. With no boat speed the
// SOG is stale, which has switched the latch off already; expire() clears it
// itself when the true wind goes stale.
void Pipeline::clearTrue() {
    twsF_.reset(); twaF_.reset(); twdF_.reset();
    tws = twa = twd = NAN;
    trueMs_ = 0;
    rawTwa_ = NAN;
}

float Pipeline::boatSpeed(const BoatInputs &b, uint32_t nowMs) {
    if (!nanf_(b.stw) && fresh(b.stwMs, nowMs, FRESH_MS)) return b.stw;
    if (!nanf_(b.sog) && fresh(b.sogMs, nowMs, FRESH_MS)) return b.sog;
    return NAN;
}

float Pipeline::trueHeading(const BoatInputs &b, uint32_t nowMs) {
    if (nanf_(b.hdg) || !fresh(b.hdgMs, nowMs, FRESH_MS)) return NAN;
    float h = b.hdg;
    if (b.hdgMagnetic) {
        if (!nanf_(b.deviation)) h += b.deviation;   // compass -> magnetic
        if (!nanf_(b.variation)) h += b.variation;   // magnetic -> true
    }
    return norm360(h);
}

float Pipeline::trueCourse(const BoatInputs &b, uint32_t nowMs) {
    if (nanf_(b.cog) || !fresh(b.cogMs, nowMs, FRESH_MS)) return NAN;
    // The SOG has to be fresh and known as well: an old fast SOG must not
    // vouch for a course taken since the boat stopped, and the latch judges
    // the speed by it (trackCogLatch).
    if (nanf_(b.sog) || !fresh(b.sogMs, nowMs, FRESH_MS)) return NAN;
    float c = b.cog;
    if (b.cogMagnetic && !nanf_(b.variation)) c += b.variation;   // magnetic -> true
    return norm360(c);
}

// The COG latch (WindCalc.h has the rules), moved by every sample before its
// TWD source is chosen. On - confirmed less than FRESH_MS ago - the SOG only
// has to stay at COG_OFF_SOG_KN or above; off, it has to reach
// COG_MIN_SOG_KN. The option off, or a COG or SOG that is stale or not
// available (trueCourse NaN), switches it off whatever the speed.
void Pipeline::trackCogLatch(const BoatInputs &b, uint32_t nowMs, bool twdFromCog) {
    const float limit = cogLatched(nowMs) ? COG_OFF_SOG_KN : COG_MIN_SOG_KN;
    const bool  on    = twdFromCog && !nanf_(trueCourse(b, nowMs)) && b.sog >= limit;
    cogLatchMs_ = on ? nowMs : 0;
}

float Pipeline::receivedTwd(uint32_t nowMs) const {
    return fresh(refMs_[2], nowMs, FRESH_MS) ? rxTwd_ : NAN;
}

// TWD for a raw TWA, from the first source there is (WindCalc.h has the
// rules): the heading, else another device's reference-0 TWD, else - only
// while the COG latch is on, which needs the owner's option - the COG. A
// heading with no TWA (a calm without a previous angle) gives NaN rather
// than falling through: the heading is there, only the angle is not.
// The caller has moved the latch with this very sample (trackCogLatch).
float Pipeline::twdFrom(float rawTwa, const BoatInputs &b, uint32_t nowMs) const {
    const float hdgT = trueHeading(b, nowMs);
    if (!nanf_(hdgT)) return nanf_(rawTwa) ? NAN : norm360(hdgT + rawTwa);
    const float rxTwd = receivedTwd(nowMs);
    if (!nanf_(rxTwd) || !cogLatched(nowMs)) return rxTwd;
    const float crs = trueCourse(b, nowMs);
    return (nanf_(crs) || nanf_(rawTwa)) ? NAN : norm360(crs + rawTwa);
}

void Pipeline::updateTrue(float rawTws, float rawTwa, float rawTwd,
                          uint32_t nowMs, float tauSpd, float tauDir) {
    const float dt = trueMs_ ? secondsBetween(trueMs_, nowMs) : 0.f;
    trueMs_ = nowMs;
    if (!nanf_(rawTwa)) rawTwa_ = rawTwa;
    tws = twsF_.update(rawTws, dt, tauSpd);
    twa = twaF_.update(rawTwa, dt, tauDir);
    twd = norm360(twdF_.update(rawTwd, dt, tauDir));
}

bool Pipeline::apparent(float awsKn, float awaDeg, const BoatInputs &b,
                        uint32_t nowMs, const WindDamping &damp,
                        bool twdFromCog) {
    if (nanf_(awsKn) && nanf_(awaDeg)) return false;
    // First, whatever TWD will come from: behind a heading or a reference-0
    // TWD the latch keeps following the SOG.
    trackCogLatch(b, nowMs, twdFromCog);
    const float dt = appMs_ ? secondsBetween(appMs_, nowMs) : 0.f;
    appMs_ = nowMs;
    aws = awsF_.update(awsKn,  dt, damp.appSpd);
    awa = awaF_.update(awaDeg, dt, damp.appDir);

    float rawTws, rawTwa;
    apparentToTrue(awsKn, awaDeg, boatSpeed(b, nowMs), rawTwa_, rawTws, rawTwa);
    if (nanf_(rawTws)) {
        // No boat speed (or half an apparent sample): there is no true wind,
        // and an old one must not linger next to a live apparent wind.
        clearTrue();
        return true;
    }
    // TWD needs a direction source on top (twdFrom). With no heading, another
    // device's TWD or the COG stands in for the direction only - next to a
    // TWS and TWA of our own it is the one value this pipeline cannot work
    // out, and without it the wind history stays empty.
    updateTrue(rawTws, rawTwa, twdFrom(rawTwa, b, nowMs),
               nowMs, damp.trueSpd, damp.trueDir);
    return true;
}

bool Pipeline::receivedTrue(uint8_t ref, float twsKn, float angleDeg,
                            const BoatInputs &b, uint32_t nowMs,
                            const WindDamping &damp, bool twdFromCog) {
    int rank;
    switch (ref) {
        case REF_TRUE_WATER:  rank = 0; break;
        case REF_TRUE_GROUND: rank = 1; break;
        case REF_TRUE_NORTH:  rank = 2; break;
        default: return false;
    }
    if (nanf_(twsKn) || nanf_(angleDeg)) return false;
    trackCogLatch(b, nowMs, twdFromCog);   // used or standing by, as in apparent()
    refMs_[rank] = nowMs;
    // Kept even when this sample is not used: it stands in for the TWD when
    // there is no heading (see receivedTwd, twdFrom).
    if (ref == REF_TRUE_NORTH) rxTwd_ = norm360(angleDeg);
    // Our own calculation wins whenever there is an apparent wind to make it from.
    if (fresh(appMs_, nowMs, FRESH_MS)) return false;
    // A better reference is still talking: this one only stands by.
    for (int r = 0; r < rank; r++)
        if (fresh(refMs_[r], nowMs, FRESH_MS)) return false;

    float rawTwa, rawTwd;
    if (ref == REF_TRUE_NORTH) {
        // TWA from a TWD needs where the bow points: the heading only, never
        // the COG - TWA does not depend on the option.
        const float hdgT = trueHeading(b, nowMs);
        rawTwd = norm360(angleDeg);
        rawTwa = nanf_(hdgT) ? NAN : norm180(rawTwd - hdgT);
    } else {
        rawTwa = norm180(angleDeg);
        rawTwd = twdFrom(rawTwa, b, nowMs);
    }
    updateTrue(twsKn, rawTwa, rawTwd, nowMs, damp.trueSpd, damp.trueDir);
    return true;
}

void Pipeline::expire(uint32_t nowMs) {
    if (appMs_  && !fresh(appMs_,  nowMs, STALE_MS)) clearApparent();
    if (trueMs_ && !fresh(trueMs_, nowMs, STALE_MS)) {
        clearTrue();
        // Received frames standing by can have kept confirming the latch;
        // after a gap this long the COG has to reach COG_MIN_SOG_KN again.
        cogLatchMs_ = 0;
    }
    for (int r = 0; r < 3; r++) dropStale(refMs_[r], nowMs, FRESH_MS);
    dropStale(cogLatchMs_, nowMs, FRESH_MS);
}

} // namespace WindCalc
