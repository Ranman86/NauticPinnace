#!/usr/bin/env python3
"""Release gate: what has to be true of an image before anyone else gets it.

tools/make_release.py (release ZIP) and tools/gen_web_flasher.py (docs/flash),
both for all three boards, run these checks on their inputs BEFORE they create
or delete any output, so a failing check leaves the previous state untouched.
Run on its own, this module is a report-only scanner for any file or
directory:

    python tools/release_checks.py docs/flash
    python tools/release_checks.py <build dir> --checks personal,brand
    python tools/release_checks.py                  (data/ only)

It never modifies anything. Exit status 1 means at least one problem.

Every check returns a list of problem strings; an empty list means clean.

  personal  the build machine's account name or home-directory path in the raw
            bytes, UTF-8 and UTF-16LE, case-insensitive. Compilers embed
            __FILE__ and build paths, and one such string ties the public
            pseudonym to a real name. Project-built files are also searched
            for user-profile paths of ANY account (X:/Users/...), which
            catches a build made under a different login.
  brand     names this project must not carry, in project-built files only
            (firmware, LittleFS image, merged image, generated text).
            Third-party archives are exempt: they are compressed, so a raw
            scan of them proves nothing, and third-party source may use the
            same letters in unrelated identifiers.
  data      data/ is packed WHOLE into littlefs.bin, git-ignored files
            included: only the four known files, blank WiFi credentials, and
            an index.html.gz that decompresses to index.html. The device
            serves the .gz, so a stale one ships old text that no grep of
            index.html ever sees.
  littlefs  the image, extracted with littlefs-python, must equal data/ byte
            for byte; its decoded files get the personal and brand checks
            as well.
  fresh     firmware.bin and littlefs.bin must be newer than every input
            under src/, data/, lib/, platformio.ini and partitions_16MB.csv;
            partitions.bin newer than the CSV. bootloader.bin and
            boot_app0.bin come from the framework package, not from this
            project's sources, and are exempt; so is src/idf_component.yml,
            which the 7B/5B build itself rewrites.
  version   firmware.bin must contain FW_VERSION from src/Version.h, and a
            release version, where one is given, must be that same version.
  descriptor firmware.bin carries the firmware descriptor that updates over
            NMEA 2000 rely on (docs/N2K-UPDATE.md 8.1): 0xE9 at 0, the app
            description at 0x20, "NPFWDESC" at 0x120, with the board id,
            panel size and board name of the board it is released for, the
            OTAGUARD flag and FW_VERSION. The app description itself must
            carry FW_VERSION and the project name "NauticPinnace"
            (tools/app_desc.py). Prints the image id (8.3). The data
            check also refuses an index.html.gz larger than one update region
            (128 KB) - over NMEA 2000 it could then never be installed.
  psram     7B and 5B only, in the release scripts: the firmware.elf next to
            firmware.bin must leave room for the full canvas arena beside the
            frame buffer (tools/psram_budget.py). Short, the device still
            boots, but the screens built last stay empty. The same ELF must
            define __wrap_esp_panic_handler_enable_rtc_wdt (the panic
            handler's RTC watchdog re-arm through src/LoopWatchdog.cpp).
  hwaes     7B and 5B only (the board is taken from the descriptor when it is
            not given): firmware.bin must not carry the log tags of IDF's
            hardware AES driver ("esp-aes", "esp-aes-gcm"). platformio.ini
            switches it off (CONFIG_MBEDTLS_HARDWARE_AES=n in
            custom_sdkconfig) because with the TLS records in PSRAM it broke
            the tide download off on most tries; an image that still has it
            was linked against Arduino IDF libs compiled from an older list.
  libs      7B and 5B, in the release scripts, BEFORE they build: the Arduino
            IDF libs in the PlatformIO core that is about to build them must
            have been compiled with every option of custom_sdkconfig in
            platformio.ini (check_hybrid_libs). pioarduino only compares a
            hash in the first line of sdkconfig.defaults, a file every core
            building this project shares - after a change of the list, the
            core that builds first rewrites it, and any other core then
            takes the hash for its own and keeps its old libs. A libs
            package without the sdkconfig that a libs build leaves (a fresh
            install) passes: the next build compiles them from the list.
  testbuild no firmware image built with a test flag that must never ship:
            -DPANIC_PATH_TEST (a web request crashes or hangs the display),
            -DLOOP_WDT_OFF (no loop watchdog), -DLOOP_WDT_USB_OK (the
            watchdog stays armed with a USB host and cuts a flash through
            ESP Web Tools), or any of -DLOOP_WDT_MS, -DSETUP_WDT_MS,
            -DRTC_STAGE_MS, -DUSB_REARM_MS (watchdog times other than the
            release ones), -DBSH_BENCH_POS (the tide download always asks
            for Cuxhaven, wherever the boat is) or -DBSH_BENCH_EVERY_S (the
            430 KB tide download on a short fixed period). Each prints a fixed
            line at boot (src/LoopWatchdog.cpp, bootReport(); for the two
            BSH flags src/net/BshTide.cpp, bshTideBegin()), which is what
            is searched for in every .bin.

Paths in the output are shown relative to the repository, or with the home
directory replaced by "~", and the account name only ever appears masked -
the report itself is meant to be pasteable.
"""
import argparse
import codecs
import configparser
import getpass
import gzip
import io
import json
import os
import re
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(ROOT, 'data')
PARTITIONS_CSV = os.path.join(ROOT, 'partitions_16MB.csv')

ALL_CHECKS = ('personal', 'brand', 'data', 'littlefs', 'fresh', 'version',
              'descriptor', 'hwaes', 'testbuild')

