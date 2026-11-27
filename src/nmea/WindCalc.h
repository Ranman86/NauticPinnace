#pragma once
// ============================================================================
// WindCalc - the wind maths of the display: true wind from apparent wind, the
// reverse (for the demo), the damping filters, and the pipeline that ties them
// together.
//
// The boat measures APPARENT wind only - the masthead unit, PGN 130306
// reference 2. True wind is computed here from that, the boat's own speed and
// its heading, instead of being taken from whatever another device on the bus
// sends (see Pipeline below for the one case where a received true wind is
// still used).
//
// CONVENTIONS - every caller and the host tests use exactly these:
//   * Boat frame: x forward, y starboard. Angles relative to the bow run
//     -180..+180 degrees, starboard positive, port negative (the DataModel's
//     "port negative"). Speeds in knots.
//   * True wind is WATER referenced: the boat moves at STW along its heading.
//     Leeway, current and heel are ignored.
//   * Apparent -> true:  ux = AWS cos AWA - STW,  uy = AWS sin AWA,
//                        TWS = |u|,  TWA = atan2(uy, ux).
//     True -> apparent:  ax = TWS cos TWA + STW,  ay = TWS sin TWA,
//                        AWS = |a|,  AWA = atan2(ay, ax).
//   * TWD (0..360, the direction the wind blows FROM) = true heading + TWA
//     (the stand-ins when there is no heading: see Pipeline).
//
// DAMPING is a first-order low-pass with time constant tau, written for an
// uneven sample rate: alpha = 1 - exp(-dt / tau), y += alpha * (x - y). After
// tau seconds a step in a speed has come 63 % of the way, and ten samples
// 0.1 s apart end where one sample after 1 s does. tau = 0 passes the input
// through unchanged.
// Speeds are filtered as plain numbers. Angles are filtered as unit vectors
// (cos, sin) and turned back with atan2: averaging the numbers would swing
// from 359 to 1 degree the long way round, through 180, and a run that
// alternates between +179 and -179 would read as a head wind. The 63 % holds
// for an angle only while the step is small: the vector cuts across the
// circle, so a large swing comes through faster (after tau, a 90 degree step
// shows 66 %, a 135 degree one 74 %, a 170 degree one 92 %). The first sample
// after a start or a reset is taken as it is - no ramp up from zero.
// Four time constants, one per group of outputs (WindDamping below): apparent
// wind speed and direction, true wind speed and direction. Apparent wind is
// for trimming and wants to react quickly; the true wind direction is for
// tactics - the shifts, not every gust - and takes far more damping; TWS lies
// in between. Each group is filtered on its own, so a time constant of one
// never touches the outputs of another.
//
// Free of Arduino, LVGL and FreeRTOS on purpose: it compiles unchanged on a
// PC, which is where its tests run. Keep it C++11 - the 4-inch board's core
// still compiles with -std=gnu++11.
// ============================================================================
#include <math.h>
#include <stdint.h>

