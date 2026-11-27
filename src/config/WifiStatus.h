#pragma once
// ============================================================================
// WifiStatus - what the WiFi start is doing, in a form the screens can show.
//
// Two things live here, both shared by code that cannot see each other:
//
//  * WifiProgress - what WebConfig::connect() reports while the boot screen
//    is up (attempt n/3, scanning, joined, fallback hotspot). WebConfig fills
//    it, BootScreen turns it into text. A plain struct with its own buffers,
//    so the PC simulator can drive exactly the same boot texts without a
//    WiFi stack (WebConfig.cpp is not part of the simulator build).
//
//  * WifiReason - the station's last disconnect reason (ESP-IDF
//    wifi_err_reason_t) in words. Until now the reason only ever appeared in
//    the serial log, as the core's "Reason: 201 - NO_AP_FOUND" line, and the
//    people who ask for help in the forum almost never have a log. The boot
//    screen, the WiFi line of the on-screen settings and /api/info show it
//    now, and this is the ONE place that maps the number to words, so the
//    three cannot disagree.
//
// Header-only and free of IDF includes on purpose (simulator, see above). The
// codes are the same numbers on IDF 4.4 (4-inch) and IDF 5.5 (7B/5B) - checked
// in both esp_wifi_types headers; 210/211 exist on 5.5 only, a 4.4 station
// simply never reports them.
// ============================================================================
#include <stdint.h>
#include "../i18n/I18n.h"

struct WifiProgress {
    enum Phase : uint8_t {
        CONNECTING,   // a join attempt is running (attempt / attempts)
        SCANNING,     // all attempts failed, one scan for the network runs
        CONNECTED,    // joined: ip is the station's address
        FALLBACK      // own hotspot up, the network is retried in the background
    };
    Phase    phase    = CONNECTING;
    uint8_t  attempt  = 0;       // CONNECTING: 1-based
    uint8_t  attempts = 0;       // CONNECTING: of how many
    uint8_t  reason   = 0;       // FALLBACK: last disconnect reason, 0 = none reported
    int8_t   seen     = -1;      // FALLBACK: the scan saw the network 1 / 0, -1 = no scan
    uint16_t retryInS = 0;       // FALLBACK: seconds to the first background retry
    char     ssid[33]   = "";    // the configured network (32 bytes + NUL)
    char     ip[16]     = "";    // CONNECTED: own address; FALLBACK: hotspot address
    char     apSsid[33] = "";    // FALLBACK: own hotspot name
};

typedef void (*WifiProgressFn)(const WifiProgress &p);

namespace WifiReason {

// Our own disconnects. WiFi.disconnect() - run after every failed attempt, and
// by the core itself before its one internal retry - raises STA_DISCONNECTED
// with ASSOC_LEAVE. Recording that would overwrite the reason that actually
// ended the attempt, so the recorder drops it.
static const uint8_t ASSOC_LEAVE = 8;

enum Kind : uint8_t { NONE, NOT_FOUND, AUTH, SECURITY, REFUSED, OTHER };

// Grouped by what the user can do about it (the WiFi investigation's table):
// not there at all / password or key exchange / security type / refused.
inline Kind kind(uint8_t r) {
    switch (r) {
        case 0:   return NONE;
        case 201: return NOT_FOUND;   // NO_AP_FOUND: not up yet, 5 GHz, hidden ...
        case 15:                      // 4WAY_HANDSHAKE_TIMEOUT - the usual wrong PSK
        case 202:                     // AUTH_FAIL
        case 204: return AUTH;        // HANDSHAKE_TIMEOUT
        case 210:                     // NO_AP_FOUND_W_COMPATIBLE_SECURITY
        case 211: return SECURITY;    // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
        case 203:                     // ASSOC_FAIL
        case 205: return REFUSED;     // CONNECTION_FAIL
        default:  return OTHER;
    }
}

// The IDF name, for the serial log and /api/info. Only the codes a join can
// end with are spelled out; the number is always printed next to it anyway.
inline const char *name(uint8_t r) {
    switch (r) {
        case 0:   return "NONE";
        case 2:   return "AUTH_EXPIRE";
        case 3:   return "AUTH_LEAVE";
        case 4:   return "ASSOC_EXPIRE";
        case 8:   return "ASSOC_LEAVE";
        case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        case 210: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
        case 211: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
        default:  return "OTHER";
    }
}

// Short user-facing text - a few words, because the settings line has to carry
// one of them next to the hotspot address in 18 px on a 494 px wide column
// (1024x600 landscape; measured with the panel fonts, ConfigOverlay.cpp).
// `seen` is the scan result (1 / 0 / -1 unknown) and overrules the code where
// the two disagree:
//  * no reason reported, and the scan did not see the network either: that
//    is "not found" as well;
//  * NO_AP_FOUND, but the scan DID see it: the station did not find an AP it
//    was willing to join - on IDF 4.4 that is also how a security or
//    threshold mismatch is reported. "Not found" would send the user looking
//    for a network that is plainly there (and contradict the serial log), so
//    it reads "not connected"; the code next to it stays the real one.
inline const char *text(uint8_t r, int8_t seen) {
    Kind k = kind(r);
    if (k == NONE && seen == 0) k = NOT_FOUND;
    if (k == NOT_FOUND && seen == 1) k = OTHER;
    switch (k) {
        case NOT_FOUND: return T(STR_WIFI_RSN_NOT_FOUND);
        case AUTH:      return T(STR_WIFI_RSN_AUTH);
        case SECURITY:  return T(STR_WIFI_RSN_SECURITY);
        case REFUSED:   return T(STR_WIFI_RSN_REFUSED);
        default:        return T(STR_WIFI_RSN_OTHER);
    }
}

}  // namespace WifiReason
