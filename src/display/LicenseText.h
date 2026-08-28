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
// copyright holder of every component. That satisfies the notice requirement of
// the permissive licences — and, for the copyleft ones, the rule that a work
// which displays copyright notices while running must list the library's notice
// among them (LGPL-2.1 section 6, LGPL-3.0 section 4c). This screen displays
// notices, so that rule applies: never drop the copyright lines from the LGPL
// block. THIRD-PARTY-NOTICES.md holds the full, unabridged version.
//
// NOTE: the licence NAMES and identifiers (LGPL-2.1-or-later, MIT, SIL OFL 1.1,
// CC BY 4.0, Apache-2.0) and every copyright line are legal text — they are
// byte-identical in both languages and must stay that way. Only the surrounding
// prose is translated.
//
// VERSIONS DIFFER BY BOARD. The 4-inch stays on the Arduino core it shipped
// with; the 1024x600 boards need a newer one for the RGB panel. This screen is
// the copy of the notice that travels with the device, so it has to name what
// THAT device links - it used to state the 4-inch versions on all three. The
// three strings below are concatenated into the text; keep them in step with
// the table in THIRD-PARTY-NOTICES.md, which carries both columns.
// ============================================================

// The 4-inch pins AsyncTCP and ESPAsyncWebServer to specific commits in
// platformio.ini (the newer releases stall large responses on its Arduino
// core); the panel boards take the current registry versions. So all three
// differ, not just the core.
//
// Read them from .pio/libdeps/<env>/, and mind the trap: the 4-inch directory
// holds BOTH a git-pinned "AsyncTCP@src-<hash>" (3.3.2, the one that is
// linked) and a stray registry "AsyncTCP" (3.5.0, the second copy the comment
// in platformio.ini warns about). The plain name is the wrong one to believe.
#if defined(BOARD_PANEL_1024X600)
#define LIC_V_CORE     "3.3.11 (ESP-IDF 5.5.5)"
#define LIC_V_WEBSRV   "3.12.0"
#define LIC_V_ASYNCTCP "3.5.0"
#else
#define LIC_V_CORE     "3.20017 (ESP-IDF 4.4.7)"
#define LIC_V_WEBSRV   "3.6.0"
#define LIC_V_ASYNCTCP "3.3.2"
#endif

static const char *const LICENSE_TEXT_DE =
"Diese Firmware nutzt Software und Daten Dritter.\n"
"Die vollständigen Lizenztexte liegen dem Projekt\n"
"als THIRD-PARTY-NOTICES.md bei.\n"
"\n"
"--- Copyleft (LGPL) ---------------------------\n"
"Statisch eingebunden. Wer diese Firmware\n"
"weitergibt, muss Lizenztext, Bibliotheks-\n"
"quellcode und die eigene Anwendung in neu\n"
"linkbarer Form beilegen.\n"
"\n"
"Arduino-ESP32-Kern " LIC_V_CORE "\n"
"   LGPL-2.1-or-later\n"
"   Copyright (c) Espressif Systems\n"
"   and contributors\n"
"ESPAsyncWebServer " LIC_V_WEBSRV "\n"
"   LGPL-3.0\n"
"   Copyright (c) 2016 Hristo Gochkov,\n"
"   ESP32Async project\n"
"AsyncTCP " LIC_V_ASYNCTCP "\n"
"   LGPL-3.0\n"
"   Copyright (c) 2016 Hristo Gochkov,\n"
"   ESP32Async project\n"
"\n"
"--- Freizügige Lizenzen -----------------------\n"
"LVGL 8.4.0 - MIT\n"
"   Copyright (c) 2021 LVGL Kft\n"
"ArduinoJson 7.4.3 - MIT\n"
"   Copyright (c) 2014-2026 Benoit Blanchon\n"
"NMEA2000 4.24.1 - MIT\n"
"   Copyright (c) 2015-2024 Timo Lappalainen,\n"
"   Kave Oy, www.kave.fi\n"
"QR-Code-Generator - MIT\n"
"   Copyright (c) Project Nayuki\n"
"LovyanGFX 1.2.21 - MIT und BSD\n"
"   Copyright (c) 2012 Adafruit Industries\n"
"   Copyright (c) 2020 Bodmer\n"
"   enthält TJpgDec (ChaN), pngle (kikuchan),\n"
"   miniz, efont, IPAex-Schriften\n"
"\n"
"--- Schriften (SIL OFL 1.1) -------------------\n"
"Montserrat\n"
"   Copyright 2011 The Montserrat Project Authors\n"
"   https://scripts.sil.org/OFL\n"
"   Die hier eingebetteten Bitmap-Schriften sind\n"
"   Formatumwandlungen und damit selbst OFL.\n"
"Font Awesome 5 Free\n"
"   Copyright Fonticons, Inc. - SIL OFL 1.1\n"
"\n"
"--- Daten -------------------------------------\n"
"Natural Earth (Küstenlinien der Weltkarte)\n"
"   gemeinfrei\n"
"Bundesamt für Seeschifffahrt und Hydrographie\n"
"   Wasserstandsvorhersage, CC BY 4.0\n"
"   creativecommons.org/licenses/by/4.0\n"
"   Ohne Gewähr. Amtliche Vorhersage des\n"
"   Bundes gemäß Paragraph 1 SeeAufG.\n"
"canboat - PGN-Definitionen, Apache-2.0\n"
"   Übernommen wurden nur Fakten (Bit-Offsets,\n"
"   Skalierungen), kein Quellcode.\n"
"\n"
"--- Hinweis -----------------------------------\n"
"Die NMEA-2000-Norm selbst ist ein kosten-\n"
"pflichtiges Dokument der NMEA. Dieses Gerät\n"
"verwendet sie nicht, sondern stützt sich auf\n"
"die oben genannten öffentlichen Quellen.\n"
"\n"
"Alle Marken gehören ihren Inhabern.\n";