# Log tags of IDF's hardware AES driver (mbedtls/port/aes): in a 7B/5B image
# exactly when it was linked against libs built with MBEDTLS_HARDWARE_AES=y.
# Matched as whole NUL-terminated strings.
HWAES_TAGS = re.compile(rb'\x00(esp-aes(?:-gcm)?)\x00')

# The env that owns custom_sdkconfig (the 5B extends it), and where a
# PlatformIO core keeps the Arduino IDF libs that both are linked against.
HYBRID_ENV = 'waveshare_esp32s3_7b'
HYBRID_LIBS = os.path.join('packages', 'framework-arduinoespressif32-libs')

# The boot lines of the test flags (src/LoopWatchdog.cpp, bootReport(); the
# BSH bench flags in src/net/BshTide.cpp, bshTideBegin()): the string is in
# the image exactly when the flag was set. Keep both sides in step.
TESTBUILD_MARKERS = (
    (b'TEST BUILD PANIC_PATH_TEST', '-DPANIC_PATH_TEST',
     'GET /api/panic-test crashes or hangs the display'),
    (b'TEST BUILD LOOP_WDT_OFF', '-DLOOP_WDT_OFF',
     'the loop watchdog is never armed'),
    (b'TEST BUILD LOOP_WDT_USB_OK', '-DLOOP_WDT_USB_OK',
     'the loop watchdog stays armed with a USB host, so a flash through '
     'ESP Web Tools is cut off'),
    (b'TEST BUILD LOOP_WDT_TIMES',
     '-DLOOP_WDT_MS / -DSETUP_WDT_MS / -DRTC_STAGE_MS / -DUSB_REARM_MS',
     'the watchdog times are not the release ones'),
    (b'TEST BUILD BSH_BENCH_POS', '-DBSH_BENCH_POS',
     'the tide forecast is always fetched for Cuxhaven, wherever the boat is'),
    (b'TEST BUILD BSH_BENCH_EVERY_S', '-DBSH_BENCH_EVERY_S',
     'the 430 KB tide download runs on a short fixed period instead of every '
     '3 h'),
)

# docs/N2K-UPDATE.md 4.1 / 8.1: board id -> (release folder, FW_BOARD_NAME,
# panel width, panel height). Board ids are only ever added.
DESC_BOARDS = {
    1: ('4',  'waveshare-4inch-480x480', 480, 480),
    2: ('7b', 'waveshare-7b-1024x600', 1024, 600),
    3: ('5b', 'waveshare-5b-1024x600', 1024, 600),
}
DESC_OFFSET = 0x120
# The smallest update region (src/n2kupdate/UpdRegions.h, UPD_REGION_BYTES):
# the largest web UI the display can take over NMEA 2000 (11.6).
WEBUI_MAX = 131072

# The only files data/ may hold - everything in it lands in littlefs.bin.
DATA_ALLOWED = ('config.json', 'index.html', 'index.html.gz', 'polar.json')
# Key words that mark a stored secret in config.json (matched per word).
_SECRET_WORDS = {'pass', 'password', 'passwd', 'passphrase', 'pwd', 'psk',
                 'secret', 'token'}

# Freshness: which inputs an artefact has to be newer than. Anything not
# listed (bootloader.bin, boot_app0.bin) comes from the framework package.
FRESH_INPUTS = ('src', 'data', 'lib', 'platformio.ini', 'partitions_16MB.csv')
FRESH_RULES = {
    'firmware.bin': FRESH_INPUTS,
    'littlefs.bin': FRESH_INPUTS,
    'partitions.bin': ('partitions_16MB.csv',),
}
# Written BY the build, so their timestamps say nothing about the sources:
# pioarduino's component manager rewrites src/idf_component.yml during every
# 7B/5B build (custom_component_remove), which would otherwise make the
# 4-inch images built just before it look stale.
FRESH_IGNORE = ('src/idf_component.yml', 'src/idf_component.yml.orig')

LFS_BLOCK = 4096   # what mklittlefs uses for every board here

# What a damaged .gz can raise: BadGzipFile (an OSError), EOFError when it
# is truncated, zlib.error for a corrupt deflate stream.
_GZIP_ERRORS = (OSError, EOFError, ValueError, zlib.error)

# Build intermediates: never published, and full of paths by design. A
# directory scan skips them unless --all-files is given.
SKIP_EXT = ('.o', '.d', '.a', '.elf', '.map', '.dblite', '.pyc')
SKIP_DIRS = ('.git', '__pycache__')

# Not built by this project: no brand check, no any-account path check.
# Archives are compressed, so a raw scan of them would prove nothing anyway.
# No .exe: since 1.3.0 no package carries esptool.exe (or any other
# executable), so an .exe that turns up gets the full checks.
ARCHIVE_EXT = ('.zip', '.tar.gz', '.tgz', '.whl')
BRAND_EXEMPT_NAMES = ('bootloader.bin', 'boot_app0.bin')

# The two words are stored ROT13-encoded on purpose: written out, this file
# would match its own pattern, and every repository-wide grep for them.
_BRAND_WORDS = [codecs.decode(w, 'rot13').encode('ascii')
                for w in ('shfvba', 'tnezva')]
BRAND_PATTERNS = [
    # (?<!con): the English word that ends in the first one is fine.
    ('brand name #1', re.compile(rb'(?i)(?<!con)' + _BRAND_WORDS[0])),
    ('brand name #2', re.compile(rb'(?i)' + _BRAND_WORDS[1])),
]

