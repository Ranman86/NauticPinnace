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
| **Arduino-ESP32 core** (incl. WiFi / Network, HTTPClient, WiFiClientSecure / NetworkClientSecure, LittleFS, FS, Preferences, Wire, SPI) | Arduino-ESP32 2.0.17 (PlatformIO package 3.20017.241212, ESP-IDF 4.4.7) | Arduino-ESP32 3.3.11 (pioarduino platform 55.03.311, ESP-IDF 5.5.5) |
| **ESPAsyncWebServer** | 3.6.0 (commit `ad3741d159f9cfd50c567e81b67f3bef1dc6d89f`) | 3.12.0 |
| **AsyncTCP** | 3.3.2 (commit `ef448a8a1dffe4ec1b72326dd5a26211ff227b49`) | 3.5.0 |

Copyright and licence, per board — the async pair changed both between the
two columns:

| Component | 4-inch | 7B / 5B |
|---|---|---|
| **Arduino-ESP32 core** | Copyright (c) Espressif Systems and contributors (see below) — LGPL-2.1-or-later; the files Espressif wrote itself Apache-2.0 | the same — LGPL-2.1-or-later; the files Espressif wrote itself Apache-2.0 |
| **ESPAsyncWebServer** | Copyright (c) 2016 Hristo Gochkov — LGPL-3.0 (the `LICENSE` file; the source headers still carry the LGPL-2.1-or-later wording) | Copyright 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles — LGPL-3.0-or-later (the `LICENSE` file and every source header) |
| **AsyncTCP** | Copyright (c) 2016 Hristo Gochkov — LGPL-3.0 (headers as above) | Copyright 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles — LGPL-3.0-or-later |

The Arduino core has neither a single copyright line nor a single licence. Its
`LICENSE.md` is LGPL-2.1-or-later; that covers the Arduino-API files this
firmware links, and every linked file without a licence header of its own
(`main.cpp`, `HardwareSerial.cpp` and others). Those files carry the notices
of Arduino (2005-2014), Nicholas Zambetti, David A. Mellis, Hernando Barragan,
Paul Stoffregen, Adrian McEwen, Ivan Grokhotkov, Hristo Gochkov, Markus Sattler
and Evandro Luis Copercini. Four groups of linked files differ:

* **The files Espressif wrote itself are Apache-2.0**, Copyright Espressif
  Systems (Shanghai) — each says so in its header (`LICENSES/Apache-2.0.txt`).
  Linked on the 4-inch: `HWCDC.cpp` and `esp32-hal-{bt,cpu,gpio,i2c,i2c-slave,
  matrix,misc,psram,rmt,spi,time,uart}.c` of the core, and `vfs_api.cpp`
  (FS), `LittleFS.cpp`, `Preferences.cpp` and `esp_crt_bundle.c`
  (WiFiClientSecure) of its libraries. On the 7B / 5B: `esp32-hal-{bt,cpu,
  gpio,i2c-ng,i2c-slave,matrix,misc,periman,psram,rgb-led,rmt,spi,time,
  uart}.c` (and `HWCDC.cpp` on the 5B) of the core, and `vfs_api.cpp`,
  `LittleFS.cpp`, `Preferences.cpp`, `NetworkEvents.cpp`,
  `NetworkInterface.cpp`, `NetworkManager.cpp` and WiFi's `AP.cpp` and
  `STA.cpp` of its libraries. (The 4-inch's `esp32-hal-rgb-led.c` has no
  header of its own and falls under the LGPL.)
* `ssl_client.cpp` (the TLS client) is **Apache-2.0**, Copyright (C) 2006-2015
  ARM Limited, additions Copyright (C) 2017 Evandro Luis Copercini.
* The libb64 base64 encoder is in the public domain.
* On the 7B / 5B, `NetworkUdp.h` carries an **MIT** notice, Copyright (c) 2008
  Bjoern Hartmann (`LICENSES/MIT.txt`).

Apache-2.0 asks for no on-screen notice here (neither package has a `NOTICE`
file), and the core is still treated as one LGPL library for relinking: its
source archives below contain every file, whatever its licence.

The two async libraries come from different places per board, which is why the
columns differ: the 4-inch pins them to those commits in `platformio.ini`
(the newer releases stall large responses on its Arduino core), while the panel
boards take ESP32Async's registry releases.

