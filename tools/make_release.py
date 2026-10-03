#!/usr/bin/env python3
"""Build the flash-it-yourself release package for ALL THREE boards.

Produces under release/:
  NauticPinnace-<version>/  and  NauticPinnace-<version>.zip of it
    4/  7b/  5b/   one folder per board, each holding the five flash images
                   (bootloader, partition table, boot_app0, firmware,
                   LittleFS), a merged full-flash image nauticpinnace-full.bin
                   and manifest.json for ESP Web Tools (browser flashing)
    flash.bat      Windows, zero-install (uses the bundled esptool.exe):
                   flash.bat [4|7b|5b] [COMx] - without a board it lists the
                   three and asks
    flash.sh       Linux/macOS (pip-installed esptool):
                   sh flash.sh <4|7b|5b> [port] - without a board it asks
    FLASHING.md, LICENSE, THIRD-PARTY-NOTICES.md, LICENSES/   once, at the top
    esptool.exe plus esptool-<ver>/ (its source, its upstream notices, the
      notices of everything frozen into the exe and the source distributions
      of the Python packages it contains)
  NauticPinnace-<version>-sources/
    the six src-*.zip archives: the corresponding source of the LGPL libraries
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
GitHub Release: the ZIP and the six source archives.

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
corresponding source goes to the same GitHub Release as the six src-*.zip
archives (FLASHING.md lists them with the upstream URLs), and the application
source needed to relink is this repository at the release tag. esptool.exe
(GPL-2.0-or-later) is an unmodified standalone tool distributed alongside,
not linked. Because the binary is redistributed, its complete source is in
the package too (GPLv2 section 3a - a link would not be enough), together
with tools/esptool-exe-notices/ and the pyserial/intelhex source
distributions that file lists. Every download is cached in release/.cache/,
so a re-run is offline-capable.
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

ESPTOOL_VERSION = '4.8.1'
ESPTOOL_WIN_URL = ('https://github.com/espressif/esptool/releases/download/'
                   'v4.8.1/esptool-v4.8.1-win64.zip')
# The release publishes no digest, so this pins the asset as downloaded for
# v1.0.0, v1.1.0 and v1.2.0 (33,608,517 bytes; the esptool.exe inside it has
# sha256 069db0f1...aaad2f664 in all three packages). The binary is
# redistributed, so a changed download must stop the run, not ship.
ESPTOOL_WIN_SHA256 = ('2483d409e241d8826ae0ff023eecf31a'
                      '7d4de6c10ca5ee855b1420cdfd53aaf6')
ESPTOOL_SRC_URL = ('https://github.com/espressif/esptool/archive/refs/tags/'
                   'v4.8.1.zip')
ESPTOOL_NOTICES = os.path.join(ROOT, 'tools', 'esptool-exe-notices')

REPO_URL = 'https://github.com/Ranman86/NauticPinnace'
WEB_FLASHER_URL = 'https://ranman86.github.io/NauticPinnace/flash.html'
CH343_DRIVER_URL = 'https://www.wch-ic.com/downloads/CH343SER_EXE.html'

# Corresponding source of the LGPL libraries, one archive per version that any
# published firmware links. Asset name, GitHub repo, ref (tag or full commit),
# what it is, licence, which boards. The release ZIP carries all three boards
# (and the web flasher of the same release serves the same images), so every
# entry here belongs to it.
LGPL_SOURCES = [
    ('src-arduino-esp32-2.0.17.zip', 'espressif/arduino-esp32', '2.0.17',
     'Arduino-ESP32 core 2.0.17 (PlatformIO package '
     'framework-arduinoespressif32 3.20017.241212, ESP-IDF 4.4.7)',
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


def _valid_download(path, sha256):
    if sha256:
        return sha256_of(path) == sha256.lower()
    # no published hash (GitHub archives): at least an intact ZIP, not an
    # HTML error page saved under a .zip name
    try:
        with zipfile.ZipFile(path) as z:
            return bool(z.namelist()) and z.testzip() is None
    except (zipfile.BadZipFile, OSError):
        return False


def fetch_cached(url, fname, sha256=None):
    """Download url once into release/.cache/fname and return that path.

    A cached file is re-verified on every use; a missing network or a failed
    verification ends the run - a release package is all or nothing."""
    os.makedirs(CACHE, exist_ok=True)
    dst = os.path.join(CACHE, fname)
    if os.path.exists(dst):
        if _valid_download(dst, sha256):
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
    if not _valid_download(tmp, sha256):
        os.remove(tmp)
        sys.exit('download failed verification (%s): %s'
                 % ('sha256 mismatch' if sha256 else 'not an intact ZIP', url))
    os.replace(tmp, dst)
    return dst


def load_esptool_notices():
    """tools/esptool-exe-notices/sources.json, validated. Without it the
    bundled esptool.exe would ship without the notices of the components
    frozen into it, so a missing or malformed file stops the run."""
    if not os.path.isdir(ESPTOOL_NOTICES):
        sys.exit('tools/esptool-exe-notices/ is missing - esptool.exe cannot '
                 'be bundled without the notices of what is frozen into it')
    sj = os.path.join(ESPTOOL_NOTICES, 'sources.json')
    if not os.path.isfile(sj):
        sys.exit('tools/esptool-exe-notices/sources.json is missing')
    try:
        with io.open(sj, encoding='utf-8') as fh:
            doc = json.load(fh)
    except ValueError as e:
        sys.exit('tools/esptool-exe-notices/sources.json: invalid JSON (%s)' % e)
    sdists = doc.get('sdists') if isinstance(doc, dict) else None
    if not isinstance(sdists, list) or not sdists:
        sys.exit('tools/esptool-exe-notices/sources.json: no "sdists" list')
    for i, s in enumerate(sdists):
        bad = [k for k in ('name', 'version', 'url', 'sha256', 'licence')
               if not isinstance(s, dict) or not isinstance(s.get(k), str) or not s.get(k)]
        if bad:
            sys.exit('tools/esptool-exe-notices/sources.json: sdists[%d] lacks %s'
                     % (i, ', '.join(bad)))
        if not s['url'].startswith('https://'):
            sys.exit('tools/esptool-exe-notices/sources.json: sdists[%d] url is '
                     'not https' % i)
        if not re.match(r'^[0-9a-fA-F]{64}$', s['sha256']):
            sys.exit('tools/esptool-exe-notices/sources.json: sdists[%d] sha256 '
                     'is not 64 hex digits' % i)
    if not os.path.isfile(os.path.join(ESPTOOL_NOTICES, 'NOTICE.md')):
        print('  WARNING: tools/esptool-exe-notices/NOTICE.md not found')
    return sdists


def sdist_filename(s):
    name = s['url'].split('?', 1)[0].rstrip('/').rsplit('/', 1)[-1]
    return name or '%s-%s.tar.gz' % (s['name'], s['version'])


def fetch_all(sdists):
    """Every download, BEFORE the build: a missing network then fails in
    seconds instead of after three full compiles. Returns the cached paths."""
    print('== fetching downloads (cached in release/.cache) ==')
    got = {
        'esptool-win': fetch_cached(ESPTOOL_WIN_URL,
                                    'esptool-v%s-win64.zip' % ESPTOOL_VERSION,
                                    ESPTOOL_WIN_SHA256),
        'esptool-src': fetch_cached(ESPTOOL_SRC_URL,
                                    'esptool-%s-source.zip' % ESPTOOL_VERSION),
    }
    for s in sdists:
        got['sdist:' + s['name']] = fetch_cached(s['url'], sdist_filename(s),
                                                 s['sha256'])
    for asset, repo, ref, _what, _lic, _boards in LGPL_SOURCES:
        got[asset] = fetch_cached('https://github.com/%s/archive/%s.zip'
                                  % (repo, ref), asset)
    return got


def merge_tool():
    """The esptool that merge_bin runs with: this interpreter's own if it has
    one, else PlatformIO's copy from the 4-inch board's package directory
    (esptool 4.x - the same command syntax as the bundled esptool.exe).
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
    """dest/: the five images, the merged full-flash image, manifest.json."""
    os.makedirs(dest)
    for _, fn in PARTS:
        shutil.copy(images[fn], os.path.join(dest, fn))
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


