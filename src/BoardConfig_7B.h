#pragma once
// ============================================================
// BoardConfig_7B.h – Pin definitions for Waveshare ESP32-S3-Touch-LCD-7B
//
// 7" 1024x600 IPS, 16-bit RGB parallel. Verified 2026-08-18 against the
// vendor's own Arduino examples (waveshareteam/ESP32-S3-Touch-LCD-7B,
// examples/Arduino/examples/{06_LCD,04_CAN,08_TOUCH}) — NOT from the wiki
// page, which omits G7 on GPIO21.
//
// Chip facts read off the actual unit with esptool:
//   ESP32-S3 QFN56 rev v0.2, 16 MB flash (eFuse: quad/QIO, 3.3 V),
//   8 MB OCTAL PSRAM (vendor 0x0d "AP", 64 Mbit), 40 MHz crystal.
//   -> board_build.arduino.memory_type = qio_opi
//
// Differences from the 4" Rev 4 board (see BoardConfig.h):
//   • No ST7701S 3-wire SPI init sequence — the panel is driven by RGB
//     timings alone; LCD reset goes through the IO expander.
//   • IO expander is a CH422G, but on the SAME I2C address (0x24) and with
//     the SAME register map as the 4" board's CH32V003. Only the bit/pin
//     assignment differs, so the existing expander code largely carries over.
//   • CAN moved to GPIO19/20 — which are the ESP32-S3's native USB pins.
//     That is why this board carries a CH343 USB-UART bridge instead.
//     GPIO0 (BOOT) is therefore FREE here; on the 4" it had to serve as CAN_RX.
//   • Serial console runs over the CH343. NOTE: an onboard DIP switch routes
//     the bridge between two UARTs — in the wrong position the port
//     enumerates but stays completely silent. It must be on UART1.
// ============================================================

// --- LCD panel geometry ------------------------------------------------------
#define LCD_WIDTH      1024
#define LCD_HEIGHT      600

// --- 7B three-column layout --------------------------------------------------
// [ nav rail 88 | instrument center 600 | data sidebar 336 ] = 1024, full height.
// The existing 480x480 screens render 1:1 centered inside the 600px center
// column (stage 1; a 1.25x zoom stage may follow). At 170 ppi the rail is
// 13 mm wide - glove-friendly buttons. Values chosen so 88+600+336 = 1024.
#define UI7_RAIL_W      88
#define UI7_CENTER_X    UI7_RAIL_W
#define UI7_CENTER_W    600
#define UI7_SIDEBAR_X   (UI7_CENTER_X + UI7_CENTER_W)
#define UI7_SIDEBAR_W   (LCD_WIDTH - UI7_SIDEBAR_X)
// Content square in the middle column. Stage 3: 600 = native fill (must
// match SCREEN_W/H in Theme.h); with 480 the stage-1 centered layout returns.
#define UI7_CONTENT_SIZE 600
#define UI7_CONTENT_X   (UI7_CENTER_X + (UI7_CENTER_W - UI7_CONTENT_SIZE) / 2)
#define UI7_CONTENT_Y   ((LCD_HEIGHT - UI7_CONTENT_SIZE) / 2)
// Stage-2 scaling: LVGL transform-zoom of the whole content container.
// 256 = 1:1 (stage 1, centered via UI7_CONTENT_X/Y); 320 = 1.25x.
// MEASURED 2026-08-20: 320 is NOT VIABLE on this device - zoomed objects
// render through a NON-subdividable draw layer that needs the full
// transformed refresh strip (~22 KB) as ONE allocation from the LVGL pool
// (steady-state free: ~24.5 KB, less with grids), and LVGL 8.4's
// lv_draw_sw_layer_create() lv_memset_00()s the buffer BEFORE its NULL
// check -> instant StoreProhibited boot loop on the first frame.
// Real scaling needs the native 600x600 port of the screens, not a zoom.
#define UI7_CONTENT_ZOOM  256