namespace WindCalc {

// Below this true wind speed [kn] the true wind ANGLE is rounding noise: TWA
// keeps its previous value instead of spinning round the dial.
constexpr float CALM_KN = 0.05f;

// Below this speed over ground [kn] the COG does not START to stand in for
// the heading (the optional last source of TWD, see Pipeline). A GPS works
// its course out from successive fixes; slower than this the boat moves less
// between two fixes than the fixes scatter, so the course is mostly position
// noise - at the dock, at anchor or drifting it swings by tens of degrees
// from one fix to the next, and a TWD built on it would wander at random.
// The limit is 1 kn as the bus can say it, hence 0.99 rather than 1.0: PGN
// 129026 carries SOG in steps of 0.01 m/s, so a GPS doing 1 kn (0.5144 m/s)
// sends 0.51 m/s, which arrives as 0.991 kn. 1.0 here would turn that away
// and take the course only from the next step up, 0.52 m/s = 1.011 kn. The
// step below, 0.50 m/s = 0.972 kn, stays out.
// This is the limit for switching the COG ON. Once on, it stays on down to
// COG_OFF_SOG_KN.
constexpr float COG_MIN_SOG_KN = 0.99f;

// ... and the way back [kn]: a COG that stands in is given up only when the
// SOG drops BELOW this, and then needs COG_MIN_SOG_KN again. With one limit
// for both, a boat doing about 1 kn - drifting, motoring slowly, coming
// alongside - would make TWD flicker between a value and "--" with every fix
// that reads a few hundredths either side. Between the two limits the COG
// keeps the state it had. The limit is 0.8 kn as the bus says it, hence 0.79
// rather than 0.8 - the same reasoning as for COG_MIN_SOG_KN: a GPS doing
// 0.80 kn (0.4116 m/s) sends 0.41 m/s, which arrives as 0.797 kn and is
// shown as "0.8"; 0.8 here would switch the COG off at a speed that is not
// below 0.8 kn. The step below, 0.40 m/s = 0.778 kn, switches it off. On the
// bus the band in which the COG keeps its state is 0.41 to 0.50 m/s.
constexpr float COG_OFF_SOG_KN = 0.79f;

// Freshness windows [ms].
// FRESH_MS: STW, SOG, heading and COG older than this do not count, and a
//   received true wind is ignored while an apparent wind sample is younger
//   than this.
// STALE_MS: a wind value that has not been refreshed for this long is blanked
//   (NaN), so a sensor that stops leaves "--" instead of a frozen number.
constexpr uint32_t FRESH_MS = 5000;
constexpr uint32_t STALE_MS = 10000;

float norm180(float deg);   // any angle -> (-180, +180]; NaN stays NaN
float norm360(float deg);   // any angle -> [0, 360);     NaN stays NaN

// Apparent wind + boat speed -> true wind. When the result is calm (below
// CALM_KN), prevTwaDeg is handed back as the angle; pass NaN when there is
// none. Any NaN input gives NaN for both outputs.
void apparentToTrue(float aws, float awaDeg, float boatSpd, float prevTwaDeg,
                    float &tws, float &twaDeg);
// The reverse: true wind + boat speed -> apparent wind. Used by the demo.
void trueToApparent(float tws, float twaDeg, float boatSpd,
                    float &aws, float &awaDeg);

// Filter weight for a sample dt seconds after the previous one. tau <= 0 (or
// NaN) means no damping and gives 1; dt <= 0 gives 0 (no time has passed).
float lowPassAlpha(float dtSec, float tauSec);

// Low-pass for a speed. A NaN input clears it and returns NaN, so the next
// valid sample starts it afresh.
struct ScalarLowPass {
    float y      = NAN;
    bool  primed = false;
    void  reset() { y = NAN; primed = false; }
    float update(float x, float dtSec, float tauSec);
};

// Low-pass for an angle, on the unit circle. Returns (-180, +180]. A NaN
// input clears it and returns NaN, like the scalar one.
struct AngleLowPass {
    float c = 0.f, s = 0.f;      // filtered unit vector
    float y      = NAN;          // last output [deg]
    bool  primed = false;
    void  reset() { c = s = 0.f; y = NAN; primed = false; }
    float update(float xDeg, float dtSec, float tauSec);
};

// The damping time constants [s] the pipeline applies, one per group of
// outputs. 0, a negative value or NaN leaves that group undamped
// (lowPassAlpha). A plain aggregate on purpose - C++11 allows no member
// initialisers in one - so it is written in this order:
//     WindDamping d = { appSpd, appDir, trueSpd, trueDir };
// The settings behind it (config cal.wind_damp_app_spd / _app_dir /
// _true_spd / _true_dir) default to 2 s each and are kept to 0..30 s, see
// src/config/WindDampCfg.h; nothing here depends on those numbers.
struct WindDamping {
    float appSpd;    // AWS
    float appDir;    // AWA
    float trueSpd;   // TWS
    float trueDir;   // TWA and TWD
};

// True when a value stamped at stampMs is younger than windowMs. 0 = never
// stamped. Signed on purpose: a stamp a few ms AHEAD of nowMs (taken by
// another task after the caller read the clock) is fresh, not 49 days old.
// The price is that a stamp left alone for 24.8 days would wrap round to
// "fresh" again - a display on a liveaboard runs that long - so every stamp
// this is used on is cleared by dropStale() once it has gone stale.
inline bool fresh(uint32_t stampMs, uint32_t nowMs, uint32_t windowMs) {
    return stampMs != 0 && (int32_t)(nowMs - stampMs) < (int32_t)windowMs;
}
inline void dropStale(uint32_t &stampMs, uint32_t nowMs, uint32_t windowMs) {
    if (stampMs && !fresh(stampMs, nowMs, windowMs)) stampMs = 0;
}

// What the pipeline needs to know about the boat, each with its timestamp
// (millis, 0 = never received).
struct BoatInputs {
    float    stw = NAN;   uint32_t stwMs = 0;   // speed through water [kn]
    float    sog = NAN;   uint32_t sogMs = 0;   // speed over ground [kn]
    float    hdg = NAN;   uint32_t hdgMs = 0;   // heading as received [deg]
    bool     hdgMagnetic = false;               // ... magnetic, not true
    float    deviation   = NAN;                 // ... its compass deviation [deg, E +]
    float    variation   = NAN;                 // magnetic variation [deg, E +]
    float    cog = NAN;   uint32_t cogMs = 0;   // course over ground [deg]
    bool     cogMagnetic = false;               // ... magnetic, not true (rare)
};

// PGN 130306 references of a received TRUE wind, in the bus's own numbering.
enum : uint8_t {
    REF_TRUE_NORTH = 0,   // ground referenced, TWD
    REF_TRUE_GROUND = 3,  // boat referenced over ground (COG/SOG), TWA
    REF_TRUE_WATER = 4,   // boat referenced over water (heading/STW), TWA
};

// ---- The pipeline -----------------------------------------------------------
// One instance holds the whole wind state of the display (it lives in the
// DataModel). The firmware feeds it every apparent wind sample and every
// received true wind sample, and calls expire() on a clock; the five outputs
// are what every screen shows.
//
// For each apparent sample:
//   * AWS and AWA are damped (appSpd, appDir) and published.
//   * Boat speed = STW if fresh, else SOG if fresh. Neither: TWS, TWA and TWD
//     are NaN - the apparent wind still shows.
//   * TWS and TWA are computed from the RAW (undamped) apparent wind and the
//     boat speed, then damped (trueSpd, trueDir). Damping first and computing
//     afterwards would give the same steady state but a slower, lopsided
//     response - and would put the apparent wind's time constants into the
//     true wind.
//   * TWD = true heading + raw TWA, then damped (trueDir, like TWA). True
//     heading = the heading as received when PGN 127250 says "true" (or
//     says nothing). A MAGNETIC heading is the compass reading: 127250's
//     deviation turns it into the magnetic heading and the variation (from
//     127250 itself or 127258) into the true one, each added when known. A
//     magnetic heading with no variation anywhere on the bus is used without
//     it - every other screen of the firmware already treats the heading
//     that way, and the error is the local variation.
//     No fresh heading: TWD comes from a reference-0 true wind another
//     device is still sending (its DIRECTION only - TWS and TWA stay this
//     pipeline's own). Neither, and ONLY when the caller passes twdFromCog
//     (config cal.twd_cog, off by default): the COG of a fresh 129026 stands
//     in for the true heading while the COG latch (below) is on.
//     COG is true already, so nothing is added to it - no deviation either,
//     a GPS has none; only a COG the GPS marks MAGNETIC gets the variation,
//     like a magnetic heading. It is an approximation, hence the option: COG
//     is the boat's track, not where the bow points, and current, leeway
//     and drift turn TWD by the angle between the two. Otherwise TWD is NaN.
//     TWS and TWA never use a heading, so none of this touches them.
//
// The COG LATCH is a hysteresis on the SOG: it switches on when a fresh SOG
// reaches COG_MIN_SOG_KN (1 kn) and off only when the SOG drops below
// COG_OFF_SOG_KN (0.8 kn), so a boat doing about 1 kn does not make TWD
// flicker. It also goes off at once - and then needs COG_MIN_SOG_KN again -
// when the option is off, when COG or SOG is not fresh or not available,
// on reset(), when the true wind goes stale (expire), and when no sample has
// confirmed it for FRESH_MS: in such a gap the boat may have slowed down, or
// the GPS dropped out and come back, without the pipeline seeing it.
// Every sample moves the latch, apparent or received, used or standing by,
// and whichever source TWD actually comes from: behind a heading or a
// reference-0 TWD it keeps following the SOG. So when that source goes
// quiet at, say, 0.9 kn, the COG takes over at once if the boat came down
// from 1 kn or more, and does not if it never got there or has been below
// 0.8 kn since.
// The latch only decides whether the COG may be the source; it never touches
// the damping. A change of source - a heading handing over to the COG - and
// the latch holding through a wobble carry the TWD filter on; only a sample
// with no direction source at all (TWD NaN) restarts it, as for any source.
//
// A RECEIVED true wind (references 4, 3, 0 - another device's calculation) is
// otherwise only a fallback: it is used while no apparent wind sample has
// arrived for FRESH_MS, i.e. on a bus that has no masthead unit of its own.
// Among the three references the best one still talking wins - 4 (water
// referenced, what this pipeline computes itself), then 3, then 0 - instead
// of whichever frame happened to arrive last. A reference-0 sample gives TWD
// directly and TWA only through the heading (never through the COG); a 3/4
// sample the other way round, with the same stand-ins for TWD when there is
// no heading - reference 0, then, if switched on, the COG.
//
// expire(): a group (apparent: AWS/AWA; true: TWS/TWA/TWD) that has not been
// refreshed for STALE_MS goes NaN and its filters are reset, so the next
// sample initialises them; the true group takes the COG latch with it. It
// also forgets received-reference stamps and a COG latch that are no longer
// fresh (see dropStale). Call it at least every few seconds.
class Pipeline {
public:
    // Damped outputs - what every screen shows. NaN = not known.
    float aws = NAN, awa = NAN;     // [kn], [deg -180..+180]
    float tws = NAN, twa = NAN;     // [kn], [deg -180..+180]
    float twd = NAN;                // [deg 0..360]

