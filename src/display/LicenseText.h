#pragma once
#include "../i18n/I18n.h"

// ============================================================
// Licence text shown on the display, in both UI languages.
//
// Umlauts are fine here: the panel font is Montserrat plus the generated
// latin_suppl_* fallback (see tools/gen_latin_supplement.py), which covers
// ae/oe/ue/ss, the en dash, quotes, degree, plus-minus and multiplication.
// Anything outside that set still draws as an empty box.
//
// This is the SHORT form for a small screen; it names the licence AND the
// copyright holder of the main components, and points to THIRD-PARTY-NOTICES.md
// for the complete list (every further holder, e.g. of single lwIP files, is
// there). It is the copy of the notices that travels with the device; for the
// copyleft ones it also follows the rule that a work
// which displays copyright notices while running must list the library's notice
// among them (LGPL-2.1 section 6, LGPL-3.0 section 4c). This screen displays
// notices, so that rule applies: never drop the copyright lines from the LGPL
// block. THIRD-PARTY-NOTICES.md holds the full, unabridged version, LICENSES/
// the licence texts; the intro names both and where to find them.
//
// NOTE: the licence NAMES and identifiers (LGPL-2.1-or-later, LGPL-3.0,
// LGPL-3.0-or-later, MIT, BSD-3-Clause, SIL OFL 1.1, CC BY 4.0, Apache-2.0)
// and every copyright line are legal text — they are byte-identical in both
// languages and must stay that way. Only the surrounding prose is translated.
//
// Lines are broken by hand at <= 49 characters: the label does not wrap (see
// LicenseOverlay.cpp), so a longer line is clipped at the right edge.
//
// VERSIONS DIFFER BY BOARD. The 4-inch stays on the Arduino core it shipped
// with; the 1024x600 boards need a newer one for the RGB panel. This screen is
// the copy of the notice that travels with the device, so it has to name what
// THAT device links - it used to state the 4-inch versions on all three. The
// LIC_* macros below are concatenated into the text; keep them in step with
// the table in THIRD-PARTY-NOTICES.md, which carries both columns.
//
// The ESP-IDF block is nearly the same on every board (read from the linker
// maps): ESP-IDF 4.4.7 and 5.5.5 link the same components under the same
// licences, except that only the 7B / 5B contain lwIP's PPP code (LIC_PPP_*).
// Where the two carry different years, the line names the holder without one.
// ============================================================

// The 4-inch pins AsyncTCP and ESPAsyncWebServer to specific commits in
// platformio.ini (the newer releases stall large responses on its Arduino
// core); the panel boards take ESP32Async's registry releases, pinned to exact
// versions as well. So the library versions differ, not just the core - and so
// do the async pair's copyright line and licence identifier: the pinned commits
// still carry the original 2016 header, the newer releases name four holders
// and LGPL-3.0-or-later.
//
// Read them from .pio/libdeps/<env>/, and mind the trap: the 4-inch directory
// holds BOTH a git-pinned "AsyncTCP@src-<hash>" (3.3.2, the one that is
// linked) and a stray registry "AsyncTCP" (3.5.0, the second copy the comment
// in platformio.ini warns about). The plain name is the wrong one to believe.
//
// The core is named by its upstream release (the tag a recipient can look up),
// not by PlatformIO's package number: 2.0.17 is framework-arduinoespressif32
// 3.20017.241212, 3.3.11 is pioarduino platform 55.03.311.
#if defined(BOARD_PANEL_1024X600)
#define LIC_V_CORE     "3.3.11 (ESP-IDF 5.5.5)"
#define LIC_V_WEBSRV   "3.12.0"
#define LIC_V_ASYNCTCP "3.5.0"
#define LIC_L_ASYNC    "LGPL-3.0-or-later"
#define LIC_C_ASYNC                                  \
    "   Copyright 2016-2026 Hristo Gochkov,\n"       \
    "   Mathieu Carbou, Emil Muratov, Will Miles\n"
#else
#define LIC_V_CORE     "2.0.17 (ESP-IDF 4.4.7)"
#define LIC_V_WEBSRV   "3.6.0"
#define LIC_V_ASYNCTCP "3.3.2"
#define LIC_L_ASYNC    "LGPL-3.0"
#define LIC_C_ASYNC                            \
    "   Copyright (c) 2016 Hristo Gochkov\n"
#endif

