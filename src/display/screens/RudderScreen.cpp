#include "RudderScreen.h"
#include "../../PsramArena.h"
#include "../../i18n/I18n.h"
#include "../UiConfig.h"
#include "../CanvasDraw.h"
#include <math.h>
#include <string.h>

// Canvas covers the full working area
// Layout: Wide horizontal arc, centred, with P/S labels on each end.
// The arc sweeps from -40° (Port) to +40° (Starboard).
// A filled coloured segment shows the actual rudder angle.
// A sharp needle points to the exact value.
// Below the arc: large numeric angle + direction word.

static constexpr int CS = RudderScreen::CS;   // 480

void RudderScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H - NAV_BAR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, OPA_FULL, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(CS, CS);
    if (!_cbuf) _cbuf = (lv_color_t *)PsramArena::alloc(sz);   // reuse on live theme rebuild
    if (_cbuf) {
        _canvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_canvas, _cbuf, CS, CS, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(_canvas, LV_ALIGN_TOP_MID, 0, 0);
    }

    _lblAngle = lv_label_create(container);
    lv_label_set_text(_lblAngle, "0.0°");
    lv_obj_set_style_text_font(_lblAngle, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(_lblAngle, CLR_TEXT, 0);
    lv_obj_align(_lblAngle, LV_ALIGN_TOP_MID, 0, CS + UI_RUDDER_ANGLE_Y);

    _lblDir = lv_label_create(container);
    lv_label_set_text(_lblDir, T(STR_RUD_MIDSHIPS));
    styleLabel(_lblDir, FONT_LARGE, CLR_TEXT_DIM);
    lv_obj_align(_lblDir, LV_ALIGN_TOP_MID, 0, CS + UI_RUDDER_DIR_Y);
}

