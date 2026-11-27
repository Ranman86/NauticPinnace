/*
 * NMEA2000_esp32.cpp  –  TWAI-based CAN driver for ESP32 and ESP32-S3
 *
 * Licence: re-write on the public ESP-IDF twai_* API; only a few lines follow
 * upstream ttlappalainen/NMEA2000_esp32 (see NMEA2000_esp32.h). The public
 * interface mirrors that library (MIT, Copyright (c) 2015-2020 Timo
 * Lappalainen, Kave Oy) for drop-in compatibility; details in
 * NMEA2000_esp32.h.  Copyright (c) 2026 Ranman86.
 *
 * Uses ESP-IDF driver/twai.h instead of bare-metal CAN register access,
 * so it compiles on both ESP32 (PERIPH_CAN_MODULE era) and ESP32-S3
 * (PERIPH_TWAI_MODULE era).
 *
 * NMEA 2000 uses:
 *   - 29-bit extended CAN frames
 *   - 250 Kbps bus speed
 *   - No RTR frames
 */

#include "NMEA2000_esp32.h"
#include "driver/twai.h"
#include "esp_attr.h"
#include "esp_idf_version.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#if ESP_IDF_VERSION_MAJOR >= 5
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_rom_uart.h"
#include "hal/uart_ll.h"
#include "soc/soc_caps.h"
#else
#include "esp32s3/rom/rtc.h"
#include "esp_cpu.h"
#include "esp_rom_uart.h"
#include "hal/cpu_hal.h"
#include "xtensa/xtensa_api.h"
#endif
#include <Arduino.h>   // millis() and the bus-off log lines

bool tNMEA2000_esp32::CanInUse     = false;
bool tNMEA2000_esp32::HwListenOnly = false;

// ---- Restart: keep the controller off the bus --------------------------------
//
// IDF's esp_restart_noos() - which no build with this library runs any more, see
// the wrapper below - resets the CPUs, but neither the TWAI controller nor the
// GPIO matrix, and on the way it puts the CPU and the APB clock back on the 40 MHz
// crystal (rtc_clk_cpu_set_to_default_config). The controller, still running and
// still wired to its pins, then works at half the bit rate - 125 instead of
// 250 kbit/s - until the second-stage bootloader raises the clock again, and puts
// an error flag on every frame it misreads. Measured with the 7B listening: about
// 300 bus errors per restart from the web interface, on the 5B and on the 4",
// enough to drive a node that is sending at that moment bus-off - on the bench
// the CANable, the only sender of data, whose firmware never comes back from
// bus-off, so every display lost every value until the simulator was restarted.
//
// So before the reset the TX pin is taken from the controller and driven high,
// recessive, and the reset is a digital system reset, which stops the controller
// as well. That reset also returns the pin to its default: from then until the
// next CANOpen() the firmware does not drive it. Measured on 7B and 5B with this
// reset: 0 or 1 bus errors per restart. The 4" has not been measured with it yet.
//
// Every restart goes through esp_restart_noos() - esp_restart() after its
// shutdown handlers, a panic, a watchdog, a reset over the USB console - and
// extra_script.py links every call to it through __wrap_esp_restart_noos()
// below (-Wl,--wrap=esp_restart_noos). Interrupts may be off and the flash
// cache with them, hence IRAM, register writes and ROM calls only.

static volatile int s_parkTxPin = -1;   // TX pin of the open controller, or -1

static void IRAM_ATTR parkTxPin() {
    const int pin = s_parkTxPin;
    if (pin < 0) return;
    const uint32_t bit = 1u << (pin & 31);
    // Level and enable first: while the pin still carries the controller's
    // signal they change nothing, and after the switch it is high at once.
    if (pin < 32) {
        REG_WRITE(GPIO_OUT_W1TS_REG, bit);
        REG_WRITE(GPIO_ENABLE_W1TS_REG, bit);
    } else {
        REG_WRITE(GPIO_OUT1_W1TS_REG, bit);
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, bit);
    }
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
}

