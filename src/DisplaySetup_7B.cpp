// ============================================================================
//  DisplaySetup_7B.cpp - display and touch backend for the
//  Waveshare ESP32-S3-Touch-LCD-7B (1024x600).
//
//  This is the 7B's counterpart to DisplaySetup.cpp. Exactly one of the two is
//  compiled, selected by the PlatformIO env's build_src_filter.
//
//  WHY A SEPARATE FILE AT ALL: the 4" board is driven through LovyanGFX. On
//  this panel LovyanGFX produces nothing but a grey haze - proven by sweeping
//  every polarity combination through it without ever getting a picture, then
//  getting one immediately through the ESP-IDF esp_lcd RGB API, which is also
//  what Waveshare's own example uses. So the 7B talks to esp_lcd directly.
//
//  That turned out to be cheap: `gfx` was only ever used inside
//  DisplaySetup.cpp, so the rest of the firmware is untouched. This file just
//  has to provide the same eight functions the header declares.
//
//  Every panel parameter used here was measured on the real unit, see the
//  reasoning in BoardConfig_7B.h.
// ============================================================================
#include "DisplaySetup.h"
#include "Entropy.h"
#include "display/DisplayManager.h"
#include "display/UiConfig.h"
#include "display/Theme.h"       // uiPortrait() - swipe guard axis
#include "config/Config.h"        // display.rotation
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>            // frame instrumentation, see dispPerfTake()
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"

// ---- CH422G IO expander ------------------------------------------------------
// Two-byte write [register, value] to 0x24 - same protocol as the 4" board's
// CH32V003, only the pin assignment differs.
//
// The output shadow starts at 0xFF, i.e. every pin HIGH. That is not cosmetic:
// EXIO6 carries the LCD supply and EXIO3 the panel reset, and the vendor code
// relies on this default rather than setting them explicitly. Starting from 0
// leaves the panel unpowered - the firmware runs, the expander answers, the
// frame buffer allocates, and the screen stays black.
// It also puts EXIO5 high, which selects CAN rather than USB on the shared
// transceiver - which is what NMEA 2000 needs, so do not clear it.
static uint8_t s_exio = 0xFF;

