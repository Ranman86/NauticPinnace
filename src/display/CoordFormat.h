#pragma once
// ============================================================================
// CoordFormat - how a latitude/longitude value is written into a data cell.
//
// The data grids and the sidebar treat every field as "a float with N
// decimals". For coordinates that gives 48.12345, which nobody reads off a
// chart: plotters and paper write degrees and minutes. This is the one place
// that knows the three notations, shared by GridScreen and SideBar so the
// two can never disagree. Requested in GitHub issue #3.
//
// The maths runs on whole units of the LAST component (tenths of a minute,
// hundredths of a second...) so rounding can never produce 07.9995' -> "08.000"
// beside a stale degree figure or 60.0" beside a stale minute figure: the
// value is rounded ONCE, as an integer, and only then split into fields.
//
// Minutes and seconds are zero-padded to two digits so a cell does not change
// width as the boat moves; degrees are not, to keep longitude short.
// ============================================================================
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../i18n/I18n.h"   // T(STR_HEMI_*): east is "O" in German

// 0 = decimal degrees (callers keep their ordinary %.Nf path for that),
// 1 = degrees + decimal minutes, 2 = degrees, minutes, seconds.
enum : uint8_t { COORD_DECIMAL = 0, COORD_DEG_MIN = 1, COORD_DEG_MIN_SEC = 2 };

// The two DataModel keys that carry a geographic coordinate.
inline bool isCoordKey(const char *key) {
    return key && (strcmp(key, "lat") == 0 || strcmp(key, "lon") == 0);
}

// Widest string the notation can produce for the given decimals - used to
// pick a value font that fits the cell BEFORE the first real value arrives.
inline void coordWidestSample(char *buf, size_t sz, uint8_t fmt, int dec);

// dec = digits of the last component. DataModel keeps lat/lon as float: seven
// significant digits, i.e. ~1e-5 deg, which is 0.0006' or 0.04". A third
// decimal of a second would be noise, so DMS is capped at two.
inline void fmtCoord(char *buf, size_t sz, float val, bool isLat,
                     uint8_t fmt, int dec) {
    if (isnan(val)) { snprintf(buf, sz, "--"); return; }
    if (dec < 0) dec = 0;
    if (dec > 3) dec = 3;
    if (fmt == COORD_DEG_MIN_SEC && dec > 2) dec = 2;

    // Language-dependent: a German chart writes east as "O", not "E".
    const char *hemi = isLat ? T(val < 0.f ? STR_HEMI_S : STR_HEMI_N)
                             : T(val < 0.f ? STR_HEMI_W : STR_HEMI_E);
    const double a = fabs((double)val);
    uint32_t pow10 = 1;
    for (int i = 0; i < dec; i++) pow10 *= 10;

    if (fmt == COORD_DEG_MIN_SEC) {
        // Whole units = hundredths (etc.) of a second. 180 deg * 3600 * 1000
        // = 6.5e8 still fits comfortably in 32 bits.
        const uint32_t perMin = 60u * pow10, perDeg = 60u * perMin;
        const uint32_t u   = (uint32_t)llround(a * (double)perDeg);
        const uint32_t deg = u / perDeg, rem = u % perDeg;
        const uint32_t min = rem / perMin, secU = rem % perMin;
        const uint32_t sec = secU / pow10, frac = secU % pow10;
        if (dec)
            snprintf(buf, sz, "%lu\xC2\xB0%02lu'%02lu.%0*lu\"%s",
                     (unsigned long)deg, (unsigned long)min, (unsigned long)sec,
                     dec, (unsigned long)frac, hemi);
        else
            snprintf(buf, sz, "%lu\xC2\xB0%02lu'%02lu\"%s",
                     (unsigned long)deg, (unsigned long)min, (unsigned long)sec, hemi);
    } else {
        // Whole units = thousandths (etc.) of a minute.
        const uint32_t perDeg = 60u * pow10;
        const uint32_t u   = (uint32_t)llround(a * (double)perDeg);
        const uint32_t deg = u / perDeg, minU = u % perDeg;
        const uint32_t min = minU / pow10, frac = minU % pow10;
        if (dec)
            snprintf(buf, sz, "%lu\xC2\xB0%02lu.%0*lu'%s",
                     (unsigned long)deg, (unsigned long)min, dec,
                     (unsigned long)frac, hemi);
        else
            snprintf(buf, sz, "%lu\xC2\xB0%02lu'%s",
                     (unsigned long)deg, (unsigned long)min, hemi);
    }
}

inline void coordWidestSample(char *buf, size_t sz, uint8_t fmt, int dec) {
    // NOT a formatted value. Montserrat's digits are proportional - at 40 px
    // '0' and '4' are 27 px wide, '9' 25, '7' 24, '1' 15 - so 179°59'59.95"W,
    // the numerically largest longitude, is built from the NARROW digits and
    // measured up to 25 px too short at the sidebar's font size: the fit test
    // then kept a font whose real values overran the card. The widest 3-digit
    // degree figure under 180 and an all-zero remainder is within ~2 % of the
    // true maximum (which depends on kerning pairs and the font size); the
    // callers add a margin for that last bit. "W" is the widest hemisphere
    // glyph in either language.
    if (dec < 0) dec = 0;
    if (dec > 3) dec = 3;
    if (fmt == COORD_DEG_MIN_SEC && dec > 2) dec = 2;
    const char *frac = (dec == 3) ? ".000" : (dec == 2) ? ".00" : (dec == 1) ? ".0" : "";
    if (fmt == COORD_DEG_MIN_SEC)
        snprintf(buf, sz, "100\xC2\xB0" "00'00%s\"%s", frac, T(STR_HEMI_W));
    else
        snprintf(buf, sz, "100\xC2\xB0" "00%s'%s", frac, T(STR_HEMI_W));
}

// Width to fit the sample into: the cell's content width less ~3 %, which
// covers the kerning-dependent remainder coordWidestSample() cannot know.
inline int coordFitWidth(int cellW, int cardPad) {
    const int avail = cellW - 2 * cardPad;
    return avail - avail / 32;
}
