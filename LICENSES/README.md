# Licence texts

Full texts of the licences of everything this project distributes — the
firmware images, the web UI and the release package — as named in
THIRD-PARTY-NOTICES.md (repo root). They exist so that a distribution of this
project — source or a built `firmware.bin` — can meet the pass-the-licence-text
obligations without relying on the `.pio/` build tree being present.

| File | Applies to | Taken from |
|---|---|---|
| `LGPL-2.1.txt` | Arduino-ESP32 core (2.0.17 on the 4-inch, 3.3.11 on the 7B / 5B) | SPDX `license-list-data` |
| `LGPL-3.0.txt` | ESPAsyncWebServer, AsyncTCP — both of the ESP32Async project (includes the GPL-3.0 base text as published by SPDX; the libraries' own LICENSE files carry only the LGPL-3.0 supplement) | SPDX `license-list-data` |
| `Apache-2.0.txt` | Espressif's ESP-IDF components linked into `firmware.bin` (esp_system, esp_wifi, driver, …, the provisioning component, the FreeRTOS port layer) **including the binary-only WiFi/PHY libraries** (net80211, pp, core, smartconfig, espnow, phy, coexist); the bootloader; mbed TLS (dual-licensed, Apache-2.0 elected here); `ssl_client.cpp` of the Arduino core; the canboat project whose PGN field layouts were used as facts; and the Google Material Icons that some OpenBridge icons are based on | same wording as ESP-IDF's `LICENSE` and the `LICENSE` files next to the WiFi/PHY libraries (only the line layout differs) |
| `BSD-3-Clause-lwIP.txt` | lwIP, the TCP/IP stack (Swedish Institute of Computer Science) | verbatim: `framework-espidf/components/lwip/lwip/COPYING` (ESP-IDF 5.5.5) |
| `BSD-3-Clause-wpa_supplicant.txt` | wpa_supplicant, the WiFi security layer (Jouni Malinen and contributors) | verbatim: `framework-espidf/components/wpa_supplicant/COPYING` followed by the whole `README` of the same directory, which holds the licence terms (ESP-IDF 5.5.5) |
| `BSD-3-Clause-littlefs.txt` | littlefs, the filesystem core inside esp_littlefs (The littlefs authors, Arm Limited) | verbatim: `src/littlefs/LICENSE.md` of the joltwallet/littlefs component (`managed_components/joltwallet__littlefs/`) |
| `BSD-3-Clause-TLSF.txt` | TLSF, the heap allocator (Matthew Conte) — one text for both linked copies: the ESP-IDF heap and LVGL's `lv_tlsf.c` | verbatim: the licence comment of LVGL 8.4.0 `src/misc/lv_tlsf.h`, lines 8-41 (ESP-IDF 4.4.7 `heap_tlsf.c` carries the same text; the ESP-IDF 5.5.5 copy has only the SPDX line "2006-2016 Matthew Conte, BSD-3-Clause") |
| `MIT.txt` | the MIT components without a file of their own: FreeRTOS kernel (Amazon), the Xtensa HAL and port (Cadence), esp_littlefs (Brian Pugh), cJSON (Dave Gamble and cJSON contributors), lv_printf (Marco Paland) and the QR code generator (Project Nayuki) bundled with LVGL, `NetworkUdp.h` of the Arduino core (Bjoern Hartmann, 7B / 5B) — the copyright lines are listed in the file | verbatim: `framework-espidf/components/freertos/FreeRTOS-Kernel/LICENSE.md` (ESP-IDF 5.5.5), with the list of copyright lines inserted after its title line |
| `newlib-COPYING.NEWLIB.txt` | newlib, the C library (`libc.a`, `libm.a`) linked from the toolchain — BSD-style licences of several holders | verbatim: `toolchain-xtensa-esp-elf/share/licenses/newlib/COPYING.NEWLIB` (GCC 14.2.0 toolchain of the 7B / 5B) |
| `GPL-2.0.txt` | esptool — the `esptool.exe` bundled in the Windows release package (a separate program, not linked into the firmware; its source ships alongside) | SPDX `license-list-data` |
| `GPL-3.0.txt` | base text incorporated by LGPL-3.0; also covers libgcc/libstdc++ (GPL-3.0-with-runtime-exception) | SPDX `license-list-data` |
| `MIT-LVGL.txt` | LVGL | verbatim: the library's `LICENCE.txt` |
| `MIT-ArduinoJson.txt` | ArduinoJson | verbatim: the library's `LICENSE.txt` |
| `MIT-NMEA2000.txt` | NMEA2000 by Timo Lappalainen | the licence text of the `NMEA2000.h` header, with the copyright line of `NMEA2000.cpp`, `N2kMsg.cpp` and `N2kMessages.cpp` (2015-2025) |
| `OFL-1.1-Montserrat.txt` | Montserrat typeface incl. the generated bitmap fonts in `src/display/fonts/` derived from it; Font Awesome glyphs - NOT the `ob_icons_*` fonts, see the next row | verbatim: `download/fonts/montserrat/OFL.txt` |
| `CC-BY-4.0.txt` | the OpenBridge Icon Pack: the `ob_icons_*` bitmap fonts in `src/display/fonts/` and the icon sprite in `data/index.html`; also the licence of the BSH tide data fetched at runtime. OpenBridge icon trial | canonical legal code from creativecommons.org |
| `LovyanGFX-license.txt` | LovyanGFX (4-inch only) — includes LovyanGFX's own FreeBSD notice (lovyan03) plus the embedded Adafruit (BSD, MIT) and TFT_eSPI (FreeBSD) notices | verbatim: the library's `license.txt` (1.2.21) |

Where the 4-inch (ESP-IDF 4.4.7, GCC 8.4.0) carries its own copy of one of
these texts, it was compared with the one taken here: the lwIP, wpa_supplicant
`README`, TLSF and FreeRTOS texts read the same; the 4-inch wpa_supplicant
`COPYING` gives the years as 2002-2016, and its toolchain's `COPYING.NEWLIB`
differs only in sections for other processor targets. None of the differences
changes a condition.

Not duplicated here: the licences of components embedded *inside* LovyanGFX
beyond the ones in its compound file (TJpgDec, pngle, miniz, efont/IPA fonts) —
they ship with the library source itself and are not linked. Neither are the
notices of what is bundled inside `esptool.exe` (Python, OpenSSL, libffi,
bzip2, xz/liblzma, zlib, pyserial, intelhex, the PyInstaller bootloader,
Microsoft runtime DLLs): they live in `tools/esptool-exe-notices/` and ship in
the release package as `esptool-4.8.1/notices/`.

The copyright NOTICES that belong with these texts live in
`THIRD-PARTY-NOTICES.md` — the two files travel together.