static bool ch422_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(CH422_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool exio_set(uint8_t pin, bool high) {
    if (high) s_exio |=  (uint8_t)(1u << pin);
    else      s_exio &= (uint8_t)~(1u << pin);
    return ch422_write(CH422_REG_OUT, s_exio);
}

// This board has no buzzer on the expander - the anchor alarm is silent here.
void boardBuzzer(bool on) { (void)on; }

// Backlight. EXIO2 is a plain on/off line on this board (the vendor drives it
// digitally); the CH422G does have a PWM register but its polarity is not
// documented for this variant, so anything non-zero simply means "on".
void setBrightness(uint8_t brightness) {
    exio_set(EXIO_BACKLIGHT, brightness ? LCD_BL_ON_LEVEL : !LCD_BL_ON_LEVEL);
}

// ---- GT911 touch -------------------------------------------------------------
// 16-bit register addresses, same bus as the expander. Unlike the 4" board we
// do NOT write a configuration: this panel ships configured for 1024x600 and
// the vendor example only ever reads it.
static uint8_t s_touchAddr = TOUCH_ADDR;

static bool gt911_i2c_read(uint16_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(s_touchAddr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    uint8_t got = Wire.requestFrom((uint8_t)s_touchAddr, len);
    for (uint8_t i = 0; i < got; i++) buf[i] = Wire.read();
    return got == len;
}

static bool gt911_i2c_write8(uint16_t reg, uint8_t val) {
    Wire.beginTransmission(s_touchAddr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool gt911_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// The GT911 latches its address from the INT level as reset is released:
// INT low -> 0x5D, INT high -> 0x14. We drive INT low for the default, then
// hand the pin back as an input. Without this the chip stays in reset and does
// not appear on the bus at all.
static void gt911_reset_and_detect() {
    pinMode(TOUCH_INT, OUTPUT);
    digitalWrite(TOUCH_INT, LOW);
    exio_set(EXIO_TOUCH_RST, false);
    delay(10);
    exio_set(EXIO_TOUCH_RST, true);
    delay(10);
    delay(50);
    pinMode(TOUCH_INT, INPUT);

    if (gt911_probe(TOUCH_ADDR))          s_touchAddr = TOUCH_ADDR;
    else if (gt911_probe(TOUCH_ADDR_ALT)) s_touchAddr = TOUCH_ADDR_ALT;
    Serial.printf("[GT911] address 0x%02X %s\n", s_touchAddr,
                  gt911_probe(s_touchAddr) ? "responding" : "NOT RESPONDING");
}

static bool gt911_read_touch(uint16_t *x, uint16_t *y) {
    uint8_t status = 0;
    if (!gt911_i2c_read(0x814E, &status, 1)) return false;
    if (!(status & 0x80)) return false;          // buffer not ready

    const uint8_t points = status & 0x0F;
    bool got = false;
    if (points > 0) {
        uint8_t p[4];
        if (gt911_i2c_read(0x8150, p, 4)) {
            *x = (uint16_t)(p[0] | (p[1] << 8));
            *y = (uint16_t)(p[2] | (p[3] << 8));
            got = (*x < LCD_WIDTH && *y < LCD_HEIGHT);
        }
    }
    gt911_i2c_write8(0x814E, 0);                 // clear the ready flag
    return got;
}

void gt911Diag() {
    uint8_t status = 0;
    if (!gt911_i2c_read(0x814E, &status, 1)) {
        Serial.println("[GT911] no answer");
        return;
    }
    uint16_t x = 0, y = 0;
    uint8_t p[4];
    if ((status & 0x0F) && gt911_i2c_read(0x8150, p, 4)) {
        x = (uint16_t)(p[0] | (p[1] << 8));
        y = (uint16_t)(p[2] | (p[3] << 8));
    }
    Serial.printf("[GT911] addr 0x%02X status 0x%02X points %u  x=%u y=%u\n",
                  s_touchAddr, status, status & 0x0F, x, y);
}

void displayDiag() {
    uint8_t in = 0;
    Wire.beginTransmission(CH422_I2C_ADDR);
    Wire.write(CH422_REG_IN);
    const bool ok = (Wire.endTransmission(false) == 0) &&
                    (Wire.requestFrom((uint8_t)CH422_I2C_ADDR, (uint8_t)1) == 1);
    if (ok) in = Wire.read();
    Serial.printf("[disp] CH422G out=0x%02X in=0x%02X (%s)  panel %dx%d @%u Hz\n",
                  s_exio, in, ok ? "ok" : "read failed",
                  LCD_WIDTH, LCD_HEIGHT, (unsigned)LCD_PCLK_HZ);
    Serial.flush();
}

// ---- esp_lcd RGB panel -------------------------------------------------------
static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;
// Panel frame buffer, fetched once after init. Only used for the rotated
// flush path, which writes transposed pixels straight into it.
static lv_color_t *s_fb = nullptr;
static uint32_t s_flushCount = 0;

// How often LVGL is allowed to refresh the display, in milliseconds. This is
// NOT LV_DISP_DEF_REFR_PERIOD: that macro is 33 ms here because lv_anim.c uses
// it as the animation timestep, and animations want to be fast. The refresh
// itself has to stay slower than that, because one 600-grid render plus flush
// is tens of milliseconds and back-to-back refreshes would leave core 1 no
// idle time for AsyncTCP and lwIP - which on this board is the difference
// between a web UI that answers and one that stalls mid-page.
// Applied once, right after lv_disp_drv_register(). Tune it here.
static const uint32_t LVGL_REFR_PERIOD_MS = 60;

// ---- Frame instrumentation ---------------------------------------------------
// Nothing in here ever prints. The numbers are collected in the flush path and
// in LVGL's monitor_cb, and handed out once per heartbeat (main.cpp). A
// Serial.printf from inside a per-strip callback floods the UART and distorts
// the very timing it is meant to measure - this project has been bitten by
// exactly that before.
//
// s_refrTotal is MONOTONIC on purpose. It has two independent consumers that
// both want "how many since I last looked": getTickFps() for the on-screen
// perf overlay, dispPerfTake() for the heartbeat. One reset-on-read counter
// would hand each of them only the frames the other had not already eaten, so
// both would read low. Each consumer keeps its own baseline instead.
static volatile uint32_t s_refrTotal   = 0;   // completed LVGL refreshes
static uint32_t          s_renderMaxMs = 0;   // worst refresh (draw + flush)
static uint32_t          s_flushMaxUs  = 0;   // worst single flush_cb body

// How many flush strips make up ONE refresh. s_flushMaxUs alone cannot say
// whether the flush is trivial or two thirds of the render - that needs the
// count it has to be multiplied by, and the draw buffer is only 5 rows, so the
// count is large and worth knowing rather than deriving on paper.
//
// The refresh boundary is monitor_cb, NOT lv_disp_flush_is_last(). Both are
// available, but monitor_cb is the one already trusted for s_refrTotal and
// s_renderMaxMs, so "strips per refresh" and "worst refresh" then describe
// exactly the same event and multiply cleanly. lv_refr.c calls monitor_cb after
// refr_invalid_areas() has returned, and our flush is synchronous, so every
// strip of that refresh is already counted when the boundary arrives. It also
// keeps the flush path at one increment with no branch - is_last would add a
// test to a callback that runs ~128 times a frame.
//
// s_stripsCur is the in-flight count and is reset by monitor_cb ALONE.
// s_stripsAtMax and s_pxAtMax are captured INSIDE the same if-block that sets
// s_renderMaxMs, so all four describe one and the same refresh. They were not,
// originally: the strip count was an independent window maximum, which meant
// "draw / strips" could divide a 226 ms frame by a strip count belonging to a
// different one. Each has exactly ONE consumer (dispPerfTake), so unlike
// s_refrTotal above they can safely reset on read.
static uint32_t          s_stripsCur   = 0;   // strips of the refresh in progress
static uint32_t          s_stripsAtMax = 0;   // strips of the WORST refresh in the window
static uint32_t          s_pxAtMax     = 0;   // pixels LVGL reports for that same refresh

// Flush time SUMMED over one refresh, not just the worst strip. The worst
// strip alone cannot answer the question that matters - how much of a refresh
// is LVGL drawing and how much is getting the pixels to the panel - because
// multiplying it by the strip count assumes every strip cost the maximum.
// The sum is captured for the SAME refresh that set s_renderMaxMs, so
// "draw = rend - flushSum" is a subtraction of two figures from one frame
// rather than of two unrelated maxima.
static uint32_t s_flushSumCur   = 0;   // us accumulated in the refresh in progress
static uint32_t s_flushSumAtMax = 0;   // us of the worst refresh in the window

static inline void flushTimeDone(int64_t t0) {
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_flushMaxUs) s_flushMaxUs = us;
    s_flushSumCur += us;
}

static void lvgl_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area,
                          lv_color_t *color_p) {
    // No esp_task_wdt_reset() here: the task is not subscribed to the TWDT,
    // and on IDF 5.x the call logs "task not found" on every single flush.
    s_flushCount++;
    // Strips of the refresh currently being drawn. Deliberately BEFORE the
    // timing start below, so the existing per-strip `flush` figure keeps
    // measuring exactly what it measured before this counter existed.
    s_stripsCur++;
    if (s_flushCount <= 3) {
        Serial.printf("[flush#%u] (%d,%d)-(%d,%d)\n", s_flushCount,
                      area->x1, area->y1, area->x2, area->y2);
        Serial.flush();
    }
    // Time the whole body from here - after the three bring-up prints above, so
    // a blocking Serial.flush() does not end up in the measurement, and OUTSIDE
    // the pixel loops below, which run up to LCD_WIDTH*5 times per call: a
    // timer read in there would cost more than the copy it is timing.
    const int64_t tFlush0 = esp_timer_get_time();
    // ---- rotated: transpose straight into the frame buffer -----------------
    // LVGL can rotate for us (disp_drv.sw_rotate), but that costs a 10 KB
    // scratch buffer out of the LVGL pool - the same pool whose exhaustion
    // once rebooted this device - AND a full extra copy per strip: first
    // rotate into the scratch, then copy that into the frame buffer.
    //
    // We can skip both. LVGL only rotates when sw_rotate is set (lv_refr.c
    // guards it), while lv_disp_get_hor_res() swaps the resolution based on
    // `rotated` alone. So the UI already thinks and lays out in the rotated
    // resolution, we simply receive the strip UNROTATED in logical coordinates
    // and place each pixel ourselves - one pass, zero scratch memory.
    //
    // The loop nesting is deliberate: the INNER loop walks the axis that maps
    // to consecutive frame-buffer addresses, so the PSRAM writes stay
    // sequential (the reads from the draw buffer take the strided hit
    // instead, and those are cached internal RAM).
    //
    // The mapping MUST be the one LVGL itself uses (lv_refr.c draw_buf_rotate),
    // because LVGL still rotates the TOUCH coordinates on its own - it does
    // that from `rotated` alone, with no sw_rotate gate. Picking our own
    // (equally valid, but mirrored) convention here would leave input and
    // output disagreeing, which is exactly how the first version killed touch.
    // `rotated` is a 2-bit bitfield, so it needs the explicit cast back.
    const lv_disp_rot_t rot = (lv_disp_rot_t)disp->rotated;
    if (rot != LV_DISP_ROT_NONE && s_fb) {
        const int w = area->x2 - area->x1 + 1;
        const int h = area->y2 - area->y1 + 1;
        if (rot == LV_DISP_ROT_180) {
            // 180 degrees is not a transpose - rows stay rows, only both axes
            // reverse. So the nesting has to be the OTHER way round from the
            // two quarter turns: row by row, walking the strip backwards so the
            // frame-buffer writes still run forwards. Folding this case into
            // the loop below (as the first version did) made every single write
            // jump a whole 2 KB scan line, and 180 is the rotation a display
            // mounted upside down actually uses.
            for (int ly = 0; ly < h; ly++) {
                const int py = LCD_HEIGHT - 1 - (area->y1 + ly);
                lv_color_t       *dst = &s_fb[(size_t)py * LCD_WIDTH];
                const lv_color_t *src = &color_p[(size_t)ly * w];
                for (int lx = w - 1; lx >= 0; lx--)
                    dst[LCD_WIDTH - 1 - (area->x1 + lx)] = src[lx];
            }
        } else {
            for (int lx = 0; lx < w; lx++) {
                const int gx = area->x1 + lx;
                for (int ly = 0; ly < h; ly++) {
                    const int gy = area->y1 + ly;
                    const int px = (rot == LV_DISP_ROT_90) ? gy : (LCD_WIDTH - 1 - gy);
                    const int py = (rot == LV_DISP_ROT_90) ? (LCD_HEIGHT - 1 - gx) : gx;
                    s_fb[(size_t)py * LCD_WIDTH + px] = color_p[(size_t)ly * w + lx];
                }
            }
        }
        lv_disp_flush_ready(disp);
        flushTimeDone(tFlush0);
        return;
    }

    // esp_lcd takes an exclusive end coordinate, LVGL an inclusive one.
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_p);
    lv_disp_flush_ready(disp);
    flushTimeDone(tFlush0);
}

// Called by LVGL once per COMPLETED refresh (lv_refr.c, and only when something
// was actually invalidated), with the elapsed milliseconds and the number of
// pixels rendered. Because our flush is synchronous - lvgl_flush_cb writes the
// pixels and calls lv_disp_flush_ready before it returns - that elapsed time
// already includes the flush.
//
// This is also the honest "a frame finished" event, which is why the frame
// counter now lives here. It used to be incremented in the flush path whenever
// a strip happened to touch the last row of the screen. That is a geometric
// guess at something LVGL will simply tell us, and it was wrong twice over:
//  * LVGL redraws only the INVALIDATED area. A refresh that repaints the
//    instrument canvas, a label or a sidebar cell never extends to the bottom
//    edge, so it was not counted at all - and since that is nearly every
//    refresh, the reading could sit at 0 indefinitely, which reads like a hang
//    on a perfectly healthy device.
//  * the two branches did not even measure the same axis: the direct
//    frame-buffer path compared against lv_disp_get_ver_res(), which is the
//    LOGICAL height and swaps in portrait, while the unrotated path compared
//    against the physical LCD_HEIGHT.
// monitor_cb fires exactly once per refresh in every orientation, so all of
// that guesswork goes away.
static void lvgl_monitor_cb(lv_disp_drv_t *drv, uint32_t timeMs, uint32_t px) {
    (void)drv;
    s_refrTotal++;
    // EVERYTHING that describes the worst refresh is captured in ONE block, so
    // the heartbeat's arithmetic divides figures from the same frame.
    //
    // This used to be half-right and it cost an afternoon: the flush sum was
    // paired here deliberately, with a comment saying why, while the strip
    // count was taken as an INDEPENDENT maximum one line below - so
    // "draw / strips" could divide a 226 ms frame by a strip count belonging
    // to a different one. Pair them, and take LVGL's own pixel count too: `px`
    // was being discarded, and it is the only figure that says how much was
    // actually drawn rather than how much we assume was drawn.
    if (timeMs > s_renderMaxMs) {
        s_renderMaxMs   = timeMs;
        s_flushSumAtMax = s_flushSumCur;
        s_stripsAtMax   = s_stripsCur;
        s_pxAtMax       = px;
    }
    s_stripsCur   = 0;
    s_flushSumCur = 0;
}

// ---- swipe tracking (same behaviour as the 4" board) -------------------------
static bool     s_swipe_active   = false;
static bool     s_swipe_done     = false;
static bool     s_swipe_suppress = false;
static uint16_t s_swipe_start_x  = 0;
static uint16_t s_swipe_start_y  = 0;
static uint16_t s_swipe_last_x   = 0;
static uint16_t s_swipe_last_y   = 0;

void swipeSuppress() { s_swipe_suppress = true; }

// Config degrees -> LVGL enum. Anything unexpected means "not rotated".
static lv_disp_rot_t rotationToLv(uint16_t deg) {
    switch (deg) {
        case 90:  return LV_DISP_ROT_90;
        case 180: return LV_DISP_ROT_180;
        case 270: return LV_DISP_ROT_270;
        default:  return LV_DISP_ROT_NONE;
    }
}

// The GT911 always reports PHYSICAL panel coordinates. LVGL turns those into
// logical ones ITSELF, in indev_pointer_proc(), whenever the driver's
// `rotated` is set - so data->point must stay RAW. Rotating it here as well
// transformed every tap twice and pushed most of them clean off the screen,
// which is what made touch look dead after the rotation feature landed.
//
// The swipe tracker below does need logical coordinates (it decides "did this
// start on the rail or in the instrument area"), so we compute them
// separately, with the very same formula LVGL uses - hor/ver_res stay the
// PHYSICAL 1024x600 in the driver struct, only lv_disp_get_*_res() swaps.
static void touchToLogical(uint16_t &x, uint16_t &y) {
    const uint16_t px = x, py = y;
    // Read the DRIVER, not the config. They agree in practice - the WebUI
    // reboots after a rotation change - but the driver flag is what LVGL and
    // uiPortrait() go by, and taking the same source everywhere removes a
    // whole class of "the two disagree for a moment" bug.
    lv_disp_t *d = lv_disp_get_default();
    switch (d && d->driver ? (lv_disp_rot_t)d->driver->rotated : LV_DISP_ROT_NONE) {
        case LV_DISP_ROT_90:
            x = (uint16_t)(LCD_HEIGHT - 1 - py);
            y = px;
            break;
        case LV_DISP_ROT_180:
            x = (uint16_t)(LCD_WIDTH  - 1 - px);
            y = (uint16_t)(LCD_HEIGHT - 1 - py);
            break;
        case LV_DISP_ROT_270:
            x = py;
            y = (uint16_t)(LCD_WIDTH - 1 - px);
            break;
        default:
            break;   // not rotated - logical == physical
    }
}

static void lvgl_touch_cb(lv_indev_drv_t *indev, lv_indev_data_t *data) {
    uint16_t x = 0, y = 0;
    const bool pressed = gt911_read_touch(&x, &y);
    uint16_t lx = x, ly = y;
    if (pressed) touchToLogical(lx, ly);   // for the swipe tracker only

    if (pressed) {
        Entropy::feed(lx, ly);
        s_swipe_last_x = lx;
        s_swipe_last_y = ly;
        if (!s_swipe_active) {
            s_swipe_active   = true;
            s_swipe_done     = false;
            s_swipe_suppress = false;
            s_swipe_start_x  = lx;
            s_swipe_start_y  = ly;
            dispMgr.requestShowNavArrows();
        }
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = x;   // RAW - LVGL rotates this itself
        data->point.y = y;
    } else {
        if (s_swipe_active && !s_swipe_done) {
            const int dx = (int)s_swipe_last_x - (int)s_swipe_start_x;
            const int dy = (int)s_swipe_last_y - (int)s_swipe_start_y;
            const int adx = dx < 0 ? -dx : dx;
            const int ady = dy < 0 ? -dy : dy;
            // A swipe only counts if it STARTS inside the instrument area -
            // drags on the rail (buttons) or the sidebar must never flip
            // screens. The old LCD_WIDTH/6 dead zones belonged to the
            // overlay-arrow layout that these boards no longer have.
            //
            // Which AXIS carries the rail depends on the orientation: in
            // landscape the three blocks sit side by side (guard on x), in
            // portrait they are stacked (guard on y). Coordinates here are
            // already rotated, so they are logical screen coordinates.
            const bool notOnButton =
                uiPortrait()
                    ? (s_swipe_start_y > UI7_RAIL_W + 10 &&
                       s_swipe_start_y < UI7_RAIL_W + UI7_CENTER_W - 10)
                    : (s_swipe_start_x > UI7_CENTER_X + 10 &&
                       s_swipe_start_x < UI7_SIDEBAR_X - 10);
            if (adx >= UI_SWIPE_THRESHOLD && adx > ady && notOnButton &&
                !s_swipe_suppress) {
                s_swipe_done = true;
                if (dx < 0) dispMgr.nextScreen();
                else        dispMgr.prevScreen();
            }
        }
        s_swipe_active = false;
        data->state = LV_INDEV_STATE_REL;
    }
}

// ---- Public ------------------------------------------------------------------

void displayInit() {
    Wire.begin(CH422_I2C_SDA, CH422_I2C_SCL, 400000);

    if (!ch422_write(CH422_REG_MODE, 0xFF))
        Serial.println("[disp] !!! CH422G did not ACK - check the I2C bus");
    ch422_write(CH422_REG_OUT, s_exio);      // EXIO6 = LCD supply on
    delay(50);

    gt911_reset_and_detect();

    exio_set(EXIO_LCD_RST, false);
    delay(20);
    exio_set(EXIO_LCD_RST, true);
    delay(120);

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src    = LCD_CLK_SRC_PLL160M;
    cfg.data_width = LCD_RGB_DATA_WIDTH;
#if ESP_IDF_VERSION_MAJOR >= 5
    // BOUNCE BUFFERS - the whole reason this board runs a newer core.
    // Without them the RGB DMA reads the frame buffer straight out of PSRAM,
    // and as soon as the application writes its canvases into that same PSRAM
    // the fetch starves and the picture smears horizontally. Measured: solid in
    // an idle bring-up at 40 MHz, torn under the full firmware even at 30 MHz.
    // With bounce buffers the DMA reads from small internal-RAM buffers that
    // are refilled in the background, which is what decouples the two.
    // Two buffers of LCD_WIDTH*10 px are allocated internally, ~40 KB total.
    cfg.num_fbs = 1;
    cfg.bounce_buffer_size_px = LCD_BOUNCE_BUF;
    cfg.dma_burst_size = 64;
    // Frees cache pressure: the frame buffer is read once per frame straight
    // out of PSRAM, so keeping it cached buys nothing and costs bandwidth.
    cfg.flags.bb_invalidate_cache = 1;
#else
    cfg.psram_trans_align = 64;
    cfg.sram_trans_align  = 4;
#endif
    cfg.hsync_gpio_num = LCD_HSYNC;
    cfg.vsync_gpio_num = LCD_VSYNC;
    cfg.de_gpio_num    = LCD_DE;
    cfg.pclk_gpio_num  = LCD_PCLK;
    cfg.disp_gpio_num  = LCD_DISP;
    const int dpins[16] = { LCD_D0,  LCD_D1,  LCD_D2,  LCD_D3,
                            LCD_D4,  LCD_D5,  LCD_D6,  LCD_D7,
                            LCD_D8,  LCD_D9,  LCD_D10, LCD_D11,
                            LCD_D12, LCD_D13, LCD_D14, LCD_D15 };
    for (int i = 0; i < 16; ++i) cfg.data_gpio_nums[i] = dpins[i];

    cfg.timings.pclk_hz = LCD_PCLK_HZ;
    cfg.timings.h_res   = LCD_WIDTH;
    cfg.timings.v_res   = LCD_HEIGHT;
    cfg.timings.hsync_pulse_width = LCD_HSYNC_PULSE;
    cfg.timings.hsync_back_porch  = LCD_HSYNC_BACK;
    cfg.timings.hsync_front_porch = LCD_HSYNC_FRONT;
    cfg.timings.vsync_pulse_width = LCD_VSYNC_PULSE;
    cfg.timings.vsync_back_porch  = LCD_VSYNC_BACK;
    cfg.timings.vsync_front_porch = LCD_VSYNC_FRONT;
    cfg.timings.flags.pclk_active_neg = LCD_PCLK_ACTIVE_NEG;
    cfg.timings.flags.de_idle_high    = LCD_DE_IDLE_HIGH;   // measured: must be 0
    cfg.timings.flags.hsync_idle_low  = LCD_SYNC_IDLE_LOW;
    cfg.timings.flags.vsync_idle_low  = LCD_SYNC_IDLE_LOW;
    cfg.flags.fb_in_psram = 1;          // 1024*600*2 = 1.2 MB, PSRAM only

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    Serial.printf("[disp] esp_lcd_new_rgb_panel -> %s\n", esp_err_to_name(err));
    if (err != ESP_OK) {
        Serial.println("[disp] !!! FATAL: panel creation failed");
        while (1) delay(1000);
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);

    setBrightness(255);

    lv_init();

    // 20 rows per buffer, as on the 4" board - but 1024 px wide, so 40 KB each
    // and 80 KB of internal DMA RAM in total. Worth remembering: free internal
    // DRAM is the shared budget of LVGL and lwIP, and that has bitten this
    // project before.
    // ONE LVGL draw buffer, on purpose. Our flush_cb is synchronous - it
    // memcpys into the frame buffer via esp_lcd_panel_draw_bitmap and calls
    // lv_disp_flush_ready before returning - so a second buffer never bought
    // any render/flush overlap; it only pinned 20 KB of internal DMA RAM.
    // That 20 KB is what decides whether the AsyncTCP task (web server) can
    // start once SoftAP has taken its share. Negotiate the height downward
    // rather than refusing to boot.
    Serial.printf("[disp] free internal DMA RAM before buffers: %u bytes\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    // Start at 10 rows (20 KB), NOT 20: with a synchronous flush a bigger
    // buffer only reduces flush_cb call overhead a little, while this 20 KB
    // is exactly the margin the AsyncTCP task needs to start once SoftAP has
    // taken its share of the heap. (First version of this negotiation started
    // at 20 rows and thereby saved nothing over the old double buffer.)
    // 5 rows since the menu-crash hunt: the 10 KB difference to the original
    // 10 rows went into LV_MEM_SIZE instead (lv_conf.h) - a starved LVGL pool
    // reboots the device mid-render, a smaller flush chunk costs nothing
    // visible (the flush is synchronous anyway).
    // The step below used to be `rows /= 2`, which made the negotiation
    // described above pure fiction: 5 / 2 = 2, and 2 >= 3 is false, so the
    // body ran exactly ONCE and a failed allocation fell straight through to
    // the FATAL spin a few lines down - the very outcome the loop exists to
    // avoid. Step down one row at a time instead (5, 4, 3, 2). There is no
    // divisibility constraint on this buffer; lv_disp_draw_buf_init() only
    // wants a pixel count, and the flush is synchronous, so a shorter strip
    // just means more flush_cb calls per frame and nothing visible.
    // Two rows (4 KB) is a deliberately poor but BOOTING last resort: a device
    // that comes up slightly slower can still be reached over WiFi and
    // reconfigured, one stuck in the spin below cannot.
    size_t buf_sz = 0;
    // 2026-08-22, back up to 10 rows, and this time with a measurement behind
    // it rather than a guess. The heartbeat instrumentation showed a
    // FULL-SCREEN refresh (any screen change, or opening the home launcher)
    // costing 820-948 ms, of which only ~108 ms is flush: 120 strips at
    // ~0.9 ms. The other ~800 ms is LVGL's own drawing, and a large part of
    // that is per-strip work - for every one of the 120 strips it re-walks the
    // object tree, re-clips and re-draws each intersecting object's slice.
    // Doubling the buffer halves the strips to 60 and therefore halves that
    // repetition. Costs 10,240 bytes of internal DRAM, which the Tier-A work
    // earlier today made available (free DRAM went 18.6 KB -> 44 KB).
    //
    // The note below about financing the LVGL pool still stands - the pool is
    // NOT being reduced again to pay for this; both now fit.
    for (int rows = 10; rows >= 2; rows--) {
        buf_sz = LCD_WIDTH * rows * sizeof(lv_color_t);
        buf1 = (lv_color_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (buf1) {
            Serial.printf("[disp] LVGL single buffer: %d rows, %u bytes\n",
                          rows, (unsigned)buf_sz);
            break;
        }
    }
    buf2 = nullptr;
    if (!buf1) {
        Serial.println("[disp] !!! FATAL: cannot allocate any LVGL draw buffer");
        while (1) delay(1000);
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_sz / sizeof(lv_color_t));

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    // hor_res/ver_res stay the PHYSICAL panel size. LVGL swaps them itself for
    // 90/270 (lv_disp_get_hor_res), which is what uiScreenW()/uiScreenH() read.
    disp_drv.hor_res  = LCD_WIDTH;
    disp_drv.ver_res  = LCD_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    // Frame instrumentation: fires once per completed refresh, see the callback.
    disp_drv.monitor_cb = lvgl_monitor_cb;
    // Screen rotation (config: display.rotation). The RGB panel cannot rotate
    // in hardware - there is no controller, just timings - so this is LVGL's
    // software rotation: it transforms every flushed strip before handing it
    // to us. Costs CPU and turns wide rows into narrow columns in the frame
    // buffer, so it is only paid for when actually rotated.
    // Rotation is NOT set here: displayInit() runs before the config is loaded
    // (LVGL has to exist first), so the value would always be the default 0.
    // main.cpp calls displayApplyRotation() right after appConfig.begin().
    lv_disp_drv_register(&disp_drv);

    // Split the two jobs LV_DISP_DEF_REFR_PERIOD used to do at once. The macro
    // stays low (33 ms) because lv_anim.c takes it as the animation timestep;
    // the display refresh timer - created by lv_disp_drv_register() a line
    // above, with the macro as its period - is slowed back down here to a value
    // that leaves core 1 idle time between frames. Only reachable AFTER the
    // register call: that is where lv_disp_t and its refr_timer come into
    // existence.
    {
        lv_disp_t *d = lv_disp_get_default();
        if (d && d->refr_timer) lv_timer_set_period(d->refr_timer, LVGL_REFR_PERIOD_MS);
        Serial.printf("[disp] LVGL refresh %u ms, animation step %u ms\n",
                      (unsigned)LVGL_REFR_PERIOD_MS,
                      (unsigned)LV_DISP_DEF_REFR_PERIOD);
    }

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);

    Serial.printf("[disp] ready: %dx%d @ %.1f MHz\n",
                  LCD_WIDTH, LCD_HEIGHT, LCD_PCLK_HZ / 1e6);
}

// Completed refreshes since THIS consumer (the on-screen perf overlay in
// DisplayManager.cpp) last read with reset. The shared counter is deliberately
// not cleared here - see s_refrTotal above.
uint32_t getTickFps(bool reset) {
    static uint32_t consumed = 0;
    const uint32_t total = s_refrTotal;
    const uint32_t v = total - consumed;   // unsigned: survives the wrap
    if (reset) consumed = total;
    return v;
}

// Second, independent readout of the same frame counter, for the 5 s heartbeat
// in main.cpp. Returns the completed refreshes since the previous call plus the
// worst case seen in that same window: `renderMaxMs` is one whole LVGL refresh
// (layout + draw + flush, from monitor_cb), `flushMaxUs` is one flush_cb body,
// and `stripsMax` is how many such bodies the worst refresh needed - the factor
// that turns a per-strip microsecond figure into a per-frame one.
// Declared extern at the call site rather than in DisplaySetup.h, because that
// header is shared with the 4" board, which has no such counters.
void dispPerfTake(uint32_t *frames, uint32_t *renderMaxMs, uint32_t *flushMaxUs,
                  uint32_t *stripsMax, uint32_t *flushSumUs,
                  uint32_t *pxAtMax) {
    static uint32_t taken = 0;
    const uint32_t total = s_refrTotal;
    if (frames)      *frames      = total - taken;
    if (renderMaxMs) *renderMaxMs = s_renderMaxMs;
    if (flushMaxUs)  *flushMaxUs  = s_flushMaxUs;
    if (stripsMax)   *stripsMax   = s_stripsAtMax;
    if (flushSumUs)  *flushSumUs  = s_flushSumAtMax;
    if (pxAtMax)     *pxAtMax     = s_pxAtMax;
    taken           = total;
    s_renderMaxMs   = 0;
    s_flushMaxUs    = 0;
    // Paired with s_renderMaxMs above: both describe the same worst refresh,
    // so they must be cleared in the same breath or the next window would
    // subtract a flush sum belonging to a frame that is already gone.
    s_flushSumAtMax = 0;
    // Cleared with the pair above, all four describing one frame. s_stripsCur
    // and s_flushSumCur belong to the refresh that may be half-finished right
    // now and are monitor_cb's alone - zeroing them here would quietly
    // undercount that frame.
    s_stripsAtMax = 0;
    s_pxAtMax     = 0;
}

void displayTick() {
    // No esp_task_wdt_reset() - see lvgl_flush_cb. This was the main source
    // of the ~100 lines/s "task_wdt: task not found" serial flood.
    lv_timer_handler();
}

// Apply the configured rotation to the already-registered driver. Separate
// from displayInit() purely because of ordering: the config is only on disk
// by the time main.cpp has mounted LittleFS.
void displayApplyRotation() {
    lv_disp_t *d = lv_disp_get_default();
    if (!d || !d->driver) return;
    lv_disp_drv_t *drv = d->driver;
    drv->rotated = rotationToLv(appConfig.cfg.displayRotation);
    drv->sw_rotate = 0;
    if (drv->rotated != LV_DISP_ROT_NONE) {
        // Prefer writing transposed pixels straight into the panel frame
        // buffer: that saves LVGL's 10 KB scratch block (out of the same pool
        // whose exhaustion once rebooted this device) AND one full copy per
        // strip. Only if the frame buffer is unreachable do we hand the job
        // back to LVGL's software rotation.
        //
        // Two things here are load-bearing and easy to break later:
        //  * the "1" is the COUNT of frame buffers to fetch, not an index -
        //    correct with cfg.num_fbs = 1 above.
        //  * writing s_fb directly skips the Cache_WriteBack that
        //    esp_lcd_panel_draw_bitmap() would do for us. That is safe ONLY
        //    because bounce buffers are enabled, which makes the refill a CPU
        //    read and therefore cache-coherent. Turn the bounce buffers off and
        //    this path starts showing stale pixels.
        void *fb = nullptr;
        if (s_panel && esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb) == ESP_OK && fb) {
            s_fb = (lv_color_t *)fb;
            Serial.printf("[disp] rotation %u deg: direct frame-buffer write, logical %dx%d\n",
                          (unsigned)appConfig.cfg.displayRotation,
                          (int)lv_disp_get_hor_res(NULL), (int)lv_disp_get_ver_res(NULL));
        } else {
            s_fb = nullptr;
            drv->sw_rotate = 1;
            Serial.println("[disp] rotation: LVGL software rotation (frame buffer unavailable)");
        }
    }
    lv_disp_drv_update(d, drv);
    Serial.flush();
}
