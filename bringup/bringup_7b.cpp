// ============================================================================
//  Panel bring-up for the Waveshare ESP32-S3-Touch-LCD-7B
//  SWEEP 4: pixel clock UPWARDS, to raise the refresh rate
//
//  Settled so far:
//    * LovyanGFX never drove this panel (grey haze). esp_lcd does. Every
//      polarity result obtained under LovyanGFX was meaningless.
//    * Pin map and porches in BoardConfig_7B.h are CONFIRMED: red reads red,
//      green green, white white.
//    * Polarity is CONFIRMED: de_idle_high must be 0. pclk_active_neg and the
//      sync idle levels make no difference to this panel.
//
//  Remaining fault: flicker. The arithmetic that explains it:
//      h total = 1024 + 162 + 152 + 48 = 1386 clocks
//      v total =  600 +  45 +  13 +  3 =  661 lines
//      => 916,146 clocks per frame
//      => 30 MHz gives 32.7 Hz, 21 MHz only 22.9 Hz
//  That is simply too slow for a flicker-free LCD, regardless of bandwidth.
//  The earlier sweep went DOWNWARDS and therefore made it worse each step.
//
//  This one goes up. It also prints the resulting refresh rate per step, and
//  60 Hz would need ~55 MHz. Whether the RGB DMA can sustain that out of PSRAM
//  on ESP-IDF 4.4 (no bounce buffers) is exactly what we are measuring:
//    * flicker gets better as the clock rises  -> it was the refresh rate
//    * picture starts tearing or breaking up   -> PSRAM bandwidth is the limit,
//      and the real fix is an Arduino core on ESP-IDF 5.x, which has bounce
//      buffers for precisely this.
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "BoardConfig.h"

// Hard ceiling: at 42 MHz the driver dies with
//   assert failed: lcd_ll_set_group_clock_coeff (div_num >= 2 && ...)
// The pixel clock is divided down from PLL160M and the divider may not go
// below 2, so anything above ~40 MHz is simply not reachable on this chip.
// 60 Hz would need 55 MHz and is therefore out of the question with these
// porches - shortening the blanking intervals is the only other lever.
static const uint32_t PCLKS[] = {
    30000000,  // 1 square - vendor value, 32.7 Hz
    32000000,  // 2
    34000000,  // 3
    36000000,  // 4
    38000000,  // 5
    40000000,  // 6 - the ceiling
};
static const int NUM_PCLKS = (int)(sizeof(PCLKS) / sizeof(PCLKS[0]));

RTC_NOINIT_ATTR static int rtcIdx;
RTC_NOINIT_ATTR static uint32_t rtcMagic;
#define SWEEP_MAGIC 0x7B40AD04
#define HOLD_MS     8000

static uint8_t exioShadow = 0xFF;
static bool ch422Write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(CH422_I2C_ADDR);
    Wire.write(reg); Wire.write(val);
    return Wire.endTransmission() == 0;
}
static bool exioSet(uint8_t pin, bool high) {
    if (high) exioShadow |=  (uint8_t)(1u << pin);
    else      exioShadow &= (uint8_t)~(1u << pin);
    return ch422Write(CH422_REG_OUT, exioShadow);
}

static esp_lcd_panel_handle_t panel = nullptr;
#define STRIP_ROWS 20
static uint16_t *strip = nullptr;

static void fillRect(int x, int y, int w, int h, uint16_t colour) {
    if (!strip || w <= 0 || h <= 0) return;
    for (int i = 0; i < w * STRIP_ROWS; ++i) strip[i] = colour;
    for (int row = y; row < y + h; row += STRIP_ROWS) {
        const int rows = (row + STRIP_ROWS > y + h) ? (y + h - row) : STRIP_ROWS;
        esp_lcd_panel_draw_bitmap(panel, x, row, x + w, row + rows, strip);
    }
}

#define RGB565(r,g,b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))
static const uint16_t C_RED   = RGB565(255,0,0);
static const uint16_t C_GREEN = RGB565(0,255,0);
static const uint16_t C_BLUE  = RGB565(0,0,255);
static const uint16_t C_WHITE = RGB565(255,255,255);
static const uint16_t C_GREY  = RGB565(128,128,128);
static const uint16_t C_BLACK = 0x0000;

