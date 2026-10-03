# Third-Party Notices — NauticPinnace

The project's **own code** is MIT-licensed — see [`LICENSE`](LICENSE) at the
repo root.

This firmware contains third-party software and data. The notices below are
required by the respective licences and must be passed on **with every
distribution** (source code or a built `firmware.bin`).

Everything here was taken from the licence files and source headers in this
project's dependency tree (`.pio/libdeps/…`) and in the installed framework and
toolchain packages, not from secondary sources; which components are linked was
read from the linker maps of all three boards.

The **full texts** of every licence referenced below live in [`LICENSES/`](LICENSES/)
at the repo root, so a distribution can satisfy the pass-the-text obligations
without the build tree.

---

## 1. Copyleft libraries (LGPL) — distribution requires more than a notice

These three are linked **statically** into `firmware.bin`. The LGPL does **not**
require you to publish your own source code — but it does require that a
recipient be able to replace the library and relink.

Anyone distributing the firmware must therefore also provide:

1. this notice and the full licence text (`LICENSES/LGPL-2.1.txt` for the
   Arduino core, `LICENSES/LGPL-3.0.txt` for the async pair),
2. the source of each library in exactly the version used — see
   [Corresponding source](#corresponding-source) below,
3. their own program in a **relinkable form** — either as source code or as
   object files (see [Relinking](#relinking) below),
4. all copyright notices unchanged.

**The three boards do not link the same versions.** The 4-inch stays on the
Arduino core it was released and tested with; the two 1024×600 boards need a
newer one for the RGB panel's bounce buffers. Since the point of naming a
version is that a recipient can obtain the corresponding source for what they
actually received, both columns are given.

| Component | 4-inch | 7B / 5B |
|---|---|---|
| **Arduino-ESP32 core** (incl. WiFi / Network, HTTPClient, WiFiClientSecure / NetworkClientSecure, LittleFS, FS, Preferences, Update, Wire, SPI) | Arduino-ESP32 2.0.17 (PlatformIO package 3.20017.241212, ESP-IDF 4.4.7) | Arduino-ESP32 3.3.11 (pioarduino platform 55.03.311, ESP-IDF 5.5.5) |
| **ESPAsyncWebServer** | 3.6.0 (commit `ad3741d159f9cfd50c567e81b67f3bef1dc6d89f`) | 3.12.0 |
| **AsyncTCP** | 3.3.2 (commit `ef448a8a1dffe4ec1b72326dd5a26211ff227b49`) | 3.5.0 |

Copyright and licence, per board — the async pair changed both between the
two columns:

| Component | 4-inch | 7B / 5B |
|---|---|---|
| **Arduino-ESP32 core** | Copyright (c) Espressif Systems and contributors (see below) — LGPL-2.1-or-later | the same — LGPL-2.1-or-later |
| **ESPAsyncWebServer** | Copyright (c) 2016 Hristo Gochkov — LGPL-3.0 (the `LICENSE` file; the source headers still carry the LGPL-2.1-or-later wording) | Copyright 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles — LGPL-3.0-or-later (the `LICENSE` file and every source header) |
| **AsyncTCP** | Copyright (c) 2016 Hristo Gochkov — LGPL-3.0 (headers as above) | Copyright 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles — LGPL-3.0-or-later |

The Arduino core has no single copyright line. Besides Espressif Systems, the
files of it that this firmware links carry the notices of Arduino (2005-2014),
Nicholas Zambetti, David A. Mellis, Hernando Barragan, Paul Stoffregen, Adrian
McEwen, Ivan Grokhotkov, Hristo Gochkov, Markus Sattler and Evandro Luis
Copercini — all under the core's LGPL-2.1-or-later. Three parts differ:
`ssl_client.cpp` (the TLS client) is **Apache-2.0**, Copyright (C) 2006-2015
ARM Limited, additions Copyright (C) 2017 Evandro Luis Copercini; the libb64
base64 encoder is in the public domain; and on the 7B / 5B `NetworkUdp.h`
carries an **MIT** notice, Copyright (c) 2008 Bjoern Hartmann
(`LICENSES/MIT.txt`).

The two async libraries come from different places per board, which is why the
columns differ: the 4-inch pins them to those commits in `platformio.ini`
(the newer releases stall large responses on its Arduino core), while the panel
boards take ESP32Async's registry releases.

`platformio.ini` pins every library to the exact version or commit named in
this document, so a fresh install resolves to the same versions; a pin is
changed deliberately, together with these notices, the device's licence screen
(`src/display/LicenseText.h`) and the source archives listed below
(`LGPL_SOURCES` in `tools/make_release.py`). When
checking the versions in `.pio/libdeps/<env>/`, mind one trap: the 4-inch
directory contains **two** AsyncTCP entries. `AsyncTCP@src-<hash>` is the
pinned 3.3.2 that is actually linked; a plain `AsyncTCP` directory alongside it
is the second copy the note in `platformio.ini` mentions, and reading its
version gives the wrong answer.

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

### Corresponding source

The source of each LGPL library, in exactly the version linked, is attached to
every [GitHub Release](https://github.com/Ranman86/NauticPinnace/releases) of
NauticPinnace as a separate asset, so binary and source come from the same
place:

| Library | Board | Upstream, exact tag or commit | Release asset |
|---|---|---|---|
| Arduino-ESP32 2.0.17 | 4-inch | <https://github.com/espressif/arduino-esp32/tree/2.0.17> | `src-arduino-esp32-2.0.17.zip` |
| Arduino-ESP32 3.3.11 | 7B / 5B | <https://github.com/espressif/arduino-esp32/tree/3.3.11> | `src-arduino-esp32-3.3.11.zip` |
| AsyncTCP 3.3.2 | 4-inch | <https://github.com/me-no-dev/AsyncTCP/tree/ef448a8a1dffe4ec1b72326dd5a26211ff227b49> | `src-AsyncTCP-ef448a8.zip` |
| ESPAsyncWebServer 3.6.0 | 4-inch | <https://github.com/me-no-dev/ESPAsyncWebServer/tree/ad3741d159f9cfd50c567e81b67f3bef1dc6d89f> | `src-ESPAsyncWebServer-ad3741d.zip` |
| AsyncTCP 3.5.0 | 7B / 5B | <https://github.com/ESP32Async/AsyncTCP/tree/v3.5.0> | `src-AsyncTCP-3.5.0.zip` |
| ESPAsyncWebServer 3.12.0 | 7B / 5B | <https://github.com/ESP32Async/ESPAsyncWebServer/tree/v3.12.0> | `src-ESPAsyncWebServer-3.12.0.zip` |

The application — the part a recipient relinks against a modified library — is
this repository at the release's tag:
`https://github.com/Ranman86/NauticPinnace/tree/<tag>`, for example
<https://github.com/Ranman86/NauticPinnace/tree/v1.2.0>.

### Relinking

To relink, build the application at the release tag with the modified library
in place of the original (put it under `lib/`, or point `lib_deps` in
`platformio.ini` at it). The object files and the linker map of a build land
in:

| Board | Environment | Build directory |
|---|---|---|
| 4-inch | `waveshare_esp32s3_4` | `${platformio.core_dir}/build/nauticpinnace-4inch/waveshare_esp32s3_4/` — built through `tools/build4.ps1`, which sets this directory and the board's own package directory |
| 7B | `waveshare_esp32s3_7b` | `${platformio.core_dir}/build/nauticpinnace/waveshare_esp32s3_7b/` |
| 5B | `waveshare_esp32s3_5b` | `${platformio.core_dir}/build/nauticpinnace/waveshare_esp32s3_5b/` |

`${platformio.core_dir}` is PlatformIO's own directory (`~/.platformio` unless
`PLATFORMIO_CORE_DIR` says otherwise); the 7B / 5B path is the `build_dir` set
in `platformio.ini`.

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

Licence text: `LICENSES/MIT-LVGL.txt`. LVGL bundles three parts by other
authors, and all three are linked:

```
QR code generator (qrcodegen.c)   MIT            Copyright (c) Project Nayuki
lv_printf.c                       MIT            (c) Marco Paland, 2014-2019, PALANDesign Hannover
lv_tlsf.c (memory allocator)      BSD-3-Clause   Copyright (c) 2006-2016, Matthew Conte
```
Licence texts: `LICENSES/MIT.txt` and `LICENSES/BSD-3-Clause-TLSF.txt`.

### ArduinoJson 7.4.3 — MIT
```
Copyright © 2014-2026, Benoit BLANCHON
```
Licence text: `LICENSES/MIT-ArduinoJson.txt`.

### NMEA2000 4.24.1 (Timo Lappalainen) — MIT
```
Copyright (c) 2015-2025 Timo Lappalainen, Kave Oy, www.kave.fi
```
Built from <https://github.com/ttlappalainen/NMEA2000>, commit
`5b7b9fc3ccc18e30ebfba92da6486cffc6251595` (`library.json` 4.24.1). The source
headers of the linked files say 2015-2024 or 2015-2025; the line above covers
both. Licence text: `LICENSES/MIT-NMEA2000.txt`.

The local CAN driver `lib/NMEA2000_esp32/` is an independent re-implementation
on the ESP-IDF `twai_*` API; its public interface mirrors the same author's
`NMEA2000_esp32` library (MIT, Copyright (c) 2015-2020) for drop-in
compatibility, but no function bodies were taken from it.

### LovyanGFX 1.2.21 — FreeBSD, with embedded BSD and MIT parts — **4-inch only**
Not linked into the 7B and 5B firmware: those drive their RGB panel through
ESP-IDF's `esp_lcd` component instead, so nothing of LovyanGFX reaches them.

The 4-inch firmware links 1.2.21, the version `platformio.ini` pins; the
licence and the copyright holders below have not changed across the 1.2
releases.

The library's `license.txt` (`LICENSES/LovyanGFX-license.txt`) holds the notices
of LovyanGFX itself and of the code it contains:
```
Copyright (c) 2020 lovyan03 (https://github.com/lovyan03)       (FreeBSD licence, LovyanGFX itself)
Copyright (c) 2012 Adafruit Industries.  All rights reserved.   (BSD licence, Adafruit GFX)
Copyright (c) 2020 Bodmer (https://github.com/Bodmer)           (FreeBSD licence, TFT_eSPI)
Adafruit_ILI9341: written by Limor Fried/Ladyada for Adafruit Industries   (MIT licence)
```
The library source also bundles **TJpgDec** (ChaN, its own permissive licence),
**pngle** (kikuchan, MIT), **miniz** (public domain), the **efont** bitmap fonts
and the **IPAex** fonts (IPA Font License 1.0). They are in the library source,
**not linked**: none of them reaches the 4-inch image (checked against its
linker map).

### 2.1 ESP-IDF components and the C library linked into `firmware.bin`

The Arduino core pulls in prebuilt ESP-IDF libraries, and the toolchain adds
its C library. The list below is not a guess: it was read from the linker maps
of all three boards (in the build directories listed under
[Relinking](#relinking)), counting only the input sections actually placed in
the image. The 7B and the 5B link the same set; where the 4-inch differs, the
entry says so. Most of these licences — BSD-3-Clause and MIT in particular —
ask for the copyright notice **and the licence text** to accompany a binary
redistribution, which is why each entry names its file in `LICENSES/`.

```
lwIP (TCP/IP stack)                     BSD-3-Clause    LICENSES/BSD-3-Clause-lwIP.txt
    Copyright (c) 2001-2004 Swedish Institute of Computer Science.
    All rights reserved.  (Adam Dunkels and contributors; the COPYING file
    says 2001, 2002)

wpa_supplicant (WiFi security)          BSD-3-Clause    LICENSES/BSD-3-Clause-wpa_supplicant.txt
    Copyright (c) 2002-2022, Jouni Malinen <j@w1.fi> and contributors
    (the ESP-IDF 4.4.7 copy of the 4-inch says 2002-2016; same licence)

mbed TLS (crypto)                       Apache-2.0      LICENSES/Apache-2.0.txt
    Copyright The Mbed TLS Contributors; parts (c) 2006-2015 ARM Limited
    Upstream is dual "Apache-2.0 OR GPL-2.0-or-later"; Apache-2.0 applies here.

FreeRTOS kernel                         MIT             LICENSES/MIT.txt
    Copyright (C) 2020 Amazon.com, Inc. or its affiliates   (4-inch, V10.4.3)
    Copyright (C) 2021 Amazon.com, Inc. or its affiliates   (7B / 5B, V10.5.1)
    Xtensa port also Copyright (c) 2015-2019 Cadence Design Systems, Inc. (MIT);
    Espressif's additions and port layer are Apache-2.0.

Xtensa HAL and interrupt code           MIT             LICENSES/MIT.txt
    (libxt_hal, libxtensa)
    Copyright (c) 1999-2015 Cadence Design Systems, Inc.
    Copyright (c) 2015-2019 Cadence Design Systems, Inc.

LittleFS (the filesystem holding the configuration and the web UI)
    littlefs core                       BSD-3-Clause    LICENSES/BSD-3-Clause-littlefs.txt
        Copyright (c) 2022, The littlefs authors.
        Copyright (c) 2017, Arm Limited. All rights reserved.
    esp_littlefs (the ESP-IDF glue)     MIT             LICENSES/MIT.txt
        Copyright 2020 Brian Pugh
    4-inch: part of the Arduino 2.0.17 SDK (libesp_littlefs);
    7B / 5B: component joltwallet/littlefs 1.22.3 (libjoltwallet__littlefs).

TLSF (heap allocator)                   BSD-3-Clause    LICENSES/BSD-3-Clause-TLSF.txt
    Copyright (c) 2006-2016, Matthew Conte
    Linked twice: the ESP-IDF heap (heap_tlsf.c on the 4-inch, tlsf.c on the
    7B / 5B) and LVGL's lv_tlsf.c (section 2, LVGL).

newlib (C library: formatted I/O, strings, time, math)
                                        BSD-style licences of several holders
                                                        LICENSES/newlib-COPYING.NEWLIB.txt
    Copyright (c) Red Hat, Inc.; Copyright (c) The Regents of the University
    of California; Sun Microsystems (libm), David M. Gay and others - each
    listed in COPYING.NEWLIB. libc.a and libm.a come from the toolchain
    (GCC 8.4.0 on the 4-inch, GCC 14.2.0 on the 7B / 5B).

cJSON                                   MIT             LICENSES/MIT.txt
    Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
    A few bytes only, pulled in by the provisioning manager below.
    4-inch: ESP-IDF's json component; 7B / 5B: espressif/cjson 1.7.19.

net80211, pp, core, smartconfig, espnow (WiFi), phy (RF), coexist
    (binary-only libraries)             Apache-2.0      LICENSES/Apache-2.0.txt
    Copyright (c) Espressif Systems - shipped as prebuilt libraries in the
    SDK; the LICENSE file next to them in ESP-IDF is Apache-2.0. (espnow is
    linked on the 7B / 5B only.)

wifi_provisioning (4-inch) / network_provisioning (7B / 5B)
                                        Apache-2.0      LICENSES/Apache-2.0.txt
    Copyright (c) Espressif Systems

esp_system, esp_wifi, esp_netif, esp_event, esp_hw_support, esp_timer, hal,
soc, driver, esp_driver_* (7B / 5B), spi_flash, nvs_flash, heap, vfs,
pthread, log, cxx, espcoredump, bootloader_support, app_update, esp_lcd,
esp_psram and esp_mm (7B / 5B), esp_diagnostics (4-inch), protocomm, and
ESP-IDF's own newlib glue, …           Apache-2.0      LICENSES/Apache-2.0.txt
    Copyright (c) Espressif Systems and contributors

libgcc, libstdc++ (from the toolchain)  GPL-3.0-or-later WITH GCC-exception-3.1
    The GCC Runtime Library Exception lets compiled programs that link these
    runtime libraries be distributed under terms of the distributor's choice;
    no condition reaches the firmware. Base text: LICENSES/GPL-3.0.txt.
```

The release package and the web flasher also carry the second-stage bootloader
(`bootloader.bin`, which the build takes from the prebuilt bootloader of the
Arduino core) and `boot_app0.bin` from the Arduino core's package — Espressif
code from ESP-IDF's bootloader components, Apache-2.0.

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
<https://scripts.sil.org/OFL>. Licence text: `LICENSES/OFL-1.1-Montserrat.txt`.

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
> with the copyright notice and the licence text**
> (`LICENSES/OFL-1.1-Montserrat.txt`). Montserrat declares **no Reserved Font
> Name** — its copyright line names none — so the OFL's renaming condition does
> not apply to it; the generated fonts carry their own names (`depth_font_*`,
> `latin_suppl_*`) anyway.

### Font Awesome 5 Free
```
Copyright Fonticons, Inc.
```
The icon glyphs (warning triangle, plus/minus, speaker, WiFi and others) are
contained in the same LVGL font tables. The font files are under
**SIL OFL 1.1**; Font Awesome code is under MIT. See
<https://fontawesome.com/license/free>.

---

## 4. Artwork — CC BY 4.0

### OpenBridge Icon Pack
```
"OpenBridge Icon Pack" by OpenBridge
https://www.figma.com/community/file/1445713209741917748/openbridge-icon-pack
Licence: Creative Commons Attribution 4.0 International (CC BY 4.0)
https://creativecommons.org/licenses/by/4.0/
Provided as is, without warranties of any kind (see Section 5 of the licence).
```
Exported from the Figma Community file on 2026-09-29.

**Changes:**

* firmware — `src/display/fonts/ob_icons_<px>.c`, created with
  `tools/gen_icons.py` at the pixel sizes the screens use:
  rasterised into 4-bpp bitmap fonts and reduced to one colour;
* web configuration — the inline SVG sprite in `data/index.html`, generated by
  the same script:
  reduced to one colour (currentColor), coordinates rounded.

No other changes were made to the icons this firmware ships.

The generated `.c` files and the sprite contain the licensed artwork. They
therefore remain under **CC BY 4.0**, not MIT, and may be redistributed only
together with this notice. The generator script is MIT.

The icons whose names end in `-google` are based on **Google Material Icons**,
© Google LLC, Apache License 2.0 (`LICENSES/Apache-2.0.txt`). The icons whose
names end in `-iec` are OpenBridge redrawings of symbols specified in IEC 62288;
no rights of the IEC are granted by the licence above.

NauticPinnace is not affiliated with or endorsed by OpenBridge, the Ocean
Industries Concept Lab or AHO; no OpenBridge logo is used. Licence text:
`LICENSES/CC-BY-4.0.txt`.

*OpenBridge icon trial: this whole section goes when the trial goes (see
`ICONS-TRIAL.md`).*

---

## 5. Data

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

## 6. Tools shipped inside the release package

### esptool 4.8.1 — GPL-2.0-or-later
```
Copyright (c) Espressif Systems (Shanghai) CO LTD and contributors
https://github.com/espressif/esptool
```
The Windows release archive bundles `esptool.exe` so that flashing needs no
installation. It is an **unmodified** upstream build and a *separate program*
that the flash script merely invokes — it is not linked into the firmware, so
its licence does not reach `firmware.bin`.

The exe is a **PyInstaller bundle**. Besides esptool it contains a Python 3.8
runtime, OpenSSL, libffi, bzip2, xz/liblzma, pyserial, intelhex, the
PyInstaller bootloader and Microsoft runtime DLLs (plus the zlib inside the
Python runtime). Their notices and licence texts ship in the package as
`esptool-4.8.1/notices/` — a copy of `tools/esptool-exe-notices/` in this
repository, whose `NOTICE.md` names each component with its version, licence
and source.

Redistributing that binary does, however, trigger GPLv2 §3(a): the complete
corresponding source must accompany it — a download link is not sufficient
under GPLv2. The release package therefore contains, next to the executable:

```
esptool-4.8.1/esptool-4.8.1-source.zip   the complete upstream esptool source
esptool-4.8.1/pyserial-3.5.tar.gz        the source distributions of the two
esptool-4.8.1/intelhex-2.3.0.tar.gz      Python libraries inside the exe (URLs
                                         and SHA-256 in
                                         tools/esptool-exe-notices/sources.json)
esptool-4.8.1/LICENSE, README.md         upstream notices, unmodified
esptool-4.8.1/notices/                   notices and licence texts of everything
                                         the exe bundles
LICENSES/GPL-2.0.txt                     the licence text
```

`tools/make_release.py` fetches the binary and these sources (the two source
distributions checked against their SHA-256); if any of them cannot be
fetched, it builds no package at all, so `esptool.exe` never ships without its
source and notices.

---

### ESP Web Tools — Apache-2.0 — *referenced, not redistributed*
The browser flasher page (`docs/flash.html`, served from GitHub Pages) loads
`esp-web-tools@10` from the unpkg CDN with a `<script>` tag. It is never copied
into this repository or into any release archive, so there is no notice
obligation — it is named here because it is a visible part of how people
receive the firmware, and because a visitor's browser contacts unpkg.com to get
it. `docs/flash/` itself carries only the firmware images plus a copy of
`LICENSE`, `LICENSES/` and this document, so the notices travel with the
binaries they describe. That copy is a snapshot, not a link: `docs/flash/` is
written only by `tools/gen_web_flasher.py`, which rebuilds the images of all
three boards and copies the licence files alongside — never update the images
there by hand.

## 7. Development only — not part of any distribution

Nothing in this section is shipped, so nothing here carries an obligation. It
is listed because the question "shouldn't the compiler be in here?" is a fair
one and deserves an answer that outlives the asking.

**Why the build chain is absent from sections 1–6:** licence duties attach to
*distribution*. What this project distributes is `firmware.bin`,
`littlefs.bin` and the release archive — those are what sections 1 through 5
cover, plus `esptool.exe` in section 6 because that one really is shipped. The
tools below run on the developer's machine and are fetched independently onto
each machine by PlatformIO and pip; handing someone a compiled binary does not
redistribute the compiler that made it.

The usual worry is GCC being GPL. It does not reach the firmware: the **GCC
Runtime Library Exception** exists precisely so that compiled output carries no
GPL obligation, and the compiler itself is never handed on.

| Tool | Licence | Purpose |
|---|---|---|
| SDL2 | zlib | PC simulator |
| freetype-py / Pillow / NumPy | BSD or MIT-style | Font and map generators under `tools/` |
| PlatformIO Core, SCons | Apache-2.0, MIT | Build system |
| toolchain-xtensa-esp32s3 (GCC 8.4.0, 4-inch), toolchain-xtensa-esp-elf (GCC 14.2.0, 7B / 5B), toolchain-riscv32-esp | GPL-3.0 **with GCC Runtime Library Exception** | Compiler |
| tool-esptoolpy, tool-mklittlefs, tool-mkspiffs, tool-mkfatfs | GPL-2.0-or-later / MIT | Image building and flashing, as tools |
| Python wheels pulled in by esptool: `cryptography`, `ecdsa`, `bitstring`, `reedsolo`, `intelhex`, `cffi`, `six`, `bitarray`, `pycparser` | Apache-2.0 / BSD / MIT-style | Installed by pip into PlatformIO's own virtualenv (the copies of pyserial and intelhex inside `esptool.exe` are shipped, see section 6) |

---

*Compiled from the licence files in this project's dependency tree, and — for
the ESP-IDF part — from the linker maps of actual builds of all three boards.
Please update when `platformio.ini` changes.*