void RudderScreen::drawRudder(float angle) {
    if (!_canvas || !_cbuf) return;
    {
        const lv_color_t bgColor = CLR_BG;
        lv_color_t *p = _cbuf;
        for (int row = 0; row < CS; row += 8) {
            int rows = (CS - row < 8) ? CS - row : 8;
            lv_color_t *rowEnd = p + (size_t)rows * CS;
            while (p < rowEnd) *p++ = bgColor;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        lv_obj_invalidate(_canvas);
    }

    // Geometry: large arc, centred horizontally, in the top 2/3 of canvas
    // Arc pivot is BELOW the visible canvas so only the upper portion shows
    // (giving a wide shallow arc that spans the full width).
    float pivotX = CS / 2.0f;
    float pivotY = CS * UI_RUDDER_PIVOT_OFFS;   // pivot well below canvas → shallow arc
    float R      = CS * UI_RUDDER_RADIUS;       // radius of arc track centreline
    float trackW = UI_RUDDER_TRACK_W;           // arc track width in pixels
    float maxAng = UI_RUDDER_MAX_ANG;           // ±40° range

    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);

    // Helper: angle in "rudder convention" → screen point on arc
    // 0° = directly above pivot (= top-centre), + = starboard, - = port
    auto pt = [&](float deg, float r) -> lv_point_t {
        float a = (90.0f + deg) * DEG_TO_RAD;   // 0° at top means angle from right
        // Actually: 0° → top of circle; left = −, right = +
        // angle from pivot: 0° = straight up = -90° from horizontal
        float rad = (deg - 90.0f) * DEG_TO_RAD;
        return {
            (lv_coord_t)(pivotX + r * cosf(rad)),
            (lv_coord_t)(pivotY + r * sinf(rad))
        };
    };

    // The gauge band is drawn as real filled ring sectors (cdFillRing), not as a
    // chain of thick 1° line strokes — the latter left wedge-shaped gaps at the
    // outer radius and composited its own alpha once per segment.
    // Rudder convention → LVGL angles (0° = 3 o'clock, CW): lvgl = rudder − 90.
    const float rI = R - trackW * 0.5f, rO = R + trackW * 0.5f;
    auto lvA = [](float rudderDeg) { return rudderDeg - 90.0f; };

    // Tints are pre-mixed against their backdrop and drawn opaque: a translucent
    // arc double-blends where LVGL's quarter passes overlap (visible seam).
    const lv_color_t trackCol = cdMix(CLR_BORDER, CLR_BG, OPA_50);
    const lv_color_t portCol  = cdMix(CLR_PORT,      trackCol, UI_RUDDER_PORT_OPA);
    const lv_color_t stbdCol  = cdMix(CLR_STARBOARD, trackCol, UI_RUDDER_STB_OPA);

    // Background track (full range) + the two zone tints.
    cdFillRing(_canvas, pivotX, pivotY, rI, rO, lvA(-maxAng), lvA(maxAng), trackCol);
    cdFillRing(_canvas, pivotX, pivotY, rI, rO, lvA(-maxAng), lvA(0.f),    portCol);
    cdFillRing(_canvas, pivotX, pivotY, rI, rO, lvA(0.f),     lvA(maxAng), stbdCol);

    // Active rudder-angle sector (0 → actual angle), the primary readout.
    // Uses the exact float angle, so no sub-degree quantisation.
    if (!isnan(angle)) {
        float a1 = max(-maxAng, min(maxAng, angle));
        if (fabsf(a1) > 0.15f) {
            lv_color_t col = (a1 < 0.f) ? CLR_PORT : CLR_STARBOARD;
            float lo = min(0.f, a1), hi = max(0.f, a1);
            cdFillRing(_canvas, pivotX, pivotY, rI, rO, lvA(lo), lvA(hi), col);
        }
    }

    // Centre line (thick white zero mark)
    ld.color = CLR_TEXT; ld.width = 4; ld.opa = OPA_FULL;
    {
        lv_point_t p1 = pt(0.0f, R - trackW/2 - 6);
        lv_point_t p2 = pt(0.0f, R + trackW/2 + 6);
        lv_point_t _l[2] = {p1, p2};
        lv_canvas_draw_line(_canvas, _l, 2, &ld);
    }

    // Tick marks every 10°
    ld.width = uiSz.rudderNeedleW; ld.opa = OPA_FULL;
    for (int a = -(int)maxAng; a <= (int)maxAng; a += 10) {
        if (a == 0) continue;
        ld.color = (a < 0) ? CLR_PORT : CLR_STARBOARD;
        lv_point_t p1 = pt((float)a, R - trackW/2 - 4);
        lv_point_t p2 = pt((float)a, R + trackW/2 + 4);
        lv_point_t _l[2] = {p1, p2};
        lv_canvas_draw_line(_canvas, _l, 2, &ld);
        // Degree label
        td.font = FONT_SMALL; td.color = (a < 0) ? CLR_PORT : CLR_STARBOARD;
        td.opa = OPA_FULL;
        char tbuf[6]; snprintf(tbuf, sizeof(tbuf), "%d", abs(a));
        lv_point_t lp = pt((float)a, R + trackW/2 + 18);
        lv_canvas_draw_text(_canvas, lp.x - 10, lp.y - 8, 28, &td, tbuf);
    }

    // Needle pointer: a SOLID triangle (was three outline strokes, so the band
    // showed through it), with a thin outline on top for a crisp edge.
    if (!isnan(angle)) {
        float ang = max(-maxAng, min(maxAng, angle));
        lv_color_t ncol = (ang < -0.5f) ? CLR_PORT : (ang > 0.5f) ? CLR_STARBOARD : CLR_TEXT;
        lv_point_t tip = pt(ang,        R - trackW/2 - 14);
        lv_point_t bl  = pt(ang - 3.0f, R + trackW/2 + 6);
        lv_point_t br  = pt(ang + 3.0f, R + trackW/2 + 6);
        cdFillTri(_canvas, tip, bl, br, ncol);
        lv_draw_line_dsc_t fd; lv_draw_line_dsc_init(&fd);
        fd.color = ncol; fd.width = 2; fd.opa = OPA_FULL;
        lv_point_t sides[2];
        sides[0]=tip; sides[1]=bl; lv_canvas_draw_line(_canvas, sides, 2, &fd);
        sides[0]=tip; sides[1]=br; lv_canvas_draw_line(_canvas, sides, 2, &fd);
        sides[0]=bl;  sides[1]=br; lv_canvas_draw_line(_canvas, sides, 2, &fd);
    }

    // P and S labels at the ends of the arc
    lv_point_t pPos = pt(-maxAng - 2.0f, R);
    lv_point_t sPos = pt( maxAng + 2.0f, R);
    td.font = FONT_LARGE; td.opa = OPA_FULL;
    td.color = CLR_PORT;
    lv_canvas_draw_text(_canvas, pPos.x - 28, pPos.y - 14, 30, &td, T(STR_RUD_PORT_MARK));
    td.color = CLR_STARBOARD;
    lv_canvas_draw_text(_canvas, sPos.x + 2,  sPos.y - 14, 30, &td, T(STR_RUD_STBD_MARK));

    lv_obj_invalidate(_canvas);
}

void RudderScreen::update() {
    float angle;
    { auto lk = data.lock(); angle = data.rudderAngle; }

    drawRudder(angle);

    if (isnan(angle)) {
        lv_label_set_text(_lblAngle, "--°");
        lv_label_set_text(_lblDir, "");
        lv_obj_set_style_text_color(_lblAngle, CLR_TEXT, 0);
        lv_obj_set_style_text_color(_lblDir, CLR_TEXT_DIM, 0);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f°", fabsf(angle));
        lv_label_set_text(_lblAngle, buf);
        if (fabsf(angle) < 0.5f) {
            lv_label_set_text(_lblDir, T(STR_RUD_MIDSHIPS));
            lv_obj_set_style_text_color(_lblAngle, CLR_TEXT, 0);
            lv_obj_set_style_text_color(_lblDir, CLR_TEXT_DIM, 0);
        } else if (angle < 0) {
            lv_label_set_text(_lblDir, T(STR_RUD_PORT));
            lv_obj_set_style_text_color(_lblAngle, CLR_PORT, 0);
            lv_obj_set_style_text_color(_lblDir, CLR_PORT, 0);
        } else {
            lv_label_set_text(_lblDir, T(STR_RUD_STARBOARD));
            lv_obj_set_style_text_color(_lblAngle, CLR_STARBOARD, 0);
            lv_obj_set_style_text_color(_lblDir, CLR_STARBOARD, 0);
        }
    }
}
