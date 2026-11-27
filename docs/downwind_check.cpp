// downwind_check.cpp - host-side geometry checker for the downwind sail on the
// Wind & Trim screen, and the generator of its test vectors. See
// WIND-AND-SAILS.md in this folder, sections 5.6 and 5.7 (how to run: 7.3).
//
// Links the REAL src/display/screens/SpiGeom.cpp - the file the firmware
// runs - and replicates what WindScreen draws around it: the mainsail foil
// (boom law, camberForTwa(), drawSailFoil() with its lv_coord_t truncation),
// the hull (truncated stations, 2-px outline through linePx), the heading
// box, the reef badge, and the cdFillPolyOpa() / cdLineAA() rasterisation
// (src/display/CanvasDraw.cpp; WindScreen's fillPolygon() and linePx() are
// inline forwarders to them). Keep those replicas in step with WindScreen.cpp
// and CanvasDraw.cpp when either changes.
//
// Build and run from the repository root (any C++11 compiler; no LVGL), as
// one command line each:
//   g++ -std=gnu++11 -O2 -I src/display/screens docs/downwind_check.cpp
//       src/display/screens/SpiGeom.cpp -o downwind_check
//   ./downwind_check docs/downwind_vectors.txt
// It prints the report on stdout and writes the test vectors to the file.
//
// Targets, checked over |TWA| 90..180 (step 1, both sides), TWS 6/10/14/20,
// STW 3/5/7, in the steady state (smoothed trim = target), wherever the kite
// is shown:
//  (a) kite (outline + half its 2-px leading-edge line) never overlaps the
//      main foil polygon: clearance >= 2 px (7B/5B) / 1.5 px (4")
//  (b) pole angle from the bow at |TWA| 178 in 68..80, non-decreasing in |AWA|
//  (c) max radius + 1 px <= heading-box bottom - 1 px (for the gennaker also
//      over every sheeting angle 44..56, not only the grid's)
//  (d) |AWA| >= 170: bow tip covered; elsewhere covered or >= 2 px outside
//  (e) gennaker tack at the stem tip: no hull-outline pixel ahead of the
//      tack, and the tack in the outline's front pixel row
//  plus the reef badge in the cockpit against everything drawn there.
#include "SpiGeom.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>

using SpiGeom::Pt;

static const float DEG2RAD = 3.14159265f / 180.f;

struct Board {
    const char *name;
    float SF, CX, CY, BS, RIN, CLR;
    float boxBottomR;   // R_INNER - 1 - 21*SF (px from centre)
    int   boxX0, boxX1, boxLastRow;
};

static Board makeBoard(const char *n, float sf) {
    Board b;
    b.name = n; b.SF = sf;
    b.CX = 240.f * sf; b.CY = 226.f * sf; b.BS = 3.8f * sf; b.RIN = 148.f * sf;
    b.CLR = sf > 1.f ? 2.0f : 1.5f;
    b.boxBottomR = b.RIN - 1.f - 21.f * sf;
    const float w = 48.f * sf, h = 21.f * sf;
    const float bx = b.CX - w * 0.5f, by = b.CY - b.RIN + 1.f;
    b.boxX0 = (int)(short)bx; b.boxX1 = (int)(short)bx + (int)(short)w - 1;
    b.boxLastRow = (int)(short)by + (int)(short)h - 1;
    return b;
}

static inline short trunc16(float v) { return (short)v; }   // lv_coord_t cast

struct IPt { short x, y; };

// ---------------------------------------------------------------- rasteriser
static const int MW = 1100, MH = 700;
struct Mask {
    std::vector<float> v;
    int x0, y0, x1, y1;   // dirty box
    Mask() : v((size_t)MW * MH, 0.f), x0(MW), y0(MH), x1(-1), y1(-1) {}
    void clear() {
        if (x1 < 0) return;
        for (int y = y0; y <= y1; y++)
            memset(&v[(size_t)y * MW + x0], 0, sizeof(float) * (size_t)(x1 - x0 + 1));
        x0 = MW; y0 = MH; x1 = -1; y1 = -1;
    }
    void put(int x, int y, float f) {
        if (x < 0 || y < 0 || x >= MW || y >= MH || f <= 0.f) return;
        if (f > 1.f) f = 1.f;
        float &c = v[(size_t)y * MW + x];
        // coverage union (like painting the same colour twice)
        c = 1.f - (1.f - c) * (1.f - f);
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    float at(int x, int y) const {
        if (x < 0 || y < 0 || x >= MW || y >= MH) return 0.f;
        return v[(size_t)y * MW + x];
    }
};

// cdFillPolyOpa (CanvasDraw.cpp), reached through WindScreen::fillPolygon
static void fillPolygon(Mask &m, const IPt *pts, int n) {
    if (n < 3) return;
    int ymin = pts[0].y, ymax = pts[0].y;
    for (int i = 1; i < n; i++) { if (pts[i].y < ymin) ymin = pts[i].y; if (pts[i].y > ymax) ymax = pts[i].y; }
    if (ymin < 0) ymin = 0;
    if (ymax >= MH) ymax = MH - 1;
    for (int y = ymin; y <= ymax; y++) {
        float xs[24]; int c = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            float y0 = pts[i].y, y1 = pts[j].y;
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                float t = (float)(y - y0) / (y1 - y0);
                if (c < 24) xs[c++] = pts[i].x + t * (pts[j].x - pts[i].x);
            }
        }
        std::sort(xs, xs + c);
        for (int i = 0; i + 1 < c; i += 2) {
            int a = (int)(xs[i] + 0.5f), b = (int)(xs[i + 1] + 0.5f);
            if (b < a) std::swap(a, b);
            for (int x = a; x <= b; x++) m.put(x, y, 1.f);
        }
    }
}