// LovyanGFX drives the 4-inch's SPI panel only; the 1024x600 boards use
// ESP-IDF's esp_lcd and link nothing of it, so the entry is empty there. Of
// the codecs and fonts in the library's source (TJpgDec, pngle, miniz, the QR
// and QOI codecs, GFXFF, the RLE fonts, efont, IPA/IPAex) none reaches the
// 4-inch image either, so none is listed; its one linked font, Font0, is the
// Adafruit GFX glcdfont, covered by the Adafruit line.
#if defined(BOARD_PANEL_1024X600)
#define LIC_LOVYAN_DE ""
#define LIC_LOVYAN_EN ""
#else
#define LIC_LOVYAN_DE                              \
    "LovyanGFX 1.2.21 - MIT und BSD\n"             \
    "   Copyright (c) 2020 lovyan03\n"             \
    "   Copyright (c) 2012 Adafruit Industries\n"  \
    "   Copyright (c) 2020 Bodmer\n"
#define LIC_LOVYAN_EN                              \
    "LovyanGFX 1.2.21 - MIT and BSD\n"             \
    "   Copyright (c) 2020 lovyan03\n"             \
    "   Copyright (c) 2012 Adafruit Industries\n"  \
    "   Copyright (c) 2020 Bodmer\n"
#endif

// ESP-IDF holders shared by both languages (copyright lines are legal text).
// lwIP: SICS holds most files; the further holders of single linked files
// (DHCP, ARP, IGMP, IPv6, SNTP, DNS, the TCP ISN hook) are listed in full in
// THIRD-PARTY-NOTICES.md - the screen names them as "other lwIP contributors".
#define LIC_C_LWIP                                  \
    "   Copyright (c) 2001-2004 Swedish Institute\n" \
    "   of Computer Science and other lwIP\n"        \
    "   contributors\n"
#define LIC_C_WPA                                   \
    "   Copyright (c) Jouni Malinen\n"               \
    "   and contributors\n"                          \
    "   Copyright (c) 2019 The Linux Foundation\n"

// lwIP's PPP code: only the 7B / 5B link it (lwIP's init pulls it in with
// CONFIG_LWIP_PPP_SUPPORT in their ESP-IDF build). Its CMU and Mackerras
// licences ask for the acknowledgments below in redistributions of ANY form,
// and vj.c's for one naming UC Berkeley - so they are quoted verbatim, in
// English in both languages. Full texts: LICENSES/BSD-lwIP-PPP.txt.
#if defined(BOARD_PANEL_1024X600)
#define LIC_PPP_BODY                                     \
    "   Copyright (c) 1984-2000 Carnegie Mellon\n"       \
    "   University\n"                                     \
    "   Copyright (c) 1989 Carnegie Mellon University\n"  \
    "   Copyright (c) 1993-2002 Paul Mackerras\n"        \
    "   Copyright (c) 1994 The Australian National\n"    \
    "   University\n"                                     \
    "   Copyright (c) 1989 Regents of the University\n"  \
    "   of California\n"                                  \
    "   Copyright (c) 2003 by Marc Boucher, Services\n"  \
    "   Informatiques (MBSI) inc.\n"                      \
    "   Copyright (c) 1997, 1998 by Global Election\n"   \
    "   Systems Inc.\n"                                   \
    "   \"This product includes software developed\n"   \
    "   by Computing Services at Carnegie Mellon\n"      \
    "   University (http://www.cmu.edu/computing/).\"\n" \
    "   \"This product includes software developed\n"   \
    "   by Paul Mackerras <paulus@samba.org>\".\n"       \
    "   vj.c was developed by the University of\n"       \
    "   California, Berkeley; parts of ppp.c by\n"       \
    "   Carnegie Mellon University.\n"
#define LIC_PPP_DE "lwIP-PPP-Code - BSD-artige Lizenzen\n" LIC_PPP_BODY
#define LIC_PPP_EN "lwIP PPP code - BSD-style licences\n" LIC_PPP_BODY
#else
#define LIC_PPP_DE ""
#define LIC_PPP_EN ""
#endif

// The only code of esp_system's ubsan.c that every board links is an empty
// stub; its file still carries Linaro's BSD-2-Clause notice.
#define LIC_UBSAN_DE                                 \
    "ubsan-Stub - BSD-2-Clause\n"                     \
    "   Copyright (c) 2016 Linaro Limited\n"
#define LIC_UBSAN_EN                                 \
    "ubsan stub - BSD-2-Clause\n"                     \
    "   Copyright (c) 2016 Linaro Limited\n"

