/*
 * NMEA2000_esp32.cpp  –  TWAI-based CAN driver for ESP32 and ESP32-S3
 *
 * Licence: independent re-write on the public ESP-IDF twai_* API — no
 * function bodies from ttlappalainen/NMEA2000_esp32.  The public interface
 * mirrors that upstream library (MIT, Copyright (c) 2015-2020 Timo
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

bool tNMEA2000_esp32::CanInUse     = false;
bool tNMEA2000_esp32::HwListenOnly = false;

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
