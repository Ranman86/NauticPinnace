#pragma once
#include <string.h>

// ============================================================
// WiFi country codes accepted for wifi.country (config.json, web interface
// tab WiFi). "" - the default - is not in the list: it means "no country call
// at all", and every board keeps the country its framework starts with
// (see applyWifiCountry() in WebConfig.cpp).
//
// Plain C++ on purpose: Config.cpp validates with it, and the PC simulator
// compiles Config.cpp but not the WiFi code.
//
// The same 35 codes, in the same order, are WIFI_CC in data/index.html -
// change one, change the other.
//
// In the list: codes that BOTH framework versions accept (IDF 4.4 on the 4",
// IDF 5.5 on the 7B/5B) AND that give the same channels in both, so a
// setting means the same on every board:
//   - Europe, channels 1-13: AT BE BG CH CY CZ DE DK EE ES FI FR GB GR HR HU
//     IE IS IT LI LT LU LV MT NL NO PL PT RO SE SI SK (EU, EFTA, GB).
//   - Australia and New Zealand, channels 1-13: AU NZ.
//   - USA, channels 1-11: US.
// Left out:
//   - CA: channels 1-11 in IDF 4.4 but 1-13 in IDF 5.5 - the boards would
//     differ.
//   - JP: channel 14 allows 802.11b only, and the own hotspot follows the
//     station's channel in AP+STA; nothing here could keep it to 11b there.
//   - TR, ME, MC, AL, EU: IDF 4.4 rejects them.
//   - "01" (world safe mode): the 7B/5B default anyway, and on the 4" it
//     would cut channels 12 and 13.
//   - Everything else the IDF knows: not needed on board so far, and not
//     checked one by one.
// ============================================================

inline bool wifiCountryValid(const char *cc) {
    // Whole two-letter entries, never a substring of a concatenated list:
    // "ATAU..." would accept "TA" out of "ATAU".
    static const char kCodes[][3] = {
        "AT", "AU", "BE", "BG", "CH", "CY", "CZ", "DE", "DK", "EE", "ES", "FI",
        "FR", "GB", "GR", "HR", "HU", "IE", "IS", "IT", "LI", "LT", "LU", "LV",
        "MT", "NL", "NO", "NZ", "PL", "PT", "RO", "SE", "SI", "SK", "US",
    };
    if (!cc || !cc[0] || !cc[1] || cc[2]) return false;   // exactly two characters
    for (size_t i = 0; i < sizeof(kCodes) / sizeof(kCodes[0]); i++)
        if (kCodes[i][0] == cc[0] && kCodes[i][1] == cc[1]) return true;
    return false;
}
