#pragma once
#include <math.h>

// ============================================================
// SunCalc – header-only date + sunrise/sunset helpers.
// All functions are static inline (internal linkage) so the header can be
// included in several translation units without ODR issues.
// ============================================================

// Days since 1970-01-01 for a civil date. scDaysFromCivil/scCivilFromDays are
// ports of Howard Hinnant's chrono-compatible date algorithms as published on
// howardhinnant.github.io/date_algorithms.html, which the author donated to the
// public domain there. The same algorithms are also in his MIT-licensed date
// library; nothing was copied from that library. See THIRD-PARTY-NOTICES.md.
static inline long scDaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    long     era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// Civil date for a days-since-1970 count.
static inline void scCivilFromDays(long z, int &y, unsigned &m, unsigned &d) {
    z += 719468;
    long     era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int      yy  = (int)yoe + (int)(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp  = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = yy + (m <= 2);
}

// Sunrise / sunset in UTC fractional hours for a civil date + position.
// state: 0 = normal, 1 = polar day (sun never sets), -1 = polar night.
// Classic "Almanac for Computers" sunrise equation.
static inline void scSunTimesUTC(int year, unsigned month, unsigned day,
                                 float lat, float lon,
                                 float &riseUTC, float &setUTC, int &state) {
    const float ZEN = 90.833f;             // official sunrise/sunset zenith
    const float D2R = 0.0174532925f, R2D = 57.2957795f;
    long N = scDaysFromCivil(year, month, day) - scDaysFromCivil(year, 1, 1) + 1;
    float lngHour = lon / 15.f;
    state = 0;
    riseUTC = setUTC = 0.f;
    for (int which = 0; which < 2; which++) {            // 0 = rise, 1 = set
        float t = (which == 0) ? N + ((6.f  - lngHour) / 24.f)
                               : N + ((18.f - lngHour) / 24.f);
        float M = (0.9856f * t) - 3.289f;
        float L = M + (1.916f * sinf(M * D2R)) + (0.020f * sinf(2.f * M * D2R)) + 282.634f;
        L = fmodf(L + 360.f, 360.f);
        float RA = R2D * atanf(0.91764f * tanf(L * D2R));
        RA = fmodf(RA + 360.f, 360.f);
        RA += (floorf(L / 90.f) * 90.f) - (floorf(RA / 90.f) * 90.f);   // same quadrant as L
        RA /= 15.f;
        float sinDec = 0.39782f * sinf(L * D2R);
        float cosDec = cosf(asinf(sinDec));
        float cosH = (cosf(ZEN * D2R) - (sinDec * sinf(lat * D2R))) / (cosDec * cosf(lat * D2R));
        if (cosH >  1.f) { state = -1; return; }         // never rises
        if (cosH < -1.f) { state =  1; return; }         // never sets
        float H = (which == 0) ? (360.f - R2D * acosf(cosH)) : (R2D * acosf(cosH));
        H /= 15.f;
        float T  = H + RA - (0.06571f * t) - 6.622f;
        float UT = fmodf(T - lngHour + 240.f, 24.f);
        if (which == 0) riseUTC = UT; else setUTC = UT;
    }
}

// Subsolar point: solar declination [deg] and the longitude [deg, -180..180]
// where the sun is overhead, for a civil date + UTC seconds-of-day.
// Low-precision solar coordinates after the US Naval Observatory's
// "Approximate Solar Coordinates" (mean anomaly, mean longitude, equation of
// centre, obliquity), with the same day count and sidereal time as
// scTideEstimate() below: within about 0.01 deg of the NOAA (Meeus) solar
// position over 1990-2060. The sun stands overhead where the local sidereal
// time equals its right ascension, i.e. at longitude RA - GMST. This replaces
// a cosine fit of the declination (off by up to 1.3 deg) and a subsolar
// longitude of 180 - 15 deg/h, which left out the equation of time (up to
// 16 min, i.e. 4 deg of longitude). The sun position behind the clock map now
// matches scSunTimesUTC()'s sunrise/sunset to about a minute (formerly up to
// 25 min); the terminator drawn there is the geometric one (altitude 0), so it
// passes the boat a few minutes after sunrise and before sunset - the
// refraction and solar radius in the 90.833 deg zenith of those times.
static inline void scSubsolar(int year, unsigned month, unsigned day,
                              double utcSec, float &decDeg, float &sunLonDeg) {
    const double D2R = 0.0174532925199433;
    // Days since J2000.0 (2000-01-01 12:00 UTC = day 10957.5 since 1970).
    const double d = (double)scDaysFromCivil(year, month, day) + utcSec / 86400.0 - 10957.5;
    const double g = (357.529 + 0.98560028 * d) * D2R;               // mean anomaly
    const double q = 280.459 + 0.98564736 * d;                        // mean longitude [deg]
    const double L = (q + 1.915 * sin(g) + 0.020 * sin(2.0 * g)) * D2R;   // ecliptic longitude
    const double e = (23.439 - 0.00000036 * d) * D2R;                 // obliquity
    decDeg = (float)(asin(sin(e) * sin(L)) / D2R);
    const double ra   = atan2(cos(e) * sin(L), cos(L)) / D2R;         // right ascension [deg]
    const double gmst = 280.46061837 + 360.98564736629 * d;           // [deg]
    double lon = fmod(ra - gmst, 360.0);                              // -360..360
    while (lon > 180.0)  lon -= 360.0;
    while (lon < -180.0) lon += 360.0;
    sunLonDeg = (float)lon;
}

// Moon: synodic age [days, 0..29.53], illuminated fraction [0..1], waxing flag.
static inline void scMoonPhase(long daysSince1970, double secOfDay,
                               float &ageDays, float &illum, bool &waxing) {
    const double SYN = 29.530588853;
    // Reference new moon 2000-01-06 18:14 UTC. Days since 1970 for 2000-01-06 = 10962.
    double now = (double)daysSince1970 + secOfDay / 86400.0;
    double refNew = 10962.0 + (18.0 * 3600.0 + 14.0 * 60.0) / 86400.0;
    double age = fmod(now - refNew, SYN);
    if (age < 0) age += SYN;
    ageDays = (float)age;
    illum   = (float)((1.0 - cos(2.0 * 3.14159265 * age / SYN)) / 2.0);
    waxing  = (age < SYN / 2.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Rough tide estimate — UNCALIBRATED. Models only the astronomical FORCING:
// the dominant semidiurnal lunar term (M2, ~12.42 h) whose two daily peaks track
// the Moon's meridian transits, modulated by a spring/neap factor from the
// Moon-Sun elongation (S2 adds to M2 near new/full, opposes near the quarters).
//
// IMPORTANT: this is NOT a real tide prediction. Without a local harmonic record
// (the "lunitidal interval"/establishment of the port) the actual high/low-water
// CLOCK TIMES at a given coast can differ from this forcing by HOURS, and the
// real range depends on bathymetry. Use only as a coarse "where are we in the
// tidal cycle" indicator. The Moon's RA here is a mean-longitude approximation.
//
//   levelNorm        : normalised tide height, roughly -1..+1 (spring-scaled)
//   rising           : true if the level is currently increasing
//   hoursToNext      : hours until the next high or low water (forcing extreme)
//   nextIsHigh       : true if that next extreme is a high water
//   springFactor     : 0.5 (neap) .. 1.5 (spring) range multiplier
//
// scTideLevel() below repeats the levelNorm part of this; change both together.
static inline void scTideEstimate(long daysSince1970, double secOfDay, float lonDeg,
                                  float &levelNorm, bool &rising,
                                  float &hoursToNext, bool &nextIsHigh,
                                  float &springFactor) {
    const double D2R = 0.0174532925;
    // Days since J2000.0 (2000-01-01 12:00 UTC = day 10957.5 since 1970).
    double d = ((double)daysSince1970 + secOfDay / 86400.0) - 10957.5;

    // Moon mean ecliptic longitude + Sun mean longitude (deg).
    double lamMoon = fmod(218.316 + 13.176396 * d, 360.0); if (lamMoon < 0) lamMoon += 360.0;
    double lamSun  = fmod(280.460 +  0.9856474 * d, 360.0); if (lamSun  < 0) lamSun  += 360.0;

    // Approx Moon right ascension from ecliptic longitude (ignore lunar latitude).
    const double eps = 23.439 * D2R;
    double raMoon = atan2(cos(eps) * sin(lamMoon * D2R), cos(lamMoon * D2R)) / D2R;
    raMoon = fmod(raMoon + 360.0, 360.0);

    // Greenwich mean sidereal time -> local hour angle of the Moon (deg).
    double gmst = fmod(280.46061837 + 360.98564736629 * d, 360.0);
    double H = fmod(gmst + lonDeg - raMoon + 720.0, 360.0);   // 0..360

    // Semidiurnal level ∝ cos(2H); highs at the Moon's upper & lower transit.
    double elong = fmod(lamMoon - lamSun + 360.0, 360.0);     // 0=new, 180=full
    springFactor = (float)(1.0 + 0.46 * cos(2.0 * elong * D2R));   // ~0.54..1.46
    levelNorm = (float)(springFactor * cos(2.0 * H * D2R));

    // Rising when d/dt cos(2H) > 0  ->  sin(2H) < 0  (H increases with time).
    rising = sin(2.0 * H * D2R) < 0.0;

    // Hour angle advances ~347.81°/day (= 14.4920°/h); extremes every 90° of H.
    const double RATE = 14.49205;   // deg/h
    double over = fmod(H, 90.0);
    double degTo = (over < 0.01) ? 90.0 : (90.0 - over);
    hoursToNext = (float)(degTo / RATE);
    // Next extreme is a HIGH if it lands on H = 0 or 180 (i.e. 2H = multiple of 360).
    double Hnext = fmod(H + degTo, 360.0);
    nextIsHigh = (fmod(Hnext, 180.0) < 0.5);
}

// levelNorm of scTideEstimate() on its own, for the tide curve, which needs
// nothing else at up to a few hundred points per redraw: one sin and three
// fmod fewer than the full estimate. The statements are those of
// scTideEstimate(), operation for operation, so the result is bit-identical -
// the curve has to be the same function as the readout above it. Keep them
// in step.
static inline float scTideLevel(long daysSince1970, double secOfDay, float lonDeg) {
    const double D2R = 0.0174532925;
    double d = ((double)daysSince1970 + secOfDay / 86400.0) - 10957.5;
    double lamMoon = fmod(218.316 + 13.176396 * d, 360.0); if (lamMoon < 0) lamMoon += 360.0;
    double lamSun  = fmod(280.460 +  0.9856474 * d, 360.0); if (lamSun  < 0) lamSun  += 360.0;
    const double eps = 23.439 * D2R;
    double raMoon = atan2(cos(eps) * sin(lamMoon * D2R), cos(lamMoon * D2R)) / D2R;
    raMoon = fmod(raMoon + 360.0, 360.0);
    double gmst = fmod(280.46061837 + 360.98564736629 * d, 360.0);
    double H = fmod(gmst + lonDeg - raMoon + 720.0, 360.0);
    double elong = fmod(lamMoon - lamSun + 360.0, 360.0);
    const float springFactor = (float)(1.0 + 0.46 * cos(2.0 * elong * D2R));
    return (float)(springFactor * cos(2.0 * H * D2R));
}