// cdLineAA (CanvasDraw.cpp), reached through WindScreen::linePx (coverage only)
static void linePx(Mask &m, float x0, float y0, float x1, float y1, float w) {
    if (w > 1.5f) {
        const float dxl = x1 - x0, dyl = y1 - y0;
        const float len2 = dxl * dxl + dyl * dyl;
        if (len2 < 1e-6f) return;
        const float len = sqrtf(len2), h = w * 0.5f, r = h + 1.f;
        int ya = (int)floorf(fminf(y0, y1) - r), yb = (int)ceilf(fmaxf(y0, y1) + r);
        if (ya < 0) ya = 0;
        if (yb > MH - 1) yb = MH - 1;
        const float xlo = fminf(x0, x1) - r, xhi = fmaxf(x0, x1) + r;
        const float A = -dyl, B = dxl, C = -(A * x0 + B * y0);
        for (int y = ya; y <= yb; y++) {
            const float py = (float)y + 0.5f;
            float xa = xlo, xb = xhi;
            if (fabsf(A) > 1e-4f) {
                const float k1 = (-r * len - B * py - C) / A, k2 = (r * len - B * py - C) / A;
                const float lo = fminf(k1, k2), hi = fmaxf(k1, k2);
                if (lo > xa) xa = lo;
                if (hi < xb) xb = hi;
            }
            int ix0 = (int)floorf(xa), ix1 = (int)ceilf(xb);
            for (int x = ix0; x <= ix1; x++) {
                const float px = (float)x + 0.5f;
                float t = ((px - x0) * dxl + (py - y0) * dyl) / len2;
                if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
                const float cx = x0 + t * dxl, cy = y0 + t * dyl;
                const float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
                float f = h + 0.5f - d;
                if (f <= 0.f) continue;
                m.put(x, y, f);
            }
        }
        return;
    }
    const bool steil = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steil) { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }
    const float dx = x1 - x0, dy = y1 - y0;
    const float grad = (dx == 0.f) ? 1.f : dy / dx;
    float y = y0 + grad * (roundf(x0) - x0);
    const int xEnd = (int)roundf(x1);
    for (int x = (int)roundf(x0); x <= xEnd; x++, y += grad) {
        const int iy = (int)floorf(y);
        const float fr = y - (float)iy;
        if (steil) { m.put(iy, x, 1.f - fr); m.put(iy + 1, x, fr); }
        else       { m.put(x, iy, 1.f - fr); m.put(x, iy + 1, fr); }
    }
}

// ---------------------------------------------------------------- geometry
struct F2 { float x, y; };
static float segDist(F2 p, F2 a, F2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y, L2 = dx * dx + dy * dy;
    float t = L2 > 0.f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / L2 : 0.f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    const float ex = p.x - a.x - t * dx, ey = p.y - a.y - t * dy;
    return sqrtf(ex * ex + ey * ey);
}
static bool inPoly(F2 p, const std::vector<F2> &P) {
    bool c = false;
    for (size_t i = 0, n = P.size(); i < n; i++) {
        const F2 a = P[i], b = P[(i + 1) % n];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) c = !c;
    }
    return c;
}
static float distToBoundary(F2 p, const std::vector<F2> &P) {
    float d = 1e9f;
    for (size_t i = 0, n = P.size(); i < n; i++) d = std::min(d, segDist(p, P[i], P[(i + 1) % n]));
    return d;
}
static std::vector<F2> densify(const std::vector<F2> &P, float step) {
    std::vector<F2> o;
    for (size_t i = 0, n = P.size(); i < n; i++) {
        const F2 a = P[i], b = P[(i + 1) % n];
        const float L = hypotf(b.x - a.x, b.y - a.y);
        const int k = std::max(1, (int)ceilf(L / step));
        for (int j = 0; j < k; j++) { F2 q = { a.x + (b.x - a.x) * j / k, a.y + (b.y - a.y) * j / k }; o.push_back(q); }
    }
    return o;
}
// signed clearance between polygons: >0 gap, <0 penetration depth
static float polyClearance(const std::vector<F2> &A, const std::vector<F2> &B) {
    const std::vector<F2> dA = densify(A, 0.25f), dB = densify(B, 0.25f);
    float pen = 0.f; bool overlap = false;
    for (const F2 &p : dA) if (inPoly(p, B)) { overlap = true; pen = std::max(pen, distToBoundary(p, B)); }
    for (const F2 &p : dB) if (inPoly(p, A)) { overlap = true; pen = std::max(pen, distToBoundary(p, A)); }
    if (overlap) return -std::max(pen, 0.01f);
    float d = 1e9f;
    for (const F2 &p : dA) d = std::min(d, distToBoundary(p, B));
    return d;
}

// ---------------------------------------------------------------- WindScreen replica
static float camberForTwa(float absTwa) {
    if (absTwa < 35.f) return 0.10f;
    if (absTwa < 60.f) return 0.15f;
    if (absTwa < 100.f) return 0.19f;
    if (absTwa < 150.f) return 0.24f;
    return 0.28f;
}
static float awaFrom(float twaDeg, float tws, float stw) {
    const float t = twaDeg * DEG2RAD;
    return atan2f(tws * sinf(t), tws * cosf(t) + stw) / DEG2RAD;
}
// main foil polygon as drawSailFoil fills it (px, truncated), + boom end
static void mainFoil(const Board &bd, float absAwa, float absTwa, float ss, IPt pts[11],
                     float &mastX, float &mastY, float &clewX, float &clewY) {
    float boom = absAwa - 13.f;
    if (boom < 4.f) boom = 4.f;
    if (boom > 86.f) boom = 86.f;
    const float sm = ss * boom;                   // steady state: smoothed = target
    const float bss = (sm >= 0.f) ? 1.f : -1.f;
    const float bAng = fabsf(sm) * DEG2RAD;
    const float BS = bd.BS;
    mastX = bd.CX; mastY = bd.CY - 6.f * BS;
    clewX = mastX + bss * 32.f * BS * sinf(bAng);
    clewY = mastY + 32.f * BS * cosf(bAng);
    const float lx = clewX - mastX, ly = clewY - mastY, len = sqrtf(lx * lx + ly * ly);
    float nx = -ly / len, ny = lx / len;
    if ((nx < 0.f) != (bss < 0.f)) { nx = -nx; ny = -ny; }
    const float belly = camberForTwa(absTwa) * len;
    const float kx = mastX + lx * 0.45f + nx * belly, ky = mastY + ly * 0.45f + ny * belly;
    pts[0].x = trunc16(mastX); pts[0].y = trunc16(mastY);
    for (int i = 1; i <= 10; i++) {
        const float t = (float)i / 10.f, u = 1.f - t;
        pts[i].x = trunc16(u * u * mastX + 2.f * u * t * kx + t * t * clewX);
        pts[i].y = trunc16(u * u * mastY + 2.f * u * t * ky + t * t * clewY);
    }
}
static const float ST[18][2] = {
    { -32.0f, 0.07f}, { -28.2f, 2.43f}, { -24.5f, 4.36f}, { -20.7f, 6.06f}, { -16.9f, 7.47f},
    { -13.2f, 8.65f}, {  -9.4f, 9.53f}, {  -5.6f,10.13f}, {  -1.9f,10.57f}, {   1.9f,10.88f},
    {   5.6f,11.08f}, {   9.4f,11.19f}, {  13.2f,11.12f}, {  16.9f,10.90f}, {  20.7f,10.53f},
    {  24.5f,10.06f}, {  28.2f, 9.35f}, {  32.0f, 4.82f} };
