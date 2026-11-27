// SpiGeom.cpp - geometry of the downwind sail. See SpiGeom.h for the axes,
// the construction and why this file is free of LVGL.
#include "SpiGeom.h"
#include <math.h>

namespace SpiGeom {

static constexpr float DEG2RAD = 3.14159265f / 180.f;

static inline Pt pt(float x, float y) {
    Pt p;
    p.x = x;
    p.y = y;
    return p;
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

Pt quarterBlock(float leeSide) { return pt(  9.6f * leeSide, 25.0f); }
Pt sheetWinch(float leeSide)   { return pt(  7.0f * leeSide, 14.7f); }
Pt twing(float leeSide)        { return pt(-10.6f * leeSide, -1.0f); }
Pt guyWinch(float leeSide)     { return pt( -7.0f * leeSide, 14.7f); }

// ── Symmetric spinnaker ──────────────────────────────────────────────────────

float symRotationDeg(float awaAbsDeg) {
    if (isnan(awaAbsDeg)) return 0.f;                 // NaN: square
    return clampf((SYM_ROT_ZERO_AWA - awaAbsDeg) * SYM_ROT_GAIN, 0.f, SYM_ROT_MAX);
}

void symOutline(float rotDeg, Pt foot[SYM_N + 1], Pt lead[SYM_N + 1]) {
    // Shape: eased linearly from "square" to "turned" with the rotation.
    const float k   = clampf(fabsf(rotDeg) / SYM_ROT_MAX, 0.f, 1.f);
    const float W   = SYM_W_SQUARE   + (SYM_W_TURNED   - SYM_W_SQUARE)   * k;
    const float f0  = SYM_F0_SQUARE  + (SYM_F0_TURNED  - SYM_F0_SQUARE)  * k;
    const float sag = SYM_SAG_SQUARE + (SYM_SAG_TURNED - SYM_SAG_SQUARE) * k;
    const float b   = SYM_B_SQUARE   + (SYM_B_TURNED   - SYM_B_SQUARE)   * k;
    // The cubic's crest is f0 + 0.75*h; h is chosen to put it at SYM_R_CREST.
    const float h   = (SYM_R_CREST - f0) / 0.75f;

    // Kite frame -> boat: u (athwartships) along (cos t, sin t), f (forward)
    // along (sin t, -cos t) - y is aft, so "forward" is negative y. t > 0
    // turns the crest towards starboard.
    const float t   = rotDeg * DEG2RAD;
    const float eux = cosf(t), euy = sinf(t);
    const float efx = sinf(t), efy = -cosf(t);

    for (int i = 0; i <= SYM_N; i++) {
        const float s = (float)i / (float)SYM_N, r = 1.f - s;
        // foot: (-W, f0) -> (0, f0 + 2*sag) -> (W, f0)
        const float fu = r * r * (-W) + s * s * W;
        const float ff = r * r * f0 + 2.f * r * s * (f0 + 2.f * sag) + s * s * f0;
        // leading edge: (-W, f0) (-W-b, f0+h) (W+b, f0+h) (W, f0)
        const float a0 = r * r * r, a1 = 3.f * r * r * s;
        const float a2 = 3.f * r * s * s, a3 = s * s * s;
        const float lu = a0 * (-W) + a1 * (-W - b) + a2 * (W + b) + a3 * W;
        const float lf = (a0 + a3) * f0 + (a1 + a2) * (f0 + h);
        foot[i] = pt(fu * eux + ff * efx, fu * euy + ff * efy);
        lead[i] = pt(lu * eux + lf * efx, lu * euy + lf * efy);
    }
}

float balloonBandEdge(int j) {
    if (j <= 0) return 0.f;
    if (j >= BALLOON_BANDS) return 1.f;
    return powf((float)j / (float)BALLOON_BANDS, 0.8f);
}

// ── Gennaker ─────────────────────────────────────────────────────────────────

int genSeamEnd(int s) {
    return (s >= 0 && s < GEN_SEAMS) ? GEN_SEAM_END[s] : GEN_SEAM_END[0];
}

float genSheetDeg(float awaAbsDeg) {
    if (isnan(awaAbsDeg)) return GEN_SHEET_MIN;       // NaN
    return clampf(GEN_SHEET_AT_90 + (awaAbsDeg - 90.f) * GEN_SHEET_GAIN,
                  GEN_SHEET_MIN, GEN_SHEET_MAX);
}

bool genOutline(float sheetDeg, GenOutline &o) {
    // Length from the angle, held to the range the sheeting law produces: in
    // a gybe the smoothed angle swings through 0 (clew dead aft), and the
    // kite must not grow to half the boat on its way across.
    const float aLen = clampf(fabsf(sheetDeg), GEN_SHEET_MIN, GEN_SHEET_MAX);
    const float len  = GEN_LEN_EASED + (GEN_SHEET_MAX - aLen) * GEN_LEN_GAIN;
    const float a    = sheetDeg * DEG2RAD;
    const float tX = 0.f, tY = GEN_TACK_Y;
    const float cX = tX + len * sinf(a), cY = tY + len * cosf(a);   // clew

    const float dx = cX - tX, dy = cY - tY;
    const float d  = sqrtf(dx * dx + dy * dy);
    if (d < 1.f) return false;
    // Chord normal pointing to leeward: its x-sign is the sheeting side.
    float nx = -dy / d, ny = dx / d;
    if ((nx < 0.f) != (sheetDeg < 0.f)) { nx = -nx; ny = -ny; }
    const float k1x = tX + dx * GEN_LEE_POS  + nx * GEN_LEE_DEPTH  * d;
    const float k1y = tY + dy * GEN_LEE_POS  + ny * GEN_LEE_DEPTH  * d;
    const float k2x = tX + dx * GEN_LUFF_POS + nx * GEN_LUFF_DEPTH * d;
    const float k2y = tY + dy * GEN_LUFF_POS + ny * GEN_LUFF_DEPTH * d;

    for (int i = 0; i <= GEN_N; i++) {
        const float s = (float)i / (float)GEN_N, r = 1.f - s;
        o.lee[i]  = pt(r * r * tX + 2.f * r * s * k1x + s * s * cX,
                       r * r * tY + 2.f * r * s * k1y + s * s * cY);
        o.luff[i] = pt(r * r * tX + 2.f * r * s * k2x + s * s * cX,
                       r * r * tY + 2.f * r * s * k2y + s * s * cY);
        o.panel[i] = lerp(o.luff[i], o.lee[i], GEN_PANEL);
    }
    return true;
}

}  // namespace SpiGeom