# Account names too generic to search for on their own - "build" or "user"
# would hit ordinary text. For those only the home-path forms are searched.
_GENERIC_ACCOUNTS = ('user', 'users', 'admin', 'administrator', 'root',
                     'runner', 'runneradmin', 'build', 'builder', 'ci', 'pi',
                     'ubuntu', 'vagrant', 'docker', 'jenkins', 'github',
                     'default', 'public', 'guest')


# ---- small helpers ----------------------------------------------------------

def shown(path):
    """A path for printing: repository-relative, else with '~' for home."""
    p = os.path.abspath(path)
    try:
        rel = os.path.relpath(p, ROOT)
        if not rel.startswith('..'):
            return rel.replace('\\', '/')
    except ValueError:          # different drive on Windows
        pass
    home = os.path.expanduser('~')
    if home and home != '~' and p.lower().startswith(home.lower()):
        p = '~' + p[len(home):]
    return p.replace('\\', '/')


def mask(name):
    return '%s***(%d)' % (name[:2], len(name))


def _read(path):
    with open(path, 'rb') as fh:
        return fh.read()


def _when(t):
    return time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(t))


def is_third_party(path):
    """True for material this project does not build (see ARCHIVE_EXT)."""
    return os.path.basename(path).lower().endswith(ARCHIVE_EXT)


def brand_applies(path):
    return (not is_third_party(path)
            and os.path.basename(path).lower() not in BRAND_EXEMPT_NAMES)


def fw_version(root=ROOT):
    """FW_VERSION from src/Version.h, without a leading 'v'."""
    src = io.open(os.path.join(root, 'src', 'Version.h'), encoding='utf-8').read()
    m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', src)
    if not m:
        raise ValueError('FW_VERSION not found in src/Version.h')
    return m.group(1)


def _views(path, data):
    """The bytes to search: the file itself, plus the decompressed content of
    a .gz (not of a .tar.gz, which is a third-party archive)."""
    views, problems = [('', data)], []
    low = path.lower()
    if low.endswith('.gz') and not low.endswith('.tar.gz'):
        try:
            views.append((' (gunzipped)', gzip.decompress(data)))
        except _GZIP_ERRORS as e:
            problems.append('%s: cannot be gunzipped, content unchecked (%s)'
                            % (shown(path), e))
    return views, problems


def _scan(data, patterns):
    """[(label, count, first offset)] for every pattern that matches."""
    hits = []
    for label, rx in patterns:
        n, first = 0, None
        for m in rx.finditer(data):
            if first is None:
                first = m.start()
            n += 1
        if n:
            hits.append((label, n, first))
    return hits


def _fmt(where, check, hits):
    return ['%s: %s: %s x%d (first at 0x%x)' % (where, check, label, n, first)
            for label, n, first in hits]


def _unique(problems):
    """Drop repeats (an unreadable .gz is reported by both file checks)."""
    seen, out = set(), []
    for p in problems:
        if p not in seen:
            seen.add(p)
            out.append(p)
    return out


# ---- personal paths ---------------------------------------------------------

def account_names():
    """The local account name(s): getpass.getuser() and the basename of the
    home directory - usually the same, deduplicated case-insensitively."""
    cands = []
    try:
        cands.append(getpass.getuser())
    except Exception:       # no USERNAME/LOGNAME and no pwd entry
        pass
    home = os.path.expanduser('~')
    if home and home != '~':
        cands.append(os.path.basename(home.rstrip('/\\')))
    names = []
    for n in cands:
        if n and n.lower() not in [x.lower() for x in names]:
            names.append(n)
    return names


def _ci(text, enc):
    """Case-insensitive byte pattern for text in the given encoding (works for
    UTF-16LE, where re.IGNORECASE would not line up with the zero bytes)."""
    out = b''
    for ch in text:
        lo, up = ch.lower(), ch.upper()
        if lo != up:
            alts = sorted({lo.encode(enc), up.encode(enc)})
            out += b'(?:' + b'|'.join(re.escape(a) for a in alts) + b')'
        else:
            out += re.escape(ch.encode(enc))
    return out


def _sep(enc):
    # one or more / or \ - covers C:/..., C:\... and JSON's C:\\...
    return rb'[/\\]+' if enc == 'utf-8' else rb'(?:[/\\]\x00)+'


def _drive(enc):
    return rb'[A-Za-z]:' if enc == 'utf-8' else rb'[A-Za-z]\x00:\x00'


def _name_end(enc):
    return rb'(?![A-Za-z0-9])' if enc == 'utf-8' else rb'(?![A-Za-z0-9]\x00)'


_PATTERN_CACHE = {}


def personal_patterns(any_account=True):
    """(label, regex) pairs for the personal-path check.

    any_account adds the generic X:/Users/<anyone>/ form; it is meant for
    files this project builds, not for third-party archives."""
    key = bool(any_account)
    if key in _PATTERN_CACHE:
        return _PATTERN_CACHE[key]
    pats = []
    home = os.path.expanduser('~')
    for enc in ('utf-8', 'utf-16-le'):
        tag = '' if enc == 'utf-8' else ', UTF-16LE'
        sep, end = _sep(enc), _name_end(enc)
        for name in account_names():
            if len(name) >= 4 and name.lower() not in _GENERIC_ACCOUNTS:
                # the bare name covers every path form it can appear in
                pats.append(('account name %s%s' % (mask(name), tag),
                             re.compile(_ci(name, enc))))
                continue
            # too generic to search alone: only as a home directory
            # (/Users/<name> also matches C:/Users/<name>)
            for parent in ('Users', 'home'):
                pats.append(('home path .../%s/%s%s' % (parent, mask(name), tag),
                             re.compile(sep + _ci(parent, enc) + sep
                                        + _ci(name, enc) + end)))
            if home and home != '~':
                parts = [p for p in re.split(r'[/\\]+', home) if p]
                pats.append(('home directory%s' % tag,
                             re.compile(sep.join(_ci(p, enc) for p in parts)
                                        + end)))
        if any_account:
            for profiles in ('Users', 'Documents and Settings'):
                pats.append(('user-profile path %s%s' % (profiles, tag),
                             re.compile(_drive(enc) + sep + _ci(profiles, enc)
                                        + sep)))
    _PATTERN_CACHE[key] = pats
    return pats


