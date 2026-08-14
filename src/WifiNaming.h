#pragma once
#include <Arduino.h>
#include "config/Config.h"
#ifndef SIMULATOR
#include <WiFi.h>
#endif
#include <string.h>

// ============================================================
// WifiNaming – internal AP (hotspot) credentials.
//   SSID:     "NauticPinnace" + last 6 hex digits of the MAC (stable, eFuse)
//   Password: random per device (cfg.apPass, generated via Entropy.h)
// Shown in plaintext + QR in the on-screen config so a phone can join.
// Used by WebConfig (AP start) and ConfigOverlay (display + reboot-into-AP).
// ============================================================

inline void wifiMacBytes(uint8_t mac[6]) {
#ifdef SIMULATOR
    static const uint8_t fake[6] = { 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33 };
    memcpy(mac, fake, 6);
#else
    // STA MAC = factory eFuse MAC (stable). Available once WiFi.mode() has run.
    WiFi.macAddress(mac);
#endif
}

inline String wifiApSsid() {
    uint8_t m[6]; wifiMacBytes(m);
    char b[24];
    snprintf(b, sizeof(b), "NauticPinnace%02X%02X%02X", m[3], m[4], m[5]);
    return String(b);
}

inline String wifiApPassword() {
    // Random password from the configuration (Entropy.h, unique per device).
    // The earlier "MdPw"+MAC scheme was derivable from the SSID: it broadcasts
    // the last three MAC bytes, and the first three are an Espressif prefix
    // from a small public list.
    if (appConfig.cfg.apPass[0]) return String(appConfig.cfg.apPass);
    // Fallback only for the simulator / before generation: fixed placeholder,
    // NOT MAC-derived.
    return String("(wird beim Start erzeugt)");
}
