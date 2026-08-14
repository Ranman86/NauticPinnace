#pragma once

#ifdef SIMULATOR
// ── Simulator: no LovyanGFX hardware driver ───────────────────────────────────
// Display output goes via SDL2 in sim_hal.cpp / sim_main.cpp.
#include <lvgl.h>
#include <cstdint>
inline void setBrightness(uint8_t) {}
inline void displayDiag()  {}
inline void gt911Diag()    {}
inline void displayInit()  {}
inline void displayTick()  { lv_timer_handler(); }
inline void boardBuzzer(bool) {}
uint32_t    getTickFps(bool reset = false);
// Real implementation in sim_hal.cpp — the simulator runs the same swipe tracker
// as the panel, so a slider drag must be able to claim the gesture here too.
void        swipeSuppress();

#else
// ── Hardware (ESP32-S3) ───────────────────────────────────────────────────────
#include <lvgl.h>
#include <LovyanGFX.hpp>
// ESP32-S3 specific RGB parallel panel and bus headers
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include "BoardConfig.h"

// ---------------------------------------------------------------------------
// Panel_ST7701_WS4: panel-specific init commands for the Waveshare
// ESP32-S3-Touch-LCD-4 Rev 4 (480×480 IPS, ST7701S).
//
// The base class Panel_ST7701_Base::init() already sends:
//   0xFF BK0 select, 0xC0 (wrong line-count), 0xC3 (RGB timing)
// Our getInitCommands() starts with page-0 exit + Sleep-Out so the panel
// is in a known state, then re-selects BK0 and overrides C0 with the
// correct 0x3B value (vs 0x3D calculated by the base class).
//
// Key differences from the generic Panel_ST7701 init:
//   BK0 C0   = 0x3B 0x00  (was 0x3D 0x00)
//   BK0 C2   = 0x21 0x08  (was 0x31 0x05)
//   BK1 B1   = 0x30        (was 0x32)
//   BK1 B2   = 0x87        (was 0x07)   ← VGH setting, very important
// ---------------------------------------------------------------------------
class Panel_ST7701_WS4 : public lgfx::Panel_ST7701_Base {
protected:
    const uint8_t* getInitCommands(uint8_t listno) const override {
        // LovyanGFX command-list format:
        //   cmd, count, data...     – count = number of data bytes
        //   cmd, 0x80|count, data..., delay_ms  – CMD_INIT_DELAY flag (0x80)
        // End of list: 0xFF, 0xFF
        static constexpr const uint8_t list0[] = {
            // Return to page 0 first (base class left us in BK0)
            0xFF,  5, 0x77, 0x01, 0x00, 0x00, 0x00,
            // Sleep Out + 120 ms (standard DCS command, works in any page)
            0x11, 0x80, 120,

            // --- BK0: timing / gamma settings ----------------------------
            0xFF,  5, 0x77, 0x01, 0x00, 0x00, 0x10,
            // C0: LNSET – correct line count for 480-line panel
            0xC0,  2, 0x3B, 0x00,    // Waveshare: 0x3B  (base class wrong: 0x3D)
            0xC1,  2, 0x0D, 0x02,
            0xC2,  2, 0x21, 0x08,    // Waveshare specific (generic: 0x31 0x05)
            0xCD,  1, 0x08,
            // Positive Voltage Gamma Control
            0xB0, 16, 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
                       0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18,
            // Negative Voltage Gamma Control
            0xB1, 16, 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
                       0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18,

            // --- BK1: power / voltage / display timing -------------------
            0xFF,  5, 0x77, 0x01, 0x00, 0x00, 0x11,
            0xB0,  1, 0x60,           // Vop
            0xB1,  1, 0x30,           // VCOM  (generic: 0x32)
            0xB2,  1, 0x87,           // VGH   (generic: 0x07)  ← critical!
            0xB3,  1, 0x80,
            0xB5,  1, 0x49,           // VGL
            0xB7,  1, 0x85,
            0xB8,  1, 0x21,           // AVDD/AVCL
            0xC1,  1, 0x78,
            0xC2, 0x81, 0x78, 20,    // param 0x78 + 20 ms delay

            0xE0,  3, 0x00, 0x1B, 0x02,
            0xE1, 11, 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44,
            0xE2, 12, 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00,
            0xE3,  4, 0x00, 0x00, 0x11, 0x11,
            0xE4,  2, 0x44, 0x44,
            0xE5, 16, 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
                       0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0,
            0xE6,  4, 0x00, 0x00, 0x11, 0x11,
            0xE7,  2, 0x44, 0x44,
            0xE8, 16, 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
                       0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0,
            0xEB,  7, 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40,
            0xEC,  2, 0x3C, 0x00,
            0xED, 16, 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
                       0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA,

            // --- Return to page 0, pixel format, display-on --------------
            0xFF,  5, 0x77, 0x01, 0x00, 0x00, 0x00,
            0x21,  0,           // IPS display inversion ON
            0x3A,  1, 0x60,    // Pixel format: RGB666
            0x29,  0,           // Display On

            0xFF, 0xFF,         // End-of-list marker
        };
        switch (listno) {
        case 0: return list0;
        default: return nullptr;
        }
    }
};

// ---------------------------------------------------------------------------
// LovyanGFX display class – ST7701 via 16-bit RGB parallel + 3-wire SPI init.
// Backlight and LCD reset are controlled through the CH32V003 I2C IO expander.
//
// Touch (GT911) is handled separately via Arduino Wire (NOT via LovyanGFX),
// because both LovyanGFX's I2C driver and Arduino Wire cannot safely share
// I2C port 0 simultaneously.  The LVGL touch callback calls gt911_read_touch()
// which uses Wire directly.
// ---------------------------------------------------------------------------
class LGFX : public lgfx::LGFX_Device {
    Panel_ST7701_WS4     _panel_instance;  // ← custom Waveshare init
    lgfx::Bus_RGB        _bus_instance;
    // Touch_GT911 intentionally removed – handled via Wire in lvgl_touch_cb

public:
    LGFX();
};

extern LGFX gfx;

// Set backlight brightness 0-255 via the CH32V003 PWM register.
// (PWM polarity is inverted: 0=full brightness, 255=off)
void setBrightness(uint8_t brightness);

// Read and print CH32V003 register states to Serial (for diagnostics)
void displayDiag();
// Read GT911 status register and X/Y of first touch point; print to Serial
void gt911Diag();
// Returns displayTick() calls since last reset; reset=true resets the counter
uint32_t getTickFps(bool reset = false);

void displayInit();
void displayTick();

// Drive the on-board buzzer (CH32V003 expander pin 7). Used by the anchor alarm.
void boardBuzzer(bool on);

// Claim the current touch for a widget that owns horizontal dragging (sliders,
// scrollable lists). The global swipe-to-change-screen gesture then ignores this
// touch — otherwise dragging a volume slider sideways flips to another screen.
// Call from an LV_EVENT_PRESSED handler; the flag clears on the next touch-down.
void swipeSuppress();

#endif  // !SIMULATOR
