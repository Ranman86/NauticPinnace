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
            Third-party material is exempt: the bundled esptool source, for
            one, uses the same letters in an unrelated Xtensa config macro.
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

Paths in the output are shown relative to the repository, or with the home
directory replaced by "~", and the account name only ever appears masked -
the report itself is meant to be pasteable.
"""
import argparse
import codecs
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

ALL_CHECKS = ('personal', 'brand', 'data', 'littlefs', 'fresh', 'version')

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
ARCHIVE_EXT = ('.zip', '.tar.gz', '.tgz', '.whl', '.exe')
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
    parts = os.path.abspath(path).replace('\\', '/').split('/')
    name = parts[-1].lower()
    if name.endswith(ARCHIVE_EXT):
        return True
    # esptool-<ver>/ in a release package: esptool's own source, notices and
    # the source distributions of what is frozen into esptool.exe
    return any(p.lower().startswith('esptool-') for p in parts[:-1])


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


# ---- combined entry points for the release scripts --------------------------

def check_build_output(images, release_version=None, data_dir=DATA_DIR):
    """All checks on one board's flash images before they are packaged.

    images: {'bootloader.bin': path, 'partitions.bin': path, ...}"""
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
    lfs = images.get('littlefs.bin')
    if lfs and os.path.isfile(lfs):
        problems += check_littlefs(lfs, data_dir)
    problems += check_freshness([p for p in images.values() if os.path.isfile(p)])
    fw = images.get('firmware.bin')
    if fw and os.path.isfile(fw):
        problems += check_version(fw, release_version)
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
