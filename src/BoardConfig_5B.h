#pragma once
// ============================================================
// BoardConfig_5B.h – Pin definitions for Waveshare ESP32-S3-Touch-LCD-5B
//
// 5" 1024x600 IPS (ST7262), 16-bit RGB parallel — the SAME resolution as the
// 7B, so the entire 600-grid UI (BOARD_PANEL_1024X600) carries over unchanged.
// Only the physical size differs: ~240 ppi here vs ~170 ppi on the 7B, so
// every element is about 30 % SMALLER in millimetres. The 88 px nav rail is
// 13 mm on the 7B but only 9.3 mm here — usable, but worth a glove test.
//
// NOTE the SKU: "…-LCD-5" and "…-LCD-5B" are the same PCB with a different
// panel (5 = 800x480, 5B = 1024x600). This file is the 1024x600 variant; the
// vendor selects it with ESP_PANEL_USE_1024_600_LCD.
//
// Sources: vendor examples in waveshareteam/ESP32-S3-Touch-LCD-5
// (examples/Arduino/examples/{02_RS485,03_SD,05_IO_Test,06_TWAItransmit,
// 08_DrawColorBar,09_lvgl_v8_demo}) plus the vendor board definition
// ESP32_Display_Panel/src/board/supported/waveshare/
// BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_5_B.h — NOT the wiki tables.
//
// Chip facts read off the actual unit with esptool:
//   ESP32-S3 QFN56 rev v0.2, 16 MB flash (eFuse: quad/QIO, 3.3 V),
//   8 MB PSRAM (AP_3v3), 40 MHz crystal.
//   (The unit's eFuse MAC was recorded here too and has been removed: it says
//    nothing about the board type, and wifiApSsid() in WifiNaming.h builds the
//    hotspot name from its last three bytes - so publishing it would publish
//    the network name a particular boat's display broadcasts.)
//   Vendor sdkconfig has CONFIG_SPIRAM_MODE_OCT=y (module is a WROOM-1-N16R8)
//   -> board_build.arduino.memory_type = qio_opi, same as the 7B.
//
// Differences from the 7B (see BoardConfig_7B.h) — only five things:
//   • CAN is on GPIO15/16, NOT 20/19.
//   • There is NO USB/CAN multiplexer. The 7B's EXIO_USB_CAN does not exist
//     here (EXIO5 is an isolated digital INPUT on this board) — nothing has
//     to be switched before the transceiver reaches the bus.
//   • Serial console is the ESP32-S3's NATIVE USB-Serial/JTAG (VID 0x303A,
//     PID 0x1001), so ARDUINO_USB_CDC_ON_BOOT must be 1 — the exact opposite
//     of the 7B. There is also no UART/DIP switch to get wrong: UART0
//     (GPIO43/44) is wired to the RS485 transceiver and unavailable.
//   • RS485 sits on GPIO43/44 (the UART0 pins), where the 7B has 15/16.
//   • Different blanking: frame is 1368 x 637 instead of the 7B's 1386 x 661.
// Everything else — all 16 RGB data pins, HSYNC/VSYNC/DE/PCLK, the I2C bus,
// the GT911 and the CH422G pin functions 1..4 — is byte-for-byte identical.
// ============================================================

// --- LCD panel geometry ------------------------------------------------------
#define LCD_WIDTH      1024
#define LCD_HEIGHT      600

// --- Three-column layout (identical to the 7B: same pixels, same UI) ---------
// [ nav rail 88 | instrument center 600 | data sidebar 336 ] = 1024, full height.
#define UI7_RAIL_W      88
#define UI7_CENTER_X    UI7_RAIL_W
#define UI7_CENTER_W    600
#define UI7_SIDEBAR_X   (UI7_CENTER_X + UI7_CENTER_W)
#define UI7_SIDEBAR_W   (LCD_WIDTH - UI7_SIDEBAR_X)
#define UI7_CONTENT_SIZE 600
#define UI7_CONTENT_X   (UI7_CENTER_X + (UI7_CENTER_W - UI7_CONTENT_SIZE) / 2)
#define UI7_CONTENT_Y   ((LCD_HEIGHT - UI7_CONTENT_SIZE) / 2)
// LVGL transform-zoom: 256 = 1:1. Do NOT set 320 - measured non-viable on the
// 7B (draw-layer allocation exceeds the LVGL pool, boot loop). Same pool here.
#define UI7_CONTENT_ZOOM  256

// --- RGB panel timings ------------------------------------------------------
// Porches are the vendor's (board definition file variant: H 24/160/160,
// V 2/23/12 -> frame 1368 x 637 = 871,416 clocks). The demo config uses
// 30/145/170 for H, which totals 1369 - functionally interchangeable.
//
// The PIXEL CLOCK is deliberately NOT the vendor's. At their 21 MHz this frame
// runs at just 24.1 Hz, well below the 32.7 Hz that was measured as visibly
// flickering on the 7B. Starting at the 7B's proven 36 MHz gives 41.3 Hz here
// - slightly MORE headroom than the 7B has at the same clock, because this
// frame is smaller (871k vs 916k clocks). If the picture smears or drifts,
// the cause is PSRAM bandwidth, not refresh: see the long analysis in
// BoardConfig_7B.h before touching this value.
#define LCD_PCLK_HZ    (36 * 1000 * 1000)
#define LCD_HSYNC_PULSE  24
#define LCD_HSYNC_BACK  160
#define LCD_HSYNC_FRONT 160
#define LCD_VSYNC_PULSE   2
#define LCD_VSYNC_BACK   23
#define LCD_VSYNC_FRONT  12
#define LCD_RGB_DATA_WIDTH 16
#define LCD_FRAME_BUFFERS   2
#define LCD_BOUNCE_BUF     (LCD_WIDTH * 10)
// Polarities: the vendor sets ONLY pclk_active_neg = 1 and leaves the rest at
// the esp_lcd default of 0 — which is exactly the combination proven on the
// 7B panel (de_idle_high = 0 is the one that matters; with 1 it is unusable).
#define LCD_DE_IDLE_HIGH     0
#define LCD_PCLK_ACTIVE_NEG  1
#define LCD_SYNC_IDLE_LOW    0