static void hullPts(const Board &bd, IPt hp[36]) {
    for (int i = 0; i < 18; i++) { hp[i].x = trunc16(bd.CX + ST[i][1] * bd.BS); hp[i].y = trunc16(bd.CY + ST[i][0] * bd.BS); }
    for (int i = 0; i < 18; i++) { hp[18 + i].x = trunc16(bd.CX - ST[17 - i][1] * bd.BS); hp[18 + i].y = trunc16(bd.CY + ST[17 - i][0] * bd.BS); }
}
static short rnd(float v) { return (short)floorf(v + 0.5f); }   // WindScreen spiRnd

// ---------------------------------------------------------------- kite in px
struct KitePx {
    std::vector<F2> poly;            // closed outline (float px)
    std::vector<F2> lead;            // leading / leeward edge (2-px line)
    std::vector<F2> foot;            // foot / luff (1-px line)
    F2 tack;
};
static F2 toPx(const Board &bd, Pt p) { F2 q = { bd.CX + p.x * bd.BS, bd.CY + p.y * bd.BS }; return q; }

static KitePx symKite(const Board &bd, float rotSigned, float ss) {
    Pt foot[SpiGeom::SYM_N + 1], lead[SpiGeom::SYM_N + 1];
    SpiGeom::symOutline(rotSigned, foot, lead);
    KitePx k;
    for (int i = 0; i <= SpiGeom::SYM_N; i++) k.lead.push_back(toPx(bd, lead[i]));
    for (int i = 0; i <= SpiGeom::SYM_N; i++) k.foot.push_back(toPx(bd, foot[i]));
    k.poly = k.lead;
    for (int i = SpiGeom::SYM_N; i >= 0; i--) k.poly.push_back(k.foot[(size_t)i]);
    k.tack = k.foot[(size_t)SpiGeom::symTackIndex(ss)];
    return k;
}
static KitePx genKite(const Board &bd, float sheetSigned) {
    SpiGeom::GenOutline o;
    SpiGeom::genOutline(sheetSigned, o);
    KitePx k;
    for (int i = 0; i <= SpiGeom::GEN_N; i++) k.lead.push_back(toPx(bd, o.lee[i]));
    for (int i = 0; i <= SpiGeom::GEN_N; i++) k.foot.push_back(toPx(bd, o.luff[i]));
    k.poly = k.lead;
    for (int i = SpiGeom::GEN_N; i >= 0; i--) k.poly.push_back(k.foot[(size_t)i]);
    k.tack = k.lead[0];
    return k;
}
static void rasterKite(Mask &m, const KitePx &k) {
    std::vector<IPt> ip;
    for (const F2 &p : k.poly) { IPt q = { rnd(p.x), rnd(p.y) }; ip.push_back(q); }
    fillPolygon(m, ip.data(), (int)ip.size());
    for (size_t i = 0; i + 1 < k.lead.size(); i++) linePx(m, k.lead[i].x, k.lead[i].y, k.lead[i + 1].x, k.lead[i + 1].y, 2.f);
    for (size_t i = 0; i + 1 < k.foot.size(); i++) linePx(m, k.foot[i].x, k.foot[i].y, k.foot[i + 1].x, k.foot[i + 1].y, 1.f);
}

// ---------------------------------------------------------------- per-state result
struct State { int twa, tws, stw, side; float awa; float trim; };
struct Res {
    float clr, clrPix;      // geometric clearance (px, minus half line), pixel-overlap count
    float rmax;             // px incl. half line width
    int   topRow;           // first kite row inside the heading box columns
    float bowS;             // geometric: max signed distance bow polyline -> kite (px, + = outside)
    float bowResid;         // pixel: max residual hull coverage in the bow region
    int   bowPoke;          // pixel: hull pixels with residual >= 0.25
    float pole;             // deg
};

static Mask gK, gM, gH;

static Res evalState(const Board &bd, const State &s, bool gen) {
    Res r;
    const float ss = (float)s.side;            // +1 = lee starboard
    KitePx k = gen ? genKite(bd, s.trim) : symKite(bd, s.trim, ss);
    IPt mp[11]; float mx, my, cx, cy;
    mainFoil(bd, s.awa, (float)s.twa, ss, mp, mx, my, cx, cy);
    std::vector<F2> mpoly;
    for (int i = 0; i < 11; i++) { F2 q = { (float)mp[i].x, (float)mp[i].y }; mpoly.push_back(q); }
    r.clr = polyClearance(k.poly, mpoly) - 1.f;
    // pixels: kite vs main (fill + 2-px leech + boom spar)
    gK.clear(); gM.clear(); gH.clear();
    rasterKite(gK, k);
    fillPolygon(gM, mp, 11);
    for (int i = 0; i < 10; i++) linePx(gM, mp[i].x, mp[i].y, mp[i + 1].x, mp[i + 1].y, 2.f);
    linePx(gM, mx, my, cx, cy, 2.f);
    int ov = 0;
    for (int y = std::max(gK.y0, gM.y0); y <= std::min(gK.y1, gM.y1); y++)
        for (int x = std::max(gK.x0, gM.x0); x <= std::min(gK.x1, gM.x1); x++)
            if (gK.at(x, y) > 0.f && gM.at(x, y) > 0.f) ov++;
    r.clrPix = (float)ov;
    // radius
    r.rmax = 0.f;
    for (const F2 &p : densify(k.poly, 0.25f)) r.rmax = std::max(r.rmax, hypotf(p.x - bd.CX, p.y - bd.CY));
    r.rmax += 1.f;
    r.topRow = 99999;
    for (int y = gK.y0; y <= gK.y1 && r.topRow == 99999; y++)
        for (int x = bd.boxX0; x <= bd.boxX1; x++) if (gK.at(x, y) > 0.f) { r.topRow = y; break; }
    // bow
    IPt hp[36]; hullPts(bd, hp);
    const float bowLimitY = bd.CY - 24.5f * bd.BS;
    // bow polyline: stations 0..2 on both sides (indices 2,1,0 then 35,34,33)
    const int bowIdx[6] = { 2, 1, 0, 35, 34, 33 };
    r.bowS = -1e9f;
    for (int j = 0; j < 5; j++) {
        const IPt a = hp[bowIdx[j]], b = hp[bowIdx[j + 1]];
        for (int t = 0; t <= 40; t++) {
            F2 p = { a.x + (b.x - a.x) * t / 40.f, a.y + (b.y - a.y) * t / 40.f };
            float d = distToBoundary(p, k.poly);
            if (inPoly(p, k.poly)) d = -d;
            r.bowS = std::max(r.bowS, d);
        }
    }
    for (int i = 0; i < 36; i++) linePx(gH, hp[i].x, hp[i].y, hp[(i + 1) % 36].x, hp[(i + 1) % 36].y, 2.f);
    r.bowResid = 0.f; r.bowPoke = 0;
    for (int y = gH.y0; y <= gH.y1 && y <= (int)bowLimitY; y++)
        for (int x = gH.x0; x <= gH.x1; x++) {
            const float res = gH.at(x, y) * (1.f - gK.at(x, y));
            r.bowResid = std::max(r.bowResid, res);
            if (res >= 0.25f) r.bowPoke++;
        }
    // pole (boat units)
    const float tx = (k.tack.x - bd.CX) / bd.BS, ty = (k.tack.y - bd.CY) / bd.BS;
    r.pole = atan2f(fabsf(tx), SpiGeom::MAST_Y - ty) / DEG2RAD;
    return r;
}

