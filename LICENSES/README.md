# Licence texts

Full texts of the licences of everything this project distributes — the
firmware images, the web UI and the release package — as named in
THIRD-PARTY-NOTICES.md (repo root). They exist so that a distribution of this
project — source or a built `firmware.bin` — can meet the pass-the-licence-text
obligations without relying on the `.pio/` build tree being present.

| File | Applies to | Taken from |
|---|---|---|
| `LGPL-2.1.txt` | Arduino-ESP32 core (2.0.17 on the 4-inch, 3.3.11 on the 7B / 5B), except the files Espressif wrote itself, which are Apache-2.0 (see the `Apache-2.0.txt` row) | SPDX `license-list-data` |
| `LGPL-3.0.txt` | ESPAsyncWebServer, AsyncTCP — both of the ESP32Async project (includes the GPL-3.0 base text as published by SPDX, word for word the same as `GPL-3.0.txt`; the libraries' own LICENSE files carry only the LGPL-3.0 supplement) | SPDX `license-list-data`, with the FSF and GNU web addresses given as `https://`, as in `GPL-3.0.txt` and the current FSF texts |
| `Apache-2.0.txt` | Espressif's ESP-IDF components linked into `firmware.bin` (esp_system, esp_wifi, driver, …, the provisioning component, the FreeRTOS port layer) **including the binary-only WiFi/PHY libraries** (net80211, pp, core, smartconfig, espnow, phy, coexist); the bootloader; mbed TLS (dual-licensed, Apache-2.0 elected here); the files of the Arduino core that Espressif wrote itself, each with an Apache-2.0 header (Copyright Espressif Systems (Shanghai)) — on all boards `esp32-hal-{bt,cpu,gpio,i2c-slave,matrix,misc,psram,rmt,spi,time,uart}.c` and the libraries' `vfs_api.cpp`, `LittleFS.cpp` and `Preferences.cpp`; on the 4-inch also `esp32-hal-i2c.c`, `HWCDC.cpp` and `esp_crt_bundle.c`; on the 7B / 5B also `esp32-hal-{i2c-ng,periman,rgb-led}.c`, `NetworkEvents.cpp`, `NetworkInterface.cpp`, `NetworkManager.cpp` and the WiFi library's `AP.cpp` and `STA.cpp`, on the 5B also `HWCDC.cpp` (core files without a licence header, such as `main.cpp`, and the 4-inch's `esp32-hal-rgb-led.c` fall under the core's LGPL-2.1-or-later); `ssl_client.cpp` of the Arduino core (ARM Limited, Evandro Luis Copercini); the canboat and Signal K projects, whose PGN field layouts were used as facts; and the Google Material Icons that some OpenBridge icons are based on | same wording as ESP-IDF's `LICENSE` and the `LICENSE` files next to the WiFi/PHY libraries (only the line layout differs) |
| `BSD-3-Clause-lwIP.txt` | lwIP, the TCP/IP stack (Swedish Institute of Computer Science); the same text covers the further holders named in the headers of linked lwIP files — Leon Woestenberg, Axon Digital Design, Inico Technologies, Frederic Bernon and Simon Goldschmidt, Adam Dunkels (uIP), The MINIX 3 Project — which are listed at the top of the file; `igmp.c` (CITEL Technologies) has its own wording, given as the file's second text | lwIP's `COPYING` verbatim (`framework-espidf/components/lwip/lwip/COPYING`, ESP-IDF 5.5.5; the 4-inch's esp-lwip copy reads the same), preceded by the list of further copyright notices and followed by the notice of `igmp.c`, taken from the source headers with only the comment markers removed |
| `BSD-lwIP-PPP.txt` | lwIP's PPP code (`src/netif/ppp`: auth, fsm, ipcp, lcp, magic, ppp, pppos, upap, vj) — **7B / 5B only**, pulled in by lwIP's initialisation because the ESP-IDF build of the 7B / 5B has `CONFIG_LWIP_PPP_SUPPORT` set (inherited from the Arduino core's sdkconfig; ESP-IDF's own default is off); the 1.2.0 7B / 5B images contain the same files. Carnegie Mellon University, Paul Mackerras, Marc Boucher (MBSI) and Global Election Systems, The Australian National University, Regents of the University of California — with the acknowledgments these licences require | the licence comments of those files in `framework-espidf/components/lwip/lwip/src/netif/ppp/` (ESP-IDF 5.5.5), verbatim except for the removed comment markers; a text several files share is given once |
| `BSD-3-Clause-wpa_supplicant.txt` | wpa_supplicant, the WiFi security layer (Jouni Malinen and contributors; further holders of linked files listed at the top of the file: The Linux Foundation, and on the 7B / 5B also Cozybit) | verbatim: `framework-espidf/components/wpa_supplicant/COPYING` followed by the whole `README` of the same directory, which holds the licence terms (ESP-IDF 5.5.5), preceded by the list of further copyright notices from the linked files' headers |
| `BSD-3-Clause-littlefs.txt` | littlefs, the filesystem core inside esp_littlefs (The littlefs authors, Arm Limited) | verbatim: `src/littlefs/LICENSE.md` of the joltwallet/littlefs component (`managed_components/joltwallet__littlefs/`) |
| `BSD-3-Clause-TLSF.txt` | TLSF, the heap allocator (Matthew Conte) — one text for both linked copies: the ESP-IDF heap and LVGL's `lv_tlsf.c` | verbatim: the licence comment of LVGL 8.4.0 `src/misc/lv_tlsf.h`, lines 8-41 (ESP-IDF 4.4.7 `heap_tlsf.c` carries the same text; the ESP-IDF 5.5.5 copy has only the SPDX line "2006-2016 Matthew Conte, BSD-3-Clause") |
| `BSD-2-Clause-ubsan.txt` | ESP-IDF's `ubsan.c` (Linaro Limited) — all boards; only an empty 5-byte hook function of it is linked | verbatim: the header of `framework-espidf/components/esp_system/ubsan.c` (ESP-IDF 5.5.5; 4.4.7 reads the same), comment markers removed |
| `MIT.txt` | the MIT components without a file of their own: FreeRTOS kernel (Amazon), the FreeRTOS Xtensa port and the Xtensa HAL, vector, context and interrupt code (Cadence), esp_littlefs (Brian Pugh), cJSON (Dave Gamble and cJSON contributors), lv_printf (Marco Paland) and the QR code generator (Project Nayuki) bundled with LVGL, `NetworkUdp.h` of the Arduino core (Bjoern Hartmann, 7B / 5B) — the copyright lines are listed in the file | verbatim: `framework-espidf/components/freertos/FreeRTOS-Kernel/LICENSE.md` (ESP-IDF 5.5.5), with the list of copyright lines inserted after its title line |
| `newlib-COPYING.NEWLIB.txt` | newlib, the C library (`libc.a`, `libm.a`) linked from the toolchain — BSD-style licences of several holders | verbatim: `toolchain-xtensa-esp-elf/share/licenses/newlib/COPYING.NEWLIB` (GCC 14.2.0 toolchain of the 7B / 5B) |
| `GPL-3.0.txt` | base text incorporated by LGPL-3.0; also the base licence of libgcc and libstdc++, which are GPL-3.0-or-later WITH GCC-exception-3.1 (see the next row) | SPDX `license-list-data` |
| `GCC-exception-3.1.txt` | the GCC Runtime Library Exception 3.1: the additional permission under which libgcc and libstdc++, linked from the toolchains (GCC 8.4.0 on the 4-inch, GCC 14.2.0 on the 7B / 5B), may be combined into the firmware and conveyed under terms of its own | verbatim: `toolchain-xtensa-esp-elf/share/licenses/gcc/COPYING.RUNTIME` (identical in the 4-inch's `toolchain-xtensa-esp32s3`) |
| `MIT-LVGL.txt` | LVGL | verbatim: the library's `LICENCE.txt` |
| `MIT-ArduinoJson.txt` | ArduinoJson | verbatim: the library's `LICENSE.txt` |
| `MIT-NMEA2000.txt` | NMEA2000 by Timo Lappalainen | the licence text of the `NMEA2000.h` header, with the copyright line of `NMEA2000.cpp`, `N2kMsg.cpp` and `N2kMessages.cpp` (2015-2025) |
| `OFL-1.1-Montserrat.txt` | Montserrat typeface incl. the generated bitmap fonts in `src/display/fonts/` derived from it; the Font Awesome glyphs in LVGL's built-in fonts (Font Awesome Free 5.9.0, Fonticons, Inc.). This file's own header names Montserrat only; the Font Awesome notice is "Copyright (c) Font Awesome", as in the font file's name table. NOT the `ob_icons_*` fonts, see the next row | verbatim: `download/fonts/montserrat/OFL.txt` |
| `CC-BY-4.0.txt` | the OpenBridge Icon Pack: the `ob_icons_*` bitmap fonts in `src/display/fonts/` and the icon sprite in `data/index.html`; also the licence of the BSH tide data fetched at runtime | canonical legal code from creativecommons.org |
| `LovyanGFX-license.txt` | LovyanGFX (4-inch only) — includes LovyanGFX's own FreeBSD notice (lovyan03) plus the embedded Adafruit (BSD, MIT) and TFT_eSPI (FreeBSD) notices | verbatim: the library's `license.txt` (1.2.21) |

Where the 4-inch (ESP-IDF 4.4.7, GCC 8.4.0) carries its own copy of one of
these texts, it was compared with the one taken here: the lwIP, wpa_supplicant
`README`, TLSF and FreeRTOS texts read the same; the 4-inch wpa_supplicant
`COPYING` gives the years as 2002-2016, and its toolchain's `COPYING.NEWLIB`
differs only in sections for other processor targets. None of the differences
changes a condition.

Not duplicated here: the licences of components embedded *inside* LovyanGFX
beyond the ones in its compound file (among them TJpgDec, pngle, miniz, the QR
and QOI codecs, the GFXFF and RLE fonts, efont and the IPA/IPAex fonts) — they
ship with the library source itself and are not linked (the one LovyanGFX font
in the 4-inch image is the Adafruit GFX glcdfont, credited in its compound
file).

The copyright NOTICES that belong with these texts live in
`THIRD-PARTY-NOTICES.md` — the two files travel together.