`platformio.ini` pins every library, the platforms and the 4-inch's framework
package to the exact version or commit named in this document, so a fresh
install resolves to the same versions; a pin is changed deliberately, together
with these notices, the device's licence screen (`src/display/LicenseText.h`)
and the source archives listed below (`LGPL_SOURCES` in
`tools/make_release.py`).

One part is not pinned by `platformio.ini`: the 7B / 5B rebuild the ESP-IDF
libraries underneath the Arduino core from ESP-IDF 5.5.5 with this project's
`custom_sdkconfig`, and during that rebuild the ESP-IDF component manager
resolves the managed components itself. Three of them are linked into the
firmware; for v1.3.0 they resolved to
joltwallet/littlefs 1.23.3, espressif/cjson 1.7.19~2 and
espressif/network_provisioning 1.0.2 — the versions named in section 2.1. A
later rebuild may pick newer ones; the notices are then checked again. When
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
the [GitHub Release](https://github.com/Ranman86/NauticPinnace/releases) of
NauticPinnace as a separate asset, so binary and source come from the same
place. (Up to v1.2.0 the 4-inch's Arduino core was attached only as the
upstream 2.0.17 tag, which differs from the PlatformIO package it links in
seven files; from v1.3.0 on the package's own source is attached as well. It
is the same package version, 3.20017.241212, that the 1.2.0 4-inch image
links.)

| Library | Board | Upstream, exact tag, commit or package | Release asset |
|---|---|---|---|
| Arduino-ESP32 2.0.17 as PlatformIO package framework-arduinoespressif32 3.20017.241212 (the 2.0.17 tag plus PlatformIO's backports) | 4-inch | <https://registry.platformio.org/tools/platformio/framework-arduinoespressif32> (version 3.20017.241212) | `src-framework-arduinoespressif32-3.20017.241212.zip` |
| Arduino-ESP32 2.0.17, the upstream tag that package is based on (for reference) | 4-inch | <https://github.com/espressif/arduino-esp32/tree/2.0.17> | `src-arduino-esp32-2.0.17.zip` |
| Arduino-ESP32 3.3.11 | 7B / 5B | <https://github.com/espressif/arduino-esp32/tree/3.3.11> | `src-arduino-esp32-3.3.11.zip` |
| AsyncTCP 3.3.2 | 4-inch | <https://github.com/me-no-dev/AsyncTCP/tree/ef448a8a1dffe4ec1b72326dd5a26211ff227b49> | `src-AsyncTCP-ef448a8.zip` |
| ESPAsyncWebServer 3.6.0 | 4-inch | <https://github.com/me-no-dev/ESPAsyncWebServer/tree/ad3741d159f9cfd50c567e81b67f3bef1dc6d89f> | `src-ESPAsyncWebServer-ad3741d.zip` |
| AsyncTCP 3.5.0 | 7B / 5B | <https://github.com/ESP32Async/AsyncTCP/tree/v3.5.0> | `src-AsyncTCP-3.5.0.zip` |
| ESPAsyncWebServer 3.12.0 | 7B / 5B | <https://github.com/ESP32Async/ESPAsyncWebServer/tree/v3.12.0> | `src-ESPAsyncWebServer-3.12.0.zip` |

The application — the part a recipient relinks against a modified library — is
this repository at the release's tag:
`https://github.com/Ranman86/NauticPinnace/tree/<tag>`, for example
<https://github.com/Ranman86/NauticPinnace/tree/v1.3.0>.

### Relinking

To relink, build the application at the release tag with the modified library
in place of the original (put it under `lib/`, or point `lib_deps` in
`platformio.ini` at it). For a modified Arduino core on the 4-inch, copy the
contents of the folder `framework-arduinoespressif32-3.20017.241212/` in
`src-framework-arduinoespressif32-3.20017.241212.zip` over the installed
package of that version (the 4-inch's `packages-4inch/framework-arduinoespressif32/`,
which keeps its precompiled ESP-IDF libraries in `tools/sdk/`), or point
`platform_packages` at such a copy. For a modified core on the 7B / 5B,
replace the installed `framework-arduinoespressif32` package (3.3.11) with the
`arduino-esp32-3.3.11/` folder of `src-arduino-esp32-3.3.11.zip`, whose
`cores/`, `libraries/` and `variants/` are those of the installed package, or
point `platform_packages` at it. The ESP-IDF libraries underneath are not part
of the LGPL library: the 7B / 5B build rebuilds them locally from ESP-IDF 5.5.5
with the `custom_sdkconfig` in `platformio.ini`. The object files and the
linker map of a build land in:

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

The local CAN driver `lib/NMEA2000_esp32/` is a re-write on the ESP-IDF
`twai_*` API. Its public interface mirrors the same author's `NMEA2000_esp32`
library (MIT, Copyright (c) 2015-2020 Timo Lappalainen, Kave Oy, www.kave.fi)
for drop-in compatibility, and a few lines follow that library: the
buffer-size defaults in `InitCANFrameBuffers()`, the `IsOpen` / `CanInUse`
guard at the top of `CANOpen()` and the constructor's initialiser list. All
other code was written independently. Licence text:
`LICENSES/MIT-NMEA2000.txt`.

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
The only LovyanGFX font in the 4-inch image is Font0, the Adafruit GFX
"glcdfont" (BSD, Copyright (c) 2012 Adafruit Industries, credited above).

The library source also bundles **TJpgDec** (ChaN, its own permissive licence),
**pngle** (kikuchan, MIT), **miniz** (public domain), the QR code codec
(Richard Moore, MIT), the QOI codec, the GFXFF FreeFonts and TomThumb, Bodmer's
RLE fonts, the **efont** bitmap fonts (BSD-3-Clause, /efont/ The Electronic
Font Open Laboratory) and the **IPA / IPAex** fonts (IPA Font License 1.0).
They are in the library source, **not linked**: none of them reaches the
4-inch image (checked against its linker map).

### 2.1 ESP-IDF components and the C library linked into `firmware.bin`

The Arduino core pulls in ESP-IDF libraries — prebuilt ones on the 4-inch;
the 7B / 5B rebuild them from ESP-IDF 5.5.5 with the `custom_sdkconfig` in
`platformio.ini` — and the toolchain adds its C library. The list below is not
a guess: it was read from the linker maps of all three boards (in the build
directories listed under [Relinking](#relinking)), counting only the input
sections actually placed in the image. The 7B and the 5B link the same set;
where the 4-inch differs (most visibly: only the 7B / 5B contain lwIP's PPP
code), the entry says so. Most of these licences — BSD-3-Clause and MIT in particular —
ask for the copyright notice **and the licence text** to accompany a binary
redistribution, which is why each entry names its file in `LICENSES/`.

```
lwIP (TCP/IP stack)                     BSD-3-Clause    LICENSES/BSD-3-Clause-lwIP.txt
    Copyright (c) 2001-2004 Swedish Institute of Computer Science.
    All rights reserved.  (Adam Dunkels and contributors; the COPYING file
    says 2001, 2002; ARP and Ethernet say 2001-2003)
    Further holders of linked files, under the same licence text:
    Copyright (c) 2001-2004 Leon Woestenberg <leon.woestenberg@gmx.net>  (DHCP)
    Copyright (c) 2001-2004 Axon Digital Design B.V., The Netherlands.   (DHCP)
    Copyright (c) 2003-2004 Leon Woestenberg <leon.woestenberg@axon.tv>  (ARP, Ethernet)
    Copyright (c) 2003-2004 Axon Digital Design B.V., The Netherlands.   (ARP, Ethernet)
    Copyright (c) 2010 Inico Technologies Ltd.                  (IPv6, ND6, MLD6)
    Copyright (c) 2007-2009 Frederic Bernon, Simon Goldschmidt  (SNTP)
    Copyright (c) 2018 Simon Goldschmidt                        (DHCPv6; 7B / 5B only)
    uIP version Copyright (c) 2002-2003, Adam Dunkels.          (DNS)
    Copyright (c) 2016 The MINIX 3 Project.                     (TCP ISN hook, ESP-IDF port)
    and, with its own wording (the second text in the licence file):
    Copyright (c) 2002 CITEL Technologies Ltd.                  (IGMP)

lwIP PPP code (7B / 5B only)            BSD-style       LICENSES/BSD-lwIP-PPP.txt
    Copyright (c) 1984-2000 Carnegie Mellon University. All rights reserved.
    Copyright (c) 1993-2002 Paul Mackerras. All rights reserved.
    Copyright (c) 2003 by Marc Boucher, Services Informatiques (MBSI) inc.
    portions Copyright (c) 1997 by Global Election Systems Inc.
    Copyright (c) 1998 by Global Election Systems Inc.
    Copyright (c) 1994 The Australian National University.
    Copyright (c) 1989 Carnegie Mellon University.
    Copyright (c) 1989 Regents of the University of California.
    The firmware never opens a PPP link, but lwIP's initialisation pulls
    this code in, because the ESP-IDF build of the 7B / 5B has
    CONFIG_LWIP_PPP_SUPPORT set. Its licences ask for these
    acknowledgments, here and in the licence file:
      "This product includes software developed by Computing Services
       at Carnegie Mellon University (http://www.cmu.edu/computing/)."
      "This product includes software developed by Paul Mackerras
       <paulus@samba.org>".
      The TCP/IP header compression code in vj.c was developed by the
      University of California, Berkeley; the part of ppp.c taken from
      if_ppp.h was developed by Carnegie Mellon University.
    The 7B and 5B images of 1.2.0 (the v1.2.0 release, and docs/flash/7b
    and docs/flash/5b at tag v1.2.0) contain the same PPP files; this
    entry applies to them as well. The 4-inch links no PPP code.

wpa_supplicant (WiFi security)          BSD-3-Clause    LICENSES/BSD-3-Clause-wpa_supplicant.txt
    Copyright (c) 2002-2022, Jouni Malinen <j@w1.fi> and contributors
    (the ESP-IDF 4.4.7 copy of the 4-inch says 2002-2016; same licence)
    Copyright (c) 2019, The Linux Foundation        (dragonfly.c, SAE / WPA3)
    Copyright (c) 2020, The Linux Foundation        (sae_pk.c; 7B / 5B only)
    Copyright (c) 2013 Cozybit, Inc.                (aes-siv.c; 7B / 5B only)

mbed TLS (crypto)                       Apache-2.0      LICENSES/Apache-2.0.txt
    Copyright The Mbed TLS Contributors; parts (c) 2006-2015 ARM Limited
    Upstream is dual "Apache-2.0 OR GPL-2.0-or-later"; Apache-2.0 applies here.
    (The 4-inch's network port also names "Additions Copyright (C) 2015
    Angus Gratton".)

FreeRTOS kernel                         MIT             LICENSES/MIT.txt
    Copyright (C) 2020 Amazon.com, Inc. or its affiliates   (4-inch, V10.4.3)
    Copyright (C) 2021 Amazon.com, Inc. or its affiliates   (7B / 5B, V10.5.1)
    Copyright (C) 2017 Amazon.com, Inc. or its affiliates   (4-inch Xtensa port.c)
    Xtensa port also Copyright (c) 2015-2019 Cadence Design Systems, Inc. (MIT),
    and on the 4-inch Copyright (c) 2003-2015 Cadence Design Systems, Inc.
    (portasm.S); Espressif's additions and port layer are Apache-2.0.

Xtensa HAL, vectors and interrupt code  MIT             LICENSES/MIT.txt
    (libxt_hal, libxtensa, the FreeRTOS Xtensa vectors and context code)
    Copyright (c) 1999-2015 Cadence Design Systems, Inc.
    Copyright (c) 2015-2019 Cadence Design Systems, Inc.
    Copyright (c) 2006-2015 Cadence Design Systems Inc.     (4-inch:
        xtensa_vectors.S, xtensa_context.S, xtensa_intr.c, xtensa_intr_asm.S)

ubsan stub (esp_system)                 BSD-2-Clause    LICENSES/BSD-2-Clause-ubsan.txt
    Copyright (c) 2016, Linaro Limited
    Modified for HelenOS use by Jiří Zárevúcky.
    Adaptations for ESP-IDF Copyright (c) 2020 Espressif Systems (Shanghai) Co. Ltd.
    Only a 5-byte hook of ubsan.c is placed in the image, on every board.

LittleFS (the filesystem holding the configuration and the web UI)
    littlefs core                       BSD-3-Clause    LICENSES/BSD-3-Clause-littlefs.txt
        Copyright (c) 2022, The littlefs authors.
        Copyright (c) 2017, Arm Limited. All rights reserved.
    esp_littlefs (the ESP-IDF glue)     MIT             LICENSES/MIT.txt
        Copyright 2020 Brian Pugh
    4-inch: part of the Arduino 2.0.17 SDK (libesp_littlefs);
    7B / 5B: component joltwallet/littlefs 1.23.3 (libjoltwallet__littlefs).

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
    4-inch: ESP-IDF's json component; 7B / 5B: espressif/cjson 1.7.19~2
    (cJSON 1.7.19).

net80211, pp, core, smartconfig, espnow (WiFi), phy (RF), coexist
    (binary-only libraries)             Apache-2.0      LICENSES/Apache-2.0.txt
    Copyright (c) Espressif Systems - shipped as prebuilt libraries in the
    SDK; the LICENSE file next to them in ESP-IDF is Apache-2.0. (espnow is
    linked on the 7B / 5B only.)

wifi_provisioning (4-inch) / espressif/network_provisioning 1.0.2 (7B / 5B)
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
    no condition reaches the firmware. Exception text:
    LICENSES/GCC-exception-3.1.txt; base text: LICENSES/GPL-3.0.txt.
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

### Font Awesome Free 5.9.0
```
Copyright (c) Font Awesome
```
The icon glyphs (warning triangle, plus/minus, speaker, WiFi and others) are
contained in the same LVGL font tables. LVGL 8.4.0 converts them from
`FontAwesome5-Solid+Brands+Regular.woff`, whose own notice is the line above
(the font's legal owner is Fonticons, Inc.). Font Awesome applies the
**SIL OFL 1.1** to its font files (<https://fontawesome.com/license/free>), so
the converted glyphs remain under the OFL like Montserrat; the font declares
no Reserved Font Name. Licence text: `LICENSES/OFL-1.1-Montserrat.txt` (the
same OFL 1.1 text; its header names Montserrat, the notice for these glyphs is
the one above).

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

The icons whose names end in `-google`, and these others drawn on the same
geometry, are based on **Google Material Icons** / Material Symbols, © Google
LLC, Apache License 2.0 (`LICENSES/Apache-2.0.txt`):

* in the display fonts: `home`, `time`, `anchorwatch`, `trend`,
  `settings-iec`, `simulation`, `media-play`, `media-pause`,
  `media-skip-next`, `media-skip-previous`;
* in the web sprite: `clipboard`, `save-proposal`, `delete`, `image`,
  `palette-night`, `simulation`.

The 1.2.0 images and web interface (the v1.2.0 release, and `docs/flash/` at
tag v1.2.0) contain several of the same icons; this attribution applies to
them as well.

The icons whose names end in `-iec` are OpenBridge redrawings of symbols
specified in IEC 62288; no rights of the IEC are granted by the licence above.
One exception to that rule: the gear outline of `settings-iec` is Material
geometry, as listed above.

NauticPinnace is not affiliated with or endorsed by OpenBridge, the Ocean
Industries Concept Lab or AHO; no OpenBridge logo is used. Licence text:
`LICENSES/CC-BY-4.0.txt`.

---

## 5. Data

### Natural Earth — public domain
The world map coastlines (`src/display/screens/WorldMask.h` on the 4-inch,
`WorldMask600.h` on the 7B / 5B, preview `tools/world_mask_preview.png`) come
from the **Natural Earth 50m land** dataset, generated with
`tools/gen_world_mask.py`.
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
under § 1 SeeAufgG, the German Maritime Tasks Act). **Changes** (CC BY 4.0,
section 3(a)(1)(B)): the heights are converted from gauge datum to chart datum
and rounded for display, and the station names are transliterated to plain
ASCII; past the official forecast horizon the service's own astronomical
prediction is shown, as the service delivers it. The times are shown
unmodified. The short form "Source: BSH (CC BY 4.0)" is shown on the clock
screen next to the values, and the device's licence screen names the change.

To fetch the forecast, the display sends its approximate position (rounded to
0.1°, or Cuxhaven while no position is known) and, like any web request, its
IP address to `gdi.bsh.de`, about every 3 hours while the download is
switched on (the default; after a failed download again after 2 and after
another 10 minutes, at most twice per outage). It can be switched off in the
web UI.

### Boat-specific data and artwork
The shipped polar table (`data/polar.json`) contains generic example values
made up by the author — it is a placeholder, not real boat data. Replace it
with a polar for your own boat (from an ORC speed guide or from your boat's
manufacturer, under their terms) via the web UI's Polar tab.
The boat silhouette (`tools/bootskontur_transparent.png`, processed by
`tools/extract_hull.py` into the Wind screen's hull outline) was drawn by the
author.

### Civil-date algorithms
The date/day conversion functions in `src/SunCalc.h` (`scDaysFromCivil`,
`scCivilFromDays`) are ports of **Howard Hinnant's** chrono-compatible date
algorithms as published on
<https://howardhinnant.github.io/date_algorithms.html>, which the author
donated to the public domain there ("Consider these donated to the public
domain."). The same algorithms are also in his MIT-licensed date library
(<https://github.com/HowardHinnant/date>); nothing was copied from that
library. The sunrise procedure in the same file follows the public-domain USNO
"Almanac for Computers" method, and its solar position the US Naval
Observatory's public-domain "Approximate Solar Coordinates".

### NMEA 2000 message definitions
Field layouts and resolutions were taken from the public database of the
**canboat** project (Apache-2.0) and, for the media module's commands, from
the **Signal K** project (Apache-2.0). Only *facts* were adopted (bit offsets,
scalings, enumeration values) — no source code.
<https://github.com/canboat/canboat>, <https://github.com/SignalK>

The NMEA 2000 standard itself is a paid document of the National Marine
Electronics Association. This project does not use it; it relies on the public
sources named above and on observing the bus traffic of real installations
(for example the autopilot's steering mode, which some pilots report
differently from the public tables — the README says where).

NMEA 2000® is a registered trademark of the National Marine Electronics
Association. NauticPinnace is not NMEA-certified and not affiliated with or
endorsed by the NMEA; other names belong to their owners.

---

## 6. Flashing tools — referenced, not redistributed

### esptool 4.8.1 — GPL-2.0-or-later — *downloaded by the user*
```
Copyright (c) Espressif Systems (Shanghai) CO LTD and contributors
https://github.com/espressif/esptool
```
`flash.bat` in the release package runs `esptool.exe`, the Windows build of
esptool 4.8.1, which the user downloads from Espressif's own release
(<https://github.com/espressif/esptool/releases/tag/v4.8.1>, file
`esptool-v4.8.1-win64.zip`) and puts next to `flash.bat`; the script checks the
file's SHA-256 before using it. From v1.3.0 on the release package does **not**
contain `esptool.exe` any more: that exe bundles GPL-licensed flasher stubs, a
Python runtime, OpenSSL and further libraries, whose complete corresponding
source a redistributor would have to ship with it. esptool is a separate
program that is never linked into the firmware, so its licence does not reach
`firmware.bin`. (The packages up to v1.2.0 still bundled the exe.)

### ESP Web Tools — Apache-2.0 — *loaded from a CDN*
The browser flasher page (`docs/flash.html`, served from GitHub Pages) loads
`esp-web-tools@10.5.2` (an exact version) from the unpkg CDN with a `<script>` tag. It is
never copied into this repository or into any release archive, so there is no
notice obligation — it is named here because it is a visible part of how
people receive the firmware, and because a visitor's browser contacts
unpkg.com to get it (the page says so). `docs/flash/` itself carries only the firmware images plus a copy of
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
cover; section 6 names the flashing tools a user fetches from their own
sources. The tools below run on the developer's machine and are fetched independently onto
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
| Python wheels pulled in by esptool: `cryptography`, `ecdsa`, `bitstring`, `reedsolo`, `intelhex`, `cffi`, `six`, `bitarray`, `pycparser` | Apache-2.0 / BSD / MIT-style | Installed by pip into PlatformIO's own virtualenv |

---

*Compiled from the licence files in this project's dependency tree, and — for
the ESP-IDF part — from the linker maps of actual builds of all three boards.
Please update when `platformio.ini` changes.*