    void reset();

    // damp and twdFromCog are settings, passed with every sample so a
    // changed one applies to the next: the four damping time constants [s]
    // and whether the COG may stand in for a missing heading in TWD.
    // twdFromCog defaults to the shipped setting (off).

    // One apparent wind sample (already calibrated). Returns false when it
    // carried nothing usable (both values NaN). Uses all four time
    // constants: the apparent pair on AWS/AWA, the true pair on what is
    // computed from it.
    bool apparent(float awsKn, float awaDeg, const BoatInputs &b,
                  uint32_t nowMs, const WindDamping &damp,
                  bool twdFromCog = false);

    // One received true wind sample: speed [kn] and angle [deg] - TWA for
    // references 3/4, TWD for 0. Returns false when it was not used. Only
    // the true pair of damp applies - this sample has no apparent wind.
    bool receivedTrue(uint8_t ref, float twsKn, float angleDeg,
                      const BoatInputs &b, uint32_t nowMs,
                      const WindDamping &damp, bool twdFromCog = false);

    // Blank what has gone stale (see above).
    void expire(uint32_t nowMs);

    static float boatSpeed(const BoatInputs &b, uint32_t nowMs);
    static float trueHeading(const BoatInputs &b, uint32_t nowMs);
    // The COG as a true heading, or NaN unless COG and SOG are both fresh and
    // known. How fast the boat has to go for it to count is the COG latch's
    // business (COG_MIN_SOG_KN / COG_OFF_SOG_KN), and whether it may be used
    // at all the caller's twdFromCog - neither is this function's.
    static float trueCourse(const BoatInputs &b, uint32_t nowMs);

private:
    ScalarLowPass awsF_, twsF_;
    AngleLowPass  awaF_, twaF_, twdF_;
    uint32_t appMs_    = 0;          // last apparent sample (0 = none)
    uint32_t trueMs_   = 0;          // last true wind update, computed or received
    uint32_t refMs_[3] = { 0, 0, 0 };// last received true wind: ref 4, 3, 0
    float    rxTwd_    = NAN;        // last received reference-0 TWD
    float    rawTwa_   = NAN;        // last raw TWA, held through a calm
    uint32_t cogLatchMs_ = 0;        // COG latch: last sample that confirmed it (0 = off)

    // On = confirmed by a sample less than FRESH_MS ago.
    bool  cogLatched(uint32_t nowMs) const { return fresh(cogLatchMs_, nowMs, FRESH_MS); }
    void  trackCogLatch(const BoatInputs &b, uint32_t nowMs, bool twdFromCog);
    float receivedTwd(uint32_t nowMs) const;
    float twdFrom(float rawTwa, const BoatInputs &b, uint32_t nowMs) const;
    // Damps the true group: tauSpd/tauDir are WindDamping::trueSpd/trueDir.
    void updateTrue(float rawTws, float rawTwa, float rawTwd,
                    uint32_t nowMs, float tauSpd, float tauDir);
    void clearApparent();
    void clearTrue();
};

} // namespace WindCalc
