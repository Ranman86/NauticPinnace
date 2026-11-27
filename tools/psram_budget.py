#!/usr/bin/env python3
"""PSRAM budget of the 7B and 5B images, checked from firmware.elf.

On these boards the firmware's code and read-only data run from PSRAM
(CONFIG_SPIRAM_XIP_FROM_PSRAM), in whole 64 KB pages, and the rest of the 8 MB
has to hold the canvas arena and the panel's 1.2 MB frame buffer. In November
2026 that left a few dozen KB: a build ~10 KB larger took one code page more
and the panel stayed dark. setup() now holds the frame buffer's room first
and lets the arena shrink instead (src/PsramArena.cpp), so running out costs
the screens built last their canvas - still a defect, just a visible one
instead of a dead display. This says before flashing how close a build is:

    python tools/psram_budget.py <build dir>/firmware.elf

It runs after every link (extra_script.py) and in the release gate
(tools/release_checks.py), which refuses an image whose arena would come out
short. Images without the budget symbol (the 4-inch board) are skipped.

What it reads from the ELF, so nothing here can drift from the firmware:
  _instruction_reserved_start/_end, _rodata_reserved_start/_end
            the code and rodata that IDF copies into PSRAM: the flash pages
            the cache maps for them (components/esp_psram/xip_impl/
            mmu_psram_flash.c on the S3), i.e. their span rounded out to
            64 KB pages. The boot log's heap total agrees: 6,029,312 bytes
            = 8 MB - 36 pages for a build with 22 + 14.
  np_psram_budget
            what setup() asks for: arena, frame buffer room, PSRAM kept free
            (src/main.cpp)

What it cannot read and takes as measured, HEAP_OVERHEAD below: the PSRAM
the heap itself and IDF/Arduino take before setup() runs. The boot log line
"[Arena] PSRAM heap: ..., N free before the arena" gives it again.

The same images must also carry the panic wrap, PANIC_WRAP_SYMBOL below: the
panic handler's RTC watchdog re-arm goes through src/LoopWatchdog.cpp
(-Wl,--wrap=esp_panic_handler_enable_rtc_wdt in platformio.ini). --wrap gives
no error when it matches nothing - a renamed or inlined IDF function, a lost
flag - and --gc-sections then drops the unused wrapper, so a 7B/5B image
without the defined symbol has lost the wrap. The post-link step fails the
build then, and the release gate refuses the image.
"""

import os
import struct
import sys

PSRAM_BYTES = 8 * 1024 * 1024    # both boards: 8 MB octal PSRAM
PAGE = 0x10000                   # CONFIG_MMU_PAGE_SIZE

# PSRAM missing from the heap's free total before setup() allocates anything:
# 8 MB - XIP pages - "free before the arena" from the boot log. TLSF's own
# control block plus what IDF and the Arduino core allocate there first.
# Measured on the 7B, 03.11.2026, with 36 XIP pages: "PSRAM heap: 6029312
# bytes, 6025396 free before the arena".
HEAP_OVERHEAD = 3916

# ESP-IDF 5 heap, as configured for these boards (sdkconfig.waveshare_esp32s3_7b):
TLSF_SL_LOG2 = 5     # size classes are 1/32 of a power of two
POISON = 12          # CONFIG_HEAP_POISONING_LIGHT: head 8 + tail 4 per block
BLOCK_HDR = 4        # TLSF's per-block size word
ALIGN_SLACK = 40     # an aligned allocation's leading gap, at most align + header

BUDGET_SYMBOL = 'np_psram_budget'
BUDGET_TAG = 0x50534254          # "PSBT", the array's last word
PANIC_WRAP_SYMBOL = '__wrap_esp_panic_handler_enable_rtc_wdt'   # src/LoopWatchdog.cpp
SHN_UNDEF = 0


def tlsf_class_up(n):
    """The size TLSF rounds a request up to - and, for a plain malloc, the
    size it then occupies."""
    rnd = 1 << (n.bit_length() - 1 - TLSF_SL_LOG2)
    return (n + rnd - 1) & ~(rnd - 1)


def _align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def _align_down(v, a):
    return v & ~(a - 1)


def read_elf(path):
    """Symbol table of a 32-bit little-endian ELF, plus a reader for the bytes
    behind a symbol. Hand-rolled so the check needs no pyelftools wherever the
    release scripts happen to run."""
    with open(path, 'rb') as fh:
        data = fh.read()
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise ValueError('not a 32-bit little-endian ELF')
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2E)
    secs = [struct.unpack_from('<10I', data, shoff + i * shentsize) for i in range(shnum)]
    syms = {}
    for s in secs:
        if s[1] != 2:                      # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name_off, value, size, _info, _other, shndx = struct.unpack_from('<IIIBBH', data, off)
            end = data.index(b'\0', strtab[4] + name_off)
            syms[data[strtab[4] + name_off:end].decode('ascii', 'replace')] = (value, size, shndx)

    def contents(name, n):
        value, _size, shndx = syms[name]
        sec = secs[shndx]
        if sec[1] == 8:                    # SHT_NOBITS: no bytes in the file
            raise ValueError('%s has no contents in the ELF' % name)
        at = sec[4] + value - sec[3]
        return data[at:at + n]

    return syms, contents