static bool visibleSym(int twa, int tws) {
    return (twa > 100 && twa < 150 && tws < 25) || (twa >= 150 && tws < 20);
}
static bool visibleGen(int twa, int tws) {
    return (twa > 90 && twa < 150 && tws < 25) || (twa >= 150 && twa <= 160 && tws < 20);
}

static void runKind(const Board &bd, bool gen) {
    const int TWS[4] = { 6, 10, 14, 20 }, STW[3] = { 3, 5, 7 };
    Res worstClr = Res(); State sClr = State(); worstClr.clr = 1e9f;
    float worstR = 0.f; State sR = State(); int worstTop = 99999; State sTop = State();
    int overlapStates = 0; float maxOv = 0.f;
    float bowDeadWorst = -1e9f, bowDeadResid = 0.f; int bowDeadPoke = 0; State sBowDead = State();
    int bowTouch = 0, bowCovered = 0, bowOutside = 0; float bowOtherWorst = -1e9f; State sBowOther = State();
    float bowOtherResid = 0.f; int bowOtherPoke = 0;
    int nStates = 0;
    std::vector<std::pair<float, float> > pole178;
    printf("\n=== %s  %s ===\n", bd.name, gen ? "GENNAKER" : "SYMMETRIC (gores / balloon)");
    printf("    BS %.2f px/unit; clearance target %.1f px; radius limit %.2f px (box bottom %.2f - 1 px; incl. +1 px half line); box columns %d..%d, last box row %d\n",
           bd.BS, bd.CLR, bd.boxBottomR - 1.f, bd.boxBottomR, bd.boxX0, bd.boxX1, bd.boxLastRow);
    // per-|TWA| table rows (TWS 14 STW 5, lee stbd) for the report
    std::vector<std::pair<State, Res> > table;
    for (int twa = 90; twa <= 180; twa++)
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 3; b++)
                for (int side = -1; side <= 1; side += 2) {
                    const int tws = TWS[a], stw = STW[b];
                    if (gen ? !visibleGen(twa, tws) : !visibleSym(twa, tws)) continue;
                    State s; s.twa = twa; s.tws = tws; s.stw = stw; s.side = side;
                    // lee starboard (side +1) = wind from port = TWA negative
                    const float twaSigned = -(float)side * (float)twa;
                    s.awa = fabsf(awaFrom(twaSigned, (float)tws, (float)stw));
                    s.trim = (float)side * (gen ? SpiGeom::genSheetDeg(s.awa) : SpiGeom::symRotationDeg(s.awa));
                    Res r = evalState(bd, s, gen);
                    nStates++;
                    if (r.clr < worstClr.clr) { worstClr = r; sClr = s; }
                    if (r.clrPix > 0.f) { overlapStates++; maxOv = std::max(maxOv, r.clrPix); }
                    if (r.rmax > worstR) { worstR = r.rmax; sR = s; }
                    if (r.topRow < worstTop) { worstTop = r.topRow; sTop = s; }
                    if (!gen) {
                        if (s.awa >= 170.f) {
                            if (r.bowS > bowDeadWorst) { bowDeadWorst = r.bowS; sBowDead = s; }
                            bowDeadResid = std::max(bowDeadResid, r.bowResid);
                            bowDeadPoke = std::max(bowDeadPoke, r.bowPoke);
                        } else {
                            if (r.bowS <= -0.5f) bowCovered++;
                            else if (r.bowS >= 2.f) bowOutside++;
                            else bowTouch++;
                            if (r.bowS > bowOtherWorst) { bowOtherWorst = r.bowS; sBowOther = s; }
                            if (r.bowS <= -0.5f) { bowOtherResid = std::max(bowOtherResid, r.bowResid); bowOtherPoke = std::max(bowOtherPoke, r.bowPoke); }
                        }
                        if (twa == 178) pole178.push_back(std::make_pair(s.awa, r.pole));
                    }
                    if (tws == 14 && stw == 5 && side == 1 && (twa % 5 == 0 || twa == 178 || twa == 101 || twa == 91))
                        table.push_back(std::make_pair(s, r));
                }
    printf("    states checked: %d (TWA 90..180 step 1, TWS 6/10/14/20, STW 3/5/7, both sides, kite visible)\n", nStates);
    printf("    (a) worst clearance to main: %+.2f px  [%s]  at TWA %d TWS %d STW %d %s AWA %.1f trim %.1f; pixel-overlap states: %d (max %.0f px)\n",
           worstClr.clr, worstClr.clr >= bd.CLR ? "OK" : "FAIL", sClr.twa, sClr.tws, sClr.stw, sClr.side > 0 ? "lee stb" : "lee bb", sClr.awa, fabsf(sClr.trim),
           overlapStates, maxOv);
    printf("    (c) max radius incl. half line: %.2f px (%.3f u) vs limit %.2f px  [%s]  at TWA %d TWS %d STW %d; topmost kite row in box columns: %d (box last row %d, needs >= %d) [%s]\n",
           worstR, (worstR - 1.f) / bd.BS, bd.boxBottomR - 1.f, worstR <= bd.boxBottomR - 1.f ? "OK" : "FAIL", sR.twa, sR.tws, sR.stw,
           worstTop, bd.boxLastRow, bd.boxLastRow + 2, worstTop >= bd.boxLastRow + 2 ? "OK" : "FAIL");
    if (!gen) {
        printf("    (d) dead run (|AWA|>=170): bow polyline max signed dist to kite %+.2f px (<= -0.5 = covered) [%s]; pixel residual max %.2f, poking px %d  (worst at TWA %d TWS %d STW %d)\n",
               bowDeadWorst, (bowDeadWorst <= -0.5f && bowDeadPoke == 0) ? "OK" : "FAIL", bowDeadResid, bowDeadPoke, sBowDead.twa, sBowDead.tws, sBowDead.stw);
        printf("    (d) other states: covered %d, clearly outside %d, touching %d  [%s]; worst %+.2f px at TWA %d TWS %d STW %d AWA %.1f; pixel residual max (covered) %.2f, poking px %d\n",
               bowCovered, bowOutside, bowTouch, bowTouch == 0 ? "OK" : "FAIL", bowOtherWorst, sBowOther.twa, sBowOther.tws, sBowOther.stw, sBowOther.awa, bowOtherResid, bowOtherPoke);
        std::sort(pole178.begin(), pole178.end());
        printf("    (b) pole angle at |TWA| 178 (|AWA| -> deg):");
        bool ok = true;
        for (size_t i = 0; i < pole178.size(); i += 2) {
            printf(" %.1f->%.1f", pole178[i].first, pole178[i].second);
            if (pole178[i].second < 68.f || pole178[i].second > 80.f) ok = false;
        }
        printf("  [%s]\n", ok ? "OK" : "see note");
    }
    printf("    table (TWS 14, STW 5, lee starboard):\n");
    printf("      TWA   AWA   trim   clr(px)  ovl  rmax(px)  topRow  bowS(px) resid poke  pole\n");
    for (size_t i = 0; i < table.size(); i++) {
        const State &s = table[i].first; const Res &r = table[i].second;
        printf("      %3d  %5.1f  %5.1f  %+7.2f  %3.0f  %7.2f  %6d  %+7.2f  %4.2f %4d  %5.1f\n",
               s.twa, s.awa, fabsf(s.trim), r.clr, r.clrPix, r.rmax, r.topRow, gen ? 0.f : r.bowS, gen ? 0.f : r.bowResid, gen ? 0 : r.bowPoke, gen ? 0.f : r.pole);
    }
}