// --- LCD RGB parallel bus (identical to the 7B) ------------------------------
#define LCD_VSYNC       3
#define LCD_HSYNC      46
#define LCD_DE          5
#define LCD_PCLK        7
#define LCD_DISP       -1              // not wired
#define LCD_RST        -1              // via IO expander, see EXIO_LCD_RST
// DATA0-15: B3 B4 B5 B6 B7 | G2 G3 G4 G5 G6 G7 | R3 R4 R5 R6 R7
#define LCD_D0         14   // B3
#define LCD_D1         38   // B4
#define LCD_D2         18   // B5
#define LCD_D3         17   // B6
#define LCD_D4         10   // B7
#define LCD_D5         39   // G2
#define LCD_D6          0   // G3   <-- also the BOOT button: never poll it
#define LCD_D7         45   // G4
#define LCD_D8         48   // G5
#define LCD_D9         47   // G6
#define LCD_D10        21   // G7
#define LCD_D11         1   // R3
#define LCD_D12         2   // R4
#define LCD_D13        42   // R5
#define LCD_D14        41   // R6
#define LCD_D15        40   // R7

// --- CH422G IO expander (shared I2C bus with the touch controller) -----------
// Same chip, same address and same register handling as the 7B - the existing
// expander code carries over unchanged.
#define CH422_I2C_SDA   8
#define CH422_I2C_SCL   9
#define CH422_I2C_ADDR 0x24
#define CH422_REG_MODE 0x02
#define CH422_REG_OUT  0x03
#define CH422_REG_IN   0x04
#define CH422_REG_PWM  0x05
#define CH422_REG_ADC  0x06
// Expander pin indices (NOT bit masks). 1..4 are identical to the 7B.
#define EXIO_TOUCH_RST  1
#define EXIO_BACKLIGHT  2
#define EXIO_LCD_RST    3
#define EXIO_SD_CS      4
// NO EXIO_USB_CAN on this board. The vendor's own 5" sources still carry a
// stray "#define USB_SEL 5" copied from the 7-inch example, but nothing uses
// it and it collides with DI1 - EXIO5 is an isolated digital INPUT here.
// Isolated I/O on the screw terminals (not used by this firmware yet):
#define EXIO_DIGITAL_IN0  0   // DI0
#define EXIO_DIGITAL_IN5  5   // DI1
#define EXIO_DIGITAL_OUT0 8   // DO0 (expander index 8 = OC0)
#define EXIO_DIGITAL_OUT1 9   // DO1 (expander index 9 = OC1)
#define LCD_BL_ON_LEVEL 1

// --- Touch (GT911, same I2C bus as the expander; identical to the 7B) -------
#define TOUCH_SDA       8
#define TOUCH_SCL       9
#define TOUCH_INT       4
#define TOUCH_RST      -1     // via IO expander, see EXIO_TOUCH_RST
#define TOUCH_ADDR   0x5D     // INT held LOW while releasing RST selects 0x5D
#define TOUCH_ADDR_ALT 0x14
#define TOUCH_MAX_POINTS 5

// --- CAN / NMEA 2000 --------------------------------------------------------
// Verified against the vendor TWAI examples (06_TWAItransmit/07_TWAIreceive):
// TX=15, RX=16 - the mirror image of the 7B's 20/19. A swap kills the bus
// silently, so do not "fix" these from memory.
// Transceiver is a TJA1051T/3/1J. No standby pin appears in any vendor
// example or pin table (presumably strapped to GND) -> nothing to drive.
// The board has a switchable 120 ohm terminator (off by default).
#define CAN_TX_PIN     15
#define CAN_RX_PIN     16
#define CAN_STB_PIN    -1

// --- RS485 (unused by this firmware, listed so nobody reuses these pins) -----
// SP3485 with automatic direction control. These ARE the UART0 pins, which is
// why the serial console has to be native USB CDC on this board.
#define RS485_RX_PIN   43
#define RS485_TX_PIN   44

// --- SD card (SPI; CS goes through the expander) ----------------------------
#define SD_MOSI_PIN    11
#define SD_CLK_PIN     12
#define SD_MISO_PIN    13
// chip select: EXIO_SD_CS above

// --- On-board RTC (PCF85063A, CR927 backed, same I2C bus) -------------------
// Not used yet. Interesting for the boat: it would survive a power cut without
// NTP, which is exactly the drift problem the Pi setups have.
#define RTC_I2C_ADDR   0x51
#define RTC_INT_PIN     6

// --- Navigation buttons / LED ------------------------------------------------
// GPIO0 is the BOOT button but also RGB DATA6 (G3) - it must never be polled.
#define BTN_PREV_PIN   -1
#define BTN_NEXT_PIN   -1
#define BTN_DEBOUNCE_MS 50
#define LED_PIN        -1