def budget(elf):
    """The numbers of one image, or None when it carries no budget."""
    syms, contents = read_elf(elf)
    if BUDGET_SYMBOL not in syms:
        return None
    want, room, keep, tag = struct.unpack('<4I', contents(BUDGET_SYMBOL, 16))
    if tag != BUDGET_TAG:
        raise ValueError('%s does not end in the expected tag - layout changed?'
                         % BUDGET_SYMBOL)

    def segment(prefix):
        start = syms[prefix + '_reserved_start'][0]
        end = syms[prefix + '_reserved_end'][0]
        pages = (_align_up(end, PAGE) - _align_down(start, PAGE)) // PAGE
        return pages, _align_up(end, PAGE) - end

    text_pages, text_left = segment('_instruction')
    ro_pages, ro_left = segment('_rodata')
    heap_free = PSRAM_BYTES - (text_pages + ro_pages) * PAGE - HEAP_OVERHEAD

    # Mirrors PsramArena::initLeavingRoom(): the arena as an aligned block (cut
    # to the bytes asked for), the frame buffer's room as a plain malloc (its
    # whole size class), and what has to stay free besides.
    arena = want + POISON + BLOCK_HDR + ALIGN_SLACK
    room_block = tlsf_class_up(room + POISON) + BLOCK_HDR
    spare = heap_free - arena - room_block - keep
    return {
        'text_pages': text_pages, 'text_left': text_left,
        'rodata_pages': ro_pages, 'rodata_left': ro_left,
        'heap_free': heap_free, 'arena': want, 'room': room_block, 'keep': keep,
        'spare': spare,
        'panic_wrap': _wrap_defined(syms),
    }


def _wrap_defined(syms):
    # defined in the image, not merely referenced
    wrap = syms.get(PANIC_WRAP_SYMBOL)
    return wrap is not None and wrap[2] != SHN_UNDEF


def panic_wrap_problem(elf):
    """The post-link gate (extra_script.py): why the image fails, or None when
    it carries the wrap or is no 7B/5B image (no budget symbol). Read on its
    own, not through budget(): a budget that cannot be read - a changed
    layout, a missing linker symbol - must not let a missing wrap through."""
    try:
        syms, _contents = read_elf(elf)
    except (OSError, ValueError, struct.error) as e:
        return 'firmware.elf unreadable (%s), the panic wrap cannot be checked' % _why(e)
    if BUDGET_SYMBOL not in syms or _wrap_defined(syms):
        return None
    return ("%s is not in the image - the panic handler's RTC watchdog re-arm is not "
            "wrapped" % PANIC_WRAP_SYMBOL)


def lines(b):
    out = ['XIP from PSRAM: code %d pages (%s bytes left in the last), rodata %d pages (%s left)'
           % (b['text_pages'], format(b['text_left'], ','), b['rodata_pages'],
              format(b['rodata_left'], ','))]
    out.append('heap ~%s free before the arena: arena %s + frame buffer %s + kept free %s'
               % tuple(format(b[k], ',') for k in ('heap_free', 'arena', 'room', 'keep')))
    if b['spare'] >= 0:
        out.append('-> %s bytes to spare: %d more code/rodata page(s) fit before the arena shrinks'
                   % (format(b['spare'], ','), b['spare'] // PAGE))
    else:
        out.append('!!! arena about %s bytes SHORT: the frame buffer still fits, but the '
                   'screens built last will have no canvas' % format(-b['spare'], ','))
    if b['panic_wrap']:
        out.append('panic handler RTC watchdog re-arm: wrapped (src/LoopWatchdog.cpp)')
    else:
        out.append('!!! %s missing: the panic handler re-arms the RTC watchdog unwrapped '
                   '(-Wl,--wrap in platformio.ini, src/LoopWatchdog.cpp)' % PANIC_WRAP_SYMBOL)
    return out


def _why(e):
    # An OSError's text carries the full path, which the release report keeps
    # out (it masks the home directory); the reason alone is enough there.
    if isinstance(e, OSError):
        return e.strerror or type(e).__name__
    return '%s: %s' % (type(e).__name__, e)


def report(elf, tag='[psram]'):
    """Print the budget of one image (extra_script.py, after the link)."""
    try:
        b = budget(elf)
    except (OSError, ValueError, KeyError) as e:
        print('%s budget check failed: %s' % (tag, _why(e)))
        return None
    if b is None:
        return None
    for ln in lines(b):
        print('%s %s' % (tag, ln))
    return b


def problems(elf, label=None):
    """Release gate: a list of problems, empty when the full arena fits.
    label: how to name the file in them (the release report masks paths)."""
    label = label or elf
    if not os.path.isfile(elf):
        return ['%s: missing - the PSRAM budget is read from it' % label]
    try:
        b = budget(elf)
    except (OSError, ValueError, KeyError) as e:
        return ['%s: PSRAM budget unreadable (%s)' % (label, _why(e))]
    if b is None:
        return ['%s: no %s symbol - not a 7B/5B image?' % (label, BUDGET_SYMBOL)]
    out = []
    if b['spare'] < 0:
        out.append('%s: PSRAM budget %s bytes short - the canvas arena would come out '
                   'smaller than asked (tools/psram_budget.py)'
                   % (label, format(-b['spare'], ',')))
    if not b['panic_wrap']:
        out.append('%s: no %s - the panic handler would re-arm the RTC watchdog without '
                   'feeding it first (platformio.ini --wrap, src/LoopWatchdog.cpp)'
                   % (label, PANIC_WRAP_SYMBOL))
    return out


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: python tools/psram_budget.py <firmware.elf>')
    b = report(sys.argv[1], tag='')
    if b is None:
        sys.exit('%s: no PSRAM budget in this image' % sys.argv[1])
    sys.exit(1 if b['spare'] < 0 or not b['panic_wrap'] else 0)