// The digital system reset IDF itself uses on the ESP32-S3 in its brownout
// handler and in bootloader_reset(): flush the UARTs, then the ROM's software
// reset - no cache shutdown, no clock switch (the clock and MSPI settings are
// digital registers, which this reset restores). A core a panic handler stalled
// is released first: its stall bit sits in the RTC domain, survives this reset,
// and a stalled core 0 would never boot.
static void IRAM_ATTR systemReset() {
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_cpu_intr_disable(0xFFFFFFFF);
    for (int i = 0; i < SOC_UART_HP_NUM; ++i)
        if (uart_ll_is_enabled(i)) esp_rom_output_tx_wait_idle(i);
    const int self = (int)esp_cpu_get_core_id();
    for (int i = 0; i < SOC_CPU_CORES_NUM; ++i)
        if (i != self) esp_cpu_unstall(i);
    esp_rom_software_reset_system();
#else
    // The same on IDF 4.4 (the 4"), which has no check for a UART that is off:
    // the console's alone, the only one the boot leaves on for certain.
    xt_ints_off(0xFFFFFFFF);
    esp_rom_uart_tx_wait_idle(0);
    esp_cpu_unstall(!cpu_hal_get_core_id());
    software_reset();
#endif
    while (true) {}
}

extern "C" void IRAM_ATTR __wrap_esp_restart_noos(void) {
    parkTxPin();
    // A digital system reset instead of IDF's CPU reset, on every board.
    // esp_restart_noos() resets and stalls the other core and then switches
    // the cache off - and on these boards Cache_Disable_ICache() never
    // returned (seen with ROM markers on the 7B: every restart of the current
    // firmware tried on 7B and 5B, now and then one of older builds, e.g.
    // right after a WiFi update). Its 1 s RTC watchdog then reset the system,
    // which could not start either: the other core's stall bit lives in the
    // RTC domain and survives a system reset. Only the RTC reset a second
    // later booted (rst:0x10 RTCWDT_RTC_RST), and the boot after it counts as
    // a crash - after an update, three restarts in a row made the crash-loop
    // guard roll the new firmware back. This reset skips the cache shutdown,
    // releases the other cores first and resets every digital peripheral, the
    // CAN controller included.
    //
    // The 4" (IDF 4.4) hangs there as well: every second web restart came back as
    // rst:0x10 (measured 04.11.2026 with -DPANIC_PATH_TEST: 7 of 14, reason
    // ESP_RST_WDT). That boot comes up, so it looked clean from outside, but
    // OtaGuard counts it as a crash: the 15th, the third in a row, rolled the
    // image back with nothing but plain restarts.
    systemReset();
}

// ---- Constructor -------------------------------------------------------------

tNMEA2000_esp32::tNMEA2000_esp32(gpio_num_t _TxPin, gpio_num_t _RxPin)
    : tNMEA2000(), IsOpen(false), TxPin(_TxPin), RxPin(_RxPin) {}

// ---- Buffer sizing -----------------------------------------------------------

void tNMEA2000_esp32::InitCANFrameBuffers() {
    // Ensure the library allocates generous internal buffers.
    if (MaxCANReceiveFrames < 10) MaxCANReceiveFrames = 50;
    if (MaxCANSendFrames    < 10) MaxCANSendFrames    = 40;
    tNMEA2000::InitCANFrameBuffers();
}

// ---- Open --------------------------------------------------------------------

bool tNMEA2000_esp32::CANOpen() {
    if (IsOpen)   return true;
    if (CanInUse) return false;

    // Listen-only: the controller never drives the bus, not even ACK bits.
    // TX is impossible in that mode, so the TX queue is disabled as well.
    twai_general_config_t g_config =
        TWAI_GENERAL_CONFIG_DEFAULT(TxPin, RxPin,
            HwListenOnly ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
    g_config.tx_queue_len = HwListenOnly ? 0 : 10;
    g_config.rx_queue_len = 65;

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK)
        return false;

    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return false;
    }

    IsOpen   = true;
    CanInUse = true;
    s_parkTxPin = TxPin;   // from now on a restart parks it (see above)
    return true;
}

// ---- Send --------------------------------------------------------------------