// OpenBridge icon trial: the "Symbole" / "Icons" section credits the OpenBridge
// Icon Pack (CC BY 4.0) under its own heading - it is artwork, not a font, so
// it does not belong under the OFL. It stays in both icon sets: the glyphs are
// compiled in either way. The change statement is the one THIRD-PARTY-NOTICES.md
// and tools/gen_icons.py use; the source link is the Figma Community file,
// broken over two lines because it is longer than one. Each macro carries its
// own trailing blank line, so ending the trial means deleting the two macros
// and the two lines marked below.
#define LIC_ICONS_DE                                       \
    "--- Symbole (CC BY 4.0) -----------------------\n"    \
    "\"OpenBridge Icon Pack\" von OpenBridge\n"            \
    "   CC BY 4.0, creativecommons.org/licenses/by/4.0\n"  \
    "   Quelle: www.figma.com/community/file/\n"           \
    "   1445713209741917748/openbridge-icon-pack\n"        \
    "   Geändert: in 4-bpp-Bitmap-Schriften\n"             \
    "   gerastert und auf eine Farbe reduziert.\n"         \
    "   Teils nach Google Material Icons,\n"               \
    "   Copyright (c) Google LLC, Apache-2.0.\n"           \
    "   Keine Verbindung zu OpenBridge, keine\n"           \
    "   Billigung durch OpenBridge.\n"                     \
    "\n"
#define LIC_ICONS_EN                                       \
    "--- Icons (CC BY 4.0) -------------------------\n"    \
    "\"OpenBridge Icon Pack\" by OpenBridge\n"             \
    "   CC BY 4.0, creativecommons.org/licenses/by/4.0\n"  \
    "   Source: www.figma.com/community/file/\n"           \
    "   1445713209741917748/openbridge-icon-pack\n"        \
    "   Changes: rasterised into 4-bpp bitmap fonts\n"     \
    "   and reduced to one colour.\n"                      \
    "   Partly based on Google Material Icons,\n"          \
    "   Copyright (c) Google LLC, Apache-2.0.\n"           \
    "   Not affiliated with or endorsed by\n"              \
    "   OpenBridge.\n"                                     \
    "\n"