void setup() {
    Serial.begin(115200);
    delay(300);

    if (rtcMagic != SWEEP_MAGIC) { rtcMagic = SWEEP_MAGIC; rtcIdx = 0; }
    const int idx = rtcIdx;
    const uint32_t pclk = PCLKS[idx];
    // Advance the counter HERE, not after the hold. A step that crashes inside
    // the LCD driver never reaches the end of the hold, and the old code then
    // sat on that same step forever, rebooting every second.
    rtcIdx = (rtcIdx + 1) % NUM_PCLKS;

    const uint32_t hTotal = LCD_WIDTH  + LCD_HSYNC_PULSE + LCD_HSYNC_BACK + LCD_HSYNC_FRONT;
    const uint32_t vTotal = LCD_HEIGHT + LCD_VSYNC_PULSE + LCD_VSYNC_BACK + LCD_VSYNC_FRONT;
    const float    fps    = (float)pclk / (float)(hTotal * vTotal);
    const float    mbps   = LCD_WIDTH * LCD_HEIGHT * 2.0f * fps / 1e6f;

    Serial.println();
    Serial.println("=== NauticPinnace 7B pixel clock sweep UPWARDS ===");
    Serial.printf("STEP %d -> %d squares: pclk %.1f MHz, refresh %.1f Hz, "
                  "PSRAM read %.0f MB/s\n",
                  idx, idx + 1, pclk / 1e6, fps, mbps);
    Serial.println("COUNT THE SQUARES at the point the picture stops flickering.");

    Wire.begin(CH422_I2C_SDA, CH422_I2C_SCL, 400000);
    ch422Write(CH422_REG_MODE, 0xFF);
    ch422Write(CH422_REG_OUT, exioShadow);
    delay(50);

    pinMode(TOUCH_INT, OUTPUT);
    digitalWrite(TOUCH_INT, LOW);
    exioSet(EXIO_TOUCH_RST, false); delay(10);
    exioSet(EXIO_TOUCH_RST, true);  delay(10);
    delay(50);
    pinMode(TOUCH_INT, INPUT);

    exioSet(EXIO_LCD_RST, false);  delay(20);
    exioSet(EXIO_LCD_RST, true);   delay(120);

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src    = LCD_CLK_SRC_PLL160M;
    cfg.data_width = LCD_RGB_DATA_WIDTH;
    cfg.psram_trans_align = 64;
    cfg.sram_trans_align  = 4;
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

    cfg.timings.pclk_hz = pclk;
    cfg.timings.h_res   = LCD_WIDTH;
    cfg.timings.v_res   = LCD_HEIGHT;
    cfg.timings.hsync_pulse_width = LCD_HSYNC_PULSE;
    cfg.timings.hsync_back_porch  = LCD_HSYNC_BACK;
    cfg.timings.hsync_front_porch = LCD_HSYNC_FRONT;
    cfg.timings.vsync_pulse_width = LCD_VSYNC_PULSE;
    cfg.timings.vsync_back_porch  = LCD_VSYNC_BACK;
    cfg.timings.vsync_front_porch = LCD_VSYNC_FRONT;
    cfg.timings.flags.pclk_active_neg = 1;   // confirmed: does not matter
    cfg.timings.flags.de_idle_high    = 0;   // confirmed: MUST be 0
    cfg.flags.fb_in_psram = 1;

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &panel);
    Serial.printf("[lcd] new_rgb_panel -> %s\n", esp_err_to_name(err));
    if (err != ESP_OK) {
        Serial.println("[lcd] this clock was refused, skipping");
        delay(1500);
        rtcIdx = (rtcIdx + 1) % NUM_PCLKS;
        ESP.restart();
    }
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    exioSet(EXIO_BACKLIGHT, LCD_BL_ON_LEVEL);

    strip = (uint16_t *)heap_caps_malloc(LCD_WIDTH * STRIP_ROWS * 2,
                                         MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!strip) { Serial.println("[lcd] strip alloc FAILED"); delay(2000); ESP.restart(); }

    fillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BLACK);
    const int sq = 70, gap = 20;
    for (int i = 0; i <= idx; ++i)
        fillRect(gap + i * (sq + gap), 30, sq, sq, C_WHITE);

    // A mid grey field is the most sensitive flicker test - far more telling
    // than saturated colour. Fine vertical lines next to it reveal tearing.
    fillRect(0, 170, LCD_WIDTH, 200, C_GREY);
    for (int x = 0; x < LCD_WIDTH; x += 16)
        fillRect(x, 390, 8, 120, C_WHITE);
    fillRect(0,                 530, LCD_WIDTH / 3, 70, C_RED);
    fillRect(LCD_WIDTH / 3,     530, LCD_WIDTH / 3, 70, C_GREEN);
    fillRect(2 * LCD_WIDTH / 3, 530, LCD_WIDTH / 3, 70, C_BLUE);

    Serial.printf("[lcd] step %d drawn (%d squares)\n", idx, idx + 1);
}

void loop() {
    static uint32_t t0 = millis();
    if (millis() - t0 >= HOLD_MS) {
        Serial.printf("[sweep] next step %d, restarting\n", rtcIdx);
        Serial.flush();
        delay(50);
        ESP.restart();
    }
    delay(10);
}
