#include "DisplaySetup.h"
#include "Entropy.h"
#include "display/DisplayManager.h"
#include "display/UiConfig.h"
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

// ---- CH32V003 IO expander helpers -------------------------------------------

static void ch32_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(CH32_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

// Anchor-alarm buzzer (CH32V003 expander pin 7, active-HIGH). Only bit6 differs
// from the steady-state OUT value 0xBF (all HIGH, resets released); on -> 0xFF,
// off -> 0xBF. Called from the LVGL task (same context as touch I2C) so the bus
// access is serialised.
void boardBuzzer(bool on) {
    ch32_write(CH32_REG_OUT, on ? (uint8_t)(0xBF | CH32_BEE_EN) : (uint8_t)0xBF);
}

// ---- CH32V003 board initialisation ------------------------------------------
// MUST be called before gfx.begin().
// Sets SYS_EN HIGH so the ST7701S gets power, resets the panel and touch
// controller, then sets the backlight to full brightness.

// Read one byte from CH32V003 register (returns -1 on error)
static int ch32_read(uint8_t reg) {
    Wire.beginTransmission(CH32_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    Wire.requestFrom((uint8_t)CH32_I2C_ADDR, (uint8_t)1);
    if (Wire.available()) return Wire.read();
    return -1;
}

// ---- GT911 I2C helpers ------------------------------------------------------

// The GT911 answers on ONE of two addresses depending on the INT level while it
// comes out of reset: 0x5D (factory default) or 0x14. Boards of the same model
// have been seen with either, so the address is probed at boot instead of being
// hard-coded — a wrong one makes every transfer fail and touch is simply dead.
// TOUCH_ADDR (BoardConfig.h) is only the first candidate to try.
static uint8_t s_touchAddr = TOUCH_ADDR;

static bool gt911_probe_addr(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission(true) == 0;
}

// Pick whichever address actually ACKs. Returns true if a GT911 was found.
static bool gt911_detect_addr() {
    const uint8_t cand[3] = { TOUCH_ADDR, 0x5D, 0x14 };
    for (uint8_t i = 0; i < 3; i++) {
        if (gt911_probe_addr(cand[i])) {
            s_touchAddr = cand[i];
            Serial.printf("[GT911] responding at 0x%02X\n", s_touchAddr);
            return true;
        }
    }
    Serial.println("[GT911] no touch controller found at 0x5D or 0x14!");
    return false;
}

static bool gt911_i2c_write(uint16_t reg, const uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(s_touchAddr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    for (uint8_t i = 0; i < len; i++) Wire.write(buf[i]);
    return Wire.endTransmission(true) == 0;
}

static bool gt911_i2c_read(uint16_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(s_touchAddr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    uint8_t got = Wire.requestFrom((uint8_t)s_touchAddr, len);
    for (uint8_t i = 0; i < got; i++) buf[i] = Wire.read();
    return got == len;
}

// Write a complete 184-byte GT911 configuration for 480×480 IPS panel.
// The GT911 stores its config (0x8047-0x80FE) in internal NOR flash.
// A valid config requires a correct checksum at 0x80FF and a fresh flag (0x01)
// at 0x8100.  Without valid config the GT911 may not detect touches at all.
static void gt911_write_config() {
    Serial.println("[GT911] Writing 480x480 configuration...");

    // 184-byte config: registers 0x8047-0x80FE
    // Key values:  ver=0x60  xmax=480(0x01E0)  ymax=480(0x01E0)  tnum=5
    // MODULE_SW1=0x0D: normal scan, no XY swap, INT rises on release
    // Everything else: reasonable defaults or zero.
    static uint8_t cfg[184] = {
        // [0]  0x8047 version
        0x60,
        // [1-2] 0x8048 X_OUTPUT_MAX = 480 (little-endian)
        0xE0, 0x01,
        // [3-4] 0x804A Y_OUTPUT_MAX = 480
        0xE0, 0x01,
        // [5]  0x804C TOUCH_NUMBER
        0x05,
        // [6]  0x804D MODULE_SWITCH1 = 0x0C (INT low on touch, no axis swap)
        0x0C,
        // [7]  0x804E MODULE_SWITCH2
        0x00,
        // [8]  0x804F SHAKE_COUNT = 1
        0x01,
        // [9]  0x8050 FILTER = 4
        0x04,
        // [10] 0x8051 LARGE_DETECT
        0x50,
        // [11] 0x8052 SCREEN_TOUCH_LEVEL = 0x28 (40 – standard, tolerates noise)
        0x28,
        // [12] 0x8053 SCREEN_LEAVE_LEVEL = 0x20 (32)
        0x20,
        // [13] 0x8054 LOW_POWER_CTL
        0x05,
        // [14] 0x8055 REFRESH_RATE
        0x05,
        // [15-16] 0x8056-57 reserved
        0x00, 0x00,
        // [17-18] X/Y thresholds
        0x00, 0x00,
        // [19-26] 0x805A-61 reserved / key-zones
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // [27-34] 0x8062-69 key-zone coords
        0x14, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // [35-50] 0x806A-79
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // [51-58] 0x807A-81
        0x00, 0x00, 0x00, 0x87, 0x28, 0x0A, 0x17, 0x15,
        // [59-66] 0x8082-89 sensitivity / grip / misc
        0x31, 0x0D, 0x00, 0x00, 0x02, 0x9B, 0x03, 0x25,
        // [67-82] 0x808A-99 (reserved / advanced)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // [83-90] 0x809A-A1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // [91-98] 0x80A2-A9
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37, 0xAB,
        // [99-106] 0x80AA-B1  (channel map X)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // [107-122] 0x80B2-C1 (channel map X cont.)
        0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10,
        0x12, 0x14, 0x16, 0x18, 0x1A, 0xFF, 0xFF, 0xFF,
        // [123-130] 0x80C2-C9
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // [131-138] 0x80CA-D1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // [139-154] 0x80D2-E1 (channel map Y)
        0x00, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0F,
        0x10, 0x12, 0x13, 0x14, 0x15, 0x16, 0x18, 0x1C,
        // [155-162] 0x80E2-E9
        0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0xFF, 0xFF,
        // [163-183] 0x80EA-FE  (remaining 21 bytes → 0xFF padding)
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };

    // Calculate checksum: (~sum + 1) & 0xFF over all 184 config bytes
    uint8_t sum = 0;
    for (int i = 0; i < 184; i++) sum += cfg[i];
    uint8_t cksum = (~sum + 1) & 0xFF;

    // Write in 2 chunks (Wire buffer is 128 bytes: 2 addr + 126 data)
    bool ok = true;
    for (int off = 0; off < 184 && ok; off += 126) {
        uint16_t addr = 0x8047 + off;
        int n = ((184 - off) < 126) ? (184 - off) : 126;
        ok = gt911_i2c_write(addr, cfg + off, (uint8_t)n);
        delay(5);
    }
    // Write checksum at 0x80FF
    if (ok) ok = gt911_i2c_write(0x80FF, &cksum, 1);
    // Write config_fresh = 0x01 at 0x8100 → GT911 saves to flash
    uint8_t fresh = 0x01;
    if (ok) ok = gt911_i2c_write(0x8100, &fresh, 1);

    Serial.printf("[GT911] config write %s (cksum=0x%02X)\n", ok?"OK":"FAILED", cksum);
    if (ok) {
        // GT911 auto-applies and auto-resets after fresh=0x01.
        // DO NOT send 0x8040=0x02 here – that is the GT911 calibration command,
        // not a soft reset, and it will erase the saved config from flash.
        delay(300); // wait for GT911 auto-reset + flash save to complete
        Serial.println("[GT911] config saved to flash");
    }
}

static void board_init() {
    Wire.begin(CH32_I2C_SDA, CH32_I2C_SCL);
    Wire.setClock(400000);
    delay(50);

    // --- I2C bus scan diagnostic ------------------------------------------
    Serial.println("[disp] I2C scan (SDA=15, SCL=7):");
    bool found24 = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[disp]   found device @ 0x%02X\n", addr);
            if (addr == CH32_I2C_ADDR) found24 = true;
        }
    }
    if (!found24) {
        Serial.printf("[disp] WARNING: CH32V003 NOT found @ 0x%02X!\n", CH32_I2C_ADDR);
        Serial.println("[disp] Continuing anyway – display may not work.");
    }

    // --- CH32V003 + GT911 init -----------------------------------------------
    // Normal I2C-expander convention: bit=1 → pin HIGH, bit=0 → pin LOW
    // BEE_EN = bit6 (active-HIGH buzzer) → always keep 0 (LOW) = silent
    // #define CH32_BEE_EN (1<<6) = 0x40
    //
    // Bit-masks for selective control:
    //   0xBF = 0xFF & ~CH32_BEE_EN          all HIGH, buzzer silent
    //   0xBB = 0xBF & ~CH32_LCD_RST  (=~4)  LCD_RST=LOW, rest HIGH
    //   0xBE = 0xBF & ~CH32_TOUCH_RST(=~1)  TOUCH_RST=LOW, INT(bit1)=HIGH
    //   0xBC = 0xBF & ~0x03                 TOUCH_RST=LOW, INT=LOW

    // Start: set all pins as outputs, all HIGH (SYS_EN=1, RSTs released, BEE=0)
    ch32_write(CH32_REG_DIR, 0xFF);
    ch32_write(CH32_REG_OUT, 0xBF);   // all HIGH except BEE
    delay(50);

    // ---- Phase A: LCD reset pulse (only LCD_RST bit2; GT911 untouched) -----
    ch32_write(CH32_REG_OUT, 0xBF & ~CH32_LCD_RST);  // = 0xBB  LCD_RST=LOW
    delay(20);
    ch32_write(CH32_REG_OUT, 0xBF);                   // LCD_RST=HIGH, released
    delay(120);   // ST7701S needs 120 ms after RST before commands
    Serial.println("[disp] LCD RST done");

    // ---- Phase B: GT911 – do NOT assert TOUCH_RST ----------------------------
    // GT911 is left in the power-on state set by CH32V003 (all outputs HIGH
    // from CH32V003's startup = TOUCH_RST=HIGH = GT911 running normally).
    // Asserting TOUCH_RST here causes GT911 to reload from blank internal flash
    // every boot.  Instead: leave it running and set INT (bit1) as INPUT.
    ch32_write(CH32_REG_DIR, 0xFD);   // 0xFF & ~(1<<1) = INT pin = INPUT
    delay(5);
    Serial.println("[disp] GT911 INT set to INPUT (no RST)");

    // Backlight full brightness
    ch32_write(CH32_REG_PWM, 0x00);
    Serial.println("[disp] CH32V003 init done");

    delay(10);  // small extra settle time

    // Find which of the two possible GT911 addresses this panel uses. Must run
    // before any other GT911 access — units of this board ship with either.
    gt911_detect_addr();

    // Read product ID to confirm GT911 is present
    uint8_t pidBuf[4] = {};
    bool gt911Found = gt911_i2c_read(0x8140, pidBuf, 4);
    Serial.printf("[disp] GT911 @ 0x%02X: found=%d PID=\"%.4s\"\n",
        s_touchAddr, (int)gt911Found, (char*)pidBuf);

    // Read config version (0x8047): 0x00 = no valid config → write one
    uint8_t cfgVer = 0;
    gt911_i2c_read(0x8047, &cfgVer, 1);
    Serial.printf("[disp] GT911 cfg version=0x%02X\n", cfgVer);
    if (cfgVer == 0x00 || cfgVer == 0xFF) {
        gt911_write_config();   // writes 184-byte config for 480×480 panel
    }

    // Clear status flag to activate touch reporting
    uint8_t clr = 0x00;
    gt911_i2c_write(0x814E, &clr, 1);
    Serial.println("[disp] GT911 ready");
}

void setBrightness(uint8_t brightness) {
    // brightness 0=off, 255=full → CH32V003 PWM is inverted
    ch32_write(CH32_REG_PWM, 255 - brightness);
}

void gt911Diag() {
    // Read raw status byte from 0x814E (bit7=data-ready, bits[3:0]=touch count)
    Wire.beginTransmission(s_touchAddr);
    Wire.write(0x81); Wire.write(0x4E);
    int ack = Wire.endTransmission(false);
    if (ack != 0) {
        Serial.printf("[GT911] diag NACK ack=%d\n", ack);
        Serial.flush();
        return;
    }
    Wire.requestFrom((uint8_t)s_touchAddr, (uint8_t)1);
    if (!Wire.available()) {
        Serial.println("[GT911] diag: no data from requestFrom");
        Serial.flush();
        return;
    }
    uint8_t st = Wire.read();
    uint8_t cnt = st & 0x0F;
    bool rdy = (st & 0x80) != 0;

    if (rdy && cnt > 0) {
        // Read first touch point
        Wire.beginTransmission(s_touchAddr);
        Wire.write(0x81); Wire.write(0x50);
        if (Wire.endTransmission(false) == 0) {
            Wire.requestFrom((uint8_t)s_touchAddr, (uint8_t)7);
            if (Wire.available() >= 7) {
                Wire.read();  // track ID
                uint8_t xl=Wire.read(), xh=Wire.read();
                uint8_t yl=Wire.read(), yh=Wire.read();
                Wire.read(); Wire.read();
                uint16_t tx = xl|((uint16_t)xh<<8);
                uint16_t ty = yl|((uint16_t)yh<<8);
                Serial.printf("[GT911] status=0x%02X cnt=%u  TOUCH x=%u y=%u\n", st, cnt, tx, ty);
            }
        }
        // Clear flag
        Wire.beginTransmission(s_touchAddr);
        Wire.write(0x81); Wire.write(0x4E); Wire.write(0x00);
        Wire.endTransmission(true);
    } else {
        Serial.printf("[GT911] status=0x%02X (no touch)\n", st);
    // Read noise level registers to see if GT911 is sensing anything at all
    // Register 0x80FD = NOISE_THRESHOLD, 0x8800 area = raw sense data
    static uint8_t s_probeCnt = 0;
    if (++s_probeCnt <= 3) {
        uint8_t noise = 0;
        gt911_i2c_read(0x80F0, &noise, 1);  // noise flag register
        Serial.printf("[GT911] noise_flag=0x%02X\n", noise);
    }
    // Once: read GT911 config registers 0x8047-0x804E
    // ver(1) + xmax_lo + xmax_hi + ymax_lo + ymax_hi + touch_num + sw1 + sw2
    static bool s_cfgDone = false;
    if (!s_cfgDone) {
        s_cfgDone = true;
        Wire.beginTransmission(s_touchAddr);
        Wire.write(0x80); Wire.write(0x47);
        int cfgAck = Wire.endTransmission(false);   // repeated-start
        uint8_t nRec = Wire.requestFrom((uint8_t)s_touchAddr, (uint8_t)8);
        Serial.printf("[GT911] cfg read: ack=%d nRec=%d avail=%d\n",
            cfgAck, nRec, Wire.available());
        if (Wire.available() >= 8) {
            uint8_t ver  = Wire.read();
            uint16_t xmax = Wire.read() | ((uint16_t)Wire.read() << 8);
            uint16_t ymax = Wire.read() | ((uint16_t)Wire.read() << 8);
            uint8_t tnum  = Wire.read();
            uint8_t sw1   = Wire.read();
            uint8_t sw2   = Wire.read();
            Serial.printf("[GT911] ver=0x%02X xmax=%u ymax=%u tnum=%u sw1=0x%02X sw2=0x%02X\n",
                ver, xmax, ymax, tnum, sw1, sw2);
            // If xmax/ymax are 0 or wrong, GT911 won't detect touches –
            // write correct resolution for 480x480 panel
            if (xmax != LCD_WIDTH || ymax != LCD_HEIGHT) {
                Serial.printf("[GT911] WRONG resolution (expected %ux%u) – writing correct config\n",
                    LCD_WIDTH, LCD_HEIGHT);
                // Write X_OUTPUT_MAX (0x8048) and Y_OUTPUT_MAX (0x804A)
                uint8_t cfgPatch[] = {
                    0x80, 0x48,
                    (uint8_t)(LCD_WIDTH & 0xFF),  (uint8_t)(LCD_WIDTH  >> 8),
                    (uint8_t)(LCD_HEIGHT & 0xFF), (uint8_t)(LCD_HEIGHT >> 8)
                };
                Wire.beginTransmission(s_touchAddr);
                for (uint8_t b : cfgPatch) Wire.write(b);
                Wire.endTransmission(true);
                Serial.println("[GT911] Resolution patched – restarting GT911 via RST");
            } else {
                Serial.println("[GT911] Resolution correct");
            }
        } else {
            // Flush any remaining bytes
            while (Wire.available()) Wire.read();
        }
    }
    }
    Serial.flush();
}

void displayDiag() {
    int dir = ch32_read(CH32_REG_DIR);
    int out = ch32_read(CH32_REG_OUT);
    int pwm = ch32_read(CH32_REG_PWM);
    // CH32V003 inverted/open-drain polarity:
    //   write 0 to a bit → that GPIO is HIGH (released/enabled)
    //   write 1 to a bit → that GPIO is LOW  (pulled down / asserted)
    // So SYS_EN (bit 4) is ACTIVE when stored bit = 0, INACTIVE when bit = 1.
    // Expected normal state: out = 0x20 (only BEE_EN bit=1 = buzzer silent, all others=0 = HIGH)
    // Normal convention: bit=1 → pin HIGH, bit=0 → pin LOW
    // Normal run state: stored=0xFF (all bits HIGH)
    //   BEE_EN (bit5) = 1 → pin HIGH → buzzer OFF (active-low: HIGH=OFF)
    bool sys_en_ok  = (out != -1) && (out & CH32_SYS_EN);   // bit4=1 → SYS_EN HIGH = OK
    bool lcd_rst_ok = (out != -1) && (out & CH32_LCD_RST);  // bit2=1 → LCD_RST HIGH = OK
    bool bee_off    = (out != -1) && !(out & CH32_BEE_EN);  // bit6=0 → BEE_EN LOW = silent (active-HIGH buzzer)
    Serial.printf("[CH32] stored=0x%02X pwm=0x%02X  SYS_EN=%s LCD_RST=%s BEE=%s\n",
        out, pwm,
        sys_en_ok  ? "HIGH(OK)" : "LOW(!)",
        lcd_rst_ok ? "HIGH(OK)" : "LOW(!)",
        bee_off    ? "OFF"      : "ON(!)");
    Serial.flush();
}

// ---- LovyanGFX LGFX class ---------------------------------------------------

LGFX gfx;

LGFX::LGFX() {
    // --- Panel config ---
    {
        auto cfg = _panel_instance.config();
        cfg.memory_width  = LCD_WIDTH;
        cfg.memory_height = LCD_HEIGHT;
        cfg.panel_width   = LCD_WIDTH;
        cfg.panel_height  = LCD_HEIGHT;
        cfg.offset_x      = 0;
        cfg.offset_y      = 0;
        cfg.rgb_order     = false;   // swap R/B if colors look wrong
        cfg.invert        = false;   // set true if colors are inverted
        cfg.readable      = false;
        _panel_instance.config(cfg);
    }

    // --- 3-wire SPI for ST7701S init commands --------------------------------
    {
        auto cfg = _panel_instance.config_detail();
        cfg.pin_cs   = LCD_SPI_CS;   // GPIO42
        cfg.pin_sclk = LCD_SPI_SCL;  // GPIO2
        cfg.pin_mosi = LCD_SPI_SDA;  // GPIO1
        // pin_rst not available in config_detail — RST handled in board_init()
        cfg.use_psram = 2;           // allocate frame buffer in PSRAM (2=PSRAM only)
        _panel_instance.config_detail(cfg);
    }

    // --- 16-bit RGB parallel bus for pixel data ------------------------------
    {
        auto cfg = _bus_instance.config();
        cfg.panel = &_panel_instance;

        // Data pins: B0–B4, G0–G5, R0–R4
        cfg.pin_d0  = LCD_D0;   // B0
        cfg.pin_d1  = LCD_D1;   // B1
        cfg.pin_d2  = LCD_D2;   // B2
        cfg.pin_d3  = LCD_D3;   // B3
        cfg.pin_d4  = LCD_D4;   // B4
        cfg.pin_d5  = LCD_D5;   // G0
        cfg.pin_d6  = LCD_D6;   // G1
        cfg.pin_d7  = LCD_D7;   // G2
        cfg.pin_d8  = LCD_D8;   // G3
        cfg.pin_d9  = LCD_D9;   // G4
        cfg.pin_d10 = LCD_D10;  // G5
        cfg.pin_d11 = LCD_D11;  // R0
        cfg.pin_d12 = LCD_D12;  // R1
        cfg.pin_d13 = LCD_D13;  // R2
        cfg.pin_d14 = LCD_D14;  // R3
        cfg.pin_d15 = LCD_D15;  // R4

        cfg.pin_henable = LCD_DE;
        cfg.pin_vsync   = LCD_VSYNC;
        cfg.pin_hsync   = LCD_HSYNC;
        cfg.pin_pclk    = LCD_PCLK;
        cfg.freq_write  = 14000000; // 14 MHz pixel clock (matches LovyanGFX reference)

        // Timing from LovyanGFX reference implementation for ST7701 480×480.
        // de_idle_high=1 is critical: DE (Data Enable) idles HIGH.
        // Wrong value (0) causes flicker/horizontal shift.
        cfg.hsync_polarity    = 0;
        cfg.hsync_front_porch = 10;
        cfg.hsync_pulse_width = 8;
        cfg.hsync_back_porch  = 50;
        cfg.vsync_polarity    = 0;
        cfg.vsync_front_porch = 10;
        cfg.vsync_pulse_width = 8;
        cfg.vsync_back_porch  = 20;
        cfg.pclk_idle_high    = 0;
        cfg.de_idle_high      = 1;   // ← critical: DE idles HIGH
        cfg.pclk_active_neg   = 0;

        _bus_instance.config(cfg);
        _panel_instance.setBus(&_bus_instance);
    }

    // Touch_GT911 NOT registered via LovyanGFX: LovyanGFX uses its own low-level
    // I2C driver that conflicts with Arduino Wire (both manipulate I2C port 0
    // hardware registers).  Touch is handled in lvgl_touch_cb() via Wire instead.

    setPanel(&_panel_instance);
}

// ---- LVGL callbacks ---------------------------------------------------------

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

static uint32_t s_flushCount = 0;
static uint32_t s_flushPassCount = 0;  // full-screen renders per second

static void lvgl_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    esp_task_wdt_reset();
    s_flushCount++;
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    if (s_flushCount <= 3) {
        Serial.printf("[flush#%u] (%u,%u)-(%u,%u) %ux%u\n",
            s_flushCount, area->x1, area->y1, area->x2, area->y2, w, h);
        Serial.flush();
    }
    gfx.startWrite();
    gfx.setAddrWindow(area->x1, area->y1, w, h);
    gfx.writePixels((lgfx::rgb565_t *)&color_p->full, w * h);
    gfx.endWrite();
    lv_disp_flush_ready(disp);
    // Count complete frames: last strip ends at bottom of screen
    if (area->y2 >= LCD_HEIGHT - 1) s_flushPassCount++;
}

// ---- GT911 direct Wire polling ----------------------------------------------
// GT911 uses 16-bit register addresses.  We read status at 0x814E, then the
// first touch point coordinates from 0x8150, then clear the flag.
// All on the same Wire bus as CH32V003 (SDA=15, SCL=7, addr=0x14).
// Called from the LVGL touch callback which runs in loop() (Core 1, sequential
// with displayDiag) — no concurrent Wire access possible.

static bool gt911_read_touch(uint16_t *x, uint16_t *y) {
    // Read status register 0x814E via repeated-start
    uint8_t status = 0;
    if (!gt911_i2c_read(0x814E, &status, 1)) return false;

    // Bit7 = buffer-ready, bits[3:0] = touch count
    static uint8_t last_status = 0xFF;
    if (status != last_status) {
        Serial.printf("[gt911] status=0x%02X\n", status);
        Serial.flush();
        last_status = status;
    }

    // IMPORTANT: always clear the buffer-status flag after reading!
    // GT911 will NOT update the status register until 0x00 is written back.
    // If we skip the clear when count=0, GT911 freezes at 0x80 forever.
    bool result = false;
    if ((status & 0x80) && (status & 0x0F) > 0) {
        // GT911 Programming Guide Rev.10: each touch point = 8 bytes at 0x8150+
        // Byte 0: X_lo   Byte 1: X_hi   (16-bit little-endian)
        // Byte 2: Y_lo   Byte 3: Y_hi   (16-bit little-endian)
        // Byte 4: Size_lo Byte 5: Size_hi  Byte 6: Reserved  Byte 7: Track ID
        // (Track ID is at the END, byte 7, not byte 0!)
        uint8_t pt[6] = {};  // read Xlo,Xhi,Ylo,Yhi,Slo,Shi — skip reserved/trackID
        if (gt911_i2c_read(0x8150, pt, 6)) {
            *x = pt[0] | ((uint16_t)pt[1] << 8);   // Xlo | (Xhi << 8) little-endian
            *y = pt[2] | ((uint16_t)pt[3] << 8);   // Ylo | (Yhi << 8) little-endian
            result = true;
        }
    }
    // Always acknowledge by clearing the buffer-status flag
    uint8_t clr = 0x00;
    gt911_i2c_write(0x814E, &clr, 1);
    return result;
}

// ── Swipe gesture tracker ─────────────────────────────────────────────────────
// Detects horizontal swipe independently of LVGL's gesture engine.
// A swipe is only fired when:
//   - Total horizontal movement >= UI_SWIPE_THRESHOLD pixels
//   - Vertical movement is less than horizontal (not a scroll)
//   - Touch started in the middle 2/3 of the screen (not on overlay buttons)
static bool     s_swipe_active  = false;
static uint16_t s_swipe_start_x = 0;
static uint16_t s_swipe_start_y = 0;
static uint16_t s_swipe_last_x  = 0;
static uint16_t s_swipe_last_y  = 0;
static bool     s_swipe_done    = false;  // prevent repeated fires per gesture
// Set by swipeSuppress() when a widget (e.g. a volume slider) claims the drag.
static bool     s_swipe_suppress = false;

void swipeSuppress() { s_swipe_suppress = true; }

static void lvgl_touch_cb(lv_indev_drv_t *indev, lv_indev_data_t *data) {
    uint16_t x = 0, y = 0;
    bool pressed = gt911_read_touch(&x, &y);

    if (pressed) {
        Entropy::feed(x, y);   // touches during initial commissioning add to the random mix
        s_swipe_last_x = x;
        s_swipe_last_y = y;
        if (!s_swipe_active) {
            s_swipe_active   = true;
            s_swipe_done     = false;
            s_swipe_suppress = false;   // a widget may claim this touch below
            s_swipe_start_x = x;
            s_swipe_start_y = y;
            // Any new touch: request arrow show (safe flag, no LVGL calls here!)
            dispMgr.requestShowNavArrows();
        }
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;
    } else {
        // Touch released – evaluate swipe
        if (s_swipe_active && !s_swipe_done) {
            int dx = (int)s_swipe_last_x - (int)s_swipe_start_x;
            int dy = (int)s_swipe_last_y - (int)s_swipe_start_y;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            // Valid swipe: horizontal > threshold, more horizontal than vertical,
            // and did not start inside the overlay button zone (left/right 1/6)
            bool notOnButton = (s_swipe_start_x > LCD_WIDTH/6) &&
                               (s_swipe_start_x < LCD_WIDTH - LCD_WIDTH/6);
            if (adx >= UI_SWIPE_THRESHOLD && adx > ady && notOnButton && !s_swipe_suppress) {
                s_swipe_done = true;
                // dispMgr declared in DisplayManager.h (included above)
                if (dx < 0) dispMgr.nextScreen();   // swipe left  → next
                else        dispMgr.prevScreen();   // swipe right → prev
            }
        }
        s_swipe_active = false;
        data->state = LV_INDEV_STATE_REL;
    }
}

// ---- Public -----------------------------------------------------------------

void displayInit() {
    // 1. Initialise CH32V003 IO expander first – this asserts SYS_EN so the
    //    ST7701S panel receives power and the LCD reset is released.
    board_init();

    Serial.println("[disp] gfx.begin() ...");
    bool ok = gfx.begin();
    Serial.printf("[disp] gfx.begin() = %d  (%dx%d)\n", ok, gfx.width(), gfx.height());
    if (!ok) {
        Serial.println("[disp] !!! Panel init failed – check RGB/SPI pins in BoardConfig.h");
    }

    gfx.fillScreen(TFT_BLACK);

    lv_init();
    Serial.println("[disp] lv_init() done");

    // LVGL draw buffers (DMA-capable DRAM, 20 rows each)
    size_t buf_sz = LCD_WIDTH * 20 * sizeof(lv_color_t);
    buf1 = (lv_color_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    buf2 = (lv_color_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    Serial.printf("[disp] DMA buffers: buf1=%p  buf2=%p  (%u bytes each)\n", buf1, buf2, buf_sz);
    if (!buf1 || !buf2) {
        Serial.println("[disp] !!! FATAL: cannot allocate LVGL draw buffers");
        while (1) delay(1000);
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_sz / sizeof(lv_color_t));

    // Register display driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = LCD_WIDTH;
    disp_drv.ver_res  = LCD_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // Register touch input driver
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);
}

uint32_t getTickFps(bool reset) {
    uint32_t v = s_flushPassCount;
    if (reset) s_flushPassCount = 0;
    return v;
}

void displayTick() {
    esp_task_wdt_reset();
    lv_timer_handler();
}
