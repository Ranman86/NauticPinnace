#!/usr/bin/env python3
"""Build the browser-flasher payload under docs/flash/ for ALL THREE boards.

docs/flash/ is served by GitHub Pages and is the route most people take: no
toolchain, no cable driver hunt, click and wait. It used to carry the 4-inch
board alone, so owners of a 7-inch or 5-inch panel had no way in at all.

This script is the ONLY thing that writes docs/flash/; tools/make_release.py
builds the release ZIP for GitHub Releases - the same three boards, through
build() and BOARDS here - and leaves this directory alone.

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

Order of work, so that a failure never leaves a half-written docs/flash/:
  1. data/ is checked (tools/release_checks.py) before anything is built
  2. all three boards are built (unless --skip-build)
  3. every input of every board is checked - account name or home paths,
     brand names, LittleFS image == data/, freshness, FW_VERSION
  4. the payload is assembled in release/.flash-staging/, checked once more,
     and only then swapped in for docs/flash/
--skip-build therefore only works when the existing build output passes 3.

Usage (from the repo root):
    python tools/gen_web_flasher.py [--version v1.2.0] [--skip-build]
--version defaults to FW_VERSION from src/Version.h and must equal it.
"""
import argparse
import filecmp
import glob
import io
import json
import os
import shutil
import subprocess
import sys

import release_checks

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, 'data')
OUT = os.path.join(ROOT, 'docs', 'flash')
STAGE = os.path.join(ROOT, 'release', '.flash-staging')
OLD = os.path.join(ROOT, 'release', '.flash-previous')

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
# Removed before a build so the build has to write them anew: SCons skips a
# relink when no object changed, and an old file would then fail (or, worse,
# fake) the freshness check.
REBUILT = ('firmware.bin', 'littlefs.bin', 'partitions.bin')

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


# =============================================================================
# BUILD-PATH NEUTRALISATION HOOK
#
# Extra environment variables for the PlatformIO run of one board, merged OVER
# the defaults that build_env() sets (PLATFORMIO_BUILD_DIR and
# PLATFORMIO_PACKAGES_DIR); a value of None removes a variable. If the result
# moves PLATFORMIO_BUILD_DIR, PLATFORMIO_PACKAGES_DIR or PLATFORMIO_CORE_DIR,
# the images and boot_app0.bin are looked up at the new place.
#
# Purpose: the 7B/5B hybrid build cannot use extra_script.py's prefix maps,
# so their firmware embeds the package paths as they are - with the account
# name in them, ~190 times (assert and log __FILE__ strings of ESP-IDF and the
# Arduino core). Their release builds therefore run in a SEPARATE PlatformIO
# core at a neutral, space-free path: the paths baked in then read
# C:\pio\packages\..., which says nothing about the build machine.
#
# Why a real directory and not a junction or subst drive: pioarduino resolves
# the framework paths with Path.resolve() (espidf.py) and ESP-IDF's CMake with
# REALPATH, both of which follow a junction straight back into the profile.
#
# The first build there installs the platform and packages (~4 GB) and
# recompiles the Arduino IDF libraries (~30 min); on a fresh core that first
# run can stop at the inner Arduino pass with "No module named
# SCons.Tool.FortranCommon" (pioarduino swaps its SCons package mid-run) -
# simply run again, the compiled libraries are kept. The 4-inch keeps its own
# path: its prefix maps work. Override the location with NP_NEUTRAL_CORE.
# Verified 2026-10-03: 7B firmware.bin built this way has 0 hits.
# =============================================================================
NEUTRAL_CORE = os.environ.get('NP_NEUTRAL_CORE') or (
    'C:\\pio' if os.name == 'nt' else '/opt/nauticpinnace-pio')


def neutral_build_env(board):
    if board['key'] == '4':
        return {}
    return {
        'PLATFORMIO_CORE_DIR': NEUTRAL_CORE,
        'PLATFORMIO_BUILD_DIR': os.path.join(NEUTRAL_CORE, 'build', 'nauticpinnace'),
        'PLATFORMIO_PACKAGES_DIR': None,
    }


def fw_version():
    """The single definition, out of src/Version.h."""
    try:
        return 'v' + release_checks.fw_version(ROOT)
    except (OSError, ValueError) as e:
        raise SystemExit('src/Version.h: %s' % e)


def run(args, env=None):
    print('  >', ' '.join(args))
    e = dict(os.environ)
    if env:
        e.update({k: v for k, v in env.items() if v is not None})
        for k, v in env.items():
            if v is None:
                e.pop(k, None)
    subprocess.run(args, check=True, cwd=ROOT, env=e)


def build_env(board):
    """Environment for this board's PlatformIO run: defaults, then the
    neutralisation hook on top."""
    envvars = {
        'PLATFORMIO_BUILD_DIR': board['build'],
        'PLATFORMIO_PACKAGES_DIR': board['pkgs'],
    }
    envvars.update(neutral_build_env(board))
    return envvars


def board_paths(board, envvars):
    """(build output dir, package dirs to search) for the given environment."""
    core = envvars.get('PLATFORMIO_CORE_DIR') or PIO_CORE
    build_root = (envvars.get('PLATFORMIO_BUILD_DIR')
                  or os.path.join(core, 'build', 'nauticpinnace'))
    pkg_roots = [envvars.get('PLATFORMIO_PACKAGES_DIR'),
                 os.path.join(core, 'packages'),
                 os.path.join(PIO_CORE, 'packages')]
    return (os.path.join(build_root, board['env']),
            [r for i, r in enumerate(pkg_roots) if r and r not in pkg_roots[:i]])


