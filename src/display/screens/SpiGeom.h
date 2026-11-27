#pragma once
// ============================================================================
// SpiGeom - geometry of the downwind sail on the Wind & Trim screen: the
// symmetric spinnaker (drawn as panels or as a shaded balloon) and the
// gennaker. Which one is drawn, if any, is the user's choice (config key
// "sail.spi_type", WebUI: Display tab, screen list, Wind & Trim);
// WindScreen does the pixel work.
//
// Pure maths on purpose: no LVGL, no Arduino, float only, C++11. Two reasons.
// The constants below were tuned against a host-side checker that links this
// very file and replicates the mainsail, the hull and the heading box of
// WindScreen - so what was checked is what runs. And the same picture is to
// be ported to NauticPi; a port that copies this file line by line, and
// compares its output with the test vectors the checker prints, cannot drift
// from the original the way a re-drawn copy would.
//
// Units and axes - the ones drawBoat() uses:
//   boat units (1 = UI_WIND_BOAT_SCALE px, 3.8 px on the 4" grid, 4.75 on the
//   1024x600 boards), origin = boat centre, x = athwartships (+ = starboard),
//   y = fore-and-aft in SCREEN direction (+ = aft, the bow is at y = -32, the
//   mast at y = -6). A point (x, y) is drawn at (CX + x*BS, CY + y*BS).
//   Angles in degrees. "Signed" means + = towards starboard.
// ============================================================================
#include <stdint.h>

