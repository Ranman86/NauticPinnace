/*
 * NMEA2000_esp32.h  –  TWAI-based CAN driver for ESP32 and ESP32-S3
 *
 * Drop-in replacement for ttlappalainen/NMEA2000_esp32 that uses the
 * ESP-IDF TWAI driver (driver/twai.h) instead of bare-metal register
 * access.  Works on both ESP32 and ESP32-S3.
 *
 * Licence: the IMPLEMENTATION is a re-write for this project on the public
 * ESP-IDF twai_* API (upstream ttlappalainen/NMEA2000_esp32 drives the CAN
 * registers directly). Only a few lines follow upstream: the buffer-size
 * defaults in InitCANFrameBuffers(), the IsOpen/CanInUse guard at the top of
 * CANOpen() and the constructor's initialiser list. The PUBLIC INTERFACE
 * mirrors that library (MIT, Copyright (c) 2015-2020 Timo Lappalainen, Kave Oy)
 * for drop-in use: class shape, the ESP32_CAN_TX_PIN / _RX_PIN mechanism,
 * constructor defaults, a couple of member names. The base class tNMEA2000 is
 * from the NMEA2000 library by the same author (MIT): LICENSES/MIT-NMEA2000.txt.
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

    // Extension over the upstream interface: NMEA2000_CAN.h constructs the
    // single instance during STATIC INITIALISATION, so the pins come from the
    // compile-time defaults long before the config file on flash can be read.
    // The values are used in exactly one place - CANOpen() builds the TWAI
    // config from them - so overriding them any time before Open() is safe.
    // Call it after the config is loaded; N2kHandler::begin() does.
    void SetCANPins(gpio_num_t tx, gpio_num_t rx) { TxPin = tx; RxPin = rx; }

    // Extension: true when nothing of ours is waiting to go onto the bus - the
    // library's own send ring is empty AND the TWAI driver has no frame queued
    // or in flight. The update receiver waits for this before a unit of flash
    // work (docs/N2K-UPDATE.md 9.1): on the 4" board the TWAI interrupt is not
    // in IRAM, so frames still queued when the cache goes off would sit there
    // for the whole erase. The ring indices are protected members of
    // tNMEA2000, which is why this lives in the driver class.
    bool TxIdle();

    // Extension: bus-off recovery. A TWAI controller that went bus-off (error
    // counter past 255, e.g. a short between CAN-H and CAN-L) stays silent
    // until twai_initiate_recovery() and a fresh twai_start() - upstream never
    // calls either, so until now the display needed a restart. Call it on
    // every pass of the N2K loop; it looks at the driver once a second, and
    // after a repeat it waits at least 10 s between recoveries so a dead short
    // does not turn into a recovery storm.
    void ServiceBus();

    // Bus-off episodes since boot, and since when the controller has not been
    // RUNNING (bus-off, recovering, stopped) - false while it runs. For the
    // OtaGuard health check H4 (docs/N2K-UPDATE.md 10.2), which tolerates a
    // bus-off that recovers but not a controller that stays down.
    uint32_t BusOffCount() const { return BusOffEpisodes; }
    bool     DownSinceMs(uint32_t &ms) const { ms = DownAtMs; return DownSeen; }
    // Down AND beyond ServiceBus(): the controller is STOPPED and twai_start()
    // fails, or the driver is gone. A bus-off and its recovery - also one
    // that keeps failing on a miswired bus - do not count; that is a wiring
    // fault, which H4 tolerates.
    bool     StuckSinceMs(uint32_t &ms) const { ms = StuckAtMs; return StuckSeen; }

private:
    volatile uint32_t BusOffEpisodes = 0;
    uint32_t LastServiceMs    = 0;
    uint32_t LastRecoveryMs   = 0;   // when the last recovery was started
    volatile uint32_t DownAtMs = 0;  // first time the driver was seen not RUNNING
    volatile bool DownSeen    = false;
    volatile uint32_t StuckAtMs = 0; // first time it was seen STOPPED for good
    volatile bool StuckSeen   = false;
};

#endif  // _NMEA2000_ESP32_H_