bool tNMEA2000_esp32::CANSendFrame(unsigned long id, unsigned char len,
                                   const unsigned char *buf, bool wait_sent) {
    twai_message_t msg = {};
    msg.extd             = 1;                   // N2K always uses 29-bit IDs
    msg.identifier       = id & 0x1FFFFFFFul;
    msg.data_length_code = len > 8 ? 8 : len;
    memcpy(msg.data, buf, msg.data_length_code);

    TickType_t timeout = wait_sent ? pdMS_TO_TICKS(5) : 0;
    return twai_transmit(&msg, timeout) == ESP_OK;
}

// ---- Receive -----------------------------------------------------------------

bool tNMEA2000_esp32::CANGetFrame(unsigned long &id, unsigned char &len,
                                  unsigned char *buf) {
    twai_message_t msg;
    if (twai_receive(&msg, 0) != ESP_OK) return false;
    // N2K only uses extended, non-RTR frames – discard anything else
    if (!msg.extd || msg.rtr) return false;

    id  = msg.identifier;
    len = msg.data_length_code;
    memcpy(buf, msg.data, len);
    return true;
}

// ---- Transmit path idle ------------------------------------------------------

bool tNMEA2000_esp32::TxIdle() {
    if (CANSendFrameBufferWrite != CANSendFrameBufferRead) return false;
    if (!IsOpen) return true;
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) return true;
    // msgs_to_tx counts the queued frames AND the one being transmitted.
    return st.msgs_to_tx == 0;
}

// ---- Bus-off recovery --------------------------------------------------------
//
// The TWAI state machine after a bus-off: BUS_OFF -> (twai_initiate_recovery)
// RECOVERING -> STOPPED once 128 x 11 recessive bits were seen -> (twai_start)
// RUNNING. Nothing in the driver walks this by itself.

void tNMEA2000_esp32::ServiceBus() {
    // Listen-only never transmits, so it cannot go bus-off either.
    if (!IsOpen || HwListenOnly) return;
    const uint32_t now = millis();
    if (now - LastServiceMs < 1000) return;
    LastServiceMs = now;

    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) {   // the driver is gone
        if (!StuckSeen) { StuckSeen = true; StuckAtMs = now; }
        return;
    }
    if (st.state == TWAI_STATE_RUNNING) {
        if (DownSeen) {
            Serial.printf("[n2k] bus running again after %u ms\n",
                          (unsigned)(now - DownAtMs));
            DownSeen = false;
        }
        StuckSeen = false;
        return;
    }
    if (!DownSeen) { DownSeen = true; DownAtMs = now; }

    switch (st.state) {
        case TWAI_STATE_BUS_OFF:
            // Taken care of: recovered at once, or after the back-off below.
            StuckSeen = false;
            // First episode: recover at once. A repeat within 10 s of the last
            // recovery waits - a dead short would otherwise keep the
            // controller cycling through recoveries once a second.
            if (BusOffEpisodes > 0 && now - LastRecoveryMs < 10000) return;
            BusOffEpisodes = BusOffEpisodes + 1;   // no ++ on a volatile (C++20)
            LastRecoveryMs = now;
            Serial.printf("[n2k] BUS-OFF (episode %u, tx_err=%u rx_err=%u) - recovering\n",
                          (unsigned)BusOffEpisodes, (unsigned)st.tx_error_counter,
                          (unsigned)st.rx_error_counter);
            if (twai_initiate_recovery() != ESP_OK)
                Serial.println("[n2k] twai_initiate_recovery failed");
            return;
        case TWAI_STATE_STOPPED:
            // Recovery has completed (or the driver was stopped otherwise):
            // start it again. A controller that will not start is the one
            // state nothing here can bring back (StuckSinceMs).
            if (twai_start() == ESP_OK) {
                Serial.println("[n2k] bus-off recovery complete - controller restarted");
                StuckSeen = false;
            } else if (!StuckSeen) {
                Serial.println("[n2k] twai_start after recovery failed");
                StuckSeen = true;
                StuckAtMs = now;
            }
            return;
        default:   // TWAI_STATE_RECOVERING: wait for the 128 recessive sequences
            StuckSeen = false;
            return;
    }
}
