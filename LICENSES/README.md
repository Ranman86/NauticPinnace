# Licence texts

Full texts of every licence that THIRD-PARTY-NOTICES.md (repo root) refers to.
They exist so that a distribution of this project — source or a built
`firmware.bin` — can meet the pass-the-licence-text obligations without relying
on the `.pio/` build tree being present.

| File | Applies to |
|---|---|
| `LGPL-2.1.txt` | Arduino-ESP32 core |
| `LGPL-3.0.txt` | ESPAsyncWebServer, AsyncTCP — both of the ESP32Async project (includes the GPL-3.0 base text as published by SPDX; the libraries' own LICENSE files carry only the LGPL-3.0 supplement) |
| `Apache-2.0.txt` | ESP-IDF components linked into `firmware.bin` (FreeRTOS, esp_system, esp_wifi, driver, …), mbed TLS (dual-licensed, Apache-2.0 elected here), and the canboat project whose PGN field layouts were used as facts |
| `GPL-2.0.txt` | esptool — the `esptool.exe` bundled in the Windows release package (a separate program, not linked into the firmware; its source ships alongside) |
| `GPL-3.0.txt` | base text incorporated by LGPL-3.0; also covers libgcc/libstdc++ (GPL-3.0-with-runtime-exception) |
| `MIT-LVGL.txt` | LVGL (verbatim from the library's `LICENCE.txt`) |
| `MIT-ArduinoJson.txt` | ArduinoJson (verbatim from the library's `LICENSE.txt`) |
| `MIT-NMEA2000.txt` | NMEA2000 by Timo Lappalainen (extracted verbatim from the source header) |
| `OFL-1.1-Montserrat.txt` | Montserrat typeface incl. the generated bitmap fonts in `src/display/fonts/`; Font Awesome glyphs (verbatim from `download/fonts/montserrat/OFL.txt`) |
| `LovyanGFX-license.txt` | LovyanGFX (verbatim from the library's `license.txt`; compound file — includes the embedded Adafruit BSD and TFT_eSPI FreeBSD notices) |

Not duplicated here: the licences of components embedded *inside* LovyanGFX
beyond the ones in its compound file (TJpgDec, pngle, miniz, efont/IPA fonts)
ship with the library source itself, and the BSH tide data fetched at runtime
is CC BY 4.0 (<https://creativecommons.org/licenses/by/4.0/>) — data, not a
distributed software component.

Sources: MIT/OFL texts are copies of the files shipped inside the respective
dependencies; the GNU texts are the canonical SPDX `license-list-data` copies.

The copyright NOTICES that belong with these texts live in
`THIRD-PARTY-NOTICES.md` — the two files travel together.
