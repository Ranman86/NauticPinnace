#pragma once
#include <Arduino.h>
#include "config/Config.h"
#ifndef SIMULATOR
#include <WiFi.h>
#include <esp_mac.h>   // esp_read_mac - the eFuse, independent of WiFi state
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
    // Zeroed FIRST, unconditionally. Everything below can fail to write, and a
    // caller must never be handed whatever happened to be on the stack.
    memset(mac, 0, 6);
#ifdef SIMULATOR
    static const uint8_t fake[6] = { 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33 };
    memcpy(mac, fake, 6);
#else
    // esp_read_mac(), NOT WiFi.macAddress().
    //
    // WiFi.macAddress(buf) reaches NetworkInterface::macAddress(uint8_t*),
    // which returns NULL WITHOUT TOUCHING THE BUFFER in two cases: when the
    // interface's esp_netif is NULL, and when esp_netif_get_mac() fails. In
    // AP-only mode - which is exactly how this device runs its hotspot - the
    // STA netif does not exist, so the first case always applies.
    //
    // The buffer was a plain `uint8_t m[6];` in wifiApSsid(), so the name was
    // built from uninitialised stack memory: a different value per call site
    // and per call. That is why the SSID on the settings screen, the one in
    // the QR code and the one the device actually broadcast were three
    // different names - nobody was wrong, they were all reading garbage.
    // (Note that the framework's own String-returning overload does zero its
    // buffer first. Only the pointer form leaves it to the caller.)
    //
    // esp_read_mac() reads the factory eFuse and works whatever the WiFi state
    // is, before or after WiFi.mode(), in STA mode or AP mode. ESP_MAC_WIFI_STA
    // is the base address and keeps the documented naming above.
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) memset(mac, 0, 6);
#endif
}

inline String wifiApSsid() {
    uint8_t m[6] = { 0, 0, 0, 0, 0, 0 };
    wifiMacBytes(m);
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