static const char *const LICENSE_TEXT_DE =
"Diese Firmware nutzt Software und Daten Dritter.\n"
"Vollständige Hinweise und Lizenztexte:\n"
"   THIRD-PARTY-NOTICES.md und LICENSES/ -\n"
"   github.com/Ranman86/NauticPinnace\n"
"\n"
"--- Copyleft (LGPL) ---------------------------\n"
"Statisch eingebunden. Wer diese Firmware\n"
"weitergibt, muss Lizenztext, Bibliotheks-\n"
"quellcode und die eigene Anwendung in neu\n"
"linkbarer Form beilegen. Der Quellcode genau\n"
"dieser Versionen hängt als src-*.zip am\n"
"GitHub-Release dieser Firmware.\n"
"\n"
"Arduino-ESP32-Kern " LIC_V_CORE "\n"
"   LGPL-2.1-or-later; die von Espressif\n"
"   selbst geschriebenen Dateien Apache-2.0\n"
"   Copyright (c) Espressif Systems\n"
"   and contributors\n"
"ESPAsyncWebServer " LIC_V_WEBSRV "\n"
"   " LIC_L_ASYNC "\n"
LIC_C_ASYNC
"AsyncTCP " LIC_V_ASYNCTCP "\n"
"   " LIC_L_ASYNC "\n"
LIC_C_ASYNC
"\n"
"--- Freizügige Lizenzen -----------------------\n"
"LVGL 8.4.0 - MIT\n"
"   Copyright (c) 2021 LVGL Kft\n"
"   enthält lv_printf - MIT\n"
"   Copyright (c) 2014-2019 Marco Paland\n"
"ArduinoJson 7.4.3 - MIT\n"
"   Copyright (c) 2014-2026 Benoit Blanchon\n"
"NMEA2000 4.24.1 - MIT\n"
"   Copyright (c) 2015-2025 Timo Lappalainen,\n"
"   Kave Oy, www.kave.fi\n"
"QR-Code-Generator - MIT\n"
"   Copyright (c) Project Nayuki\n"
LIC_LOVYAN_DE
"\n"
"--- ESP-IDF (unter dem Arduino-Kern) ----------\n"
"lwIP - BSD-3-Clause\n"
LIC_C_LWIP
LIC_PPP_DE
"wpa_supplicant - BSD-3-Clause\n"
LIC_C_WPA
"mbed TLS - Apache-2.0\n"
"   Copyright The Mbed TLS Contributors\n"
"FreeRTOS-Kernel - MIT\n"
"   Copyright (c) Amazon.com, Inc. or its\n"
"   affiliates; Espressif-Port Apache-2.0\n"
"littlefs - BSD-3-Clause\n"
"   Copyright (c) The littlefs authors\n"
"   Copyright (c) Arm Limited\n"
"esp_littlefs - MIT\n"
"   Copyright 2020 Brian Pugh\n"
"TLSF (Heap von ESP-IDF, LVGL) - BSD-3-Clause\n"
"   Copyright (c) 2006-2016 Matthew Conte\n"
"cJSON - MIT\n"
"   Copyright (c) 2009-2017 Dave Gamble\n"
"   and cJSON contributors\n"
"Xtensa-HAL und -Port - MIT\n"
"   Copyright (c) Cadence Design Systems, Inc.\n"
"newlib (C-Bibliothek) - COPYING.NEWLIB\n"
"   BSD-artige Lizenzen mehrerer Inhaber, u. a.\n"
"   Copyright (c) Red Hat, Inc.\n"
"   Copyright (c) The Regents of the\n"
"   University of California\n"
LIC_UBSAN_DE
"Espressif-Komponenten, auch die nur binär\n"
"   gelieferten WLAN-/PHY-Bibliotheken\n"
"   (net80211, pp, core, smartconfig, phy,\n"
"   coexist u. a.) - Apache-2.0\n"
"   Copyright (c) Espressif Systems\n"
"\n"
"--- Schriften (SIL OFL 1.1) -------------------\n"
"Montserrat\n"
"   Copyright 2011 The Montserrat Project Authors\n"
"   https://scripts.sil.org/OFL\n"
"   Die hier eingebetteten Bitmap-Schriften sind\n"
"   Formatumwandlungen und damit selbst OFL.\n"
"Font Awesome Free 5.9.0 - SIL OFL 1.1\n"
"   Copyright (c) Font Awesome (Fonticons, Inc.)\n"
"\n"
LIC_ICONS_DE                                            // OpenBridge icon trial
"--- Daten -------------------------------------\n"
"Natural Earth (Küstenlinien der Weltkarte)\n"
"   gemeinfrei\n"
"Bundesamt für Seeschifffahrt und Hydrographie\n"
"   Wasserstandsvorhersage, CC BY 4.0\n"
"   creativecommons.org/licenses/by/4.0\n"
"   Geändert: Höhen auf Seekartennull\n"
"   umgerechnet und gerundet, Pegelnamen\n"
"   ohne Umlaute.\n"
"   Ohne Gewähr. Amtliche Vorhersage des\n"
"   Bundes gemäß Paragraph 1 SeeAufgG.\n"
"canboat, Signal K - PGN-Definitionen,\n"
"   Apache-2.0\n"
"   Übernommen wurden nur Fakten (Bit-Offsets,\n"
"   Skalierungen), kein Quellcode.\n"
"\n"
"--- Hinweis -----------------------------------\n"
"Die NMEA-2000-Norm selbst ist ein kosten-\n"
"pflichtiges Dokument der NMEA. Dieses Gerät\n"
"verwendet sie nicht, sondern stützt sich auf\n"
"die oben genannten öffentlichen Quellen und\n"
"auf Beobachtung des Busverkehrs realer\n"
"Anlagen.\n"
"\n"
"NMEA 2000 ist eine eingetragene Marke der\n"
"National Marine Electronics Association.\n"
"Alle Marken gehören ihren Inhabern.\n";