static const char *const LICENSE_TEXT_EN =
"This firmware uses third-party software and data.\n"
"The full licence texts are included with the\n"
"project as THIRD-PARTY-NOTICES.md.\n"
"\n"
"--- Copyleft (LGPL) ---------------------------\n"
"Statically linked. Anyone distributing this\n"
"firmware must supply the licence text, the\n"
"library source, and their own application in a\n"
"relinkable form.\n"
"\n"
"Arduino-ESP32 core " LIC_V_CORE "\n"
"   LGPL-2.1-or-later\n"
"   Copyright (c) Espressif Systems\n"
"   and contributors\n"
"ESPAsyncWebServer " LIC_V_WEBSRV "\n"
"   LGPL-3.0\n"
"   Copyright (c) 2016 Hristo Gochkov,\n"
"   ESP32Async project\n"
"AsyncTCP " LIC_V_ASYNCTCP "\n"
"   LGPL-3.0\n"
"   Copyright (c) 2016 Hristo Gochkov,\n"
"   ESP32Async project\n"
"\n"
"--- Permissive licences -----------------------\n"
"LVGL 8.4.0 - MIT\n"
"   Copyright (c) 2021 LVGL Kft\n"
"ArduinoJson 7.4.3 - MIT\n"
"   Copyright (c) 2014-2026 Benoit Blanchon\n"
"NMEA2000 4.24.1 - MIT\n"
"   Copyright (c) 2015-2024 Timo Lappalainen,\n"
"   Kave Oy, www.kave.fi\n"
"QR code generator - MIT\n"
"   Copyright (c) Project Nayuki\n"
"LovyanGFX 1.2.21 - MIT and BSD\n"
"   Copyright (c) 2012 Adafruit Industries\n"
"   Copyright (c) 2020 Bodmer\n"
"   contains TJpgDec (ChaN), pngle (kikuchan),\n"
"   miniz, efont, IPAex fonts\n"
"\n"
"--- Fonts (SIL OFL 1.1) -----------------------\n"
"Montserrat\n"
"   Copyright 2011 The Montserrat Project Authors\n"
"   https://scripts.sil.org/OFL\n"
"   The bitmap fonts embedded here are format\n"
"   conversions and are themselves under the OFL.\n"
"Font Awesome 5 Free\n"
"   Copyright Fonticons, Inc. - SIL OFL 1.1\n"
"\n"
"--- Data --------------------------------------\n"
"Natural Earth (world map coastlines)\n"
"   public domain\n"
"Bundesamt fuer Seeschifffahrt und Hydrographie\n"
"   (German Federal Maritime and Hydrographic\n"
"   Agency) water level forecast, CC BY 4.0\n"
"   creativecommons.org/licenses/by/4.0\n"
"   No warranty. Official federal forecast under\n"
"   section 1 SeeAufG.\n"
"canboat - PGN definitions, Apache-2.0\n"
"   Only facts were adopted (bit offsets,\n"
"   scalings), no source code.\n"
"\n"
"--- Note --------------------------------------\n"
"The NMEA 2000 standard itself is a paid document\n"
"of the NMEA. This device does not use it; it\n"
"relies on the public sources named above.\n"
"\n"
"All trademarks belong to their owners.\n";

// Licence text in the active UI language.
static inline const char *licenseText() {
    return (i18nLang() == Lang::EN) ? LICENSE_TEXT_EN : LICENSE_TEXT_DE;
}
