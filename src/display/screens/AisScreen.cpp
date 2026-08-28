#include "AisScreen.h"
#include "RenderYield.h"
#include "../../PsramArena.h"
#include "../CanvasDraw.h"
#include <math.h>
#include <string.h>

void AisScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H - NAV_BAR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, OPA_FULL, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(CS, CS);
    if (!_cbuf) _cbuf = (lv_color_t*)PsramArena::alloc(sz);   // reuse on live theme rebuild
    if (_cbuf) {
        // Buffer pre-zeroed by PsramArena::init() (before WiFi).
        _canvas = lv_canvas_create(container);
        lv_canvas_set_buffer(_canvas, _cbuf, CS, CS, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(_canvas, (SCREEN_W-CS)/2, 0);
        lv_obj_add_event_cb(_canvas, onCanvasClick, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);
    } else {
        Serial.printf("[AisScreen] canvas buf alloc FAILED (%u B)\n", (unsigned)sz);
        Serial.flush();
    }
    // Scale and count labels
    _scaleLbl = lv_label_create(container);
    char sl[12]; snprintf(sl, sizeof(sl), "5%s", T(STR_UNIT_NM));
    lv_label_set_text(_scaleLbl, sl);
    styleLabel(_scaleLbl, FONT_SMALL, CLR_TEXT_DIM);
    lv_obj_set_pos(_scaleLbl, UI_S(4), UI_S(4));

    _countLbl = lv_label_create(container);
    {
        char cb[16]; snprintf(cb, sizeof(cb), "0 %s", T(STR_AIS_TARGETS));
        lv_label_set_text(_countLbl, cb);
    }
    styleLabel(_countLbl, FONT_SMALL, CLR_TEXT_DIM);
    lv_obj_align(_countLbl, LV_ALIGN_TOP_RIGHT, UI_S(-4), UI_S(4));

    // Info box (shown on target tap)
    _infoBox = lv_obj_create(container);
    lv_obj_set_size(_infoBox, SCREEN_W - UI_S(8), UI_S(80));
    lv_obj_align(_infoBox, LV_ALIGN_BOTTOM_MID, 0, UI_S(-2));
    styleCard(_infoBox);
    lv_obj_add_flag(_infoBox, LV_OBJ_FLAG_HIDDEN);

    _infoLbl = lv_label_create(_infoBox);
    lv_label_set_text(_infoLbl, "");
    styleLabel(_infoLbl, FONT_SMALL, CLR_TEXT);
    lv_obj_align(_infoLbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_long_mode(_infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_infoLbl, SCREEN_W - UI_S(24));
}

void AisScreen::drawRadar() {
    if (!_canvas || !_cbuf) return;
    // Chunked fill so there is somewhere to yield mid-clear; renderYield()
    // decides how many of those offers are worth a tick (see RenderYield.h).
    {
        const lv_color_t bgColor = CLR_BG;
        lv_color_t *p = _cbuf, *end = _cbuf + (size_t)CS * CS;
        for (int row = 0; row < CS; row += 8) {
            int rows = (CS - row < 8) ? CS - row : 8;
            lv_color_t *rowEnd = p + (size_t)rows * CS;
            while (p < rowEnd) *p++ = bgColor;
            renderYield();
        }
        lv_obj_invalidate(_canvas);
    }

    int cx = CS/2, cy = CS/2;
    int maxR = CS/2 - UI_S(10);
    float nm2px = (float)maxR / _rangeNm;

    lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    lv_draw_label_dsc_t td; lv_draw_label_dsc_init(&td);
    td.font = FONT_TINY; td.opa = OPA_75;

    // Range rings
    ld.color = CLR_GRID_LINE; ld.width = 1; ld.opa = OPA_50;
    for (int ring = 1; ring <= 5; ring++) {
        int ri = (int)(ring * (maxR / 5));
        for (int a = 0; a < 360; a += 3) {
            float a1=a*DEG_TO_RAD, a2=(a+3)*DEG_TO_RAD;
            lv_point_t p1={(lv_coord_t)(cx+ri*sinf(a1)),(lv_coord_t)(cy-ri*cosf(a1))};
            lv_point_t p2={(lv_coord_t)(cx+ri*sinf(a2)),(lv_coord_t)(cy-ri*cosf(a2))};
            lv_point_t _l[2]={p1,p2}; lv_canvas_draw_line(_canvas,_l,2,&ld);
        }
        // Range label
        float labelNm = (float)ring * _rangeNm / 5.0f;
        char rlbl[12]; snprintf(rlbl, sizeof(rlbl), "%.1f%s", labelNm, T(STR_UNIT_NM));
        td.color = CLR_GRID_LINE;
        lv_canvas_draw_text(_canvas,
            (lv_coord_t)(cx+UI_S(4)), (lv_coord_t)(cy-ri-UI_S(14)), UI_S(60), &td, rlbl);
    }

    // Cross-hair lines
    ld.color = CLR_TEXT_DIM; ld.width = 1; ld.opa = OPA_50;
    for (int a : {0, 90, 180, 270}) {
        float ar = a*DEG_TO_RAD;
        lv_point_t p1={(lv_coord_t)cx,(lv_coord_t)cy};
        lv_point_t p2={(lv_coord_t)(cx+(int)(maxR*sinf(ar))),
                       (lv_coord_t)(cy-(int)(maxR*cosf(ar)))};
        lv_point_t _l[2]={p1,p2}; lv_canvas_draw_line(_canvas,_l,2,&ld);
    }

    // Own ship dot
    rd.bg_color = CLR_TEXT; rd.bg_opa = OPA_FULL; rd.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(_canvas,
        (lv_coord_t)(cx-UI_S(5)), (lv_coord_t)(cy-UI_S(5)), UI_S(10), UI_S(10), &rd);

    // Heading vector
    float ownHdg;
    { auto lk=data.lock(); ownHdg=isnan(data.cog)?data.hdg:data.cog; }
    if (!isnan(ownHdg)) {
        ld.color = CLR_TEXT; ld.width = 2; ld.opa = OPA_FULL;
        float ar = ownHdg*DEG_TO_RAD;
        lv_point_t p1={(lv_coord_t)cx,(lv_coord_t)cy};
        lv_point_t p2={(lv_coord_t)(cx+(int)(UI_S(40)*sinf(ar))),(lv_coord_t)(cy-(int)(UI_S(40)*cosf(ar)))};
        lv_point_t _l[2]={p1,p2}; lv_canvas_draw_line(_canvas,_l,2,&ld);
    }

    // AIS targets
    AisTarget tgts[DataModel::MAX_AIS];
    int cnt; float ownLat, ownLon;
    {
        auto lk = data.lock();
        cnt = data.aisCount;
        ownLat = data.lat; ownLon = data.lon;
        memcpy(tgts, data.aisTargets, sizeof(AisTarget)*cnt);
    }

    for (int i = 0; i < cnt; i++) {
        AisTarget &t = tgts[i];
        if (isnan(t.lat)||isnan(t.lon)||isnan(ownLat)) continue;

        float avgLat = (ownLat+t.lat)/2.0f*DEG_TO_RAD;
        float dx = (t.lon - ownLon)*60.0f*cosf(avgLat);  // nm east
        float dy = (t.lat - ownLat)*60.0f;                // nm north
        float distNm = sqrtf(dx*dx+dy*dy);
        if (distNm > _rangeNm*1.1f) continue;

        int tx = cx + (int)(dx*nm2px);
        int ty = cy - (int)(dy*nm2px);
        if (tx<0||tx>=CS||ty<0||ty>=CS) continue;

        // Colour by CPA/TCPA. Thresholds come from the config (WebUI: AIS
        // editor, "Alarm CPA/TCPA") instead of being hardcoded; the yellow
        // pre-warning tier is 2x the red alarm tier - with the defaults
        // (0.5 nm / 10 min) that reproduces the old fixed 0.5/10 and 1.0/20.
        lv_color_t col = CLR_GREEN;
        float cpa = t.cpa, tcpa = t.tcpa;
        const float cpaAl  = appConfig.cfg.aisCpaAlarm;
        const float tcpaAl = appConfig.cfg.aisTcpaAlarm;
        if (!isnan(cpa) && !isnan(tcpa)) {
            if (cpa < cpaAl && tcpa > 0 && tcpa < tcpaAl)          col = CLR_RED;
            else if (cpa < 2.f * cpaAl && tcpa < 2.f * tcpaAl)     col = CLR_YELLOW;
        }

        // Draw triangle pointing in COG direction
        if (!isnan(t.cog)) {
            float cr = t.cog*DEG_TO_RAD;
            lv_point_t tri[3] = {
                {(lv_coord_t)(tx+(int)(UI_S(10)*sinf(cr))),    (lv_coord_t)(ty-(int)(UI_S(10)*cosf(cr)))},
                {(lv_coord_t)(tx+(int)(UI_S(6)*sinf(cr+2.3f))),(lv_coord_t)(ty-(int)(UI_S(6)*cosf(cr+2.3f)))},
                {(lv_coord_t)(tx+(int)(UI_S(6)*sinf(cr-2.3f))),(lv_coord_t)(ty-(int)(UI_S(6)*cosf(cr-2.3f)))}
            };
            // Solid target symbol (was a hollow 2 px wireframe, which was hard to
            // pick out against the radar rings at the CPA warning colours).
            cdFillTri(_canvas, tri[0], tri[1], tri[2], col);
            ld.color = col; ld.width = 2; ld.opa = OPA_FULL;
            { lv_point_t _l[2]={tri[0],tri[1]}; lv_canvas_draw_line(_canvas,_l,2,&ld); }
            { lv_point_t _l[2]={tri[1],tri[2]}; lv_canvas_draw_line(_canvas,_l,2,&ld); }
            { lv_point_t _l[2]={tri[2],tri[0]}; lv_canvas_draw_line(_canvas,_l,2,&ld); }

            // COG vector (6min = 0.1 * sog nm)
            if (!isnan(t.sog)) {
                float vecNm = t.sog * 6.0f / 60.0f;
                lv_point_t vp1={(lv_coord_t)tx,(lv_coord_t)ty};
                lv_point_t vp2={(lv_coord_t)(tx+(int)(vecNm*nm2px*sinf(cr))),
                                (lv_coord_t)(ty-(int)(vecNm*nm2px*cosf(cr)))};
                ld.width = 1; ld.opa = OPA_50;
                lv_point_t _l[2]={vp1,vp2}; lv_canvas_draw_line(_canvas,_l,2,&ld);
            }
        } else {
            rd.bg_color = col; rd.radius = LV_RADIUS_CIRCLE; rd.bg_opa = OPA_FULL;
            lv_canvas_draw_rect(_canvas,
                (lv_coord_t)(tx-UI_S(4)), (lv_coord_t)(ty-UI_S(4)), UI_S(8), UI_S(8), &rd);
        }

        // Name label (truncated)
        if (strlen(t.name) > 0) {
            char nl[8]; strncpy(nl, t.name, 7); nl[7]=0;
            td.color = col; td.font = FONT_TINY;
            lv_canvas_draw_text(_canvas,
                (lv_coord_t)(tx+UI_S(6)), (lv_coord_t)(ty-UI_S(6)), UI_S(60), &td, nl);
        }

        // Highlight selected
        if (i == _selIdx) {
            ld.color = CLR_ACCENT; ld.width = 1; ld.opa = OPA_FULL;
            for (int a=0;a<360;a+=10) {
                float a1=(a)*DEG_TO_RAD, a2=(a+10)*DEG_TO_RAD;
                lv_point_t p1={(lv_coord_t)(tx+UI_S(14)*sinf(a1)),(lv_coord_t)(ty-UI_S(14)*cosf(a1))};
                lv_point_t p2={(lv_coord_t)(tx+UI_S(14)*sinf(a2)),(lv_coord_t)(ty-UI_S(14)*cosf(a2))};
                lv_point_t _l[2]={p1,p2}; lv_canvas_draw_line(_canvas,_l,2,&ld);
            }
        }
    }
    lv_obj_invalidate(_canvas);
}

void AisScreen::showTargetInfo(int idx) {
    AisTarget t;
    int cnt;
    { auto lk=data.lock(); cnt=data.aisCount; if(idx>=0&&idx<cnt) t=data.aisTargets[idx]; }
    if (idx<0||idx>=cnt) { lv_obj_add_flag(_infoBox, LV_OBJ_FLAG_HIDDEN); return; }

    char buf[200];
    char cpaStr[12], tcpaStr[12];
    if (isnan(t.cpa))  snprintf(cpaStr,  sizeof(cpaStr),  "--");
    else               snprintf(cpaStr,  sizeof(cpaStr),  "%.2f", t.cpa);
    if (isnan(t.tcpa)) snprintf(tcpaStr, sizeof(tcpaStr), "--");
    else               snprintf(tcpaStr, sizeof(tcpaStr), "%.1f", t.tcpa);

    snprintf(buf, sizeof(buf),
        "%s  MMSI:%lu\nSOG:%.1fkn COG:%.0f°  CPA:%snm TCPA:%smin",
        strlen(t.name)>0?t.name:T(STR_AIS_UNKNOWN),
        (unsigned long)t.mmsi,
        isnan(t.sog)?0:t.sog,
        isnan(t.cog)?0:t.cog,
        cpaStr, tcpaStr);
    lv_label_set_text(_infoLbl, buf);
    lv_obj_clear_flag(_infoBox, LV_OBJ_FLAG_HIDDEN);
}

void AisScreen::onCanvasClick(lv_event_t *e) {
    AisScreen *self = (AisScreen*)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p; lv_indev_get_point(indev, &p);

    int cx = self->CS/2, cy = self->CS/2;
    int maxR = self->CS/2 - UI_S(10);
    float nm2px = (float)maxR / self->_rangeNm;

    // The tap arrives in SCREEN coordinates, so the canvas's own position on
    // the screen has to come off BOTH axes before comparing against a target's
    // canvas-relative position.
    //
    // This used to be derived as (SCREEN_W - CS) / 2 - the canvas's offset
    // inside its CONTAINER - which silently assumed the container sits at the
    // screen origin. On the 1024x600 boards it does not: the instrument block
    // starts at (88, 0) in landscape and (0, 88) in portrait. Every tap was
    // therefore evaluated 88 px away from where it landed, and with a pick
    // radius of UI_S(20) = 25 px no target could ever be selected under your
    // finger - you had to tap beside the ship. The 4-inch board's container IS
    // at (0, 0), which is why this survived unnoticed since the 600-grid port.
    //
    // lv_obj_get_coords() reports the absolute position, so this is correct on
    // every board and in every orientation. On the 4-inch it evaluates to
    // exactly the old numbers (x1 = 45 = (480-390)/2, y1 = 0).
    lv_area_t ca;
    lv_obj_get_coords(self->_canvas, &ca);

    float ownLat, ownLon;
    int best = -1; float bestDist = UI_S(20);
    {
        auto lk = data.lock();
        ownLat=data.lat; ownLon=data.lon;
        for (int i=0;i<data.aisCount;i++) {
            AisTarget &t=data.aisTargets[i];
            if (isnan(t.lat)||isnan(t.lon)||isnan(ownLat)) continue;
            float avgLat=(ownLat+t.lat)/2.0f*DEG_TO_RAD;
            float dx=(t.lon-ownLon)*60.0f*cosf(avgLat);
            float dy=(t.lat-ownLat)*60.0f;
            int tx=cx+(int)(dx*nm2px), ty=cy-(int)(dy*nm2px);
            // Convert screen click to canvas coordinates before comparing
            float d=sqrtf(powf(p.x - ca.x1 - tx, 2.f)+powf(p.y - ca.y1 - ty, 2.f));
            if (d<bestDist) { bestDist=d; best=i; }
        }
    }
    if (best == self->_selIdx) {
        self->_selIdx = -1;
        lv_obj_add_flag(self->_infoBox, LV_OBJ_FLAG_HIDDEN);
    } else {
        self->_selIdx = best;
        self->showTargetInfo(best);
    }
}

void AisScreen::update() {
    _rangeNm = appConfig.cfg.aisRange;
    char buf[16]; snprintf(buf, sizeof(buf), "%d%s", _rangeNm, T(STR_UNIT_NM));
    lv_label_set_text(_scaleLbl, buf);
    int cnt; { auto lk=data.lock(); cnt=data.aisCount; }
    snprintf(buf, sizeof(buf), "%d %s", cnt,
             cnt != 1 ? T(STR_AIS_TARGETS) : T(STR_AIS_TARGET_ONE));
    lv_label_set_text(_countLbl, buf);
    { auto lk=data.lock(); data.purgeAisTargets(); }
    drawRadar();
}