// ---------------------------------------------------------------- reef badge
// While a downwind sail is drawn, drawReefBadge() puts the badge in the
// cockpit: lv_canvas_draw_rect at ((short)(CX - 28*SF), (short)(CY + 8*BS)),
// size ((short)(56*SF), (short)(18*SF)), corner radius UI_S(4). Distance of
// that rectangle to everything drawBoat() puts there, in px, minus the half
// width of each line (+ = gap, <= 0 = the badge hides part of it). Steady
// states of the kite window, both kinds. The pill's corners are rounded
// (radius g_badgeR px): its distance from a point is the distance from the
// rectangle shrunk by that radius, minus the radius.
static float g_badgeR = 4.f;
static float rectSegDist(float x0, float y0, float x1, float y1, F2 a, F2 b) {
    // rounded rectangle [x0,x1]x[y0,y1] (pixel edges) to segment ab
    const float r = g_badgeR;
    const float ix0 = x0 + r, ix1 = x1 - r, iy0 = y0 + r, iy1 = y1 - r;
    float d = 1e9f;
    for (int t = 0; t <= 400; t++) {
        const F2 p = { a.x + (b.x - a.x) * t / 400.f, a.y + (b.y - a.y) * t / 400.f };
        const float dx = std::max(std::max(ix0 - p.x, 0.f), p.x - ix1);
        const float dy = std::max(std::max(iy0 - p.y, 0.f), p.y - iy1);
        d = std::min(d, sqrtf(dx * dx + dy * dy) - r);
    }
    return d < 0.f ? -1.f : d;
}
static float rectPolyDist(float x0, float y0, float x1, float y1, const std::vector<F2> &P) {
    float d = 1e9f;
    for (size_t i = 0; i < P.size(); i++) d = std::min(d, rectSegDist(x0, y0, x1, y1, P[i], P[(i + 1) % P.size()]));
    // a polygon that swallows the rectangle
    F2 c = { (x0 + x1) * 0.5f, (y0 + y1) * 0.5f };
    if (inPoly(c, P)) return -1.f;
    return d;
}
struct BadgeWorst { float d; State s; BadgeWorst() : d(1e9f), s() {} };
static void badgeCheck(const Board &bd) {
    const int TWS[4] = { 6, 10, 14, 20 }, STW[3] = { 3, 5, 7 };
    const float SF = bd.SF, BS = bd.BS;
    const float bx = bd.CX - 56.f * SF * 0.5f, by = bd.CY + 8.f * BS;
    const float x0 = (float)(short)bx, y0 = (float)(short)by;
    const float x1 = x0 + (float)(short)(56.f * SF), y1 = y0 + (float)(short)(18.f * SF);
    g_badgeR = SF > 1.f ? 5.f : 4.f;   // UI_S(4)
    printf("\n=== %s  REEF BADGE in the cockpit (rect x %.0f..%.0f, rows %.0f..%.0f = %.2f..%.2f u aft) ===\n",
           bd.name, x0, x1 - 1.f, y0, y1 - 1.f, (y0 - bd.CY) / BS, (y1 - bd.CY) / BS);
    enum { MAINSAIL, BOOM, KITE, POLE, GUY, SHEET, KEEL, NK };
    const char *NAME[NK] = { "mainsail (fill + leech, 1 px)", "boom spar (1 px)", "kite (outline + 1 px)", "pole (1 px)",
                             "guy (0.5 px)", "sheet (0.5 px)", "keel (ellipse + 1.5 px line)" };
    BadgeWorst w[NK]; int n = 0, hides[NK] = { 0 };
    float hideAwaMax[NK]; int hideFaster[NK];
    for (int k = 0; k < NK; k++) { hideAwaMax[k] = 0.f; hideFaster[k] = 0; }
    float minBoom = 1e9f, minBoomSlower = 1e9f;
    for (int gen = 0; gen <= 1; gen++)
    for (int twa = 90; twa <= 180; twa++)
        for (int a = 0; a < 4; a++) for (int b = 0; b < 3; b++) for (int side = -1; side <= 1; side += 2) {
            const int tws = TWS[a], stw = STW[b];
            if (gen ? !visibleGen(twa, tws) : !visibleSym(twa, tws)) continue;
            State s; s.twa = twa; s.tws = tws; s.stw = stw; s.side = side;
            s.awa = fabsf(awaFrom(-(float)side * (float)twa, (float)tws, (float)stw));
            s.trim = (float)side * (gen ? SpiGeom::genSheetDeg(s.awa) : SpiGeom::symRotationDeg(s.awa));
            const float ss = (float)side;
            n++;
            float boom = s.awa - 13.f; if (boom < 4.f) boom = 4.f; if (boom > 86.f) boom = 86.f;
            minBoom = std::min(minBoom, boom);
            if (stw <= tws) minBoomSlower = std::min(minBoomSlower, boom);
            IPt mp[11]; float mx, my, cx, cy;
            mainFoil(bd, s.awa, (float)s.twa, ss, mp, mx, my, cx, cy);
            std::vector<F2> mpoly;
            for (int i = 0; i < 11; i++) { F2 q = { (float)mp[i].x, (float)mp[i].y }; mpoly.push_back(q); }
            float d[NK];
            for (int k = 0; k < NK; k++) d[k] = 1e9f;
            d[MAINSAIL] = rectPolyDist(x0, y0, x1, y1, mpoly) - 1.f;
            { F2 m0 = { mx, my }, m1 = { cx, cy }; d[BOOM] = rectSegDist(x0, y0, x1, y1, m0, m1) - 1.f; }
            KitePx k = gen ? genKite(bd, s.trim) : symKite(bd, s.trim, ss);
            d[KITE] = rectPolyDist(x0, y0, x1, y1, k.poly) - 1.f;
            const Pt q = SpiGeom::quarterBlock(ss), wv = SpiGeom::sheetWinch(ss);
            const F2 clew = gen ? k.lead.back() : k.foot[(size_t)SpiGeom::symClewIndex(ss)];
            d[SHEET] = std::min(rectSegDist(x0, y0, x1, y1, clew, toPx(bd, q)), rectSegDist(x0, y0, x1, y1, toPx(bd, q), toPx(bd, wv))) - 0.5f;
            if (!gen) {
                const F2 mast = { bd.CX, bd.CY + SpiGeom::MAST_Y * BS };
                d[POLE] = rectSegDist(x0, y0, x1, y1, mast, k.tack) - 1.f;
                const Pt tw = SpiGeom::twing(ss), gw = SpiGeom::guyWinch(ss);
                d[GUY] = std::min(rectSegDist(x0, y0, x1, y1, k.tack, toPx(bd, tw)), rectSegDist(x0, y0, x1, y1, toPx(bd, tw), toPx(bd, gw))) - 0.5f;
            }
            // keel: fillEllipse(CX, CY + 3.3 BS, 1.8 BS, 4.3 BS) and a 3-px line CY - 1 BS .. CY + 7 BS
            { std::vector<F2> el;
              for (int t = 0; t < 72; t++) { F2 p = { bd.CX + 1.8f * BS * cosf(t * 5.f * DEG2RAD), bd.CY + 3.3f * BS + 4.3f * BS * sinf(t * 5.f * DEG2RAD) }; el.push_back(p); }
              F2 k0 = { bd.CX, bd.CY - 1.f * BS }, k1 = { bd.CX, bd.CY + 7.f * BS };
              d[KEEL] = std::min(rectPolyDist(x0, y0, x1, y1, el), rectSegDist(x0, y0, x1, y1, k0, k1) - 1.5f); }
            for (int kk = 0; kk < NK; kk++) {
                if (d[kk] <= 0.f) { hides[kk]++; hideAwaMax[kk] = std::max(hideAwaMax[kk], s.awa); if (stw > tws) hideFaster[kk]++; }
                if (d[kk] < w[kk].d) { w[kk].d = d[kk]; w[kk].s = s; }
            }
        }
    printf("    states: %d (both kinds, check grid, kite visible); smallest boom angle %.1f deg, %.1f deg where the boat is not faster than the wind\n",
           n, minBoom, minBoomSlower);
    for (int kk = 0; kk < NK; kk++) {
        if (w[kk].d > 1e8f) continue;
        printf("    %-30s nearest %+7.2f px  at TWA %d TWS %d STW %d (AWA %.1f); badge hides part of it in %d states",
               NAME[kk], w[kk].d, w[kk].s.twa, w[kk].s.tws, w[kk].s.stw, w[kk].s.awa, hides[kk]);
        if (hides[kk]) printf(" (largest |AWA| among them %.1f; boat faster than the wind in %d of them)", hideAwaMax[kk], hideFaster[kk]);
        printf("\n");
    }
}

