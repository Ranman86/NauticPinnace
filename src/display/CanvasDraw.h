#pragma once
#include <lvgl.h>

// ============================================================
// CanvasDraw – shared FILLED-shape primitives for the canvas screens.
//
// Why this module exists: several screens used to fake a filled area by
// stroking dozens of thick 1° line segments in a loop. That renders as a comb
// with wedge-shaped gaps at the outer radius, blends its own alpha once per
// segment (so a "subtle tint" composites ~40× and turns opaque, with darker
// seams), and quantises the shape to whole degrees. These helpers draw the real
// thing in a single pass instead.
//
// ⚠ Never use lv_canvas_draw_polygon in this project. It is convex-only and
//   loops forever on a non-convex outline (PC: hang, ESP32: watchdog reset), its
//   mask buffer allocation is unchecked (NULL deref), it silently truncates
//   above 16 edges, and it segfaults in the PC simulator's LVGL build. Polygons
//   here are filled by scanline directly into the canvas buffer.
// ============================================================

// ---- Filled annular sector (ring band) --------------------------------------
// Solid, anti-aliased band between rInner and rOuter, from a0Deg to a1Deg.
//
// Angles use the LVGL convention: 0° = 3 o'clock, increasing CLOCKWISE
// (90° = bottom, 180° = left, 270° = top). Note the LVGL header comment claims
// otherwise — it is wrong; this matches the actual sw renderer.
//
// Angles may be passed negative or >360: they are normalised here. This is not
// cosmetic — lv_draw_arc takes uint16_t, so a raw negative angle wraps to
// (65536 - a) % 360 and lands 16° off (−30° would draw at 346°, not 330°).
//
// Always drawn at full opacity by design: LVGL's arc renderer issues
// overlapping quarter passes, so a translucent arc double-blends into a visible
// seam, and any opa < COVER also forces a slower read-modify-write per pixel.
// For a tint, pre-mix the colour against its backdrop with cdMix() and pass that.
void cdFillRing(lv_obj_t *cv, float cx, float cy, float rInner, float rOuter,
                float a0Deg, float a1Deg, lv_color_t col);

// ---- Filled disc (anti-aliased) ---------------------------------------------

// ---- Filled polygon / triangle (even-odd scanline) --------------------------
// Written straight into the canvas buffer: one lv_canvas_draw_line per row would
// re-run LVGL's fake-display init (a malloc/free pair) and an invalidate for
// every single row. Solid fill, no AA on the edges.
void cdFillPoly(lv_obj_t *cv, const lv_point_t *pts, int n, lv_color_t col);
void cdFillTri(lv_obj_t *cv, lv_point_t a, lv_point_t b, lv_point_t c, lv_color_t col);

// ---- Colour helper ----------------------------------------------------------
// Blend fg over bg. mix = 0..255 (255 = fully fg). Use to pre-flatten a tint so
// it can be drawn opaque — see the note on cdFillRing.
static inline lv_color_t cdMix(lv_color_t fg, lv_color_t bg, uint8_t mix) {
    return lv_color_mix(fg, bg, mix);
}