def personal_hits(path, data=None, any_account=None):
    """[(label, count, first offset)] over all views of one file."""
    if data is None:
        data = _read(path)
    if any_account is None:
        any_account = not is_third_party(path)
    views, _ = _views(path, data)
    hits = []
    for suffix, view in views:
        hits += [(label + suffix, n, first) for label, n, first
                 in _scan(view, personal_patterns(any_account))]
    return hits


def check_personal_paths(path, data=None, any_account=None):
    """Account name / home path in one file (raw, and gunzipped for .gz)."""
    if data is None:
        data = _read(path)
    _, problems = _views(path, data)
    return problems + _fmt(shown(path), 'personal path',
                           personal_hits(path, data, any_account))


# ---- brand ------------------------------------------------------------------

def brand_hits(path, data=None):
    if data is None:
        data = _read(path)
    views, _ = _views(path, data)
    hits = []
    for suffix, view in views:
        hits += [(label + suffix, n, first) for label, n, first
                 in _scan(view, BRAND_PATTERNS)]
    return hits


def check_brand(path, data=None):
    """Brand names in one project-built file. Returns [] for files outside
    the brand scope (third-party archives, bootloader, boot_app0)."""
    if not brand_applies(path):
        return []
    if data is None:
        data = _read(path)
    _, problems = _views(path, data)
    return problems + _fmt(shown(path), 'brand', brand_hits(path, data))


def _content_checks(label, name, content):
    """personal + brand over one extracted file (raw, and gunzipped)."""
    problems = []
    views = [('', content)]
    if name.lower().endswith('.gz'):
        try:
            views.append((' (gunzipped)', gzip.decompress(content)))
        except _GZIP_ERRORS as e:
            problems.append('%s: cannot be gunzipped, content unchecked (%s)'
                            % (label, e))
    for suffix, view in views:
        for check, patterns in (('personal path', personal_patterns(True)),
                                ('brand', BRAND_PATTERNS)):
            problems += _fmt(label, check, [(lab + suffix, n, first) for
                                            lab, n, first in _scan(view, patterns)])
    return problems


# ---- data/ ------------------------------------------------------------------

def _walk_json(obj, path=''):
    if isinstance(obj, dict):
        for k, v in obj.items():
            p = '%s.%s' % (path, k) if path else str(k)
            yield p, str(k), v
            for item in _walk_json(v, p):
                yield item
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            for item in _walk_json(v, '%s[%d]' % (path, i)):
                yield item


def _has_value(v):
    if v is None or v == '':
        return False
    if isinstance(v, (dict, list)):
        return bool(v)
    return True


def check_config_json(path):
    """The factory config: blank WiFi credentials, no stored passwords."""
    where = shown(path)
    try:
        with io.open(path, encoding='utf-8') as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as e:
        return ['%s: not readable as JSON (%s)' % (where, e)]
    problems = []
    wifi = doc.get('wifi') if isinstance(doc, dict) else None
    if not isinstance(wifi, dict):
        problems.append('%s: has no "wifi" object' % where)
    else:
        for k in ('ssid', 'password'):
            if _has_value(wifi.get(k)):
                problems.append('%s: wifi.%s is not empty (%d characters) - '
                                'the factory config must carry no network'
                                % (where, k, len(str(wifi.get(k)))))
    # ap_pass / sta_pass are what an exported device config adds; any other
    # key that names a secret is refused as well. Matched by whole word, not
    # substring: "compass_north_up" is a real key. Values are never printed.
    for keypath, key, v in _walk_json(doc):
        if keypath in ('wifi.ssid', 'wifi.password'):
            continue
        words = {w.lower() for w in
                 re.split(r'[_\-.\s]+|(?<=[a-z])(?=[A-Z])', key) if w}
        if key.lower() in ('ap_pass', 'sta_pass') or words & _SECRET_WORDS:
            if _has_value(v):
                problems.append('%s: %s carries a value - exported device '
                                'config in data/?' % (where, keypath))
    return problems


