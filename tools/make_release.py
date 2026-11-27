#!/usr/bin/env python3
"""Build the flash-it-yourself release package for ALL THREE boards.

Produces under release/:
  NauticPinnace-<version>/  and  NauticPinnace-<version>.zip of it
    4/  7b/  5b/   one folder per board, each holding the five flash images
                   (bootloader, partition table, boot_app0, firmware,
                   LittleFS), a merged full-flash image nauticpinnace-full.bin,
                   manifest.json for ESP Web Tools (browser flashing) and
                   index.html.gz (the web interface on its own, for the WiFi
                   and NMEA 2000 updates)
    flash.bat      Windows: flash.bat [4|7b|5b] [COMx] - without a board it
                   lists the three and asks. It runs Espressif's own
                   esptool.exe, which the user downloads (ESPTOOL_WIN_URL)
                   and puts next to it, and refuses one whose SHA-256 is not
                   ESPTOOL_EXE_SHA256
    flash.sh       Linux/macOS (pip-installed esptool):
                   sh flash.sh <4|7b|5b> [port] - without a board it asks
    FLASHING.md, LICENSE, THIRD-PARTY-NOTICES.md, LICENSES/   once, at the top
    No esptool: the package carries none since 1.3.0 (see the licence note)
  NauticPinnace-<version>-sources/
    the seven src-*.zip archives: the corresponding source of the LGPL libraries
    in exactly the versions the 4-inch AND the 7-inch/5-inch firmware link

THE BOARDS CANNOT BE TOLD APART OVER USB. All three are ESP32-S3, so neither
esptool nor the scripts can see which one is connected, and the wrong
firmware leaves the screen dark without any warning (its panel driver does
not match). The scripts therefore never guess: the board is their first
argument, or they list the three and ask.

The board list, the build directories and the flash layout come from
tools/gen_web_flasher.py (BOARDS, PARTS, build()), so the ZIP and the web
flasher cannot drift apart: the 4-inch builds in the normal PlatformIO core
with its private package directory, the 7B and 5B in the neutral core
(C:\\pio, see neutral_build_env() there), one board after the other.

docs/flash/ (the GitHub Pages web flasher) is NOT touched here. A release is
two runs:
  python tools/make_release.py --version vX.Y.Z    (this: release ZIP)
  python tools/gen_web_flasher.py --skip-build     (docs/flash)
Both build all three boards into the same directories, so the second run may
package what the first has just built; the release gate below still refuses
any image that is older than the sources.

Nothing is uploaded. The script ends with the list of files to attach to the
GitHub Release: the ZIP and the seven source archives.

Usage (from the repo root):
  python tools/make_release.py [--version vX.Y.Z] [--skip-build]
--version defaults to FW_VERSION from src/Version.h and must equal it.

Release gate: tools/release_checks.py runs over data/ before the build and
over every image of every board before anything is written to release/
(account name or home paths, brand names, the data/ allow-list and blank WiFi
credentials, LittleFS image == data/, freshness against the sources,
FW_VERSION in firmware.bin), and once more over the assembled folder before it
is zipped. --skip-build packages the existing build output and is refused
whenever those checks fail, so a stale image can no longer be released under a
new version number.

Licence note: firmware.bin links LGPL libraries statically (Arduino-ESP32
core, AsyncTCP, ESPAsyncWebServer - different versions on the 4-inch and on
the 7B/5B). The package carries their notices and licence texts; their
corresponding source goes to the same GitHub Release as the seven src-*.zip
archives (FLASHING.md lists them with the upstream URLs), and the application
source needed to relink is this repository at the release tag. Every
download (those source archives) is cached in release/.cache/, so a re-run
is offline-capable.

esptool is NOT redistributed. Up to 1.2.0 the package carried esptool.exe
(GPL-2.0-or-later, a PyInstaller bundle) with esptool's source, but the exe
also contains GPL flasher stubs, a Python runtime, OpenSSL and more whose
complete source never travelled with it. Since 1.3.0 flash.bat expects the
user's own copy from Espressif's release instead and checks its SHA-256;
the web flasher remains the main route.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_web_flasher as gwf   # noqa: E402  (BOARDS, PARTS, build())
import release_checks           # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, 'data')
CACHE = os.path.join(ROOT, 'release', '.cache')

# Flash layout, shared with the web flasher (one partition table for all
# three boards, partitions_16MB.csv).
PARTS = gwf.PARTS

# What the flash scripts and FLASHING.md tell the user about each board,
# keyed like gen_web_flasher.BOARDS (whose 'key' is also the folder name in
# the package). baud = the board's upload_speed in platformio.ini: the 5B's
# native USB-Serial/JTAG ignores the rate, but esptool's stub plus a baud
# change has stalled there at 921600.
BOARD_INFO = {
    '4':  {'name': '4-inch', 'model': 'ESP32-S3-Touch-LCD-4 Rev 4',
           'res': '480 x 480', 'baud': 921600},
    '7b': {'name': '7-inch', 'model': 'ESP32-S3-Touch-LCD-7B',
           'res': '1024 x 600', 'baud': 921600},
    '5b': {'name': '5-inch', 'model': 'ESP32-S3-Touch-LCD-5B',
           'res': '1024 x 600', 'baud': 460800},
}
if sorted(BOARD_INFO) != sorted(b['key'] for b in gwf.BOARDS):
    sys.exit('make_release.py: BOARD_INFO does not match '
             'gen_web_flasher.BOARDS - update both together')

# The esptool that flash.bat runs: Espressif's standalone Windows build,
# which the user downloads from the official release. Nothing of it goes into
# the package. The release publishes no digest, so the SHA-256 of the
# esptool.exe inside that ZIP is pinned here, as it shipped unchanged in the
# packages v1.0.0, v1.1.0 and v1.2.0 (6,760,840 bytes, Authenticode-signed by
# Espressif; the ZIP itself, 33,608,517 bytes, had sha256 2483d409e241d882
# 6ae0ff023eecf31a7d4de6c10ca5ee855b1420cdfd53aaf6). flash.bat refuses any
# other exe. Change version, URL and hash only together.
ESPTOOL_VERSION = '4.8.1'
ESPTOOL_WIN_URL = ('https://github.com/espressif/esptool/releases/download/'
                   'v4.8.1/esptool-v4.8.1-win64.zip')
# The folder Espressif's ZIP unpacks to; flash.bat also looks in there.
ESPTOOL_WIN_DIR = 'esptool-win64'
ESPTOOL_EXE_SHA256 = ('069db0f1e4d8f929f36d0f60243499eb'
                      '670626c03a686376728998aaaad2f664')

REPO_URL = 'https://github.com/Ranman86/NauticPinnace'
WEB_FLASHER_URL = 'https://ranman86.github.io/NauticPinnace/flash.html'
CH343_DRIVER_URL = 'https://www.wch-ic.com/downloads/CH343SER_EXE.html'

# Corresponding source of the LGPL libraries, one archive per version that any
# published firmware links. Asset name, GitHub repo, ref (tag or full commit),
# what it is, licence, which boards. The release ZIP carries all three boards
# (and the web flasher of the same release serves the same images), so every
# entry here belongs to it.
#
# A repo of the form "pio:<package>" is not on GitHub: the archive is packed
# from the installed PlatformIO package (pack_pio_package()), and ref is the
# package version its .piopm must name. The 4-inch's Arduino core is such a
# case. Up to 1.2.0 the release attached the 2.0.17 tag, but the 4-inch links
# PlatformIO's package framework-arduinoespressif32 3.20017.241212, and that
# is the tag plus backports PlatformIO applied itself: seven files of cores/
# and libraries/ differ (Arduino.h, Esp.cpp, HardwareSerial.cpp, the RMT, SPI
# and UART HAL, HTTPClient.cpp). Its base commit dcc1105b in
# espressif/arduino-esp32 is the tag plus one CI script, so the exact source
# exists only in the package.
LGPL_SOURCES = [
    ('src-framework-arduinoespressif32-3.20017.241212.zip',
     'pio:framework-arduinoespressif32', '3.20017.241212+sha.dcc1105b',
     'Arduino-ESP32 core 2.0.17 as PlatformIO package '
     'framework-arduinoespressif32 3.20017.241212 (the 2.0.17 tag plus '
     "PlatformIO's backports; ESP-IDF 4.4.7)",
     'LGPL-2.1-or-later', '4-inch'),
    # The upstream tag that package is based on, attached as before, for
    # reference - the 4-inch links the package above.
    ('src-arduino-esp32-2.0.17.zip', 'espressif/arduino-esp32', '2.0.17',
     'Arduino-ESP32 2.0.17, the upstream tag the package above is based on '
     '(for reference; the 4-inch links the package)',
     'LGPL-2.1-or-later', '4-inch'),
    ('src-AsyncTCP-ef448a8.zip', 'me-no-dev/AsyncTCP',
     'ef448a8a1dffe4ec1b72326dd5a26211ff227b49',
     'AsyncTCP 3.3.2, commit ef448a8a1dffe4ec1b72326dd5a26211ff227b49',
     'LGPL-3.0 licence file; source headers LGPL-2.1-or-later', '4-inch'),
    ('src-ESPAsyncWebServer-ad3741d.zip', 'me-no-dev/ESPAsyncWebServer',
     'ad3741d159f9cfd50c567e81b67f3bef1dc6d89f',
     'ESPAsyncWebServer 3.6.0, commit ad3741d159f9cfd50c567e81b67f3bef1dc6d89f',
     'LGPL-3.0 licence file; source headers LGPL-2.1-or-later', '4-inch'),
    ('src-arduino-esp32-3.3.11.zip', 'espressif/arduino-esp32', '3.3.11',
     'Arduino-ESP32 core 3.3.11 (pioarduino platform 55.03.311, '
     'ESP-IDF 5.5.5)', 'LGPL-2.1-or-later', '7-inch, 5-inch'),
    ('src-AsyncTCP-3.5.0.zip', 'ESP32Async/AsyncTCP', 'v3.5.0',
     'AsyncTCP 3.5.0', 'LGPL-3.0-or-later', '7-inch, 5-inch'),
    ('src-ESPAsyncWebServer-3.12.0.zip', 'ESP32Async/ESPAsyncWebServer',
     'v3.12.0', 'ESPAsyncWebServer 3.12.0', 'LGPL-3.0-or-later',
     '7-inch, 5-inch'),
]


def run(args, **kw):
    print('  >', ' '.join(args))
    subprocess.run(args, check=True, cwd=ROOT, **kw)


def board_title(board):
    info = BOARD_INFO[board['key']]
    return '%s (%s, %s)' % (info['name'], info['model'], board['env'])


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as fh:
        for block in iter(lambda: fh.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def _valid_download(path):
    # no published hash (GitHub archives): at least an intact ZIP, not an
    # HTML error page saved under a .zip name
    try:
        with zipfile.ZipFile(path) as z:
            return bool(z.namelist()) and z.testzip() is None
    except (zipfile.BadZipFile, OSError):
        return False


def fetch_cached(url, fname):
    """Download url once into release/.cache/fname and return that path.

    A cached file is re-verified on every use; a missing network or a failed
    verification ends the run - a release package is all or nothing."""
    os.makedirs(CACHE, exist_ok=True)
    dst = os.path.join(CACHE, fname)
    if os.path.exists(dst):
        if _valid_download(dst):
            return dst
        os.remove(dst)
    tmp = dst + '.part'
    print('  downloading %s ...' % url)
    try:
        with urllib.request.urlopen(url, timeout=300) as r, open(tmp, 'wb') as out:
            shutil.copyfileobj(r, out, 1 << 20)
    except Exception as e:
        if os.path.exists(tmp):
            os.remove(tmp)
        sys.exit('download failed: %s (%s)' % (url, e))
    if not _valid_download(tmp):
        os.remove(tmp)
        sys.exit('download failed verification (not an intact ZIP): %s' % url)
    os.replace(tmp, dst)
    return dst


def fetch_all():
    """Every download, BEFORE the build: a missing network then fails in
    seconds instead of after three full compiles. Returns the cached paths."""
    print('== fetching downloads (cached in release/.cache) ==')
    got = {}
    for asset, repo, ref, _what, _lic, _boards in LGPL_SOURCES:
        if repo.startswith('pio:'):
            got[asset] = pack_pio_package(asset, repo[4:], ref)
        else:
            got[asset] = fetch_cached('https://github.com/%s/archive/%s.zip'
                                      % (repo, ref), asset)
    return got


def upstream_url(repo, ref):
    """Where a recipient finds an LGPL source upstream (FLASHING.md)."""
    if repo.startswith('pio:'):
        return ('https://registry.platformio.org/tools/platformio/%s (version %s)'
                % (repo[4:], ref.split('+', 1)[0]))
    return 'https://github.com/%s/tree/%s' % (repo, ref)


# What of an installed PlatformIO framework package is its source: the
# library itself, its variants, the build scripts that compile it and the
# package's metadata. tools/sdk (ESP-IDF's precompiled libraries and their
# headers, Apache-2.0, from ESP-IDF 4.4.7) and the bundled executables are not
# - the 7B/5B core's GitHub archive does not carry its IDF libraries either.
PIO_PKG_DIRS = ('cores', 'libraries', 'variants')
PIO_PKG_TOOLS = re.compile(r'^tools/(?:[^/]+\.py|partitions/[^/]+)$')
PIO_PKG_DATE = (2024, 12, 12, 0, 0, 0)    # the package's release, 3.20017.241212


def pack_pio_package(asset, name, version):
    """Archive the source of the installed PlatformIO package `name` - the
    one the 4-inch build links - into release/.cache/asset. The .piopm must
    name `version`, or the run stops: the archive would not be the source of
    what was linked. Deterministic (sorted entries, fixed timestamps and
    attributes), so an unchanged package gives the same bytes every run."""
    roots = [b['pkgs'] for b in gwf.BOARDS if b['key'] == '4' and b['pkgs']]
    pkg = os.path.join(roots[0], name) if roots else ''
    meta = os.path.join(pkg, '.piopm')
    try:
        with io.open(meta, encoding='utf-8') as fh:
            have = json.load(fh).get('version')
    except (OSError, ValueError) as e:
        sys.exit('%s: cannot read the package version (%s) - build the 4-inch '
                 'once so the package is installed' % (release_checks.shown(meta), e))
    if have != version:
        sys.exit('%s: installed package is %s, LGPL_SOURCES names %s - the '
                 'archive would not be the source of what the 4-inch links'
                 % (release_checks.shown(pkg), have, version))
    files = []
    for base, dirs, names in os.walk(pkg):
        dirs.sort()
        for n in sorted(names):
            rel = os.path.relpath(os.path.join(base, n), pkg).replace('\\', '/')
            top = rel.split('/', 1)[0]
            if ('/' not in rel or top in PIO_PKG_DIRS or PIO_PKG_TOOLS.match(rel)):
                files.append(rel)
    os.makedirs(CACHE, exist_ok=True)
    dst = os.path.join(CACHE, asset)
    tmp = dst + '.part'
    prefix = '%s-%s/' % (name, version.split('+', 1)[0])
    # The package carries no licence file of its own (package.json names
    # LGPL-2.1-or-later); the text goes in, so the archive stands on its own.
    extra = {'LICENSE-LGPL-2.1.txt': os.path.join(ROOT, 'LICENSES', 'LGPL-2.1.txt')}
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        entries = [(rel, os.path.join(pkg, rel)) for rel in files] + sorted(extra.items())
        for rel, src in sorted(entries):
            with open(src, 'rb') as fh:
                data = fh.read()
            # Built here, from a local directory: no other release check
            # looks into this archive, so its members are checked one by one.
            bad = release_checks.check_personal_paths(prefix + rel, data, any_account=True)
            if bad:
                z.close()
                os.remove(tmp)
                release_checks.report(bad, asset)
                sys.exit('%s not written' % asset)
            zi = zipfile.ZipInfo(prefix + rel, PIO_PKG_DATE)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
    os.replace(tmp, dst)
    print('  packed %s from the installed package (%d files)' % (asset, len(files)))
    return dst


def merge_tool():
    """The esptool that merge_bin runs with: this interpreter's own if it has
    one, else PlatformIO's copy from the 4-inch board's package directory
    (esptool 4.x - the same command syntax as the esptool.exe that flash.bat
    runs).
    Looked up once, before release/ is touched."""
    probe = subprocess.run([sys.executable, '-c', 'import esptool'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if probe.returncode == 0:
        return [sys.executable, '-m', 'esptool']
    py = sys.executable
    for cand in (os.path.join(gwf.PIO_CORE, 'penv', 'Scripts', 'python.exe'),
                 os.path.join(gwf.PIO_CORE, 'penv', 'bin', 'python')):
        if os.path.isfile(cand):
            py = cand
            break
    four = next(b for b in gwf.BOARDS if b['key'] == '4')
    _, pkg_roots = gwf.board_paths(four, gwf.build_env(four))
    for r in pkg_roots:
        script = os.path.join(r, 'tool-esptoolpy', 'esptool.py')
        if os.path.isfile(script):
            return [py, script]
    sys.exit('esptool not found for merge_bin: pip install esptool, or build '
             'the 4-inch once (its package directory brings a copy)')


def manifest_for(board, version):
    """ESP Web Tools manifest of one board folder - the same shape as the one
    tools/gen_web_flasher.py writes to docs/flash/<board>/."""
    return {
        'name': 'NauticPinnace %s' % board['label'].split('(')[0].strip(),
        'version': version,
        'new_install_prompt_erase': True,
        'builds': [{
            'chipFamily': 'ESP32-S3',
            'parts': [{'path': fn, 'offset': int(off, 16)} for off, fn in PARTS],
        }],
    }


def assemble_board(dest, board, images, version, esptool):
    """dest/: the five images, the merged full-flash image, manifest.json,
    and the web interface as index.html.gz."""
    os.makedirs(dest)
    for _, fn in PARTS:
        shutil.copy(images[fn], os.path.join(dest, fn))
    # The web interface on its own, next to its firmware: what the WiFi update
    # (?target=webui) and the update over NMEA 2000 install - the updater
    # takes firmware and companion web UI from one folder (N2K-UPDATE.md 11.6).
    # data/ was checked to equal littlefs.bin before this point.
    shutil.copy(os.path.join(DATA, 'index.html.gz'), os.path.join(dest, 'index.html.gz'))
    # merged single image (flash everything at offset 0x0). It spans the
    # whole 16 MB - LittleFS ends at the end of the flash - and the gaps are
    # filled with 0xFF, so writing it leaves the same state as erase + write.
    tail = ['--chip', 'esp32s3', 'merge_bin',
            '-o', os.path.join(dest, 'nauticpinnace-full.bin'),
            '--flash_size', '16MB']
    for off, fn in PARTS:
        tail += [off, os.path.join(dest, fn)]
    run(esptool + tail)
    with io.open(os.path.join(dest, 'manifest.json'), 'w') as fh:
        json.dump(manifest_for(board, version), fh, indent=1)


# ---- flash scripts ------------------------------------------------------------

_CONTROL = re.compile(r'[\x00-\x1f\x7f]')


def _script(name, lines):
    """Join the lines of a generated script, refusing any control character
    INSIDE a line - the newline and the tab included. That is the check
    that catches a Windows path typed in a plain (not raw) Python string:
    '5b\\nauticpinnace' turns into a newline, '5b\\tools' into a tab and
    '%BOARD%\\firmware' into a form feed, silently. On the joined text
    such a newline would be indistinguishable from a real line end."""
    for i, line in enumerate(lines):
        bad = _CONTROL.search(line)
        if bad:
            raise ValueError('%s, line %d: control character %r at column %d '
                             '- an unescaped backslash in the template? %r'
                             % (name, i + 1, bad.group(), bad.start() + 1, line))
    return '\n'.join(lines) + '\n'


def _write_text(path, text, newline):
    """Write a generated script (text from _script(), lines joined with
    '\\n'). Refuses every other control character once more, as a second
    net for text that did not come through _script()."""
    bad = re.search(r'[\x00-\x09\x0b-\x1f\x7f]', text)
    if bad:
        raise ValueError('%s: control character %r at offset %d - an '
                         'unescaped backslash in the template?'
                         % (os.path.basename(path), bad.group(), bad.start()))
    with io.open(path, 'w', encoding='ascii', newline=newline) as fh:
        fh.write(text)


def _keys():
    return [b['key'] for b in gwf.BOARDS]


def _board_rows():
    """[(key, name, model, resolution)] in BOARDS order, for the board lists."""
    return [(k, BOARD_INFO[k]['name'], BOARD_INFO[k]['model'],
             BOARD_INFO[k]['res']) for k in _keys()]


def flash_bat(version):
    """flash.bat for the package root, with LF line ends (_write_text turns
    them into CRLF, which cmd.exe needs for its labels and gotos).

    NOTE on quoting: this text is NOT %-formatted, so batch variables use a
    SINGLE % (%~dp0, %BOARD%) - doubling them would make cmd.exe treat
    "%~dp0" as literal text - and the for-loop variable the %%a that batch
    files require. Every set that depends on a path or a variable is a
    SINGLE-LINE if, and there are no parenthesised blocks at all, only
    gotos: inside a block cmd.exe expands %VAR% when it parses the block,
    i.e. before a set in it runs, and a ')' in the folder name (an unzipped
    "NauticPinnace-v1.2.0 (1)") would end the block early. Lines with
    Windows paths are raw strings - _script() refuses the control character
    that a plain string would turn '5b\\nauticpinnace' into. Delayed
    expansion stays off, so a '!' in the folder name is just a character.

    esptool is not in the package: the script looks for Espressif's
    esptool.exe next to itself (or in the folder the ZIP unpacks to), stops
    with download instructions when there is none, and runs it only when
    certutil (part of Windows) gives ESPTOOL_EXE_SHA256 for it.
    """
    keys = _keys()
    choice = ', '.join(keys[:-1]) + ' or ' + keys[-1]
    table = ['echo     %-4s %-8s %-28s %10s' % row for row in _board_rows()]
    write_args = ' '.join('%s %%BOARD%%\\%s' % (off, fn) for off, fn in PARTS)
    win_dir = ESPTOOL_WIN_DIR
    # where Explorer's "Extract all" puts it when the ZIP is unpacked here
    zip_dir = ESPTOOL_WIN_URL.rsplit('/', 1)[-1][:-len('.zip')] + '\\' + win_dir
    L = [
        '@echo off',
        'setlocal EnableExtensions DisableDelayedExpansion',
        'rem NauticPinnace ' + version + ' flasher for Windows.',
        'rem',
        "rem It runs Espressif's esptool.exe " + ESPTOOL_VERSION + ', which is NOT part of this',
        'rem package: download',
        'rem   ' + ESPTOOL_WIN_URL,
        'rem and put the esptool.exe from it next to this script (its folder',
        'rem ' + win_dir + ' works too). The script checks the SHA-256 of that file',
        'rem with certutil before it runs it, and refuses any other file.',
        'rem',
        'rem   flash.bat BOARD [PORT]',
        'rem',
        'rem   BOARD  ' + choice + ': which board is connected, i.e. which folder',
        'rem          is flashed. Without it the script lists the boards and asks.',
        'rem          All three are ESP32-S3 and look alike over USB, so nothing',
        'rem          can check the choice, and the wrong firmware leaves the',
        'rem          screen dark: the script never guesses.',
        'rem   PORT   the serial port (COM5 ...), if auto-detection picks the',
        'rem          wrong one.',
        'rem',
        'rem For testing: with the environment variable NP_FLASH_DRYRUN set, the',
        'rem esptool commands are printed instead of run, and there is no pause.',
        'rem',
        'rem Work in the folder this script lives in: esptool.exe and the board',
        'rem folders are referenced relative to it, so any other cwd would fail.',
        'rem pushd (not cd) copes with the trailing backslash of %~dp0.',
        'pushd "%~dp0"',
        'rem No parenthesised blocks anywhere, only single-line ifs and gotos:',
        'rem inside a block cmd.exe expands %TOOL% and the like when it parses',
        'rem the block, i.e. before a set in it runs, and a closing parenthesis',
        'rem in the folder name (an unzipped "... (1)") would end it early.',
        'set RC=0',
        'echo(',
        'echo  NauticPinnace ' + version + ' flasher',
        'rem Started from inside the ZIP, Explorer copies only this file to a',
        'rem temporary folder: say "unpack" before asking for esptool.exe.',
        ' '.join('if not exist "%s\\firmware.bin"' % k for k in keys) + ' goto :not_unpacked',
        'rem ---- esptool: present, and exactly the published file? ------------',
        'rem Looked up and hashed relative to this folder (the pushd above), so',
        'rem the certutil command line holds no path that could need quoting.',
        'set "ESPTOOL_URL=' + ESPTOOL_WIN_URL + '"',
        'set ESPTOOL_SHA256=' + ESPTOOL_EXE_SHA256,
        'set TOOL_FILE=',
        'if exist "esptool.exe" set TOOL_FILE=esptool.exe',
        'if not defined TOOL_FILE if exist "' + win_dir + '\\esptool.exe" set TOOL_FILE='
        + win_dir + '\\esptool.exe',
        'if not defined TOOL_FILE if exist "' + zip_dir + '\\esptool.exe" set TOOL_FILE='
        + zip_dir + '\\esptool.exe',
        'if not defined TOOL_FILE goto :no_tool',
        'rem certutil prints a header line, then the hash - with blanks between',
        'rem the bytes on older Windows, hence their removal. A certutil error',
        'rem leaves text there that matches no hash, so it is refused as well.',
        'set GOT=',
        'for /f "skip=1 delims=" %%h in (\'certutil -hashfile "%TOOL_FILE%" SHA256\')'
        ' do if not defined GOT set "GOT=%%h"',
        'if defined GOT set "GOT=%GOT: =%"',
        'if /i not "%GOT%"=="%ESPTOOL_SHA256%" goto :bad_tool',
        'rem Full path to the tool because some environments drop the cwd from',
        'rem the search path.',
        'set TOOL="%~dp0%TOOL_FILE%"',
        'set RUN=',
        'if defined NP_FLASH_DRYRUN set RUN=echo  [dry run]',
        'set BOARD=',
        'set ANSWER=',
        'set TRIES=0',
        'if "%~1"=="" goto :ask',
        'set "ANSWER=%~1"',
        'call :pick',
        'if defined BOARD goto :chosen',
        'echo(',
        'echo  "%~1" is not one of the boards - nothing was flashed.',
        'call :usage',
        'set RC=2',
        'goto :end',
        '',
        ':ask',
        'echo(',
        'echo  Which board is connected? Check the model: the wrong firmware',
        'echo  leaves the screen dark, and nothing warns you before it happens.',
        'echo(',
        'call :boards',
        'echo(',
        ':ask_again',
        'set ANSWER=',
        'rem (set /p drops leading blanks from its prompt)',
        'set /p "ANSWER=Board (' + choice + '): "',
        'call :pick',
        'if defined BOARD goto :chosen',
        'rem A limit instead of an endless loop: at the end of piped input',
        'rem set /p returns at once, every time.',
        'set /a TRIES+=1',
        'if %TRIES% geq 3 goto :no_answer',
        'echo  Please type ' + choice + ' and press Enter.',
        'goto :ask_again',
        ':no_answer',
        'echo(',
        'echo  No board chosen - nothing was flashed.',
        'call :usage',
        'set RC=2',
        'goto :end',
        '',
        ':chosen',
        'set PORT=',
        'if not "%~2"=="" set PORT=--port %~2',
    ]
    for k in keys:
        info = BOARD_INFO[k]
        L.append('if "%%BOARD%%"=="%s" set BAUD=%d' % (k, info['baud']))
    for k in keys:
        info = BOARD_INFO[k]
        L.append('if "%%BOARD%%"=="%s" set "MODEL=%s, %s, %s"'
                 % (k, info['name'], info['model'], info['res']))
    L += [
        r'if not exist "%BOARD%\firmware.bin" goto :missing',
        'echo(',
        'echo  Board:   %MODEL%',
        r'echo  Folder:  %BOARD%\   Baud: %BAUD%',
        'echo  esptool: %TOOL_FILE% (' + ESPTOOL_VERSION + ', SHA-256 checked)',
        'echo  Connect the display via USB-C. Takes about a minute.',
        r'echo  Wrong port picked? Run:  .\flash.bat %BOARD% COMx',
        'echo(',
        'echo  Step 1/2: erasing the entire flash...',
        'echo(',
        'rem call :ok resets ERRORLEVEL: a dry run only echoes, which would',
        'rem leave the 1 of a set /p that hit the end of its input.',
        'call :ok',
        '%RUN% %TOOL% --chip esp32s3 %PORT% --baud %BAUD% erase_flash',
        'if errorlevel 1 goto :failed',
        'echo(',
        'echo  Step 2/2: writing bootloader, firmware and web UI...',
        'echo(',
        'call :ok',
        '%RUN% %TOOL% --chip esp32s3 %PORT% --baud %BAUD% write_flash ' + write_args,
        'if errorlevel 1 goto :failed',
        'echo(',
        'if defined NP_FLASH_DRYRUN goto :dry_done',
        'echo  Done. The display reboots into the first-run setup.',
        'goto :end',
        ':dry_done',
        'echo  Dry run: nothing was written to the board.',
        'goto :end',
        '',
        ':missing',
        'echo(',
        r'echo  The folder %BOARD%\ is missing next to this script. Unpack the',
        'echo  whole ZIP first and run flash.bat from the unpacked folder.',
        'set RC=1',
        'goto :end',
        '',
        ':failed',
        'set RC=1',
        'echo(',
        'echo  FAILED. Things to try:',
        r'echo   - pass the port explicitly:  .\flash.bat %BOARD% COMx',
        'echo   - close any serial monitor using the port',
        'echo   - hold BOOT, tap RESET, release BOOT (buttons on the left edge),',
        'echo     then run again',
        'if "%BOARD%"=="7b" goto :failed_7b',
        'if "%BOARD%"=="5b" goto :failed_5b',
        'goto :failed_end',
        ':failed_7b',
        'echo   - 7-inch: no COM port at all? It talks through a WCH CH343 USB',
        'echo     bridge, and Windows needs the CH343 driver for it:',
        'echo     ' + CH343_DRIVER_URL,
        'goto :failed_end',
        ':failed_5b',
        "echo   - 5-inch: if the transfer stalled partway, flash without esptool's",
        'echo     stub loader. erase_flash needs the stub, so write the merged',
        'echo     image instead: it covers all 16 MB, so the board ends up just',
        'echo     as clean. From a command prompt in this folder (slower, allow',
        'echo     a few minutes):',
        r'if defined PORT echo     .\%TOOL_FILE% --chip esp32s3 %PORT% --no-stub write_flash --compress 0x0 5b\nauticpinnace-full.bin',
        r'if not defined PORT echo     .\%TOOL_FILE% --chip esp32s3 --no-stub write_flash --compress 0x0 5b\nauticpinnace-full.bin',
        ':failed_end',
        'echo  If the erase already succeeded the display stays blank until a',
        'echo  write completes - just run this script again.',
        'goto :end',
        '',
        ':not_unpacked',
        'echo(',
        'echo  The board folders are missing next to this script. Unpack the',
        'echo  whole ZIP first and run flash.bat from the unpacked folder.',
        'set RC=1',
        'goto :end',
        '',
        ':no_tool',
        'echo(',
        "echo  esptool.exe is missing. flash.bat runs Espressif's esptool "
        + ESPTOOL_VERSION + ' for',
        'echo  Windows, which does not come with this package. To get it:',
        'echo    1. download  %ESPTOOL_URL%',
        'echo    2. unpack it and put esptool.exe next to flash.bat',
        'echo       (the unpacked folder ' + win_dir + ' next to it works too)',
        'echo    3. run flash.bat again - it checks the file before using it.',
        'echo  Nothing to install at all: flash from Chrome or Edge at',
        'echo    ' + WEB_FLASHER_URL,
        'set RC=1',
        'goto :end',
        '',
        ':bad_tool',
        'echo(',
        'echo  %TOOL_FILE% is not esptool ' + ESPTOOL_VERSION
        + ' as Espressif publishes it: its SHA-256',
        'echo  does not match, so flash.bat does not run it. Nothing was flashed.',
        'echo(',
        'echo  expected:  %ESPTOOL_SHA256%',
        'echo  found:',
        'certutil -hashfile "%TOOL_FILE%" SHA256',
        'echo(',
        'echo  Download the ZIP again, unpack it and put its esptool.exe next to',
        'echo  flash.bat in place of this one:',
        'echo    %ESPTOOL_URL%',
        'set RC=1',
        'goto :end',
        '',
        ':end',
        'popd',
        'if not defined NP_FLASH_DRYRUN pause',
        'exit /b %RC%',
        '',
        'rem ---- subroutines ---------------------------------------------------',
        '',
        ':pick',
        'rem Sets BOARD from ANSWER, case-insensitive; anything else leaves BOARD',
        'rem empty. Quotes are dropped first and only the first word counts, so',
        'rem a stray space after the answer does no harm.',
        'set BOARD=',
        'if not defined ANSWER goto :eof',
        'set "ANSWER=%ANSWER:"=%"',
        'if not defined ANSWER goto :eof',
        'for /f "tokens=1" %%a in ("%ANSWER%") do set "ANSWER=%%a"',
    ]
    for k in keys:
        L.append('if /i "%%ANSWER%%"=="%s" set BOARD=%s' % (k, k))
    L += [
        'goto :eof',
        '',
        ':boards',
    ] + table + [
        'goto :eof',
        '',
        ':usage',
        'echo(',
        'echo  Usage:  flash.bat [' + '^|'.join(keys) + '] [COMx]',
        'echo(',
        'call :boards',
        'echo(',
        r'echo  Example:  .\flash.bat 7b COM5     (the port is optional)',
        'echo  Without a board the script lists the boards and asks.',
        'goto :eof',
        '',
        ':ok',
        'exit /b 0',
    ]
    return _script('flash.bat', L)


def flash_sh(version):
    """flash.sh for the package root (POSIX sh: Linux, macOS)."""
    keys = _keys()
    choice = ', '.join(keys[:-1]) + ' or ' + keys[-1]
    table = ['    echo "    %-4s %-8s %-28s %10s"' % row for row in _board_rows()]
    write_args = ' '.join('%s "$BOARD/%s"' % (off, fn) for off, fn in PARTS)
    L = [
        '#!/bin/sh',
        '# NauticPinnace ' + version + ' flasher for Linux and macOS - needs esptool',
        '# (pip install "esptool>=4.8,<6").',
        '#',
        '#   sh flash.sh <' + '|'.join(keys) + '> [port]      e.g.  sh flash.sh 7b /dev/ttyACM0',
        '#',
        '# The board says which folder is flashed. Without it the script lists the',
        '# boards and asks. All three are ESP32-S3 and look alike over USB, so',
        '# nothing can check the choice, and the wrong firmware leaves the screen',
        '# dark: the script never guesses.',
        '#',
        '# For testing: with NP_FLASH_DRYRUN set in the environment, the esptool',
        '# commands are printed instead of run.',
        '#',
        '# The board folders are referenced relative to this script -> work from here.',
        'cd "$(dirname "$0")" || exit 1',
        '',
        'boards() {',
    ] + table + [
        '}',
        '',
        'usage() {',
        '    echo',
        '    echo "Usage:  sh flash.sh <' + '|'.join(keys) + '> [port]"',
        '    echo',
        '    boards',
        '    echo',
        '    echo "Example:  sh flash.sh 7b /dev/ttyACM0     (the port is optional)"',
        '    echo "Without a board the script lists the boards and asks."',
        '}',
        '',
        '# board name -> folder (case-insensitive), empty for anything else',
        'pick() {',
        '    b=$(printf \'%s\' "$1" | tr \'A-Z\' \'a-z\')',
        '    case "$b" in',
        '        ' + '|'.join(keys) + ') printf \'%s\' "$b" ;;',
        '    esac',
        '}',
        '',
        'run() {',
        '    if [ -n "$NP_FLASH_DRYRUN" ]; then',
        '        echo "  [dry run] $*"',
        '    else',
        '        "$@"',
        '    fi',
        '}',
        '',
        'fail() {',
        '    echo',
        '    echo "FAILED. Things to try:"',
        '    echo "  - pass the port explicitly:  sh flash.sh $BOARD /dev/ttyXXX"',
        '    echo "    (on macOS e.g. /dev/cu.usbmodem1101)"',
        '    echo "  - close any serial monitor using the port"',
        '    echo "  - hold BOOT, tap RESET, release BOOT (buttons on the left edge),"',
        '    echo "    then run again"',
        '    case "$BOARD" in',
        '    7b)',
        '        echo "  - 7-inch: no serial port at all? It talks through a WCH CH343"',
        '        echo "    USB bridge rather than the ESP32\'s own USB; look for a driver"',
        '        echo "    for that bridge (Windows needs WCH\'s CH343 driver)."',
        '        ;;',
        '    5b)',
        '        echo "  - 5-inch: if the transfer stalled partway, flash without esptool\'s"',
        '        echo "    stub loader. erase_flash needs the stub, so write the merged"',
        '        echo "    image instead: it covers all 16 MB, so the board ends up just"',
        '        echo "    as clean. From this folder (slower, allow a few minutes):"',
        '        echo "    python3 -m esptool --chip esp32s3${PORT:+ --port $PORT} --no-stub write_flash --compress 0x0 5b/nauticpinnace-full.bin"',
        '        ;;',
        '    esac',
        '    echo "If the erase already succeeded the display stays blank until a"',
        '    echo "write completes - just run this script again."',
        '    exit 1',
        '}',
        '',
        'echo',
        'echo "NauticPinnace ' + version + ' flasher"',
        'if [ -n "$1" ]; then',
        '    BOARD=$(pick "$1")',
        '    if [ -z "$BOARD" ]; then',
        '        echo',
        '        echo "\\"$1\\" is not one of the boards - nothing was flashed."',
        '        usage',
        '        exit 2',
        '    fi',
        'else',
        '    echo',
        '    echo "Which board is connected? Check the model: the wrong firmware"',
        '    echo "leaves the screen dark, and nothing warns you before it happens."',
        '    echo',
        '    boards',
        '    echo',
        '    BOARD=""',
        '    tries=0',
        '    while [ -z "$BOARD" ]; do',
        '        printf \'  Board (' + choice + '): \'',
        '        # only the first word counts; at the end of the input give up',
        '        if ! read -r answer _rest && [ -z "$answer" ]; then',
        '            echo',
        '            echo "No board chosen - nothing was flashed."',
        '            usage',
        '            exit 2',
        '        fi',
        '        BOARD=$(pick "$answer")',
        '        if [ -z "$BOARD" ]; then',
        '            tries=$((tries + 1))',
        '            if [ "$tries" -ge 3 ]; then',
        '                echo "No board chosen - nothing was flashed."',
        '                usage',
        '                exit 2',
        '            fi',
        '            echo "  Please type ' + choice + ' and press Enter."',
        '        fi',
        '    done',
        'fi',
        'PORT="$2"',
        '',
        'case "$BOARD" in',
    ]
    for k in keys:
        info = BOARD_INFO[k]
        L.append('    %s) MODEL="%s, %s, %s"; BAUD=%d ;;'
                 % (k, info['name'], info['model'], info['res'], info['baud']))
    L += [
        'esac',
        'if [ ! -f "$BOARD/firmware.bin" ]; then',
        '    echo "The folder $BOARD/ is missing next to this script. Unpack the whole"',
        '    echo "ZIP first and run flash.sh from the unpacked folder."',
        '    exit 1',
        'fi',
        '',
        'ESPTOOL="python3 -m esptool"',
        'echo',
        'echo "Board:   $MODEL"',
        'echo "Folder:  $BOARD/   Baud: $BAUD"',
        'echo "Connect the display via USB-C. Takes about a minute."',
        'echo',
        'echo "Step 1/2: erasing the entire flash..."',
        'run $ESPTOOL --chip esp32s3 ${PORT:+--port "$PORT"} --baud "$BAUD" erase_flash || fail',
        'echo "Step 2/2: writing bootloader, firmware and web UI..."',
        'run $ESPTOOL --chip esp32s3 ${PORT:+--port "$PORT"} --baud "$BAUD" write_flash ' + write_args + ' || fail',
        'echo',
        'if [ -n "$NP_FLASH_DRYRUN" ]; then',
        '    echo "Dry run: nothing was written to the board."',
        'else',
        '    echo "Done. The display reboots into the first-run setup."',
        'fi',
    ]
    return _script('flash.sh', L)


def write_flash_scripts(dest, version):
    """flash.bat (CRLF) and flash.sh (LF) into dest - the package root."""
    _write_text(os.path.join(dest, 'flash.bat'), flash_bat(version), '\r\n')
    _write_text(os.path.join(dest, 'flash.sh'), flash_sh(version), '\n')


# ---- FLASHING.md ----------------------------------------------------------------

def flashing_md(version):
    """FLASHING.md for the package."""
    rows = ''.join('| %s | %s | %s | `%s` | %s |\n'
                   % (what, boards, lic, asset, upstream_url(repo, ref))
                   for asset, repo, ref, what, lic, boards in LGPL_SOURCES)
    usb = {'4': "the ESP32-S3's own USB",
           '7b': 'WCH CH343 USB-serial bridge (Windows needs a driver, see '
                 'below)',
           '5b': "the ESP32-S3's own USB"}
    folders = ''.join(
        '| `%s/` | %s | %s | %s | %s |\n'
        % (k, BOARD_INFO[k]['name'],
           BOARD_INFO[k]['model'].replace(' Rev 4', ' **Rev 4**'),
           BOARD_INFO[k]['res'].replace(' x ', ' × '), usb.get(k, ''))
        for k in _keys())
    return (
        '# Flashing NauticPinnace %(v)s\n\n'
        'This package carries the firmware for all three supported Waveshare\n'
        'boards, one folder each. Connect the board via USB-C.\n\n'
        '| Folder | Board | Waveshare model | Screen | USB connection |\n'
        '|---|---|---|---|---|\n'
        '%(folders)s\n'
        '**Pick the right board.** All three are ESP32-S3 and look identical over\n'
        'USB, so neither the scripts nor esptool can tell them apart: the scripts\n'
        'take the board as their first argument or ask for it, and never guess.\n'
        'The wrong firmware leaves the screen dark, because its panel driver does\n'
        'not match the hardware. That is not fatal — flashing the right one fixes\n'
        'it — but there is no warning before it happens.\n\n'
        '## Browser (easiest, nothing to install)\n'
        'Chrome/Edge can flash over WebSerial: the web flasher at\n'
        '%(web)s offers all three boards\n'
        'and is the recommended way. The scripts below are for flashing from\n'
        'this package instead.\n\n'
        '## Windows\n'
        '`flash.bat` runs esptool, the flashing tool by Espressif, which is not\n'
        'part of this package. Get it once from Espressif\'s esptool %(e)s\n'
        'release:\n\n'
        '1. Download `%(ezip)s`:\n'
        '   %(eurl)s\n'
        '2. Unpack it and put the `esptool.exe` from it next to `flash.bat`\n'
        '   (the unpacked folder `%(edir)s\\` next to `flash.bat` works too).\n\n'
        'Before every run `flash.bat` checks the SHA-256 of that `esptool.exe`\n'
        'with `certutil`, which comes with Windows, and refuses any file other\n'
        'than the one Espressif published:\n'
        '`%(esha)s`\n'
        'Without `esptool.exe` it stops and says where to get it.\n\n'
        'Then double-click `flash.bat`. It lists the three boards and asks which\n'
        'one is connected: type `4`, `7b` or `5b` and press Enter. The port is\n'
        'detected automatically.\n\n'
        'From a command prompt or PowerShell in this folder the board can be\n'
        'given directly, and the port as well if the wrong one is picked:\n'
        '`.\\flash.bat 7b` or `.\\flash.bat 7b COM5`\n\n'
        '## Linux / macOS\n'
        '`pip install "esptool>=4.8,<6"`, then `sh flash.sh` with your board — `4`, `7b` or\n'
        '`5b` — and optionally the port: `sh flash.sh 7b /dev/ttyACM0` (on macOS\n'
        'e.g. `/dev/cu.usbmodem1101`). Without a board the script asks.\n\n'
        '## Board notes\n'
        '- **7-inch:** it talks through a WCH CH343 USB-serial bridge rather than\n'
        "  the ESP32's own USB. Windows needs the\n"
        '  [WCH CH343 driver](%(ch343)s);\n'
        '  without it Windows gives the board no COM port at all. The 4-inch and\n'
        '  the 5-inch need nothing.\n'
        '- **5-inch:** the scripts flash it at 460800 baud (the others at 921600).\n'
        "  esptool's flasher stub has stalled on this board during long\n"
        '  transfers. If a run stops partway through, flash without the stub.\n'
        '  `erase_flash` needs the stub, so write the merged image instead: it\n'
        '  covers all 16 MB, so the board ends up just as clean. This is slower —\n'
        '  allow a few minutes. `--compress` keeps the transfer small (without\n'
        '  the stub esptool would otherwise send all 16 MB as they are), and\n'
        "  there is deliberately no `--baud`: the 5-inch's own USB ignores the\n"
        '  rate, and leaving it out also skips a baud change, which is suspected\n'
        '  in the stalls.\n'
        '  Put in your port, or leave `--port ...` out to let esptool find it:\n'
        '  - Windows, from a command prompt in this folder, with `esptool.exe`\n'
        '    next to `flash.bat` (see "Windows" above):\n'
        '    `.\\esptool.exe --chip esp32s3 --port COM5 --no-stub write_flash --compress 0x0 5b\\nauticpinnace-full.bin`\n'
        '  - Linux/macOS:\n'
        '    `python3 -m esptool --chip esp32s3 --port /dev/ttyACM0 --no-stub write_flash --compress 0x0 5b/nauticpinnace-full.bin`\n'
        '- **No port at all, or the board does not answer:** a board in a crashed\n'
        '  state may need a manual bootloader entry — hold `BOOT`, tap `RESET`,\n'
        '  release `BOOT` (buttons on the left edge), then run the script again.\n\n'
        '## What the scripts do\n'
        'Two steps, both on the whole 16 MB flash, with the images from the folder\n'
        'of the board you chose:\n\n'
        '1. `erase_flash` — wipes **everything**, including the NVS area the\n'
        '   ESP-IDF uses for WiFi calibration and cached credentials. This is\n'
        '   what guarantees a genuine factory state rather than "new firmware\n'
        '   on top of old leftovers".\n'
        '2. `write_flash` — writes bootloader, partition table, boot_app0,\n'
        '   firmware and the LittleFS image (web UI, factory config, polar).\n\n'
        'The whole run takes about a minute; leave the cable in until the script\n'
        'says "Done".\n\n'
        'If the erase succeeds but the write is interrupted (cable pulled), the\n'
        'display stays blank — harmless, just run the script again. Should the\n'
        'board then no longer enumerate as a serial port, hold `BOOT`, tap\n'
        '`RESET`, release `BOOT` to force the ROM bootloader (buttons on the\n'
        'left edge), and run it once more.\n\n'
        '## Updating without losing your settings\n'
        'The scripts above always start from scratch. The settings live in the\n'
        'LittleFS area, so an update of the firmware alone keeps them. Take\n'
        "`firmware.bin` from **your board's** folder: over USB nothing checks\n"
        'this, nor does the WiFi update of a board still running 1.1.0 or 1.2.0,\n'
        'and the wrong board leaves the screen dark here too (from 1.3.0 on, the\n'
        'WiFi update refuses a firmware for another board). Export your\n'
        'configuration first anyway (web interface, **Import / Export**).\n\n'
        '**Over WiFi, board running 1.3.0 or later:** web interface, tab\n'
        "**Update**, choose `firmware.bin` from your board's folder, then\n"
        '**Install firmware**. The display restarts and then checks the new\n'
        'firmware for one to three minutes ("Checking update" on the display):\n'
        'leave the power on — cut during the check, the board goes back to the\n'
        'old firmware — and expect uploads to be refused until then ("Not\n'
        'possible right now"). If the release changes the web interface (see\n'
        'the release notes), install it afterwards in the same tab, once\n'
        '"Checking update" has gone from the display: **Install web\n'
        "interface**, `index.html.gz` from your board's folder. The settings\n"
        'stay as they are, and no restart is needed.\n\n'
        '**Over WiFi, board running 1.1.0 or 1.2.0** (once; their Update tab\n'
        'has no web interface field yet, and they cannot report success). The\n'
        'old page says to install the filesystem first, labels its second field\n'
        '"Filesystem (littlefs.bin)" and warns that the settings get\n'
        'overwritten. Ignore all three: the firmware goes first, never put\n'
        '`littlefs.bin` into that field, and once 1.3.0 runs the field installs\n'
        'only the web interface.\n\n'
        '1. Export the settings (**Import / Export**) anyway.\n'
        "2. Tab **Update**, choose `firmware.bin` from your board's folder,\n"
        '   **Install firmware**. About a minute later the page says "Update\n'
        '   failed" although the update worked: the board restarts before its\n'
        '   answer leaves. Do not try again, and leave the power on.\n'
        '3. Wait about three minutes, until the display no longer shows\n'
        '   "Checking update", then reload the page. On the **Update** tab,\n'
        '   "Currently installed" must now read v1.3.0. If it still names 1.1.0\n'
        '   or 1.2.0, the firmware did not take: restart the display and begin\n'
        '   again at step 2 — and do NOT go on with step 4, because on those\n'
        '   versions its field overwrites the whole file system.\n'
        '4. Still on the old page: in the **Install filesystem** field choose\n'
        "   `index.html.gz` from your board's folder (file type \"all files\")\n"
        '   and install it. The new firmware installs it as the web interface;\n'
        '   settings, polar table and boot logo stay. Reload the page: the\n'
        '   Update tab now has a **Web interface (index.html.gz)** field.\n'
        '   Do this right after step 3. If the page answers "Update failed"\n'
        '   with BUSY in it, the check is still running: wait a minute and try\n'
        '   again, but do not restart or switch off the display. If it has\n'
        '   restarted since step 3, reload the page and repeat step 3 first.\n\n'
        'If the firmware upload in step 2 breaks off, restart the display\n'
        'before trying again. v1.0.0 has no update over WiFi: use USB.\n\n'
        '**Over USB:** write the firmware together with the partition table and\n'
        '`boot_app0.bin` — here the 7-inch:\n\n'
        '`python3 -m esptool --chip esp32s3 --baud 921600 write_flash 0x8000 7b/partitions.bin 0xe000 7b/boot_app0.bin 0x10000 7b/firmware.bin`\n\n'
        'On Windows, from a command prompt in this folder, with `esptool.exe`\n'
        'next to `flash.bat` (see "Windows" above):\n'
        '`.\\esptool.exe --chip esp32s3 --baud 921600 write_flash 0x8000 7b\\partitions.bin 0xe000 7b\\boot_app0.bin 0x10000 7b\\firmware.bin`\n\n'
        'For the 4-inch use the files from `4/`, for the 5-inch those from `5b/`\n'
        'with `--baud 460800`. `boot_app0.bin` makes the board start the\n'
        'firmware written at 0x10000: after an update over WiFi a board may be\n'
        'running from its second app slot, and without `boot_app0.bin` it would\n'
        'go on starting the old firmware from there as if nothing had been\n'
        'written. `partitions.bin` matters only on a board still running v1.0.0:\n'
        'it brings the layout of v1.1.0 and later (with the core-dump area);\n'
        'everywhere else it is identical to what the board already has. The\n'
        'settings area stays where it was in every version.\n\n'
        'A USB update keeps the web interface the board has: that lives in\n'
        'LittleFS, next to the settings. If the release changes the web\n'
        'interface, install it afterwards over WiFi — with **Install web\n'
        "interface** on 1.3.0's own page, or as in step 4 above on a page from\n"
        '1.1.0 or 1.2.0; after a USB update there is no check to wait for — or\n'
        'over USB by adding\n'
        "`0xA10000` and the folder's `littlefs.bin` to the command above. That\n"
        'replaces the stored settings, the polar table and an uploaded boot\n'
        'logo with the factory ones, WiFi off included (the 7-inch and 5-inch\n'
        'keep a network they had joined), and a hotspot gets a new password\n'
        '(shown on the display). So export the settings, and on the **Polar**\n'
        'tab the polar table as CSV, keep your logo file (the display serves\n'
        'it at `/logo.bin`), and import / upload them again afterwards. This is\n'
        'also the way for a board coming from v1.0.0, whose web interface has\n'
        'no Update tab.\n\n'
        'With a CANable USB-CAN adapter, firmware and web interface can also\n'
        'go over NMEA 2000, without WiFi: see "Updates over NMEA 2000" in the\n'
        'README, %(repo)s/blob/%(v)s/README.md#updates-over-nmea-2000 .\n'
        'The display has to be running this release already.\n\n'
        '## Single-image alternative\n'
        "Each board's folder also holds `nauticpinnace-full.bin`: everything\n"
        'merged into a single image of the whole 16 MB flash, with the space\n'
        'between the parts left blank, as after an erase. Flash it at offset 0\n'
        '— it replaces everything on the board, settings included, so no\n'
        'separate erase is needed. Use the folder of your board (`4/`, `7b/` or\n'
        '`5b/`); for the 7-inch:\n\n'
        '`python3 -m esptool --chip esp32s3 write_flash 0x0 7b/nauticpinnace-full.bin`\n\n'
        'On Windows, from a command prompt in this folder, with `esptool.exe`\n'
        'next to `flash.bat` (see "Windows" above):\n'
        '`.\\esptool.exe --chip esp32s3 write_flash 0x0 7b\\nauticpinnace-full.bin`\n\n'
        '## Safety\n'
        'NauticPinnace is a hobby project. It is **not NMEA-certified**, and it\n'
        'is not sold or supported as a product: you build it, you are\n'
        'responsible for it. This display is an aid, **not a navigation\n'
        'device** — never rely on it as your primary source for depth, position\n'
        'or collision avoidance.\n\n'
        'Connecting a self-built device to an NMEA 2000 backbone may have\n'
        'warranty implications for the certified equipment on that network.\n'
        'For that case the firmware has a **listen-only mode**: it sends no\n'
        'messages, not even an address claim, and opens the CAN controller in\n'
        'hardware listen-only mode, so it does not even drive ACK bits.\n'
        'Listen-only is **off by default**. If you want it, power the display\n'
        'from USB-C first and switch it on in the settings (on the display, or\n'
        'on the **NMEA 2000** tab of the web interface) before you connect the\n'
        'drop cable.\n\n'
        '## First boot\n'
        'The panel lights up within a few seconds (at most about 8, while the\n'
        'onboard IO controller wakes up), then the display starts in English,\n'
        'asks for your language and shows the licences.\n\n'
        '**Demo mode is OFF by default**: the screens show live data from the\n'
        'NMEA 2000 bus, and "--" for anything nothing on the bus sends. To try\n'
        'the display on a desk without a bus, open the web interface (below),\n'
        'go to **Display**, tick **Enable demo mode** in the "Demo mode" box and\n'
        'press **Save**: every screen then runs on animated synthetic data,\n'
        'marked by a demo banner. Untick it and save again before the display\n'
        'goes onto a real bus — a display that was started in demo mode then\n'
        'restarts by itself so it reads the bus.\n\n'
        '**WiFi is OFF by default** (a boat rarely has any). To reach the web\n'
        'interface, open the settings: on the 4-inch tap the gear icon at the top\n'
        'of the screen — it fades out, so tap once to reveal it and again to\n'
        'open — and on the 7-inch and 5-inch the settings button on the\n'
        'navigation rail. Then switch WiFi on, or press "Hotspot & restart". The\n'
        'hotspot name, its randomly generated password and a QR code for joining\n'
        'are shown right there; the web UI is then at http://192.168.4.1.\n'
        'Joining your own network instead shows the address in the same place.\n\n'
        '## Licences\n'
        'This package contains the built firmware of all three boards; the\n'
        'notices are in THIRD-PARTY-NOTICES.md, the full licence texts in\n'
        'LICENSES/.\n\n'
        'esptool is not part of this package. `flash.bat` and `flash.sh` run\n'
        "the copy you install yourself, a separate program by Espressif under\n"
        "its own licence (GPL-2.0-or-later), whose notices come with\n"
        "Espressif's download.\n\n"
        'NMEA 2000 is a registered trademark of the National Marine Electronics\n'
        'Association; other names belong to their owners; this project is not\n'
        'affiliated with or endorsed by any of them.\n\n'
        '## Corresponding source\n'
        'The firmware links three LGPL libraries statically: the 4-inch one set\n'
        'of versions, the 7-inch and 5-inch newer ones. Their source, in exactly\n'
        'the versions each board links, is attached to the same GitHub Release\n'
        'as this package (%(repo)s/releases/tag/%(v)s) under the asset names\n'
        'below, and is available upstream at the given URL.\n\n'
        '| Library and version | Boards | Licence | Release asset | Upstream |\n'
        '|---|---|---|---|---|\n'
        '%(rows)s\n'
        'The application source — what you need to relink the firmware against\n'
        'a modified library — is the NauticPinnace repository at tag `%(v)s`:\n'
        '%(repo)s/tree/%(v)s . Building it with PlatformIO (see the README)\n'
        'compiles these libraries from source - or from your modified copy -\n'
        'and links them with the application.\n'
        % {'v': version, 'e': ESPTOOL_VERSION, 'repo': REPO_URL, 'rows': rows,
           'folders': folders, 'web': WEB_FLASHER_URL,
           'ch343': CH343_DRIVER_URL, 'eurl': ESPTOOL_WIN_URL,
           'ezip': ESPTOOL_WIN_URL.rsplit('/', 1)[-1],
           'edir': ESPTOOL_WIN_DIR, 'esha': ESPTOOL_EXE_SHA256})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default=None,
                    help='default: v + FW_VERSION from src/Version.h; must '
                         'equal FW_VERSION')
    ap.add_argument('--skip-build', action='store_true',
                    help='package the existing build output of all three '
                         'boards; refused unless every release check passes')
    a = ap.parse_args()
    try:
        fw = release_checks.fw_version(ROOT)
    except (OSError, ValueError) as e:
        sys.exit('src/Version.h: %s' % e)
    version = a.version or 'v' + fw
    # At once, not only in the gate after the builds: a typo in --version
    # would otherwise cost three full board builds before it is refused.
    # (check_version in the gate compares the same way and stays as the
    # check against what firmware.bin really contains.)
    if (version[1:] if version[:1] in ('v', 'V') else version) != fw:
        sys.exit('--version %s does not match FW_VERSION "%s" in src/Version.h '
                 '- set it there first. Nothing was built or written.'
                 % (version, fw))

    # ---- before anything is built ---------------------------------------
    # data/ is packed WHOLE into littlefs.bin, so its contents are checked
    # before buildfs can pick up an exported device config. The .gz sync is
    # checked after the build: the build is what regenerates it.
    if not release_checks.report(
            release_checks.check_data_dir(DATA, gzip_sync=False), 'data/'):
        sys.exit(1)
    got = fetch_all()

    # ---- build (or locate) every board, one after the other ----------------
    # Strictly sequential: two PlatformIO runs at once would fight over the
    # shared package and build directories.
    built = []
    for b in gwf.BOARDS:
        print('\n== %s ==' % board_title(b))
        try:
            built.append((b, gwf.build(b, a.skip_build)))
        except subprocess.CalledProcessError as e:
            sys.exit('\nbuild of %s failed (%s) - nothing was packaged'
                     % (board_title(b), e))

    # ---- gate: every input checked BEFORE release/<name> is touched ------
    ok = release_checks.report(release_checks.check_data_dir(DATA),
                               'data/ after the build')
    for b, images in built:
        ok = release_checks.report(
            release_checks.check_build_output(images, version, DATA, board=b['key']),
            '%s build output' % board_title(b)) and ok
    if not ok:
        if a.skip_build:
            print('\n--skip-build refused: the existing build output fails the '
                  'checks above. Run without --skip-build.')
        sys.exit('nothing was written to release/')
    esptool = merge_tool()

    name = 'NauticPinnace-%s' % version
    rel = os.path.join(ROOT, 'release', name)
    zpath = os.path.join(ROOT, 'release', name + '.zip')
    srcdir = os.path.join(ROOT, 'release', name + '-sources')
    for old in (rel, srcdir):
        shutil.rmtree(old, ignore_errors=True)
    if os.path.exists(zpath):
        os.remove(zpath)
    os.makedirs(rel)

    # ---- one folder per board: images, merged image, manifest -------------
    for b, images in built:
        print('== %s/ : %s ==' % (b['key'], board_title(b)))
        try:
            assemble_board(os.path.join(rel, b['key']), b, images, version,
                           esptool)
        except subprocess.CalledProcessError as e:
            shutil.rmtree(rel, ignore_errors=True)
            sys.exit('merge_bin for %s failed (%s) - nothing was packaged'
                     % (board_title(b), e))

    # ---- shared, once at the top -------------------------------------------
    # No esptool here (see the licence note at the top): flash.bat runs the
    # user's own copy of Espressif's esptool.exe and checks its SHA-256.
    write_flash_scripts(rel, version)
    for f in ('LICENSE', 'THIRD-PARTY-NOTICES.md'):
        shutil.copy(os.path.join(ROOT, f), rel)
    shutil.copytree(os.path.join(ROOT, 'LICENSES'), os.path.join(rel, 'LICENSES'))
    io.open(os.path.join(rel, 'FLASHING.md'), 'w', encoding='utf-8').write(
        flashing_md(version))

    # ---- docs/flash is NOT written here ------------------------------------
    # tools/gen_web_flasher.py owns that directory: it serves the same three
    # boards from docs/flash/<board>/ and refreshes the licence copies that
    # travel with them. Writing it from here as well would give the directory
    # two owners again (this script once emptied it for a 4-inch-only payload
    # and the page then 404'd for the other two boards).

    # ---- last gate: the assembled folder, before it is zipped --------------
    if not release_checks.report(release_checks.check_tree(rel),
                                 'assembled package %s' % name):
        shutil.rmtree(rel, ignore_errors=True)
        sys.exit('nothing was packaged')

    # ---- zip ----------------------------------------------------------------
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for base, dirs, files in os.walk(rel):
            dirs.sort()
            for f in sorted(files):
                p = os.path.join(base, f)
                z.write(p, os.path.join(name, os.path.relpath(p, rel)))

    # ---- LGPL corresponding source, as separate release assets --------------
    os.makedirs(srcdir)
    for asset, _repo, _ref, _what, _lic, _boards in LGPL_SOURCES:
        shutil.copy(got[asset], os.path.join(srcdir, asset))

    print('\nPackage %s: one folder per board' % name)
    for b, _images in built:
        d = os.path.join(rel, b['key'])
        total = sum(os.path.getsize(os.path.join(d, f)) for f in os.listdir(d))
        print('  %-4s %-58s %6.1f MB' % (b['key'] + '/', board_title(b),
                                         total / 1e6))
    print('\nFiles to attach to the GitHub Release %s (nothing was uploaded):'
          % version)
    attach = [zpath] + [os.path.join(srcdir, s[0]) for s in LGPL_SOURCES]
    for p in attach:
        print('  %-72s %8.1f MB  sha256 %s'
              % (os.path.relpath(p, ROOT).replace('\\', '/'),
                 os.path.getsize(p) / 1e6, sha256_of(p)))
    print('\ndocs/flash/ (web flasher, the same three boards) is written '
          'separately:')
    print('    python tools/gen_web_flasher.py --skip-build')


if __name__ == '__main__':
    main()
