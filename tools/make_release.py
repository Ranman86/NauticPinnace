#!/usr/bin/env python3
"""Build a flash-it-yourself release package for NauticPinnace.

Produces release/NauticPinnace-<version>/ with:
  - the five flash images (bootloader, partition table, boot_app0, firmware,
    LittleFS) plus a single merged full-flash image
  - flash.bat (Windows, zero-install: bundles esptool.exe if available) and
    flash.sh (Linux/macOS, uses pip-installed esptool)
  - manifest.json for ESP Web Tools (browser flashing)
  - FLASHING.md instructions, LICENSE, THIRD-PARTY-NOTICES.md, LICENSES/
and zips the folder. Also refreshes docs/flash/ (web flasher payload).

Usage (from the repo root):
  python tools/make_release.py --version v1.0.0 [--skip-build]

Licence note: distributing the built firmware triggers the LGPL relink
obligations documented in THIRD-PARTY-NOTICES.md. They are satisfied because
the complete application source is public (relinking = rebuilding from
source) and the notices + licence texts ship inside this package.
esptool.exe (GPL-2.0-or-later) is an unmodified standalone tool distributed
alongside, not linked. Because the binary is redistributed, its complete
source is fetched into the package too (GPLv2 section 3a — a link would not
be enough), together with the upstream notices.
"""
import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = 'waveshare_esp32s3_4'

# PlatformIO's own directory. Everything below derives from it rather than
# being written out, so this agrees with platformio.ini, build_deploy.ps1 and
# tools/build4.ps1 - all four used to compute the 4-inch paths separately and
# had already drifted apart.
PIO_CORE = os.environ.get('PLATFORMIO_CORE_DIR') or os.path.expanduser('~/.platformio')
PIO = os.path.join(PIO_CORE, 'penv', 'Scripts', 'pio.exe')
if not os.path.exists(PIO):
    PIO = 'pio'  # PATH fallback (Linux/macOS, pipx installs)

# The 4" env builds with a PRIVATE package dir (see tools/build4.ps1 and the
# comment in platformio.ini): the pioarduino platform of the 7B env fights
# over the shared package dir by NAME (tool-esptoolpy). Keep every package
# path in this script consistent with that dir - and the BUILD dir with it,
# because changing the package dir makes PlatformIO treat the project as a
# different configuration and build somewhere else.
PKGS = os.path.join(PIO_CORE, 'packages-4inch')
BUILD = os.path.join(PIO_CORE, 'build', 'nauticpinnace-4inch', ENV)
os.environ['PLATFORMIO_PACKAGES_DIR'] = PKGS
os.environ['PLATFORMIO_BUILD_DIR'] = os.path.join(PIO_CORE, 'build',
                                                  'nauticpinnace-4inch')

# flash layout — keep in sync with partitions_16MB.csv
PARTS = [
    ('0x0',      'bootloader.bin'),
    ('0x8000',   'partitions.bin'),
    ('0xe000',   'boot_app0.bin'),
    ('0x10000',  'firmware.bin'),
    ('0xA10000', 'littlefs.bin'),
]

ESPTOOL_VERSION = '4.8.1'
ESPTOOL_WIN_URL = ('https://github.com/espressif/esptool/releases/download/'
                   'v4.8.1/esptool-v4.8.1-win64.zip')
ESPTOOL_SRC_URL = ('https://github.com/espressif/esptool/archive/refs/tags/'
                   'v4.8.1.zip')


def run(args, **kw):
    print('  >', ' '.join(args))
    subprocess.run(args, check=True, cwd=ROOT, **kw)


def find_boot_app0():
    import glob
    # Private package dir first (the framework may sit in a "@version" or
    # "@src-<hash>" suffixed dir there); shared dir as legacy fallback.
    for pattern in (
        os.path.join(PKGS, 'framework-arduinoespressif32*',
                     'tools', 'partitions', 'boot_app0.bin'),
        os.path.expanduser('~/.platformio/packages/'
                           'framework-arduinoespressif32*/tools/'
                           'partitions/boot_app0.bin'),
    ):
        hits = glob.glob(pattern)
        if hits:
            return hits[0]
    sys.exit('boot_app0.bin not found — build the project once via tools/build4.ps1 first')