// Gennaker tack at the stem tip: the hull outline (truncated stations, 2-px
// linePx) against the gennaker as drawn (fill, 2-px leeward edge, 1-px luff),
// pixel by pixel. "Ahead of the tack" = pixel centre forward of the tack
// point. A hull pixel counts as showing when the kite leaves at least a
// quarter of its coverage (residual = hull * (1 - kite) >= 0.25).
static void tackCheck(const Board &bd) {
    const int TWS[4] = { 6, 10, 14, 20 }, STW[3] = { 3, 5, 7 };
    IPt hp[36]; hullPts(bd, hp);
    gH.clear();
    for (int i = 0; i < 36; i++) linePx(gH, hp[i].x, hp[i].y, hp[(i + 1) % 36].x, hp[(i + 1) % 36].y, 2.f);
    int hullTop = 99999;
    for (int y = gH.y0; y <= gH.y1 && hullTop == 99999; y++)
        for (int x = gH.x0; x <= gH.x1; x++) if (gH.at(x, y) > 0.f) { hullTop = y; break; }
    const float tackX = bd.CX, tackY = bd.CY + SpiGeom::GEN_TACK_Y * bd.BS;
    int n = 0, statesShowing = 0, worstCount = 0; float worstRes = 0.f; State sW = State();
    int kiteTopMin = 99999, kiteTopMax = -1;
    for (int twa = 90; twa <= 180; twa++)
        for (int a = 0; a < 4; a++) for (int b = 0; b < 3; b++) for (int side = -1; side <= 1; side += 2) {
            const int tws = TWS[a], stw = STW[b];
            if (!visibleGen(twa, tws)) continue;
            State s; s.twa = twa; s.tws = tws; s.stw = stw; s.side = side;
            s.awa = fabsf(awaFrom(-(float)side * (float)twa, (float)tws, (float)stw));
            s.trim = (float)side * SpiGeom::genSheetDeg(s.awa);
            gK.clear();
            rasterKite(gK, genKite(bd, s.trim));
            n++;
            int cnt = 0; float res = 0.f;
            for (int y = gH.y0; y <= gH.y1 && (float)y + 0.5f < tackY; y++)
                for (int x = gH.x0; x <= gH.x1; x++) {
                    const float r = gH.at(x, y) * (1.f - gK.at(x, y));
                    res = std::max(res, r);
                    if (r >= 0.25f) cnt++;
                }
            if (cnt) statesShowing++;
            if (res > worstRes) { worstRes = res; sW = s; }
            worstCount = std::max(worstCount, cnt);
            int kt = 99999;
            for (int y = gK.y0; y <= gK.y1 && kt == 99999; y++)
                for (int x = gK.x0; x <= gK.x1; x++) if (gK.at(x, y) > 0.f) { kt = y; break; }
            kiteTopMin = std::min(kiteTopMin, kt); kiteTopMax = std::max(kiteTopMax, kt);
        }
    printf("\n=== %s  GENNAKER TACK at the stem tip ===\n", bd.name);
    printf("    tack (0, %.2f) = px (%.2f, %.2f), pixel row %d; hull outline: tip vertices (%d, %d) and (%d, %d), front row %d (centre %.1f)\n",
           SpiGeom::GEN_TACK_Y, tackX, tackY, (int)floorf(tackY), hp[0].x, hp[0].y, hp[35].x, hp[35].y, hullTop, (float)hullTop + 0.5f);
    printf("    hull pixels ahead of the tack (residual >= 0.25): %d states of %d, at most %d px; largest residual %.2f%s  [%s]\n",
           statesShowing, n, worstCount, worstRes, worstRes > 0.f ? "" : " (none at all)", statesShowing == 0 ? "OK" : "FAIL");
    if (worstRes > 0.f) printf("      largest residual at TWA %d TWS %d STW %d %s\n", sW.twa, sW.tws, sW.stw, sW.side > 0 ? "lee stb" : "lee bb");
    printf("    tack row vs hull front row: %+d (0 = the tack sits in the hull's front row)  [%s]; kite top row %d..%d\n",
           (int)floorf(tackY) - hullTop, (int)floorf(tackY) == hullTop ? "OK" : "FAIL", kiteTopMin, kiteTopMax);
}