namespace SpiGeom {

struct Pt {
    float x;
    float y;
};

inline Pt lerp(Pt a, Pt b, float t) {
    Pt p;
    p.x = a.x + (b.x - a.x) * t;
    p.y = a.y + (b.y - a.y) * t;
    return p;
}

// ── Rig points (drawBoat's coordinates) ──────────────────────────────────────
constexpr float MAST_Y = -6.f;            // the mast stands at (0, MAST_Y)

// Sheet: clew -> quarter block -> winch, on the lee side. Guy (symmetric kite
// only): tack -> twing at the widest beam -> winch, on the windward side.
// leeSide = +1 lee to starboard, -1 lee to port (SailState::leewardSide).
Pt quarterBlock(float leeSide);           // ( 9.6 * lee, 25.0)
Pt sheetWinch(float leeSide);             // ( 7.0 * lee, 14.7)
Pt twing(float leeSide);                  // (-10.6 * lee, -1.0)
Pt guyWinch(float leeSide);               // (-7.0 * lee, 14.7)

// ── Symmetric spinnaker (config "gores" and "balloon") ───────────────────────
// A crescent ahead of the mast, seen from above. Built in its own frame (u =
// athwartships, f = forward), then turned about the BOAT CENTRE by the trim
// rotation, so it swings round the bow like a kite going from a dead run to a
// broad reach.
//   foot          quadratic: (-W, f0) -> control (0, f0 + 2*sag) -> (W, f0);
//                 its middle stands sag ahead of the clews
//   leading edge  cubic: (-W, f0), (-W-b, f0+h), (W+b, f0+h), (W, f0); the
//                 outer control points stand outboard of the clews, which
//                 gives the kite its shoulders; its crest is at f0 + 0.75*h
// The crest radius is held at SYM_R_CREST for every shape (h follows from f0),
// and turning about the boat centre does not change a radius - so the kite
// never grows towards the heading box, whatever its trim.
//
// The shape eases with the rotation: at rotation 0 ("square", dead run) it is
// the wide, low kite with the pole well aft; at SYM_ROT_MAX ("turned") it is
// narrower, its clews higher. Linear in k = |rotation| / SYM_ROT_MAX.
constexpr int   SYM_N           = 24;     // segments along foot and leading edge
constexpr float SYM_R_CREST     = 32.45f; // crest of the leading edge from the boat centre
constexpr float SYM_W_SQUARE    = 19.f;   // half-width clew to clew
constexpr float SYM_W_TURNED    = 17.f;
constexpr float SYM_F0_SQUARE   = 12.f;   // clews ahead of the boat centre
constexpr float SYM_F0_TURNED   = 15.f;
constexpr float SYM_SAG_SQUARE  = 8.f;    // foot: middle ahead of the clews
constexpr float SYM_SAG_TURNED  = 6.5f;
constexpr float SYM_B_SQUARE    = 10.f;   // shoulders: control points outboard of the clews
constexpr float SYM_B_TURNED    = 12.f;

// Trim rotation from |AWA|: none at and near a dead run (above
// SYM_ROT_ZERO_AWA), then SYM_ROT_GAIN degrees per degree of apparent wind
// angle, up to SYM_ROT_MAX (reached at |AWA| 116). The rotation does not grow
// further on a reach: the mainsail is eased to its limit from |AWA| 99 on and
// bellies forward, and a kite turned any further would put its leeward clew
// on top of it.
constexpr float SYM_ROT_ZERO_AWA = 172.f;
constexpr float SYM_ROT_GAIN     = 0.25f;
constexpr float SYM_ROT_MAX      = 14.f;

// Rotation target, unsigned degrees. NaN in -> 0 (square).
float symRotationDeg(float awaAbsDeg);

// Outline for a SIGNED rotation (+ = turned towards starboard, i.e. leeward
// when the lee is starboard). foot[] and lead[] each get SYM_N + 1 points,
// index 0 = the port end, SYM_N = the starboard end; foot[0] == lead[0] and
// foot[SYM_N] == lead[SYM_N] are the clews.
void symOutline(float rotDeg, Pt foot[SYM_N + 1], Pt lead[SYM_N + 1]);

// The tack is the WINDWARD clew - where the pole and the guy are made fast.
// Lee to starboard (+1) -> the port end, index 0.
inline int symTackIndex(float leeSide)  { return leeSide > 0.f ? 0 : SYM_N; }
inline int symClewIndex(float leeSide)  { return SYM_N - symTackIndex(leeSide); }

// Panels ("gores"): SYM_GORES vertical strips, boundaries at every
// SYM_N / SYM_GORES-th sample, alternating in two tones.
constexpr int SYM_GORES = 6;

// Balloon: BALLOON_BANDS bands between the foot (0) and the leading edge (1).
// Edge j of a band (j = 0..BALLOON_BANDS) lies at (j / BALLOON_BANDS)^0.8 of
// the way, so the bands narrow towards the leading edge - the pale foot reads
// as the sail's inside, the strong orange edge as its curved front.
// Band boundary point i: lerp(foot[i], lead[i], balloonBandEdge(j)).
constexpr int BALLOON_BANDS = 5;
float balloonBandEdge(int j);
// Two seams across the balloon, foot[i] -> lead[i] at these sample indices.
constexpr int BALLOON_SEAM_A = 8;
constexpr int BALLOON_SEAM_B = 16;

// The pixel layer sizes its arrays from these and walks them without range
// checks, so a retune that breaks one of these must not compile.
static_assert(SYM_N % SYM_GORES == 0,
              "every panel must span the same whole number of samples");
static_assert(0 < BALLOON_SEAM_A && BALLOON_SEAM_A < BALLOON_SEAM_B &&
              BALLOON_SEAM_B < SYM_N, "balloon seams must lie inside the outline");
static_assert(BALLOON_BANDS >= 2,
              "the band colour ramp divides by BALLOON_BANDS - 1");

// ── Gennaker (config "gennaker") ─────────────────────────────────────────────
// Asymmetric kite: tack on the centreline at the tip of the stem, clew out to
// leeward and aft. Two quadratic edges between tack and clew:
//   leeward edge   control at GEN_LEE_POS of the chord, GEN_LEE_DEPTH * chord
//                  out to leeward - a deep foil, nearly twice the jib's draft
//   luff           control at GEN_LUFF_POS, GEN_LUFF_DEPTH out to leeward
//   panel line     GEN_PANEL of the way from the luff to the leeward edge;
//                  luff -> panel line is drawn in the second tone
// The sheeting angle is measured from the AFT centreline: 0 = clew dead aft,
// 90 = clew square abeam of the tack.
//
// GEN_TACK_Y: the stem tip, ahead of the jib's and Code Zero's tack (-30), so
// no hull shows ahead of the gennaker. The hull's tip station is y = -32;
// drawBoat() truncates it to whole pixels (y = -32.105 on both grids) and
// draws a 2-px outline through it, whose front pixel row is one row further
// forward. -32.25 puts the tack in that front row on both grids (4": tack
// y 103.45 px, row 103; 1024x600: 129.31 px, row 129), just ahead of the
// row's pixel centres, so no hull pixel lies ahead of the tack and the kite's
// edges start on the tip of the outline. Allowed range for that: -32.316 ..
// -32.237 (front row of the 1024x600 grid .. centre of the 4" one).
//
// GEN_LEE_DEPTH: the tack at the stem tip stands 32.25 units from the boat
// centre, 0.38 units (1.45 px) inside the 4" radius budget (see the chord
// below). The leeward edge leaves the tack towards its control point, i.e.
// along GEN_LEE_POS * chord + GEN_LEE_DEPTH * chord * normal; it heads
// forward of square, and straight out of the budget, as soon as
// GEN_LEE_DEPTH > GEN_LEE_POS / tan(sheeting angle). At the eased limit
// (GEN_SHEET_MAX 56) that bound is 0.55 / tan 56 = 0.371. The first
// tuning's 0.50 would need the chord cut to 13 units at 56 deg; 0.37 keeps
// the edge square to the centreline there and the chord law unchanged.
constexpr int   GEN_N           = 16;
constexpr float GEN_TACK_Y      = -32.25f;
constexpr float GEN_LEE_POS     = 0.55f;
constexpr float GEN_LEE_DEPTH   = 0.37f;
constexpr float GEN_LUFF_POS    = 0.45f;
constexpr float GEN_LUFF_DEPTH  = 0.15f;
constexpr float GEN_PANEL       = 0.33f;

// Sheeting angle from |AWA|: GEN_SHEET_AT_90 at |AWA| 90, eased by
// GEN_SHEET_GAIN per degree as the wind goes aft, limited to
// GEN_SHEET_MIN..GEN_SHEET_MAX. Never sheeted closer than 44 deg: from |AWA|
// 99 on the mainsail stands at its 86 deg limit and bellies forward, and a
// clew any further aft would lie on it.
constexpr float GEN_SHEET_AT_90 = 47.f;
constexpr float GEN_SHEET_GAIN  = 0.20f;
constexpr float GEN_SHEET_MIN   = 44.f;
constexpr float GEN_SHEET_MAX   = 56.f;
// Chord length tack -> clew: GEN_LEN_EASED when eased to GEN_SHEET_MAX,
// GEN_LEN_GAIN longer per degree sheeted closer - there is more room to
// leeward further aft. An eased gennaker with the full 32.5-unit chord would
// reach 32.8 units from the boat centre about 29 deg off the bow, which
// breaks the radius budget every downwind sail is held to: the bottom of the
// heading box (33.2 units) minus 1 px, about 125 px on the 4" board (the
// same budget as the symmetric kite's SYM_R_CREST). With this law the
// outline reaches 32.47 units (124.4 px on the 4" board, half the edge line
// included), at the eased limit about 20 deg off the bow; sheeted closer
// than about 54.65 deg, nothing lies further out than the tack. The angle is
// clamped to GEN_SHEET_MIN..GEN_SHEET_MAX for this, so the chord is
// 29.5..32.5 units whatever angle it is handed.
constexpr float GEN_LEN_EASED   = 29.5f;
constexpr float GEN_LEN_GAIN    = 0.25f;

// Seams: from panel[GEN_SEAM_END[s] - GEN_SEAM_BACK] to lee[GEN_SEAM_END[s]].
constexpr int GEN_SEAMS     = 3;
constexpr int GEN_SEAM_BACK = 3;
constexpr int GEN_SEAM_END[GEN_SEAMS] = { 5, 9, 13 };
int genSeamEnd(int s);                    // GEN_SEAM_END[s]
static_assert(GEN_SEAM_END[0] - GEN_SEAM_BACK >= 0 &&
              GEN_SEAM_END[GEN_SEAMS - 1] <= GEN_N,
              "gennaker seams must start and end on the outline");

// Sheeting target, unsigned degrees. NaN in -> GEN_SHEET_MIN.
float genSheetDeg(float awaAbsDeg);

struct GenOutline {
    Pt lee[GEN_N + 1];      // leeward edge, tack (0) -> clew (GEN_N)
    Pt luff[GEN_N + 1];     // luff, tack -> clew
    Pt panel[GEN_N + 1];    // luff panel line, tack -> clew
};
// Outline for a SIGNED sheeting angle (+ = clew to starboard). false (and o
// untouched) only for a degenerate chord, which the constants above cannot
// produce.
bool genOutline(float sheetDeg, GenOutline &o);

}  // namespace SpiGeom
