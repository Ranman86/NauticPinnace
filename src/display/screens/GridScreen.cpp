#include "GridScreen.h"
#include "../../i18n/I18n.h"
#include <string.h>

// Maps config key names to DataModel fields (shared helper).
float GridScreen::getFieldValue(const char *key) { return dmFieldByKey(key); }

void GridScreen::create(lv_obj_t *parent) {
    container = lv_obj_create(parent);
    lv_obj_set_size(container, SCREEN_W, SCREEN_H - NAV_BAR_H);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, CLR_BG, 0);
    lv_obj_set_style_bg_opa(container, OPA_FULL, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    buildGrid();
}

void GridScreen::onShow() {
    // Rebuild cells if config has changed
    for (int i=0;i<MAX_CELLS;i++) {
        if (_cells[i].container) { lv_obj_del(_cells[i].container); _cells[i]={};  }
    }
    buildGrid();
}

// Per-cell rectangle + "big" flag (large hero field). Positions are computed
// for the chosen layout. Uniform grids fall back to rows×cols.
struct GCellRect { int x, y, w, h; bool big; };

static int gridLayoutRects(const GridConfig &gc, GCellRect *r, int maxN) {
    const int W = SCREEN_W, H = SCREEN_H - NAV_BAR_H, m = 4, g = 4;
    const int innerW = W - 2 * m;
    const char *L = gc.layout;
    auto isL = [&](const char *s) { return strcmp(L, s) == 0; };
    int n = 0;

    if (isL("hero1_2_2")) {                       // wide top + 2 + 2
        int heroH = (int)(H * 0.42);
        int rowH  = (H - m - heroH - 3 * g - m) / 2;
        r[n++] = { m, m, innerW, heroH, true };
        int cw = (innerW - g) / 2;
        for (int row = 0; row < 2; row++) {
            int y = m + heroH + g + row * (rowH + g);
            r[n++] = { m,            y, cw, rowH, false };
            r[n++] = { m + cw + g,   y, cw, rowH, false };
        }
        return n;
    }
    if (isL("hero1_2") || isL("hero1_3")) {        // wide top + one row
        int heroH = (int)(H * 0.55);
        r[n++] = { m, m, innerW, heroH, true };
        int y2 = m + heroH + g, botH = H - y2 - m;
        int cells = isL("hero1_3") ? 3 : 2;
        int cw = (innerW - (cells - 1) * g) / cells;
        for (int c = 0; c < cells; c++)
            r[n++] = { m + c * (cw + g), y2, cw, botH, false };
        return n;
    }

    // Uniform rows×cols
    int rows = max(1, min(gc.rows, 3));
    int cols = max(1, min(gc.cols, 3));
    int cw = (innerW - (cols - 1) * g) / cols;
    int ch = (H - 2 * m - (rows - 1) * g) / rows;
    for (int rr = 0; rr < rows && n < maxN; rr++)
        for (int cc = 0; cc < cols && n < maxN; cc++)
            r[n++] = { m + cc * (cw + g), m + rr * (ch + g), cw, ch, false };
    return n;
}

void GridScreen::buildGrid() {
    const GridConfig &gc = appConfig.cfg.grids[_slot];
    GCellRect rects[MAX_CELLS];
    _count = gridLayoutRects(gc, rects, MAX_CELLS);
    Serial.printf("[Grid] buildGrid slot=%d layout='%s' cells=%d\n", _slot, gc.layout, _count);
    Serial.flush();

    for (int idx = 0; idx < _count; idx++) {
        // Yield every cell so the WiFi beacon ISR gets a SPI0 window (IWDT safety).
        vTaskDelay(pdMS_TO_TICKS(2));
        const GridCell &cfg = gc.cells[idx];
        Cell &cell = _cells[idx];
        cell.container = lv_obj_create(container);
        lv_obj_set_size(cell.container, rects[idx].w, rects[idx].h);
        lv_obj_set_pos(cell.container, rects[idx].x, rects[idx].y);
        styleCard(cell.container);
        lv_obj_clear_flag(cell.container, LV_OBJ_FLAG_SCROLLABLE);

        cell.lblLabel = lv_label_create(cell.container);
        const char *labelText = strlen(cfg.label) > 0 ? cfg.label : i18nFieldName(cfg.pgn);
        lv_label_set_text(cell.lblLabel, labelText);
        styleLabel(cell.lblLabel, FONT_SMALL, CLR_TEXT_DIM);
        lv_obj_align(cell.lblLabel, LV_ALIGN_TOP_MID, 0, 0);

        // Hero cell uses the 192 px font (FONT_DEPTH_XL covers digits . - space);
        // others scale to their height. Grid values are numeric, so the limited
        // glyph set of the big font is sufficient.
        int hh = rects[idx].h;
        const lv_font_t *vFont = rects[idx].big ? FONT_DEPTH_XL :
                                 (hh > 100) ? FONT_HUGE :
                                 (hh > 75)  ? FONT_XXL  :
                                 (hh > 55)  ? FONT_XL   : FONT_LARGE;
        cell.lblValue = lv_label_create(cell.container);
        lv_label_set_text(cell.lblValue, "--");
        styleLabel(cell.lblValue, vFont, CLR_TEXT);
        lv_obj_align(cell.lblValue, LV_ALIGN_CENTER, 0, 4);

        cell.lblUnit = lv_label_create(cell.container);
        lv_label_set_text(cell.lblUnit, cfg.unit);
        styleLabel(cell.lblUnit, FONT_TINY, CLR_TEXT_DIM);
        lv_obj_align(cell.lblUnit, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }
}

void GridScreen::update() {
    const GridConfig &gc = appConfig.cfg.grids[_slot];

    for (int i=0; i<_count && i<MAX_CELLS; i++) {
        if (!_cells[i].container) continue;
        const GridCell &cfg = gc.cells[i];
        float val = getFieldValue(cfg.pgn);
        char buf[20];
        fmtVal(buf, sizeof(buf), val, cfg.decimals);
        lv_label_set_text(_cells[i].lblValue, buf);
        // Stale data dim
        lv_color_t col = isnan(val) ? CLR_TEXT_DIM : CLR_TEXT;
        lv_obj_set_style_text_color(_cells[i].lblValue, col, 0);
    }
}