def find_boot_app0(pkg_roots):
    """boot_app0.bin lives in the Arduino framework package, not in the build."""
    for r in pkg_roots:
        hits = glob.glob(os.path.join(r, 'framework-arduinoespressif32*',
                                      'tools', 'partitions', 'boot_app0.bin'))
        if hits:
            return hits[0]
    raise SystemExit('boot_app0.bin not found - build at least once first')


def build(board, skip):
    """Build one board (unless skip); return {image name: path}."""
    envvars = build_env(board)
    bdir, pkg_roots = board_paths(board, envvars)
    if not skip:
        for fn in REBUILT:
            p = os.path.join(bdir, fn)
            if os.path.exists(p):
                os.remove(p)
        run([PIO, 'run', '-e', board['env']], envvars)
        run([PIO, 'run', '-t', 'buildfs', '-e', board['env']], envvars)
    images = {fn: os.path.join(bdir, fn) for _, fn in PARTS if fn != 'boot_app0.bin'}
    images['boot_app0.bin'] = find_boot_app0(pkg_roots)
    return images


def tree_differences(src, dst):
    """Files that differ between two trees, recursively (names and bytes)."""
    diffs = []
    cmp = filecmp.dircmp(src, dst)
    diffs += [os.path.join(src, n) for n in cmp.left_only + cmp.right_only]
    for n in cmp.common_files:
        if not filecmp.cmp(os.path.join(src, n), os.path.join(dst, n), shallow=False):
            diffs.append(os.path.join(src, n))
    for d in cmp.common_dirs:
        diffs += tree_differences(os.path.join(src, d), os.path.join(dst, d))
    return diffs


def swap_in(stage, out):
    """Replace out with stage: the old tree is moved aside first and put back
    if the move fails, so docs/flash/ is never left half-written."""
    shutil.rmtree(OLD, ignore_errors=True)
    had_old = os.path.isdir(out)
    if had_old:
        shutil.move(out, OLD)
    try:
        shutil.move(stage, out)
    except Exception:
        if had_old and not os.path.exists(out):
            shutil.move(OLD, out)
        raise
    shutil.rmtree(OLD, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default=None,
                    help='default: FW_VERSION from src/Version.h (must match it)')
    ap.add_argument('--skip-build', action='store_true',
                    help='use what is already built; refused unless every '
                         'release check passes')
    a = ap.parse_args()
    fw = fw_version()[1:]                  # 'v1.2.0' -> '1.2.0'
    version = a.version or 'v' + fw
    # At once, not only in step 3: a typo in --version would otherwise cost
    # three full board builds before it is refused (step 3 compares the same
    # way, through release_checks.check_version).
    if (version[1:] if version[:1] in ('v', 'V') else version) != fw:
        sys.exit('--version %s does not match FW_VERSION "%s" in src/Version.h '
                 '- set it there first. Nothing was built and docs/flash/ was '
                 'not changed.' % (version, fw))

    # 1. data/ is packed whole into every littlefs.bin: check it first. The
    #    .gz sync waits until after the build, which is what regenerates it.
    if not release_checks.report(
            release_checks.check_data_dir(DATA, gzip_sync=False), 'data/'):
        sys.exit(1)

    # 2. build (or locate) every board's images
    built = []
    for b in BOARDS:
        print('\n== %s (%s) ==' % (b['label'], b['env']))
        built.append((b, build(b, a.skip_build)))

    # 3. validate every input BEFORE docs/flash/ is touched
    problems = release_checks.check_data_dir(DATA)
    for b, images in built:
        problems += release_checks.check_build_output(images, version, DATA)
    if not release_checks.report(problems, 'build output of all boards'):
        if a.skip_build:
            print('\n--skip-build refused: the existing build output fails the '
                  'checks above. Run without --skip-build.')
        print('docs/flash/ was not changed.')
        sys.exit(1)

    # 4. assemble in a staging directory (release/ is git-ignored)
    shutil.rmtree(STAGE, ignore_errors=True)
    os.makedirs(STAGE)

    # Licences once, at the top, next to every board directory. LICENSES/ is
    # copied as a whole tree, so a newly added licence text travels too.
    for f in ('LICENSE', 'THIRD-PARTY-NOTICES.md'):
        shutil.copy(os.path.join(ROOT, f), STAGE)
    shutil.copytree(os.path.join(ROOT, 'LICENSES'), os.path.join(STAGE, 'LICENSES'))
    diffs = tree_differences(os.path.join(ROOT, 'LICENSES'),
                             os.path.join(STAGE, 'LICENSES'))
    if diffs:
        shutil.rmtree(STAGE, ignore_errors=True)
        raise SystemExit('LICENSES/ copy incomplete: %s' % ', '.join(
            release_checks.shown(d) for d in diffs))

    for b, images in built:
        dest = os.path.join(STAGE, b['key'])
        os.makedirs(dest)
        for _, fn in PARTS:
            shutil.copy(images[fn], os.path.join(dest, fn))

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

    if not release_checks.report(release_checks.check_tree(STAGE),
                                 'assembled docs/flash payload'):
        shutil.rmtree(STAGE, ignore_errors=True)
        print('docs/flash/ was not changed.')
        sys.exit(1)

    swap_in(STAGE, OUT)

    print('\nWeb flasher payload written for %d boards, version %s'
          % (len(BOARDS), version))
    print('Remember: docs/flash/THIRD-PARTY-NOTICES.md is a SNAPSHOT that '
          'travels with these binaries - it was refreshed just now.')
    print('Commit docs/flash/ with "git add -A docs/flash" so removed files '
          'are recorded too.')
    print('\nTo try the page, SERVE it - opening the file gives "Failed to')
    print('download manifest", because fetch() and WebSerial both refuse a')
    print('file:// origin:')
    print('    cd docs && python -m http.server 8765')
    print('    http://127.0.0.1:8765/flash.html')


if __name__ == '__main__':
    main()
