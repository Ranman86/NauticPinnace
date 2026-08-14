/*
 * NMEA2000_esp32.h  –  TWAI-based CAN driver for ESP32 and ESP32-S3
 *
 * Drop-in replacement for ttlappalainen/NMEA2000_esp32 that uses the
 * ESP-IDF TWAI driver (driver/twai.h) instead of bare-metal register
 * access.  Works on both ESP32 and ESP32-S3.
 *
 * Licence: the IMPLEMENTATION is an independent re-write for this project on
 * the public ESP-IDF twai_* API — no function bodies were taken from
 * ttlappalainen/NMEA2000_esp32 (which drives the CAN registers directly).
 * The PUBLIC INTERFACE deliberately mirrors that upstream library (MIT,
 * Copyright (c) 2015-2020 Timo Lappalainen, Kave Oy) so it works as a
 * drop-in replacement: class shape, the ESP32_CAN_TX_PIN / _RX_PIN
 * configuration mechanism, constructor defaults, and a couple of member
 * names.  The base class tNMEA2000 comes from the NMEA2000 library by the
 * same author (MIT) — see LICENSES/MIT-NMEA2000.txt.
 *
 * Copyright (c) 2026 Ranman86.  MIT licence — see LICENSE at the repo root.
 */

#ifndef _NMEA2000_ESP32_H_
#define _NMEA2000_ESP32_H_

#include "driver/gpio.h"
#include "NMEA2000.h"
#include "N2kMsg.h"

#ifndef ESP32_CAN_TX_PIN
#define ESP32_CAN_TX_PIN GPIO_NUM_16
#endif
#ifndef ESP32_CAN_RX_PIN
#define ESP32_CAN_RX_PIN GPIO_NUM_4
#endif

class tNMEA2000_esp32 : public tNMEA2000 {
private:
    bool IsOpen;
    static bool CanInUse;
    static bool HwListenOnly;   // open the controller in TWAI_MODE_LISTEN_ONLY
    gpio_num_t TxPin;
    gpio_num_t RxPin;

protected:
    bool CANSendFrame(unsigned long id, unsigned char len,
                      const unsigned char *buf, bool wait_sent = true);
    bool CANOpen();
    bool CANGetFrame(unsigned long &id, unsigned char &len, unsigned char *buf);
    virtual void InitCANFrameBuffers();

public:
    tNMEA2000_esp32(gpio_num_t _TxPin = ESP32_CAN_TX_PIN,
                    gpio_num_t _RxPin = ESP32_CAN_RX_PIN);

    // Extension over the upstream interface: N2km_ListenOnly only silences the
    // NMEA2000 library (no messages), but a controller in TWAI_MODE_NORMAL
    // still puts ACK bits on the wire. Call this BEFORE Open() to make
    // listen-only electrically passive too.
    static void SetHwListenOnly(bool v) { HwListenOnly = v; }
};

#endif  // _NMEA2000_ESP32_H_