// --- RGB panel timings ------------------------------------------------------
// Porches are the vendor's. The PIXEL CLOCK is NOT: at the vendor's 30 MHz the
// panel visibly flickers, because these porches make a frame 1386 x 661 =
// 916,146 clocks, i.e. only 32.7 Hz. Measured on the real panel, flicker stops
// from 36 MHz (39.3 Hz) upwards, so we run at the hardware ceiling of 40 MHz
// (43.7 Hz) to keep the most margin.
//
// 40 MHz really is the ceiling: the pixel clock is divided down from PLL160M
// and the divider may not drop below 2. At 42 MHz the driver aborts with
//   assert failed: lcd_ll_set_group_clock_coeff (div_num >= 2 && ...)
// 60 Hz would need ~55 MHz and is therefore unreachable with these porches.
// If more refresh is ever needed, shorten the blanking instead: the vendor
// spends 362 blanking columns on 1024 visible ones, and trimming that to a
// more usual ~160 would give roughly 54 Hz at the same 40 MHz.
// TEMPORARILY back to 30 MHz to separate two effects. At 40 MHz the bring-up
// was rock solid, but the full firmware smears horizontally - and that is DMA
// underrun, not refresh: the app draws its canvases into the same PSRAM the
// LCD is reading 54 MB/s out of. Dropping to 30 MHz costs refresh (32.7 Hz,
// which flickers on its own) but cuts the read to 40 MB/s. If the SMEARING
// stops here, bandwidth is proven to be the cause.
// 36 MHz, not the 40 MHz ceiling: with WiFi active the PSRAM bus carries the
// frame buffer scan-out AND XIP code fetches AND canvas writes AND (since the
// DRAM rescue) the WiFi/lwIP buffers - at 40 MHz that oversubscription showed
// as renewed flicker plus a shifted frame (bounce-buffer underrun + driver
// auto-restart). 36 MHz (39.3 Hz) is the lowest refresh the flicker sweep
// measured as visually steady, and buys ~10% PSRAM headroom.
#define LCD_PCLK_HZ    (36 * 1000 * 1000)
#define LCD_HSYNC_PULSE 162
#define LCD_HSYNC_BACK  152
#define LCD_HSYNC_FRONT  48
#define LCD_VSYNC_PULSE  45
#define LCD_VSYNC_BACK   13
#define LCD_VSYNC_FRONT   3
#define LCD_RGB_DATA_WIDTH 16
#define LCD_FRAME_BUFFERS   2          // vendor uses double buffering
// Bounce buffer height in rows. Bigger = more slack before the DMA starves.
// This competes with the LVGL draw buffers for the same internal DMA RAM, and
// DisplaySetup_7B.cpp negotiates those down to fit. Raised from 10 to 20 rows
// after the picture drifted both ways: with bounce buffers an underrun shifts
// the frame, and CONFIG_LCD_RGB_RESTART_IN_VSYNC then re-aligns it every frame,
// which is exactly what continuous drifting looks like.
// The real fix would be CONFIG_LCD_RGB_ISR_IRAM_SAFE, but that sdkconfig option
// cannot be changed against the prebuilt Arduino libraries.
// MEASURED: going from 10 to 20 rows changed the character of the artefact but
// did not reduce it, while costing 43 KB of DRAM (free heap fell from 70 KB to
// 27 KB, with WiFi not even started). So buffer size is NOT the limiting factor
// here - the LCD ISR living in flash is. Reverted to 10 rows.
#define LCD_BOUNCE_BUF     (LCD_WIDTH * 10)
// Signal polarities, established on the real panel by sweeping every
// combination through esp_lcd:
//   de_idle_high MUST be 0. With 1 the picture is unusable.
//   pclk_active_neg and the hsync/vsync idle levels make NO difference here,
//   so the vendor's pclk_active_neg = 1 is kept purely for consistency.
#define LCD_DE_IDLE_HIGH     0
#define LCD_PCLK_ACTIVE_NEG  1
#define LCD_SYNC_IDLE_LOW    0

// --- LCD RGB parallel bus ----------------------------------------------------
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
#define LCD_D6          0   // G3
#define LCD_D7         45   // G4
#define LCD_D8         48   // G5
#define LCD_D9         47   // G6
#define LCD_D10        21   // G7  <-- missing from the wiki page
#define LCD_D11         1   // R3
#define LCD_D12         2   // R4
#define LCD_D13        42   // R5
#define LCD_D14        41   // R6
#define LCD_D15        40   // R7

// --- CH422G IO expander (I2C @ 0x24, shared bus with the touch controller) ---
#define CH422_I2C_SDA   8
#define CH422_I2C_SCL   9
#define CH422_I2C_ADDR 0x24
#define CH422_REG_MODE 0x02   // same map as the 4" board's CH32V003
#define CH422_REG_OUT  0x03
#define CH422_REG_IN   0x04
#define CH422_REG_PWM  0x05
#define CH422_REG_ADC  0x06
// Expander pin indices (NOT bit masks — the vendor driver addresses them by index)
#define EXIO_TOUCH_RST  1
#define EXIO_BACKLIGHT  2
#define EXIO_LCD_RST    3
#define EXIO_SD_CS      4
#define EXIO_USB_CAN    5     // 0 = USB, 1 = CAN  <-- MUST be 1 for NMEA 2000
#define LCD_BL_ON_LEVEL 1

// --- Touch (GT911, same I2C bus as the expander) ----------------------------
#define TOUCH_SDA       8
#define TOUCH_SCL       9
#define TOUCH_INT       4
#define TOUCH_RST      -1     // via IO expander, see EXIO_TOUCH_RST
#define TOUCH_ADDR   0x5D     // default; 0x14 if INT is high leaving reset
#define TOUCH_ADDR_ALT 0x14
#define TOUCH_MAX_POINTS 5

// --- CAN / NMEA 2000 --------------------------------------------------------
// Verified against the vendor CAN example: TX=20, RX=19. A swap here kills
// the bus silently, so do not "fix" these from memory.
#define CAN_TX_PIN     20
#define CAN_RX_PIN     19
#define CAN_STB_PIN    -1
// The transceiver only reaches the bus once EXIO_USB_CAN is driven high.

// --- RS485 (unused by this firmware, listed for completeness) ---------------
#define RS485_RX_PIN   15
#define RS485_TX_PIN   16

// --- Navigation buttons / LED ------------------------------------------------
// GPIO0 is the BOOT button and, unlike the 4" board, is NOT needed for CAN.
// It is however RGB DATA6 (G3) here, so it still must not be polled.
#define BTN_PREV_PIN   -1
#define BTN_NEXT_PIN   -1
#define BTN_DEBOUNCE_MS 50
#define LED_PIN        -1