// Gennaker radius over EVERY sheeting angle the trim can produce (the
// smoothed angle lies anywhere in GEN_SHEET_MIN..MAX on either side, not only
// on the grid above): outline radius + 1 px and the top row in the heading-box
// columns, step 0.05 deg, both sides.
static void genSweep(const Board &bd) {
    float worstR = 0.f, atR = 0.f, firstOut = -1.f;
    int worstTop = 99999; float atTop = 0.f;
    const float tackR = -SpiGeom::GEN_TACK_Y * bd.BS + 1.f;
    for (int side = -1; side <= 1; side += 2)
        for (int i = 0; i <= 240; i++) {
            const float a = SpiGeom::GEN_SHEET_MIN + 0.05f * (float)i;
            const KitePx k = genKite(bd, (float)side * a);
            float r = 0.f;
            for (const F2 &p : densify(k.poly, 0.25f)) r = std::max(r, hypotf(p.x - bd.CX, p.y - bd.CY));
            r += 1.f;
            if (r > worstR) { worstR = r; atR = a; }
            if (side > 0 && firstOut < 0.f && r > tackR + 0.01f) firstOut = a;
            gK.clear(); rasterKite(gK, k);
            int top = 99999;
            for (int y = gK.y0; y <= gK.y1 && top == 99999; y++)
                for (int x = bd.boxX0; x <= bd.boxX1; x++) if (gK.at(x, y) > 0.f) { top = y; break; }
            if (top < worstTop) { worstTop = top; atTop = a; }
        }
    printf("\n=== %s  GENNAKER over every sheeting angle %.0f..%.0f (step 0.05, both sides) ===\n", bd.name,
           SpiGeom::GEN_SHEET_MIN, SpiGeom::GEN_SHEET_MAX);
    printf("    max radius incl. half line %.2f px (%.3f u) at %.2f deg vs limit %.2f px  [%s]; the tack alone %.2f px; first beyond the tack at %.2f deg\n",
           worstR, (worstR - 1.f) / bd.BS, atR, bd.boxBottomR - 1.f, worstR <= bd.boxBottomR - 1.f ? "OK" : "FAIL", tackR, firstOut);
    printf("    topmost kite row in box columns %d at %.2f deg (box last row %d, needs >= %d)  [%s]\n",
           worstTop, atTop, bd.boxLastRow, bd.boxLastRow + 2, worstTop >= bd.boxLastRow + 2 ? "OK" : "FAIL");
}

static void monotonic() {
    // pole angle as a function of |AWA| 0..180 (step 0.05): non-decreasing
    float prev = -1.f, worstDrop = 0.f, at = 0.f, pmin = 1e9f, pmax = -1e9f;
    for (int i = 0; i <= 3600; i++) {
        const float awa = (float)i * 0.05f;
        Pt foot[SpiGeom::SYM_N + 1], lead[SpiGeom::SYM_N + 1];
        SpiGeom::symOutline(SpiGeom::symRotationDeg(awa), foot, lead);   // lee stbd
        const Pt t = foot[SpiGeom::symTackIndex(1.f)];
        const float p = atan2f(fabsf(t.x), SpiGeom::MAST_Y - t.y) / DEG2RAD;
        if (prev >= 0.f && p < prev - 1e-4f && prev - p > worstDrop) { worstDrop = prev - p; at = awa; }
        prev = p; pmin = std::min(pmin, p); pmax = std::max(pmax, p);
    }
    printf("\n(b) pole angle over |AWA| 0..180: %.2f .. %.2f deg, largest decrease while |AWA| grows: %.4f deg%s [%s]\n",
           pmin, pmax, worstDrop, worstDrop > 0.f ? "" : "", worstDrop <= 1e-3f ? "OK, non-decreasing" : "FAIL");
    (void)at;
    // gennaker sheeting: non-decreasing in |AWA|
    float gp = -1.f; bool gok = true;
    for (int i = 0; i <= 3600; i++) { const float g = SpiGeom::genSheetDeg((float)i * 0.05f); if (g < gp) gok = false; gp = g; }
    printf("    gennaker sheeting angle non-decreasing in |AWA|: %s\n", gok ? "yes" : "NO");
}

