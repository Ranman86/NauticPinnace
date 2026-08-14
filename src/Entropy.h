#pragma once
#include <stdint.h>
#include <stddef.h>
#include <Arduino.h>
#ifndef SIMULATOR
#include <esp_system.h>   // esp_random(): hardware TRNG of the ESP32-S3
#else
#include <cstdlib>
#endif

// ============================================================
// Entropy – random password for the internal hotspot.
//
// BACKGROUND: The old scheme derived the AP password from the MAC
// ("MdPw" + full MAC). But the SSID broadcasts the last three MAC bytes,
// and the first three are an Espressif prefix from a small public
// list — so with the source code published, the password could be
// guessed from the SSID. Therefore: a true random password, generated
// once per device and stored in the configuration.
//
// SOURCES: The basis is esp_random() (hardware TRNG). In addition, a
// small pool collects the touches of the initial commissioning
// (coordinates and timestamps of the language selection and licence
// scrolling) — feed() is fed from the touch callback. The mix can never
// be weaker than the strongest source; the pool does not replace the
// TRNG, it supplements it.
// ============================================================
namespace Entropy {

inline uint32_t &pool() { static uint32_t p = 0x9E3779B9u; return p; }

// From the touch callback: every touch stirs in position + time.
inline void feed(uint16_t x, uint16_t y) {
    uint32_t &p = pool();
    p ^= (uint32_t)x * 2654435761u + (uint32_t)y * 40503u + (uint32_t)millis();
    p = p * 1664525u + 1013904223u;      // LCG step: spreads the bits
    p ^= p >> 16;
}

inline uint32_t rnd32() {
#ifndef SIMULATOR
    return esp_random() ^ pool();
#else
    return ((uint32_t)rand() << 16 ^ (uint32_t)rand()) ^ pool();
#endif
}

// 12 characters without confusable ones (no 0/O/1/l/I): ~68 bits — considerably
// more than a WPA2 handshake attack tries offline in a reasonable time.
inline void generateApPassword(char *out, size_t n) {
    static const char CS[] =
        "abcdefghjkmnpqrstuvwxyzABCDEFGHJKMNPQRSTUVWXYZ23456789";
    const size_t len = (n > 13) ? 12 : n - 1;
    for (size_t i = 0; i < len; i++) {
        // Reject instead of modulo so no character is favoured
        uint32_t r;
        do { r = rnd32() & 0x3F; } while (r >= sizeof(CS) - 1);
        out[i] = CS[r];
    }
    out[len] = '\0';
}

}  // namespace Entropy
