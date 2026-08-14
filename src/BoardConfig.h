#pragma once
// ============================================================
// BoardConfig.h – Pin definitions for Waveshare ESP32-S3-Touch-LCD-4 REV 4
//
// Rev 4 hardware differences from Rev 1/2/3:
//   • Display: ST7701S via 16-bit RGB parallel + 3-wire SPI init
//   • Backlight / LCD RST / SYS_EN: via CH32V003 MCU (I2C IO expander @ 0x24)
//   • Touch: GT911 (was CST820) — same I2C bus as CH32V003 (SDA=15, SCL=7)
//   • CAN: TX=GPIO6, RX=GPIO0 (was GPIO19/20)
// ============================================================

// --- LCD 3-wire SPI (init commands only, pixel data via RGB parallel) --------
#define LCD_SPI_SDA     1    // MOSI for ST7701S init
#define LCD_SPI_SCL     2    // SCLK for ST7701S init
#define LCD_SPI_CS     42    // CS for ST7701S init

// --- LCD RGB parallel bus (pixel data) ---------------------------------------
#define LCD_PCLK       41
#define LCD_DE         40    // Data Enable
#define LCD_VSYNC      39
#define LCD_HSYNC      38
// DATA0-DATA15 (B0-B4, G0-G5, R0-R4)
#define LCD_D0          5
#define LCD_D1         45
#define LCD_D2         48
#define LCD_D3         47
#define LCD_D4         21
#define LCD_D5         14
#define LCD_D6         13
#define LCD_D7         12
#define LCD_D8         11
#define LCD_D9         10
#define LCD_D10         9
#define LCD_D11        46
#define LCD_D12         3
#define LCD_D13         8
#define LCD_D14        18
#define LCD_D15        17

#define LCD_WIDTH      480
#define LCD_HEIGHT     480

// --- CH32V003 IO expander (I2C @ 0x24, SDA=15, SCL=7) -----------------------
// Controls: SYS_EN, LCD_RST, Touch_RST, Backlight PWM
#define CH32_I2C_SDA   15
#define CH32_I2C_SCL    7
#define CH32_I2C_ADDR 0x24
// CH32V003 register map
#define CH32_REG_DIR  0x02   // direction: 1=output per bit
#define CH32_REG_OUT  0x03   // output state bitmask
#define CH32_REG_IN   0x04   // input state
#define CH32_REG_PWM  0x05   // PWM duty 0–255 (0=max brightness, inverted)
// CH32V003 pin bit masks (1-indexed → bit N-1)
#define CH32_TOUCH_RST (1<<0)  // expander pin 1
#define CH32_LCD_RST   (1<<2)  // expander pin 3
#define CH32_SYS_EN    (1<<4)  // expander pin 5
#define CH32_BEE_EN    (1<<6)  // expander pin 7 – active-HIGH buzzer (confirmed bit6=0x40)

// --- Touch (GT911, I2C @ 0x5D or 0x14, shared bus with CH32V003) -------------
#define TOUCH_SDA      15    // shared with CH32V003
#define TOUCH_SCL       7    // shared with CH32V003
#define TOUCH_INT      -1    // routed via CH32V003 (not a direct GPIO)
#define TOUCH_RST      -1    // routed via CH32V003 (not a direct GPIO)
// GT911 answers on 0x5D (factory default) or 0x14, depending on the INT level as
// it leaves reset — units of this same board ship with either. This is only the
// FIRST candidate; DisplaySetup probes both at boot (gt911_detect_addr).
#define TOUCH_ADDR   0x14

// --- CAN / NMEA 2000 (Rev 4: TX=GPIO6, RX=GPIO0) ----------------------------
// WARNING: GPIO0 is also the BOOT button. Enable CAN only if BOOT button
//          is not needed (or use a different GPIO for RX on your schematic).
#define CAN_TX_PIN      6
#define CAN_RX_PIN      0
#define CAN_STB_PIN    -1

// --- Navigation buttons (active-low, internal pull-up) ----------------------
// GPIO0 is now used as CAN_RX (NMEA 2000). It must NOT be polled as a button —
// bus traffic on GPIO0 would be read as continuous "prev screen" presses and the
// display would cycle screens. Navigation is via touch swipe + on-screen arrows.
#define BTN_PREV_PIN   -1    // was 0 (BOOT); freed for CAN_RX
#define BTN_NEXT_PIN   -1    // GPIO35 = RGB DATA1 – must NOT be used as GPIO
#define BTN_DEBOUNCE_MS 50

// --- Status LED (optional) --------------------------------------------------
#define LED_PIN        -1