static void vectors(FILE *f) {
    fprintf(f, "# SpiGeom test vectors - printed by docs/downwind_check.cpp from the real SpiGeom.cpp\n");
    fprintf(f, "# boat units, x + = starboard, y + = aft; %%.4f\n\n");
    const float awas[] = { 180.f, 175.f, 172.f, 165.f, 150.f, 135.f, 120.f, 116.f, 100.f, 60.f, 12.f };
    fprintf(f, "symRotationDeg(|AWA|):\n");
    for (float a : awas) fprintf(f, "  %6.1f -> %.4f\n", a, SpiGeom::symRotationDeg(a));
    fprintf(f, "\ngenSheetDeg(|AWA|):\n");
    const float gaw[] = { 40.f, 75.f, 90.f, 100.f, 116.f, 135.f, 150.f, 157.f };
    for (float a : gaw) fprintf(f, "  %6.1f -> %.4f\n", a, SpiGeom::genSheetDeg(a));
    fprintf(f, "\nballoonBandEdge(j):");
    for (int j = 0; j <= SpiGeom::BALLOON_BANDS; j++) fprintf(f, " %.4f", SpiGeom::balloonBandEdge(j));
    fprintf(f, "\n\nanchors (lee = +1): quarterBlock (%.4f, %.4f) sheetWinch (%.4f, %.4f) twing (%.4f, %.4f) guyWinch (%.4f, %.4f)\n",
            SpiGeom::quarterBlock(1.f).x, SpiGeom::quarterBlock(1.f).y, SpiGeom::sheetWinch(1.f).x, SpiGeom::sheetWinch(1.f).y,
            SpiGeom::twing(1.f).x, SpiGeom::twing(1.f).y, SpiGeom::guyWinch(1.f).x, SpiGeom::guyWinch(1.f).y);
    const float rots[] = { 0.f, 7.f, -14.f };
    for (float r : rots) {
        Pt foot[SpiGeom::SYM_N + 1], lead[SpiGeom::SYM_N + 1];
        SpiGeom::symOutline(r, foot, lead);
        fprintf(f, "\nsymOutline(rot = %+.1f):\n   i      foot.x     foot.y      lead.x     lead.y\n", r);
        for (int i = 0; i <= SpiGeom::SYM_N; i++)
            fprintf(f, "  %2d  %10.4f %10.4f  %10.4f %10.4f\n", i, foot[i].x, foot[i].y, lead[i].x, lead[i].y);
    }
    const float sh[] = { 44.f, -50.f, 56.f };
    for (float s : sh) {
        SpiGeom::GenOutline o;
        SpiGeom::genOutline(s, o);
        fprintf(f, "\ngenOutline(sheet = %+.1f):\n   i       lee.x      lee.y      luff.x     luff.y     panel.x    panel.y\n", s);
        for (int i = 0; i <= SpiGeom::GEN_N; i++)
            fprintf(f, "  %2d  %10.4f %10.4f  %10.4f %10.4f  %10.4f %10.4f\n", i, o.lee[i].x, o.lee[i].y, o.luff[i].x, o.luff[i].y, o.panel[i].x, o.panel[i].y);
    }
}

int main(int argc, char **argv) {
    const Board b7 = makeBoard("7B/5B (1024x600, SF 1.25)", 1.25f);
    const Board b4 = makeBoard("4-inch (480x480, SF 1.0)", 1.0f);
    printf("SpiGeom constants: R_CREST %.2f  W %.1f->%.1f  f0 %.1f->%.1f  sag %.1f->%.1f  b %.1f->%.1f  rot: zero above |AWA| %.0f, gain %.2f, max %.1f\n",
           SpiGeom::SYM_R_CREST, SpiGeom::SYM_W_SQUARE, SpiGeom::SYM_W_TURNED, SpiGeom::SYM_F0_SQUARE, SpiGeom::SYM_F0_TURNED,
           SpiGeom::SYM_SAG_SQUARE, SpiGeom::SYM_SAG_TURNED, SpiGeom::SYM_B_SQUARE, SpiGeom::SYM_B_TURNED,
           SpiGeom::SYM_ROT_ZERO_AWA, SpiGeom::SYM_ROT_GAIN, SpiGeom::SYM_ROT_MAX);
    printf("gennaker: tack y %.2f, sheet %.1f + %.2f*(|AWA|-90) in %.0f..%.0f, len %.1f + %.2f*(%.0f-sheet), lee %.2f@%.2f, luff %.2f@%.2f, panel %.2f\n",
           SpiGeom::GEN_TACK_Y, SpiGeom::GEN_SHEET_AT_90, SpiGeom::GEN_SHEET_GAIN, SpiGeom::GEN_SHEET_MIN, SpiGeom::GEN_SHEET_MAX,
           SpiGeom::GEN_LEN_EASED, SpiGeom::GEN_LEN_GAIN, SpiGeom::GEN_SHEET_MAX, SpiGeom::GEN_LEE_DEPTH, SpiGeom::GEN_LEE_POS,
           SpiGeom::GEN_LUFF_DEPTH, SpiGeom::GEN_LUFF_POS, SpiGeom::GEN_PANEL);
    runKind(b7, false);
    runKind(b4, false);
    runKind(b7, true);
    runKind(b4, true);
    genSweep(b7);
    genSweep(b4);
    tackCheck(b7);
    tackCheck(b4);
    monotonic();
    badgeCheck(b7);
    badgeCheck(b4);
    if (argc > 1) {
        FILE *f = fopen(argv[1], "w");
        if (f) { vectors(f); fclose(f); printf("\ntest vectors -> %s\n", argv[1]); }
    }
    return 0;
}
