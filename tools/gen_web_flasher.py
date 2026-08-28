#!/usr/bin/env python3
"""Build the browser-flasher payload under docs/flash/ for ALL THREE boards.

docs/flash/ is served by GitHub Pages and is the route most people take: no
toolchain, no cable driver hunt, click and wait. It used to carry the 4-inch
board alone, so owners of a 7-inch or 5-inch panel had no way in at all.

Layout produced here:

    docs/flash/
        LICENSE, THIRD-PARTY-NOTICES.md, LICENSES/   once, shared
        4/    manifest.json + the five images
        7b/   manifest.json + the five images
        5b/   manifest.json + the five images

The licences sit at the top because this directory is a distribution channel
in its own right - GitHub Pages hands the firmware to anyone who clicks - and
the LGPL wants the licence text to travel WITH the binary. They are identical
for all three boards, so one copy is honest and three would only rot apart.

THE BOARDS CANNOT BE TOLD APART OVER USB. All three are ESP32-S3, so ESP Web
Tools sees one chip family and cannot choose for the user; flash.html has to
ask, and does. Picking wrong writes a firmware whose panel driver does not
match: the screen stays dark. It is not fatal - flashing the right one fixes
it - but the page says so plainly.

Usage (from the repo root):
    python tools/gen_web_flasher.py [--version v1.1.0] [--skip-build]
"""
import argparse
import glob
import io
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PIO_CORE = os.environ.get('PLATFORMIO_CORE_DIR') or os.path.expanduser('~/.platformio')
PIO = os.path.join(PIO_CORE, 'penv', 'Scripts', 'pio.exe')
if not os.path.exists(PIO):
    PIO = 'pio'

# Flash layout - the same partition table for every board (see
# partitions_16MB.csv), so one offset list covers all three.
PARTS = [
    ('0x0',      'bootloader.bin'),
    ('0x8000',   'partitions.bin'),
    ('0xe000',   'boot_app0.bin'),
    ('0x10000',  'firmware.bin'),
    ('0xA10000', 'littlefs.bin'),
]

# The 4-inch builds with a PRIVATE package directory: the two ESP32 platforms
# resolve tool-esptoolpy by NAME in a shared one and evict each other. Its
# build directory is separate for the same reason. Keep this in step with
# tools/build4.ps1 and build_deploy.ps1, which compute the identical paths.
BOARDS = [
    {'key': '4',  'env': 'waveshare_esp32s3_4',
     'label': '4-inch  (480x480)',
     'build': os.path.join(PIO_CORE, 'build', 'nauticpinnace-4inch'),
     'pkgs':  os.path.join(PIO_CORE, 'packages-4inch')},
    {'key': '7b', 'env': 'waveshare_esp32s3_7b',
     'label': '7-inch  (1024x600)',
     'build': os.path.join(PIO_CORE, 'build', 'nauticpinnace'),
     'pkgs':  None},
    {'key': '5b', 'env': 'waveshare_esp32s3_5b',
     'label': '5-inch  (1024x600)',
     'build': os.path.join(PIO_CORE, 'build', 'nauticpinnace'),
     'pkgs':  None},
]


def fw_version():
    """The single definition, out of src/Version.h."""
    src = io.open(os.path.join(ROOT, 'src', 'Version.h'), encoding='utf-8').read()
    m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', src)
    if not m:
        raise SystemExit('FW_VERSION not found in src/Version.h')
    return 'v' + m.group(1)


def run(args, env=None):
    print('  >', ' '.join(args))
    e = dict(os.environ)
    if env:
        e.update({k: v for k, v in env.items() if v is not None})
        for k, v in env.items():
            if v is None:
                e.pop(k, None)
    subprocess.run(args, check=True, cwd=ROOT, env=e)


def find_boot_app0(pkgs):
    """boot_app0.bin lives in the Arduino framework package, not in the build."""
    roots = [pkgs] if pkgs else []
    roots.append(os.path.join(PIO_CORE, 'packages'))
    for r in roots:
        if not r:
            continue
        hits = glob.glob(os.path.join(r, 'framework-arduinoespressif32*',
                                      'tools', 'partitions', 'boot_app0.bin'))
        if hits:
            return hits[0]
    raise SystemExit('boot_app0.bin not found - build at least once first')


def build(board, skip):
    envvars = {
        'PLATFORMIO_BUILD_DIR': board['build'],
        'PLATFORMIO_PACKAGES_DIR': board['pkgs'],
    }
    if not skip:
        run([PIO, 'run', '-e', board['env']], envvars)
        run([PIO, 'run', '-t', 'buildfs', '-e', board['env']], envvars)
    return os.path.join(board['build'], board['env'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default=None,
                    help='default: FW_VERSION from src/Version.h')
    ap.add_argument('--skip-build', action='store_true',
                    help='use whatever is already built')
    a = ap.parse_args()
    version = a.version or fw_version()

    out = os.path.join(ROOT, 'docs', 'flash')
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)

    # Licences once, at the top, next to every board directory.
    for f in ('LICENSE', 'THIRD-PARTY-NOTICES.md'):
        shutil.copy(os.path.join(ROOT, f), out)
    shutil.copytree(os.path.join(ROOT, 'LICENSES'), os.path.join(out, 'LICENSES'))

    for b in BOARDS:
        print('\n== %s (%s) ==' % (b['label'], b['env']))
        bdir = build(b, a.skip_build)
        dest = os.path.join(out, b['key'])
        os.makedirs(dest)

        for _, fn in PARTS:
            if fn == 'boot_app0.bin':
                shutil.copy(find_boot_app0(b['pkgs']), os.path.join(dest, fn))
                continue
            src = os.path.join(bdir, fn)
            if not os.path.exists(src):
                raise SystemExit('missing %s - did the build for %s run?' % (src, b['env']))
            shutil.copy(src, os.path.join(dest, fn))

        manifest = {
            'name': 'NauticPinnace %s' % b['label'].split('(')[0].strip(),
            'version': version,
            'new_install_prompt_erase': True,
            'builds': [{
                'chipFamily': 'ESP32-S3',
                'parts': [{'path': fn, 'offset': int(off, 16)} for off, fn in PARTS],
            }],
        }
        json.dump(manifest, io.open(os.path.join(dest, 'manifest.json'), 'w'), indent=1)
        total = sum(os.path.getsize(os.path.join(dest, fn)) for _, fn in PARTS)
        print('   -> docs/flash/%s  (%.1f MB)' % (b['key'], total / 1048576.0))

    print('\nWeb flasher payload written for %d boards, version %s'
          % (len(BOARDS), version))
    print('Remember: docs/flash/THIRD-PARTY-NOTICES.md is a SNAPSHOT that '
          'travels with these binaries - it was refreshed just now.')
    print('\nTo try the page, SERVE it - opening the file gives "Failed to')
    print('download manifest", because fetch() and WebSerial both refuse a')
    print('file:// origin:')
    print('    cd docs && python -m http.server 8765')
    print('    http://127.0.0.1:8765/flash.html')


if __name__ == '__main__':
    main()