static const char *const LICENSE_TEXT_EN =
"This firmware uses third-party software and data.\n"
"Full notices and licence texts:\n"
"   THIRD-PARTY-NOTICES.md and LICENSES/ -\n"
"   github.com/Ranman86/NauticPinnace\n"
"\n"
"--- Copyleft (LGPL) ---------------------------\n"
"Statically linked. Anyone distributing this\n"
"firmware must supply the licence text, the\n"
"library source, and their own application in a\n"
"relinkable form. The source of exactly these\n"
"versions is attached as src-*.zip to this\n"
"firmware's GitHub release.\n"
"\n"
"Arduino-ESP32 core " LIC_V_CORE "\n"
"   LGPL-2.1-or-later; the files Espressif\n"
"   wrote itself Apache-2.0\n"
"   Copyright (c) Espressif Systems\n"
"   and contributors\n"
"ESPAsyncWebServer " LIC_V_WEBSRV "\n"
"   " LIC_L_ASYNC "\n"
LIC_C_ASYNC
"AsyncTCP " LIC_V_ASYNCTCP "\n"
"   " LIC_L_ASYNC "\n"
LIC_C_ASYNC
"\n"
"--- Permissive licences -----------------------\n"
"LVGL 8.4.0 - MIT\n"
"   Copyright (c) 2021 LVGL Kft\n"
"   contains lv_printf - MIT\n"
"   Copyright (c) 2014-2019 Marco Paland\n"
"ArduinoJson 7.4.3 - MIT\n"
"   Copyright (c) 2014-2026 Benoit Blanchon\n"
"NMEA2000 4.24.1 - MIT\n"
"   Copyright (c) 2015-2025 Timo Lappalainen,\n"
"   Kave Oy, www.kave.fi\n"
"QR code generator - MIT\n"
"   Copyright (c) Project Nayuki\n"
LIC_LOVYAN_EN
"\n"
"--- ESP-IDF (beneath the Arduino core) --------\n"
"lwIP - BSD-3-Clause\n"
LIC_C_LWIP
LIC_PPP_EN
"wpa_supplicant - BSD-3-Clause\n"
LIC_C_WPA
"mbed TLS - Apache-2.0\n"
"   Copyright The Mbed TLS Contributors\n"
"FreeRTOS kernel - MIT\n"
"   Copyright (c) Amazon.com, Inc. or its\n"
"   affiliates; Espressif port Apache-2.0\n"
"littlefs - BSD-3-Clause\n"
"   Copyright (c) The littlefs authors\n"
"   Copyright (c) Arm Limited\n"
"esp_littlefs - MIT\n"
"   Copyright 2020 Brian Pugh\n"
"TLSF (ESP-IDF and LVGL heap) - BSD-3-Clause\n"
"   Copyright (c) 2006-2016 Matthew Conte\n"
"cJSON - MIT\n"
"   Copyright (c) 2009-2017 Dave Gamble\n"
"   and cJSON contributors\n"
"Xtensa HAL and port - MIT\n"
"   Copyright (c) Cadence Design Systems, Inc.\n"
"newlib (C library) - COPYING.NEWLIB\n"
"   BSD-style licences of several holders, incl.\n"
"   Copyright (c) Red Hat, Inc.\n"
"   Copyright (c) The Regents of the\n"
"   University of California\n"
LIC_UBSAN_EN
"Espressif components, including the binary-only\n"
"   Wi-Fi/PHY libraries\n"
"   (net80211, pp, core, smartconfig, phy,\n"
"   coexist and others) - Apache-2.0\n"
"   Copyright (c) Espressif Systems\n"
"\n"
"--- Fonts (SIL OFL 1.1) -----------------------\n"
"Montserrat\n"
"   Copyright 2011 The Montserrat Project Authors\n"
"   https://scripts.sil.org/OFL\n"
"   The bitmap fonts embedded here are format\n"
"   conversions and are themselves under the OFL.\n"
"Font Awesome Free 5.9.0 - SIL OFL 1.1\n"
"   Copyright (c) Font Awesome (Fonticons, Inc.)\n"
"\n"
LIC_ICONS_EN                                            // OpenBridge icon trial
"--- Data --------------------------------------\n"
"Natural Earth (world map coastlines)\n"
"   public domain\n"
"Bundesamt für Seeschifffahrt und Hydrographie\n"
"   (German Federal Maritime and Hydrographic\n"
"   Agency) water level forecast, CC BY 4.0\n"
"   creativecommons.org/licenses/by/4.0\n"
"   Changes: heights converted to chart datum\n"
"   and rounded, gauge names in plain ASCII.\n"
"   No warranty. Official federal forecast under\n"
"   section 1 SeeAufgG.\n"
"canboat, Signal K - PGN definitions,\n"
"   Apache-2.0\n"
"   Only facts were adopted (bit offsets,\n"
"   scalings), no source code.\n"
"\n"
"--- Note --------------------------------------\n"
"The NMEA 2000 standard itself is a paid document\n"
"of the NMEA. This device does not use it; it\n"
"relies on the public sources named above and\n"
"on observing the bus traffic of real\n"
"installations.\n"
"\n"
"NMEA 2000 is a registered trademark of the\n"
"National Marine Electronics Association.\n"
"All trademarks belong to their owners.\n";

// Licence text in the active UI language.
static inline const char *licenseText() {
    return (i18nLang() == Lang::EN) ? LICENSE_TEXT_EN : LICENSE_TEXT_DE;
}
