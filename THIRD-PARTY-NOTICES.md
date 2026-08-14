# Third-Party Notices — NauticPinnace

The project's **own code** is MIT-licensed — see [`LICENSE`](LICENSE) at the
repo root.

This firmware contains third-party software and data. The notices below are
required by the respective licences and must be passed on **with every
distribution** (source code or a built `firmware.bin`).

Everything here was taken from the licence files in this project's dependency
tree (`.pio/libdeps/…`), not from secondary sources.

The **full texts** of every licence referenced below live in [`LICENSES/`](LICENSES/)
at the repo root, so a distribution can satisfy the pass-the-text obligations
without the build tree.

---

## 1. Copyleft libraries (LGPL) — distribution requires more than a notice

These three are linked **statically** into `firmware.bin`. The LGPL does **not**
require you to publish your own source code — but it does require that a
recipient be able to replace the library and relink.

Anyone distributing the firmware must therefore also provide:

1. this notice and the full licence text (LGPL-2.1 or LGPL-3.0 respectively),
2. the unmodified source of each library in the version used,
3. their own program in a **relinkable form** — either as source code or as
   object files (`.pio/build/waveshare_esp32s3_4/**/*.o`),
4. all copyright notices unchanged.

| Component | Version | Copyright | Licence |
|---|---|---|---|
| **Arduino-ESP32 core** (incl. WiFi, HTTPClient, LittleFS, Wire, SPI) | 3.20017.241212 (ESP-IDF 4.4.7) | Espressif Systems and contributors | LGPL-2.1-or-later |
| **ESPAsyncWebServer** | 3.6.0 (commit `ad3741d1`) | © 2016 Hristo Gochkov; maintained by the ESP32Async project | LGPL-3.0 (some source headers still say LGPL-2.1-or-later) |
| **AsyncTCP** | 3.3.2 (commit `ef448a8a`) | © 2016 Hristo Gochkov; maintained by the ESP32Async project | LGPL-3.0 (some source headers still say LGPL-2.1-or-later) |

> The LGPL obligation arises solely from the Arduino layer; the ESP-IDF
> components underneath carry their own, non-copyleft licences — listed in
> section 2.1, because several of them also want their notice reproduced.

> **On the async pair's origin:** both started as `me-no-dev/AsyncTCP` and
> `me-no-dev/ESPAsyncWebServer` (the URLs `platformio.ini` pins) and were handed
> over to the **ESP32Async** organisation, which now maintains them — the
> packages that get built name ESP32Async as author. The original copyright of
> Hristo Gochkov (me-no-dev) remains in the sources. `LICENSES/LGPL-3.0.txt`
> carries the licence text, including the GPL-3.0 base text that LGPL-3.0
> incorporates by reference.
>
> ESPAsyncWebServer also contains `BackPort_SHA1Builder.cpp` — mbed TLS SHA-1,
> **Apache-2.0**, © 2006-2015 ARM Limited, adapted by Espressif. It serves the
> WebSocket handshake, which this firmware does not use, so the linker drops it
> and it is not present in the shipped `firmware.bin` (verified). It is listed
> here for completeness in case a fork enables WebSockets.

---

## 2. Permissively licensed libraries — a notice is sufficient

### LVGL 8.4.0 — MIT
```
Copyright (c) 2021 LVGL Kft
```
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction … The above copyright notice and this
permission notice shall be included in all copies or substantial portions of the
Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

### ArduinoJson 7.4.3 — MIT
```
Copyright © 2014-2026, Benoit BLANCHON
```

### NMEA2000 4.24.1 (Timo Lappalainen) — MIT
```
Copyright (c) 2015-2024 Timo Lappalainen, Kave Oy, www.kave.fi
```
The local CAN driver `lib/NMEA2000_esp32/` is an independent re-implementation
on the ESP-IDF `twai_*` API; its public interface mirrors the same author's
`NMEA2000_esp32` library (MIT, Copyright (c) 2015-2020) for drop-in
compatibility, but no function bodies were taken from it.