def bundle_esptool(dest_dir, sdists, got):
    """Bundle the standalone Windows esptool so flash.bat needs zero installs.

    esptool is GPL-2.0-or-later. Shipping the binary therefore also means
    shipping its licence AND its complete source (GPLv2 section 3a) — a link
    would not be enough, so the source archive goes into the package next to
    the exe, in esptool-<ver>/. The exe is a PyInstaller build: the notices
    of what is frozen into it (Python runtime and the libraries it carries)
    come from tools/esptool-exe-notices/ into esptool-<ver>/notices/, and the
    source distributions listed there next to the esptool source.
    """
    exe = os.path.join(dest_dir, 'esptool.exe')
    sub = os.path.join(dest_dir, 'esptool-' + ESPTOOL_VERSION)
    os.makedirs(sub, exist_ok=True)
    with zipfile.ZipFile(got['esptool-win']) as z:
        for n in z.namelist():
            base = os.path.basename(n)
            if base == 'esptool.exe':
                with z.open(n) as s, open(exe, 'wb') as out:
                    shutil.copyfileobj(s, out)
            # keep the upstream notices instead of stripping them
            elif base.upper().startswith(('LICENSE', 'NOTICE', 'README')):
                with z.open(n) as s, open(os.path.join(sub, base), 'wb') as out:
                    shutil.copyfileobj(s, out)
    if not os.path.isfile(exe):
        sys.exit('esptool.exe not found in %s' % ESPTOOL_WIN_URL)

    shutil.copy(got['esptool-src'],
                os.path.join(sub, 'esptool-%s-source.zip' % ESPTOOL_VERSION))
    # .gitattributes there only keeps git from touching the line ends of the
    # licence texts - a repository setting, not a notice
    shutil.copytree(ESPTOOL_NOTICES, os.path.join(sub, 'notices'),
                    ignore=shutil.ignore_patterns('.git*'))
    sdist_lines = ''
    for s in sdists:
        fn = sdist_filename(s)
        shutil.copy(got['sdist:' + s['name']], os.path.join(sub, fn))
        sdist_lines += '  %s  (%s %s, %s)\n' % (fn, s['name'], s['version'],
                                                s['licence'])

    io.open(os.path.join(sub, 'README-WHY-THIS-IS-HERE.txt'), 'w',
            encoding='utf-8').write(
        'esptool.exe in the parent directory is an unmodified build of\n'
        'esptool %s (https://github.com/espressif/esptool), licensed\n'
        'GPL-2.0-or-later. It is a separate program that NauticPinnace only\n'
        'invokes — it is not linked into the firmware.\n\n'
        'Because the binary is redistributed here, its complete\n'
        'corresponding source accompanies it:\n'
        '  esptool-%s-source.zip\n'
        'and the licence text is in ../LICENSES/GPL-2.0.txt.\n\n'
        'esptool.exe is a PyInstaller bundle: it also contains a Python\n'
        'runtime and the libraries esptool uses. Their notices and licence\n'
        'texts are in notices/, and the source distributions of the Python\n'
        'packages frozen into it are here:\n'
        '%s'
        % (ESPTOOL_VERSION, ESPTOOL_VERSION, sdist_lines))


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
    that a plain string would turn '5b\\nauticpinnace' into.
    """
    keys = _keys()
    choice = ', '.join(keys[:-1]) + ' or ' + keys[-1]
    table = ['echo     %-4s %-8s %-28s %10s' % row for row in _board_rows()]
    write_args = ' '.join('%s %%BOARD%%\\%s' % (off, fn) for off, fn in PARTS)
    L = [
        '@echo off',
        'setlocal',
        'rem NauticPinnace ' + version + ' flasher for Windows - uses the bundled esptool.exe.',
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
        'rem Full path to the tool because some environments drop the cwd from',
        'rem the search path.',
        'set TOOL=python -m esptool',
        'if exist "%~dp0esptool.exe" set TOOL="%~dp0esptool.exe"',
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
        r'if defined PORT echo     .\esptool.exe --chip esp32s3 %PORT% --no-stub write_flash --compress 0x0 5b\nauticpinnace-full.bin',
        r'if not defined PORT echo     .\esptool.exe --chip esp32s3 --no-stub write_flash --compress 0x0 5b\nauticpinnace-full.bin',
        ':failed_end',
        'echo  If the erase already succeeded the display stays blank until a',
        'echo  write completes - just run this script again.',
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
    rows = ''.join('| %s | %s | %s | `%s` | https://github.com/%s/tree/%s |\n'
                   % (what, boards, lic, asset, repo, ref)
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
        '## Windows (zero install)\n'
        'Double-click `flash.bat`. It lists the three boards and asks which one is\n'
        'connected: type `4`, `7b` or `5b` and press Enter. It uses the bundled\n'
        '`esptool.exe`; the port is detected automatically.\n\n'
        'From a command prompt or PowerShell in this folder the board can be\n'
        'given directly, and the port as well if the wrong one is picked:\n'
        '`.\\flash.bat 7b` or `.\\flash.bat 7b COM5`\n\n'
        '## Linux / macOS\n'
        '`pip install "esptool>=4.8,<6"`, then `sh flash.sh` with your board — `4`, `7b` or\n'
        '`5b` — and optionally the port: `sh flash.sh 7b /dev/ttyACM0` (on macOS\n'
        'e.g. `/dev/cu.usbmodem1101`). Without a board the script asks.\n\n'
        '## Browser\n'
        'Chrome/Edge can flash over WebSerial: the web flasher at\n'
        '%(web)s offers all three boards\n'
        '(see also the "Flashing" section of the project README).\n\n'
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
        '  - Windows, from a command prompt in this folder:\n'
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
        "`firmware.bin` from **your board's** folder: nothing checks this route\n"
        'either, and the wrong board leaves the screen dark here too. Export your\n'
        'configuration first anyway (web interface, **Import / Export**).\n\n'
        '**Over WiFi, no cable:** web interface, tab **Update**, choose\n'
        "`firmware.bin` from your board's folder, then **Install firmware**.\n\n"
        '**Over USB:** write the firmware together with the partition table and\n'
        '`boot_app0.bin` — here the 7-inch:\n\n'
        '`python3 -m esptool --chip esp32s3 --baud 921600 write_flash 0x8000 7b/partitions.bin 0xe000 7b/boot_app0.bin 0x10000 7b/firmware.bin`\n\n'
        'On Windows, from a command prompt in this folder:\n'
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
        'Either way the board keeps the web interface it has: that lives in\n'
        'LittleFS, next to the settings. If the release changes the web\n'
        'interface (see the release notes), its new options appear only once\n'
        '`littlefs.bin` from the same folder is installed too — and that\n'
        'replaces the stored settings with the factory ones, WiFi off included\n'
        '(the 7-inch and 5-inch keep a network they had joined). So export the\n'
        'settings, install `littlefs.bin` as well, and import them again:\n'
        '- over WiFi before the firmware (tab **Update**, **Install\n'
        '  filesystem**); if the board comes back without WiFi, switch it on at\n'
        '  the display as described under "First boot" below;\n'
        "- over USB by adding `0xA10000` and the folder's `littlefs.bin` to the\n"
        '  command above.\n\n'
        '## Single-image alternative\n'
        "Each board's folder also holds `nauticpinnace-full.bin`: everything\n"
        'merged into a single image of the whole 16 MB flash, with the space\n'
        'between the parts left blank, as after an erase. Flash it at offset 0\n'
        '— it replaces everything on the board, settings included, so no\n'
        'separate erase is needed. Use the folder of your board (`4/`, `7b/` or\n'
        '`5b/`); for the 7-inch:\n\n'
        '`python3 -m esptool --chip esp32s3 write_flash 0x0 7b/nauticpinnace-full.bin`\n\n'
        'On Windows, from a command prompt in this folder:\n'
        '`.\\esptool.exe --chip esp32s3 write_flash 0x0 7b\\nauticpinnace-full.bin`\n\n'
        '## First boot\n'
        'The panel stays dark for about 8 seconds (USB serial and the onboard\n'
        'IO controller need that long), then the display starts in English, asks\n'
        'for your language and shows the licences.\n\n'
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
        '`esptool.exe` is an unmodified build of esptool (GPL-2.0-or-later), a\n'
        'separate program that the flash script invokes — it is not part of the\n'
        'firmware. Its complete source and upstream notices sit next to it in\n'
        '`esptool-%(e)s/`, the licence text in `LICENSES/GPL-2.0.txt`. The\n'
        'notices of the Python runtime and libraries frozen into the exe are in\n'
        '`esptool-%(e)s/notices/`, the source of its Python packages next to\n'
        'the esptool source.\n\n'
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
           'ch343': CH343_DRIVER_URL})


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
    sdists = load_esptool_notices()
    got = fetch_all(sdists)

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
            release_checks.check_build_output(images, version, DATA),
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
    bundle_esptool(rel, sdists, got)
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