def check_data_dir(data_dir=DATA_DIR, gzip_sync=True):
    """data/ before it is packed into littlefs.bin.

    gzip_sync=False skips the index.html.gz comparison (and its scan): before
    a build the .gz may legitimately lag behind, because extra_script.py
    regenerates it at the start of every pio run."""
    if not os.path.isdir(data_dir):
        return ['%s: missing' % shown(data_dir)]
    problems = []
    for e in sorted(os.listdir(data_dir)):
        p = os.path.join(data_dir, e)
        if e not in DATA_ALLOWED or not os.path.isfile(p):
            problems.append('%s: not allowed in data/ - littlefs.bin packs the '
                            'whole directory (allowed: %s)'
                            % (shown(p), ', '.join(DATA_ALLOWED)))
    cfg = os.path.join(data_dir, 'config.json')
    if os.path.isfile(cfg):
        problems += check_config_json(cfg)
    else:
        problems.append('%s: missing' % shown(cfg))

    html = os.path.join(data_dir, 'index.html')
    gz = html + '.gz'
    if gzip_sync:
        if not (os.path.isfile(html) and os.path.isfile(gz)):
            problems.append('%s: index.html and index.html.gz must both exist'
                            % shown(data_dir))
        else:
            try:
                same = gzip.decompress(_read(gz)) == _read(html)
            except _GZIP_ERRORS:
                same = False
            if not same:
                problems.append('%s: does not decompress to index.html - the '
                                'device serves the .gz; run any pio build '
                                '(extra_script.py regenerates it)' % shown(gz))
            # N2K-UPDATE.md 11.6: it has to fit one update region, or the
            # display can neither cache it nor take it over NMEA 2000.
            if os.path.getsize(gz) > WEBUI_MAX:
                problems.append('%s: %d bytes, larger than one update region (%d) - '
                                'it could not be installed over NMEA 2000'
                                % (shown(gz), os.path.getsize(gz), WEBUI_MAX))

    for e in DATA_ALLOWED:
        p = os.path.join(data_dir, e)
        if not os.path.isfile(p) or (e.endswith('.gz') and not gzip_sync):
            continue
        problems += check_personal_paths(p, any_account=True)
        problems += check_brand(p)
    return _unique(problems)


# ---- littlefs.bin -----------------------------------------------------------

def _parse_size(s):
    s = s.strip()
    mult = 1
    if s[-1:] in ('K', 'k'):
        mult, s = 1024, s[:-1]
    elif s[-1:] in ('M', 'm'):
        mult, s = 1024 * 1024, s[:-1]
    return int(s, 0) * mult


def littlefs_partition_size(csv=PARTITIONS_CSV):
    """Size of the littlefs (data/spiffs) partition, or None."""
    try:
        lines = io.open(csv, encoding='utf-8').read().splitlines()
    except OSError:
        return None
    for line in lines:
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(',')]
        if len(cols) >= 5 and (cols[0] == 'littlefs'
                               or cols[2] in ('spiffs', 'littlefs')):
            try:
                return _parse_size(cols[4])
            except ValueError:
                return None
    return None