### QR code generator (bundled with LVGL) — MIT
```
Copyright (c) Project Nayuki
```

### LovyanGFX 1.2.21 — MIT and BSD
Contains code from several authors (the library's `license.txt`):
```
Copyright (c) 2012 Adafruit Industries.  All rights reserved.   (BSD licence, Adafruit GFX)
Copyright (c) 2020 Bodmer (https://github.com/Bodmer)           (FreeBSD licence, TFT_eSPI)
```
Further embedded: **TJpgDec** (ChaN, its own permissive licence), **pngle**
(kikuchan, MIT), **miniz** (public domain), the **efont** bitmap fonts, and the
**IPAex** fonts (IPA Font License 1.0).

### 2.1 ESP-IDF components linked into `firmware.bin`

The Arduino core pulls in prebuilt ESP-IDF libraries. The list below is not a
guess: it was read from the linker map
(`.pio/build/waveshare_esp32s3_4/firmware.map`), counting the input sections
actually placed in the image. Two of the largest are BSD-3-Clause, which asks
for its copyright notice to be reproduced in binary redistributions — hence
this section.

```
lwIP (TCP/IP stack)            BSD-3-Clause
    Copyright (c) 2001-2004 Swedish Institute of Computer Science.
    All rights reserved.  (Adam Dunkels and contributors)

wpa_supplicant (WiFi security) BSD-3-Clause
    Copyright (c) 2002-2012, Jouni Malinen <j@w1.fi> and contributors

mbed TLS (crypto)              Apache-2.0
    Copyright The Mbed TLS Contributors; parts (c) 2006-2015 ARM Limited
    Upstream is dual "Apache-2.0 OR GPL-2.0-or-later"; Apache-2.0 applies here.

net80211 / pp (WiFi MAC/PHY)   Espressif's own binary-library licence
    Copyright (c) Espressif Systems — shipped as prebuilt blobs in the SDK

FreeRTOS, esp_system, esp_wifi, driver, spi_flash, …   Apache-2.0
    Copyright (c) Espressif Systems and contributors
```

Licence texts: `LICENSES/Apache-2.0.txt`. The BSD-3-Clause text is short and
reproduced with each component's own source in the ESP-IDF; the copyright
notices above are the part that must travel with a binary.

> The same `BackPort_SHA1Builder.cpp` note from section 1 applies to mbed TLS in
> general: mbed TLS itself **is** linked (crypto for TLS), unlike the SHA-1
> backport inside ESPAsyncWebServer, which the linker drops.

---

## 3. Fonts — SIL Open Font License 1.1

### Montserrat
```
Copyright 2011 The Montserrat Project Authors
(https://github.com/JulietaUla/Montserrat)
```
Licensed under the **SIL Open Font License, Version 1.1** —
<https://scripts.sil.org/OFL>

Used in two forms:

* the `lv_font_montserrat_*` fonts built into LVGL (10–48 px),
* the bitmap fonts generated in this project:
  `src/display/fonts/depth_font_96.c` and `depth_font_192.c`
  (created with `tools/gen_depth_font.py` from `Montserrat-Medium.ttf`), and the
  `src/display/fonts/latin_suppl_*.c` fallback fonts that supply the umlauts and
  other glyphs the built-in fonts lack (created with
  `tools/gen_latin_supplement.py` from the same typeface).

> **Important:** the OFL explicitly treats a format conversion as a "Modified
> Version". The generated `.c` files are therefore *Font Software* in their own
> right and remain under the OFL. They may be redistributed, but **only together
> with this notice and the licence text**, and — being Modified Versions — **not
> under the reserved name "Montserrat"**. (The unmodified TTFs in
> `download/fonts/montserrat/` keep their original name; the reserved-name
> clause restricts Modified Versions only.)

### Font Awesome 5 Free
```
Copyright Fonticons, Inc.
```
The icon glyphs (warning triangle, plus/minus, speaker, WiFi and others) are
contained in the same LVGL font tables. The font files are under
**SIL OFL 1.1**; Font Awesome code is under MIT. See
<https://fontawesome.com/license/free>.

---

## 4. Data

### Natural Earth — public domain
The world map coastlines (`src/display/screens/WorldMask.h`) come from the
**Natural Earth 50m land** dataset, generated with `tools/gen_world_mask.py`.
Natural Earth is in the **public domain**; attribution is not required, but is
gladly given here. <https://www.naturalearthdata.com/>

### BSH water level forecast — CC BY 4.0
The tide forecast is fetched at runtime from the official service of the German
Federal Maritime and Hydrographic Agency
(**Bundesamt für Seeschifffahrt und Hydrographie, BSH**)
(`https://gdi.bsh.de/ldproxy/rest/services/WaterLevelForecast`).

```
© Bundesamt für Seeschifffahrt und Hydrographie (BSH)
Licence: Creative Commons Attribution 4.0 International (CC BY 4.0)
https://creativecommons.org/licenses/by/4.0/
```
The BSH gives no warranty for the data (official federal water level forecast
under § 1 SeeAufG, the German Maritime Tasks Act). This firmware displays the
values **unmodified**; only the height reference is converted from gauge datum to
chart datum. The short form "Source: BSH (CC BY 4.0)" is shown on the clock
screen next to the values.

### Boat-specific data and artwork
The shipped polar table (`data/polar.json`) contains generic example values
made up by the author — it is a placeholder, not real boat data. Replace it
with a polar for your own boat (e.g. from <https://weatherrouting.online/>)
via the web UI's Polar tab.
The boat silhouette (`tools/bootskontur_transparent.png`, processed by
`tools/extract_hull.py` into the Wind screen's hull outline) was drawn by the
author.

### Civil-date algorithms
The date/day conversion functions in `src/SunCalc.h` (`scDaysFromCivil`,
`scCivilFromDays`) are ports of **Howard Hinnant's** publicly documented
chrono-compatible date algorithms
(<https://howardhinnant.github.io/date_algorithms.html>), which are also
published under MIT in <https://github.com/HowardHinnant/date>. The sunrise
procedure in the same file follows the public-domain USNO
"Almanac for Computers" method.

### NMEA 2000 message definitions
Field layouts and resolutions were taken from the public database of the
**canboat** project (Apache-2.0). Only *facts* were adopted (bit offsets,
scalings, enumeration values) — no source code.
<https://github.com/canboat/canboat>

The NMEA 2000 standard itself is a paid document of the National Marine
Electronics Association. This project does not use it; it relies on the public
sources named above.

---

## 5. Tools shipped inside the release package

### esptool 4.8.1 — GPL-2.0-or-later
```
Copyright (c) Espressif Systems (Shanghai) CO LTD and contributors
https://github.com/espressif/esptool
```
The Windows release archive bundles `esptool.exe` so that flashing needs no
installation. It is an **unmodified** upstream build and a *separate program*
that the flash script merely invokes — it is not linked into the firmware, so
its licence does not reach `firmware.bin`.

Redistributing that binary does, however, trigger GPLv2 §3(a): the complete
corresponding source must accompany it — a download link is not sufficient
under GPLv2. The release package therefore contains, next to the executable:

```
esptool-4.8.1/esptool-4.8.1-source.zip   the complete upstream source
esptool-4.8.1/LICENSE, README            upstream notices, unmodified
LICENSES/GPL-2.0.txt                     the licence text
```

`tools/make_release.py` fetches both the binary and the source; if it cannot
(offline build), it ships neither and the flash script falls back to a
`pip install esptool` on the user's side.

---

## 6. Development only — not part of any distribution

| Tool | Licence | Purpose |
|---|---|---|
| SDL2 | zlib | PC simulator |
| freetype-py / Pillow / NumPy | BSD or MIT-style | Font and map generators under `tools/` |

---

*Compiled from the licence files in this project's dependency tree, and — for
the ESP-IDF part — from the linker map of an actual build.
Please update when `platformio.ini` changes.*