def fetch_esptool_exe(dest_dir):
    """Bundle the standalone Windows esptool so flash.bat needs zero installs.

    esptool is GPL-2.0-or-later. Shipping the binary therefore also means
    shipping its licence AND its complete source (GPLv2 section 3a) — a link
    would not be enough, so the source archive goes into the package next to
    the exe, in esptool-%s/.
    """ % ESPTOOL_VERSION
    exe = os.path.join(dest_dir, 'esptool.exe')
    sub = os.path.join(dest_dir, 'esptool-' + ESPTOOL_VERSION)
    if os.path.exists(exe) and os.path.isdir(sub):
        return True
    try:
        os.makedirs(sub, exist_ok=True)
        print('  downloading standalone esptool (win64) ...')
        zpath = os.path.join(dest_dir, '_esptool.zip')
        urllib.request.urlretrieve(ESPTOOL_WIN_URL, zpath)
        with zipfile.ZipFile(zpath) as z:
            for n in z.namelist():
                base = os.path.basename(n)
                if base == 'esptool.exe':
                    with z.open(n) as s, open(exe, 'wb') as out:
                        shutil.copyfileobj(s, out)
                # keep the upstream notices instead of stripping them
                elif base.upper().startswith(('LICENSE', 'NOTICE', 'README')):
                    with z.open(n) as s, open(os.path.join(sub, base), 'wb') as out:
                        shutil.copyfileobj(s, out)
        os.remove(zpath)

        print('  downloading esptool source (GPLv2 section 3a) ...')
        spath = os.path.join(sub, 'esptool-%s-source.zip' % ESPTOOL_VERSION)
        urllib.request.urlretrieve(ESPTOOL_SRC_URL, spath)

        io.open(os.path.join(sub, 'README-WHY-THIS-IS-HERE.txt'), 'w',
                encoding='utf-8').write(
            'esptool.exe in the parent directory is an unmodified build of\n'
            'esptool %s (https://github.com/espressif/esptool), licensed\n'
            'GPL-2.0-or-later. It is a separate program that NauticPinnace only\n'
            'invokes — it is not linked into the firmware.\n\n'
            'Because the binary is redistributed here, its complete\n'
            'corresponding source accompanies it:\n'
            '  esptool-%s-source.zip\n'
            'and the licence text is in ../LICENSES/GPL-2.0.txt.\n'
            % (ESPTOOL_VERSION, ESPTOOL_VERSION))
        return os.path.exists(exe)
    except Exception as e:  # offline builds still produce a valid package
        print('  WARNING: could not fetch esptool (%s) — flash.bat will '
              'fall back to "python -m esptool"' % e)
        shutil.rmtree(sub, ignore_errors=True)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='dev')
    ap.add_argument('--skip-build', action='store_true')
    a = ap.parse_args()

    if not a.skip_build:
        print('== building firmware ==')
        run([PIO, 'run', '-e', ENV])
        print('== building LittleFS image ==')
        run([PIO, 'run', '-e', ENV, '-t', 'buildfs'])

    name = 'NauticPinnace-%s' % a.version
    rel = os.path.join(ROOT, 'release', name)
    shutil.rmtree(rel, ignore_errors=True)
    os.makedirs(rel)

    # ---- collect images ----------------------------------------------------
    shutil.copy(find_boot_app0(), os.path.join(rel, 'boot_app0.bin'))
    for _, fn in PARTS:
        if fn == 'boot_app0.bin':
            continue
        src = os.path.join(BUILD, fn)
        if not os.path.exists(src):
            sys.exit('missing %s — build failed?' % src)
        shutil.copy(src, rel)

    # ---- merged single image (flash everything at offset 0x0) --------------
    print('== merging full-flash image ==')
    tail = ['--chip', 'esp32s3', 'merge_bin',
            '-o', os.path.join(rel, 'nauticpinnace-full.bin'),
            '--flash_size', '16MB']
    for off, fn in PARTS:
        tail += [off, os.path.join(rel, fn)]
    try:
        run([sys.executable, '-m', 'esptool'] + tail)
    except Exception:
        # esptool not importable with this interpreter — use PlatformIO's copy
        # (from the 4" env's private package dir, see PKGS above)
        run([os.path.expanduser('~/.platformio/penv/Scripts/python.exe'),
             os.path.join(PKGS, 'tool-esptoolpy', 'esptool.py')]
            + tail)

    # ---- flash scripts ------------------------------------------------------
    have_exe = fetch_esptool_exe(rel)
    flash_args = ' '.join('%s %s' % (off, fn) for off, fn in PARTS)

    # Optional first argument = COM port. Auto-detect picks the first
    # responding port, which on machines with other USB-serial gadgets
    # (audio interfaces, Bluetooth bridges) can be the wrong one — hence the
    # explicit escape hatch, mentioned in the output.
    # NOTE on quoting: this string is NOT %-formatted (flash_args goes in via
    # .replace), so batch variables use a SINGLE % — doubling them would make
    # cmd.exe treat "%~dp0" as literal text. Newlines are plain \n; the file is
    # opened with newline='\r\n' so Python does the CRLF translation once.
    io.open(os.path.join(rel, 'flash.bat'), 'w', newline='\r\n').write(
        '@echo off\n'
        'setlocal\n'
        'rem Work in the folder this script lives in: esptool.exe and the .bin\n'
        'rem files are referenced by bare name, so any other cwd would fail.\n'
        'rem pushd (not cd) copes with the trailing backslash of %~dp0.\n'
        'pushd "%~dp0"\n'
        'echo(\n'
        'echo  NauticPinnace flasher\n'
        'echo  Connect the display via USB-C. Takes about half a minute.\n'
        'echo  Wrong port picked? Run:  flash.bat COM5\n'
        'echo(\n'
        'set PORT=\n'
        'if not "%~1"=="" set PORT=--port %~1\n'
        'rem Tool selection as a SINGLE-LINE if: inside a parenthesised block\n'
        'rem cmd.exe expands %TOOL% at parse time, i.e. before the set runs.\n'
        'rem Full path because some environments drop the cwd from the search path.\n'
        'set TOOL=python -m esptool\n'
        'if exist "%~dp0esptool.exe" set TOOL="%~dp0esptool.exe"\n'
        'echo  Step 1/2: erasing the entire flash...\n'
        'echo(\n'
        '%TOOL% --chip esp32s3 %PORT% --baud 921600 erase_flash\n'
        'if errorlevel 1 goto :failed\n'
        'echo(\n'
        'echo  Step 2/2: writing bootloader, firmware and web UI...\n'
        'echo(\n'
        '%TOOL% --chip esp32s3 %PORT% --baud 921600 write_flash {a}\n'
        'if errorlevel 1 goto :failed\n'
        'echo(\n'
        'echo  Done. The display reboots into the first-run setup.\n'
        'goto :end\n'
        ':failed\n'
        'echo(\n'
        'echo  FAILED. Things to try:\n'
        'echo   - pass the port explicitly:  flash.bat COM5\n'
        'echo   - close any serial monitor using the port\n'
        'echo   - hold BOOT, tap RESET, release BOOT, then run again\n'
        'echo  If the erase already succeeded the display stays blank until a\n'
        'echo  write completes - just run this script again.\n'
        ':end\n'
        'popd\n'
        'pause\n'.replace('{a}', flash_args))

    io.open(os.path.join(rel, 'flash.sh'), 'w', newline='\n').write(
        '#!/bin/sh\n'
        '# NauticPinnace flasher - needs esptool (pip install esptool).\n'
        '# Optional first argument: the serial port, e.g. ./flash.sh /dev/ttyACM0\n'
        '# The .bin files are referenced by bare name -> run from this folder.\n'
        'set -e\n'
        'cd "$(dirname "$0")" || exit 1\n'
        'PORT=""\n'
        '[ -n "$1" ] && PORT="--port $1"\n'
        'ESPTOOL="python3 -m esptool --chip esp32s3 $PORT --baud 921600"\n'
        'echo "Step 1/2: erasing the entire flash..."\n'
        '$ESPTOOL erase_flash\n'
        'echo "Step 2/2: writing bootloader, firmware and web UI..."\n'
        '$ESPTOOL write_flash %s\n'
        'echo "Done. The display reboots into the first-run setup."\n'
        % flash_args)

    # ---- ESP Web Tools manifest --------------------------------------------
    manifest = {
        'name': 'NauticPinnace',
        'version': a.version,
        'new_install_prompt_erase': True,
        'builds': [{
            'chipFamily': 'ESP32-S3',
            'parts': [{'path': fn, 'offset': int(off, 16)} for off, fn in PARTS],
        }],
    }
    json.dump(manifest, io.open(os.path.join(rel, 'manifest.json'), 'w'), indent=1)

    # ---- docs/flash is NOT written here any more ---------------------------
    # It used to be: this script emptied docs/flash/ and dropped the 4-inch
    # images straight into it. That layout only ever fitted one board, and the
    # browser flasher now serves all three from docs/flash/<board>/ - so doing
    # it from here would delete the 7-inch and 5-inch payloads every time a
    # 4-inch release was cut, silently, and the page would 404 for two of its
    # three buttons.
    #
    # tools/gen_web_flasher.py owns that directory now. It builds every board,
    # writes one manifest each, and refreshes the licence copies that travel
    # with the binaries.
    print('\nNOTE: docs/flash/ is built separately, for all three boards:')
    print('      python tools/gen_web_flasher.py')

    # ---- licences + instructions -------------------------------------------
    for f in ('LICENSE', 'THIRD-PARTY-NOTICES.md'):
        shutil.copy(os.path.join(ROOT, f), rel)
    shutil.copytree(os.path.join(ROOT, 'LICENSES'), os.path.join(rel, 'LICENSES'))

    io.open(os.path.join(rel, 'FLASHING.md'), 'w', encoding='utf-8').write(
        '# Flashing NauticPinnace %s\n\n'
        'Hardware: Waveshare ESP32-S3-Touch-LCD-4 **Rev 4**. Connect via USB-C.\n\n'
        '## Windows (zero install)\n'
        'Double-click `flash.bat`. It uses the bundled `esptool.exe`; the port\n'
        'is detected automatically. If the wrong port is picked, pass it:\n'
        '`flash.bat COM5`\n\n'
        '## Linux / macOS\n'
        '`pip install esptool`, then `sh flash.sh` (optionally\n'
        '`sh flash.sh /dev/ttyACM0`).\n\n'
        '## Browser\n'
        'Chrome/Edge can flash over WebSerial — see the "Flashing" section of\n'
        'the project README for the web flasher link.\n\n'
        '## What the scripts do\n'
        'Two steps, both on the whole 16 MB flash:\n\n'
        '1. `erase_flash` — wipes **everything**, including the NVS area the\n'
        '   ESP-IDF uses for WiFi calibration and cached credentials. This is\n'
        '   what guarantees a genuine factory state rather than "new firmware\n'
        '   on top of old leftovers". A few seconds.\n'
        '2. `write_flash` — writes bootloader, partition table, boot_app0,\n'
        '   firmware and the LittleFS image (web UI, factory config, polar).\n\n'
        'If the erase succeeds but the write is interrupted (cable pulled), the\n'
        'display stays blank — harmless, just run the script again. Should the\n'
        'board then no longer enumerate as a serial port, hold `BOOT`, tap\n'
        '`RESET`, release `BOOT` to force the ROM bootloader (buttons on the\n'
        'left edge), and run it once more.\n\n'
        '## Updating without losing your settings\n'
        'The scripts above always start from scratch. To update the firmware\n'
        'while keeping the on-device configuration, write just the app:\n\n'
        '`esptool --chip esp32s3 --baud 921600 write_flash 0x10000 firmware.bin`\n\n'
        'Export your configuration first anyway (web UI -> Import/Export).\n\n'
        '## Single-image alternative\n'
        '`nauticpinnace-full.bin` is everything merged; flash it at offset 0\n'
        '(erase first for a clean state):\n'
        '`esptool --chip esp32s3 write_flash 0x0 nauticpinnace-full.bin`\n\n'
        '## First boot\n'
        'The panel stays dark for about 8 seconds (USB serial and the onboard\n'
        'IO controller need that long), then the display starts in English, asks\n'
        'for your language and shows the licences.\n\n'
        '**Demo mode is ON by default**, so every screen shows animated data\n'
        'even with no bus attached — a red banner says so. Turn it off once the\n'
        'display sits on a real NMEA 2000 network (web UI → Display), then\n'
        'restart so it reads the bus.\n\n'
        '**WiFi is OFF by default** (a boat rarely has any). To reach the web\n'
        'interface: tap the gear icon at the top of the screen — it fades out,\n'
        'so tap once to reveal it and again to open — then switch WiFi on, or\n'
        'press "Hotspot & restart". The hotspot name, its randomly generated\n'
        'password and a QR code for joining are shown right there; the web UI is\n'
        'then at http://192.168.4.1. Joining your own network instead shows the\n'
        'address in the same place.\n\n'
        '## Licences\n'
        'This package contains the built firmware; see THIRD-PARTY-NOTICES.md\n'
        'and LICENSES/ (LGPL components: relink by rebuilding from the public\n'
        'source repository).\n\n'
        '`esptool.exe` is an unmodified build of esptool (GPL-2.0-or-later), a\n'
        'separate program that the flash script invokes — it is not part of the\n'
        'firmware. Its complete source and upstream notices sit next to it in\n'
        '`esptool-%s/`, the licence text in `LICENSES/GPL-2.0.txt`.\n'
        % (a.version, ESPTOOL_VERSION))

    # ---- zip ----------------------------------------------------------------
    zpath = os.path.join(ROOT, 'release', name + '.zip')
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(rel):
            for f in files:
                p = os.path.join(base, f)
                z.write(p, os.path.join(name, os.path.relpath(p, rel)))
    sha = hashlib.sha256(open(zpath, 'rb').read()).hexdigest()
    print('\nRelease: %s\nSHA256:  %s\nSize:    %.1f MB'
          % (zpath, sha, os.path.getsize(zpath) / 1e6))


if __name__ == '__main__':
    main()