def littlefs_files(image_bytes, block_size=LFS_BLOCK):
    """{relative name: bytes} of every file in a LittleFS image."""
    from littlefs import LittleFS   # pip install littlefs-python
    fs = LittleFS(block_size=block_size,
                  block_count=len(image_bytes) // block_size, mount=False)
    fs.context.buffer = bytearray(image_bytes)
    fs.mount()
    files = {}
    for root, _dirs, names in fs.walk('/'):
        for n in names:
            p = root.rstrip('/') + '/' + n
            with fs.open(p, 'rb') as fh:
                files[p.lstrip('/')] = fh.read()
    return files


def check_littlefs(image, data_dir=DATA_DIR, partitions_csv=PARTITIONS_CSV,
                   compare=True):
    """Extract the image; its files must equal data/ byte for byte, and pass
    the personal and brand checks once decoded (the .gz included)."""
    where = shown(image)
    try:
        import littlefs   # noqa: F401  (availability only)
    except ImportError:
        return ['%s: cannot be checked - littlefs-python is not installed '
                '(pip install littlefs-python)' % where]
    data = _read(image)
    problems = []
    part = littlefs_partition_size(partitions_csv)
    if len(data) % LFS_BLOCK:
        problems.append('%s: size %d is not a multiple of the %d-byte block'
                        % (where, len(data), LFS_BLOCK))
    if part is not None and part != len(data):
        problems.append('%s: %d bytes, but the littlefs partition in %s is %d'
                        % (where, len(data), os.path.basename(partitions_csv),
                           part))
    try:
        files = littlefs_files(data[:len(data) - len(data) % LFS_BLOCK])
    except Exception as e:      # littlefs raises its own error types
        return problems + ['%s: cannot be mounted as LittleFS (block size %d): '
                           '%s' % (where, LFS_BLOCK, e)]

    for name in sorted(files):
        problems += _content_checks('%s:/%s' % (where, name), name, files[name])

    if compare:
        if not os.path.isdir(data_dir):
            return problems + ['%s: missing, image not compared' % shown(data_dir)]
        disk = {e for e in os.listdir(data_dir)
                if os.path.isfile(os.path.join(data_dir, e))}
        for n in sorted(disk - set(files)):
            problems.append('%s: data/%s is not in the image - stale image, '
                            'rebuild with "pio run -t buildfs"' % (where, n))
        for n in sorted(set(files) - disk):
            problems.append('%s: /%s is in the image but not in data/'
                            % (where, n))
        for n in sorted(disk & set(files)):
            if files[n] != _read(os.path.join(data_dir, n)):
                problems.append('%s: /%s differs from data/%s - stale image, '
                                'rebuild with "pio run -t buildfs"'
                                % (where, n, n))
    return problems


# ---- freshness --------------------------------------------------------------

def newest_input(rels, root=ROOT):
    """(mtime, repo-relative path) of the newest file among rels, or None."""
    best = None
    for rel in rels:
        p = os.path.join(root, rel)
        cands = []
        if os.path.isfile(p):
            cands.append(p)
        elif os.path.isdir(p):
            for base, dirs, names in os.walk(p):
                dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
                cands += [os.path.join(base, n) for n in names]
        for c in cands:
            name = os.path.relpath(c, root).replace('\\', '/')
            if name in FRESH_IGNORE:
                continue
            t = os.path.getmtime(c)
            if best is None or t > best[0]:
                best = (t, name)
    return best


def check_freshness(artefacts, root=ROOT):
    """Every artefact with a rule in FRESH_RULES must be newer than its
    newest input; the others are skipped (see the module docstring)."""
    problems, cache = [], {}
    for path in artefacts:
        rule = FRESH_RULES.get(os.path.basename(path).lower())
        if not rule or not os.path.isfile(path):
            continue
        if rule not in cache:
            cache[rule] = newest_input(rule, root)
        newest = cache[rule]
        t = os.path.getmtime(path)
        if newest and t <= newest[0]:
            problems.append('%s: stale - built %s, but %s changed %s; rebuild '
                            '(without --skip-build)'
                            % (shown(path), _when(t), newest[1], _when(newest[0])))
    return problems


# ---- version ----------------------------------------------------------------

def check_version(firmware, release_version=None, root=ROOT, data=None):
    """firmware.bin contains FW_VERSION as a whole string (so "1.2.0" is not
    satisfied by a "1.2.0-dev" firmware), and a given release version is
    the same version with or without its leading 'v'."""
    try:
        fw = fw_version(root)
    except (OSError, ValueError) as e:
        return ['src/Version.h: %s' % e]
    problems = []
    if release_version is not None:
        rv = release_version[1:] if release_version[:1] in ('v', 'V') else release_version
        if rv != fw:
            problems.append('release version %s does not match FW_VERSION "%s" '
                            'in src/Version.h - set it there and rebuild'
                            % (release_version, fw))
    if data is None:
        data = _read(firmware)
    token = (rb'(?<![0-9A-Za-z.+\-])' + re.escape(fw.encode('ascii'))
             + rb'(?![0-9A-Za-z.+\-])')
    if not re.search(token, data):
        problems.append('%s: does not contain FW_VERSION "%s" - built from '
                        'other sources?' % (shown(firmware), fw))
    return problems


# ---- firmware descriptor (docs/N2K-UPDATE.md 8.1) ---------------------------

def check_descriptor(firmware, board=None, root=ROOT, data=None, quiet=False):
    """firmware.bin carries a descriptor that matches its board and
    src/Version.h. board: the release folder key ('4', '7b', '5b'), or None
    to take the board from the descriptor and check it is consistent."""
    if data is None:
        data = _read(firmware)
    where = shown(firmware)
    problems = []
    if len(data) < DESC_OFFSET + 64:
        return ['%s: too short for a firmware image' % where]
    if data[0] != 0xE9:
        problems.append('%s: no 0xE9 at offset 0 - not an ESP32 image' % where)
    if int.from_bytes(data[0x20:0x24], 'little') != 0xABCD5432:
        problems.append('%s: no app description at 0x20' % where)
    else:
        # IDF's app description: version and project name are written into
        # the ELF after the link (tools/app_desc.py, extra_script.py); an
        # image without that still says what the framework's libs said.
        here = os.path.dirname(os.path.abspath(__file__))
        if here not in sys.path:
            sys.path.insert(0, here)
        import app_desc   # tools/, next to this module
        app_ver = data[0x30:0x50].split(b'\0')[0].decode('ascii', 'replace')
        app_proj = data[0x50:0x70].split(b'\0')[0].decode('ascii', 'replace')
        try:
            if app_ver != fw_version(root) or app_proj != app_desc.PROJECT_NAME:
                problems.append('%s: app description at 0x20 says version "%s", '
                                'project "%s" - expected FW_VERSION and "%s" '
                                '(tools/app_desc.py runs after every link; was '
                                'the ELF relinked?)'
                                % (where, app_ver, app_proj, app_desc.PROJECT_NAME))
        except (OSError, ValueError):
            pass                            # reported below with the descriptor

    d = data[DESC_OFFSET:DESC_OFFSET + 64]
    if d[:8] != b'NPFWDESC':
        problems.append('%s: no firmware descriptor at 0x120 - this image cannot be '
                        'updated over NMEA 2000, nor offered by the updater '
                        '(src/n2kupdate/FwDesc.c, -Wl,-u,np_fw_desc)' % where)
        return problems
    desc_version, board_id = d[8], d[9]
    desc_size = int.from_bytes(d[10:12], 'little')
    panel_w = int.from_bytes(d[12:14], 'little')
    panel_h = int.from_bytes(d[14:16], 'little')
    proto, flags = d[16], d[17]
    version = d[0x14:0x24].split(b'\0')[0].decode('ascii', 'replace')
    name = d[0x24:0x40].split(b'\0')[0].decode('ascii', 'replace')
    if desc_version < 1 or desc_size < 64:
        problems.append('%s: descriptor version %d / size %d' % (where, desc_version, desc_size))
    if proto < 1:
        problems.append('%s: descriptor names no update protocol' % where)
    if not flags & 0x01:
        problems.append('%s: OTAGUARD flag not set in the descriptor' % where)
    expect = DESC_BOARDS.get(board_id)
    if expect is None:
        problems.append('%s: unknown board id %d in the descriptor' % (where, board_id))
    else:
        key, bname, w, h = expect
        if board is not None and board != key:
            problems.append('%s: descriptor says board %d (%s), released as %s/'
                            % (where, board_id, bname, board))
        if name != bname or (panel_w, panel_h) != (w, h):
            problems.append('%s: descriptor board %d is "%s" %dx%d, expected "%s" %dx%d'
                            % (where, board_id, name, panel_w, panel_h, bname, w, h))
    try:
        fw = fw_version(root)
        if version != fw:
            problems.append('%s: descriptor version "%s", src/Version.h says "%s"'
                            % (where, version, fw))
    except (OSError, ValueError) as e:
        problems.append('src/Version.h: %s' % e)
    if not quiet:
        print('  %s: board %d (%s), version %s, image id %s'
              % (where, board_id, name, version, data[0xB0:0xB8].hex()))
    return problems


# ---- test builds ------------------------------------------------------------

def check_test_build(image, data=None):
    """An image (firmware.bin, or a merged image holding it) built with a
    test flag: its fixed boot line is in the bytes."""
    if data is None:
        data = _read(image)
    problems = []
    for marker, flag, why in TESTBUILD_MARKERS:
        at = data.find(marker)
        if at >= 0:
            problems.append('%s: test build (%s) at 0x%x - %s; rebuild without it'
                            % (shown(image), flag, at, why))
    return problems


# ---- 7B/5B: software AES, and the libs it takes ------------------------------

def descriptor_board(data):
    """The release folder key ('4', '7b', '5b') named by the firmware
    descriptor at 0x120, or None without a (known) descriptor."""
    d = data[DESC_OFFSET:DESC_OFFSET + 16]
    if len(d) < 16 or d[:8] != b'NPFWDESC':
        return None
    entry = DESC_BOARDS.get(d[9])
    return entry[0] if entry else None


def check_hw_aes(firmware, board=None, data=None):
    """A 7B/5B firmware.bin that still carries IDF's hardware AES driver.
    board: the release folder key; None takes it from the descriptor. The
    4-inch keeps hardware AES (IDF 4.4, its TLS records in DRAM)."""
    if data is None:
        data = _read(firmware)
    if board is None:
        board = descriptor_board(data)
    if board not in ('7b', '5b'):
        return []
    m = HWAES_TAGS.search(data)
    if not m:
        return []
    return ['%s: hardware AES driver linked (log tag "%s" at 0x%x) - '
            'platformio.ini sets CONFIG_MBEDTLS_HARDWARE_AES=n for the 7B/5B, '
            'because with it the tide download breaks off with "esp-aes: '
            'Failed to allocate memory". The Arduino IDF libs this was linked '
            'against were compiled from an older custom_sdkconfig: rebuild '
            'them in that PlatformIO core (check_hybrid_libs says how), then '
            'the image'
            % (shown(firmware), m.group(1).decode('ascii'), m.start(1))]


def custom_sdkconfig(ini=None, env=HYBRID_ENV):
    """{CONFIG_name: value} of custom_sdkconfig in platformio.ini, as
    pioarduino merges it ('y', 'n', a number or a quoted string). Full-line
    comments inside the value are skipped, as PlatformIO's parser does."""
    ini = ini or os.path.join(ROOT, 'platformio.ini')
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    with io.open(ini, encoding='utf-8') as f:
        cp.read_file(f)
    opts = {}
    for line in cp.get('env:' + env, 'custom_sdkconfig', fallback='').splitlines():
        m = re.match(r'\s*(CONFIG_\w+)=(.*?)\s*$', line)
        if m:
            opts[m.group(1)] = m.group(2)
    return opts


def sdkconfig_values(path):
    """{CONFIG_name: value} of an sdkconfig file; '# CONFIG_X is not set'
    reads as 'n'. The first entry of a name wins (the compatibility block
    at the end repeats old names)."""
    vals = {}
    with io.open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = (re.match(r'(CONFIG_\w+)=(.*?)\s*$', line)
                 or re.match(r'# (CONFIG_\w+) is not set\s*$', line))
            if m and m.group(1) not in vals:
                vals[m.group(1)] = m.group(2) if m.lastindex == 2 else 'n'
    return vals


def check_hybrid_libs(core_dir, ini=None, env=HYBRID_ENV):
    """Before a 7B/5B build in PlatformIO core core_dir: were its Arduino IDF
    libs compiled with every option custom_sdkconfig lists? An option the
    libs' sdkconfig does not name at all is not visible in this IDF
    configuration (CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM with static TX
    buffers, for one) and cannot be compared."""
    libs = os.path.join(core_dir, HYBRID_LIBS)
    cfg = os.path.join(libs, 'sdkconfig')
    if not os.path.isfile(cfg):
        return []       # never built from a list: the next build compiles them
    try:
        want = custom_sdkconfig(ini, env)
    except (OSError, configparser.Error) as e:
        return ['platformio.ini: custom_sdkconfig of env:%s unreadable: %s' % (env, e)]
    have = sdkconfig_values(cfg)
    differ = ['%s=%s (the libs have %s)' % (k, v, have[k])
              for k, v in sorted(want.items())
              if k in have and have[k].lower() != v.lower()]
    if not differ:
        return []
    return ['%s: the Arduino IDF libs of this PlatformIO core were compiled '
            'from another custom_sdkconfig than platformio.ini lists - %s. '
            'pioarduino will not notice: it compares only the hash in the '
            'first line of sdkconfig.defaults, which every core building this '
            'project shares. Delete %s, and the next build in this core '
            'compiles the libs from the list (about 30 min); then run again'
            % (shown(libs), '; '.join(differ), shown(cfg))]


# ---- combined entry points for the release scripts --------------------------

def check_build_output(images, release_version=None, data_dir=DATA_DIR, board=None):
    """All checks on one board's flash images before they are packaged.

    images: {'bootloader.bin': path, 'partitions.bin': path, ...}
    board:  the release folder key ('4', '7b', '5b') for the descriptor
            check; None takes the board from the descriptor itself."""
    problems = []
    for name in sorted(images):
        path = images[name]
        if not os.path.isfile(path):
            problems.append('%s: missing - did the build run?' % shown(path))
            continue
        data = _read(path)
        problems += check_personal_paths(path, data, any_account=True)
        if name.lower() not in BRAND_EXEMPT_NAMES:
            problems += check_brand(path, data)
        if name.lower().endswith('.bin'):
            problems += check_test_build(path, data)
    lfs = images.get('littlefs.bin')
    if lfs and os.path.isfile(lfs):
        problems += check_littlefs(lfs, data_dir)
    problems += check_freshness([p for p in images.values() if os.path.isfile(p)])
    fw = images.get('firmware.bin')
    if fw and os.path.isfile(fw):
        problems += check_version(fw, release_version)
        problems += check_descriptor(fw, board)
        problems += check_hw_aes(fw, board)
        if board in ('7b', '5b'):
            import psram_budget   # tools/, next to this module
            elf = os.path.join(os.path.dirname(fw), 'firmware.elf')
            problems += psram_budget.problems(elf, shown(elf))
            psram_budget.report(elf, tag='  %s psram:' % board)
    return _unique(problems)


def collect_files(path, all_files=False):
    """A file, or every file under a directory (build intermediates skipped
    unless all_files)."""
    if os.path.isfile(path):
        return [path]
    out = []
    for base, dirs, names in os.walk(path):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for n in sorted(names):
            if all_files or not n.lower().endswith(SKIP_EXT):
                out.append(os.path.join(base, n))
    return out


def check_tree(top):
    """personal (everything) + brand (project-built files) over an
    assembled output folder - the last gate before it is zipped or moved
    into place."""
    problems = []
    for p in collect_files(top, all_files=True):
        data = _read(p)
        problems += check_personal_paths(p, data)
        problems += check_brand(p, data)
    return _unique(problems)


def report(problems, what):
    """Print the outcome of one gate; True when clean."""
    problems = _unique(problems)
    if not problems:
        print('  release checks: %s - OK' % what)
        return True
    print('\nRELEASE CHECK FAILED: %s (%d problem(s))' % (what, len(problems)))
    for p in problems:
        print('  - ' + p)
    return False


# ---- report-only CLI --------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Report-only release checks (never modifies anything). '
                    'Exit status 1 when a problem is found.')
    ap.add_argument('paths', nargs='*',
                    help='files or directories to scan (default: data/ only)')
    ap.add_argument('--checks', default='all',
                    help='comma-separated subset of %s (default: all)'
                         % ','.join(ALL_CHECKS))
    ap.add_argument('--data', default=DATA_DIR,
                    help='data/ directory that LittleFS images are compared '
                         'with, and that the data check examines')
    ap.add_argument('--release-version', default=None,
                    help='also require this version to equal FW_VERSION')
    ap.add_argument('--all-files', action='store_true',
                    help='also scan build intermediates (%s)' % ' '.join(SKIP_EXT))
    a = ap.parse_args(argv)

    checks = set(ALL_CHECKS) if a.checks == 'all' else \
        {c.strip() for c in a.checks.split(',') if c.strip()}
    unknown = checks - set(ALL_CHECKS)
    if unknown:
        ap.error('unknown check(s): %s' % ', '.join(sorted(unknown)))

    print('Account name(s) searched: %s'
          % (', '.join(mask(n) for n in account_names()) or '(none found)'))

    # per check: [problems, files with problems, hits]; per pattern: hits
    stats = dict((c, [0, 0, 0]) for c in ALL_CHECKS)
    by_label = dict((c, {}) for c in ('personal', 'brand'))
    problems = []

    def add(check, found, hits=()):
        for label, n, _ in hits:
            by_label[check][label] = by_label[check].get(label, 0) + n
        if found:
            problems.extend(found)
            stats[check][0] += len(found)
            stats[check][1] += 1
            stats[check][2] += sum(h[1] for h in hits)

    if 'data' in checks and (not a.paths or a.checks != 'all'):
        add('data', check_data_dir(a.data))

    files = []
    for p in a.paths:
        if not os.path.exists(p):
            ap.error('no such file or directory: %s' % p)
        files += collect_files(p, a.all_files)

    for p in files:
        data = _read(p)
        name = os.path.basename(p).lower()
        if 'personal' in checks:
            hits = personal_hits(p, data)
            _, gz_problems = _views(p, data)
            add('personal', gz_problems + _fmt(shown(p), 'personal path', hits),
                hits)
        if 'brand' in checks and brand_applies(p):
            hits = brand_hits(p, data)
            add('brand', _fmt(shown(p), 'brand', hits), hits)
        if 'littlefs' in checks and name.startswith('littlefs') and name.endswith('.bin'):
            add('littlefs', check_littlefs(p, a.data))
        if 'fresh' in checks:
            add('fresh', check_freshness([p]))
        if 'version' in checks and name == 'firmware.bin':
            add('version', check_version(p, a.release_version, data=data))
        if 'descriptor' in checks and name == 'firmware.bin':
            add('descriptor', check_descriptor(p, data=data))
        if 'hwaes' in checks and name == 'firmware.bin':
            add('hwaes', check_hw_aes(p, data=data))
        if 'testbuild' in checks and name.endswith('.bin'):
            add('testbuild', check_test_build(p, data=data))

    for line in problems:
        print('  - ' + line)
    print('\nSummary: %d file(s) scanned' % len(files))
    for c in ALL_CHECKS:
        if (c not in checks or (c == 'data' and a.paths and a.checks == 'all')
                or (c != 'data' and not files)):
            print('  %-9s not run' % c)
            continue
        n, nfiles, hits = stats[c]
        extra = ', %d hit(s)' % hits if c in ('personal', 'brand') else ''
        print('  %-9s %d problem(s) in %d file(s)%s' % (c, n, nfiles, extra))
        for label, count in sorted(by_label.get(c, {}).items()):
            print('  %-9s   %s: %d' % ('', label, count))
    print('RESULT: %s' % ('clean' if not problems else
                          '%d problem(s)' % len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
