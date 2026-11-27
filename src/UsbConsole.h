#pragma once
// ============================================================================
// UsbConsole - the 5B's USB console and its short TX timeout.
//
// The whole story is in main.cpp ("A USB console nobody reads"). In short:
// from the end of setup() on, the 5B runs its console (the S3's USB-Serial/
// JTAG, Arduino 3.x's HWCDC) with a TX timeout of USB_TX_TIMEOUT_MS instead
// of the core's 100 ms, so that a host on the socket which does not read
// costs a write 0.1 s instead of 2 s. The core uses the same value in
// HWCDC::flush(): after that many 1-ms polls without progress, counted over
// the whole flush, it drops what is still queued. A host that reads gets
// through that; a line dropped right before a restart, though, is gone for
// good, so the last lines before one go out with consoleFlushFull().
//
// The 4" (Arduino 2.0.17) and the 7B (a UART console) keep the core's
// defaults; there everything here is a plain Serial.flush().
// ============================================================================
#include <Arduino.h>

#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE && ESP_ARDUINO_VERSION_MAJOR >= 3
#define USB_CONSOLE_BOUNDED 1
static constexpr uint32_t USB_TX_TIMEOUT_MS      = 5;     // from the end of setup() on
static constexpr uint32_t USB_TX_TIMEOUT_FULL_MS = 100;   // HWCDC's default
#else
#define USB_CONSOLE_BOUNDED 0
#endif

// Serial.flush() for the last lines before a restart or a deliberate hang:
// on the 5B with the core's full margin again - 100 polls without progress
// before it gives up - and then back to the short timeout. With a host that
// does not read this costs 0.1 s once, right before the device goes away.
inline void consoleFlushFull() {
#if USB_CONSOLE_BOUNDED
    Serial.setTxTimeoutMs(USB_TX_TIMEOUT_FULL_MS);
    Serial.flush();
    Serial.setTxTimeoutMs(USB_TX_TIMEOUT_MS);
#else
    Serial.flush();
#endif
}
