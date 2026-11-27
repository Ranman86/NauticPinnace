#!/usr/bin/env python3
"""Version and project name in ESP-IDF's app description of firmware.elf.

Every image carries IDF's esp_app_desc_t at image offset 0x20; esptool
image_info shows its version and project name (IDF's start-up log would
print them at log level Info; these builds log errors only). Neither came from this project: the 4-inch got the strings of
the precompiled Arduino libs ("esp-idf: v4.4.7 38eeba213a",
"arduino-lib-builder"), the 7B and 5B those of the moment their IDF libs were
compiled - `git describe` of the working tree then ("v1.2.0-dirty", on the
1.3.0 release) and the first word of the project folder's name ("Waveshare").
The structure is compiled into those libs, so a build of this project never
touched it.

patch() writes FW_VERSION from src/Version.h and "NauticPinnace" into the
ELF right after the link (extra_script.py), before esptool turns it into
firmware.bin and hashes it into the image id - so the bin, its SHA-256 and the
ELF a core dump is read against all agree. The firmware itself reads neither
field (the update code only takes app_elf_sha256 and the structure's
address); the firmware descriptor at 0x120 (docs/N2K-UPDATE.md 8.1) stays
what identifies board and version.

Layout (IDF 4.4 and 5.5 alike): magic_word 0x00, secure_version 0x04,
reserv1 0x08, version[32] 0x10, project_name[32] 0x30, time[16] 0x50,
date[16] 0x60, idf_ver[32] 0x70, app_elf_sha256[32] 0x90.
"""
import struct

APP_DESC_MAGIC = 0xABCD5432
PROJECT_NAME = 'NauticPinnace'
VERSION_OFF, PROJECT_OFF, FIELD_LEN = 0x10, 0x30, 32


def _symbol_offset(data, name):
    """File offset of the bytes behind a symbol of a 32-bit little-endian ELF."""
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise ValueError('not a 32-bit little-endian ELF')
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2E)
    secs = [struct.unpack_from('<10I', data, shoff + i * shentsize) for i in range(shnum)]
    want = name.encode('ascii')
    for s in secs:
        if s[1] != 2:                      # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name_off, value, _size, _info, _other, shndx = struct.unpack_from('<IIIBBH', data, off)
            start = strtab[4] + name_off
            if data[start:start + len(want) + 1] != want + b'\0':
                continue
            sec = secs[shndx]
            if sec[1] == 8:                # SHT_NOBITS
                raise ValueError('%s has no bytes in the ELF' % name)
            return sec[4] + value - sec[3]
    raise ValueError('no symbol %s in the ELF' % name)


def _field(text):
    raw = text.encode('ascii')
    if len(raw) >= FIELD_LEN:
        raise ValueError('"%s" does not fit the %d-byte field' % (text, FIELD_LEN))
    return raw + b'\0' * (FIELD_LEN - len(raw))


def read(elf):
    """(version, project_name) of the app description in an ELF."""
    with open(elf, 'rb') as fh:
        data = fh.read()
    at = _symbol_offset(data, 'esp_app_desc')
    def s(off):
        return data[at + off:at + off + FIELD_LEN].split(b'\0')[0].decode('ascii', 'replace')
    return s(VERSION_OFF), s(PROJECT_OFF)


def patch(elf, version, project=PROJECT_NAME):
    """Write version and project name into esp_app_desc of an ELF, in place.
    Returns the (version, project) it held before."""
    with open(elf, 'rb') as fh:
        data = bytearray(fh.read())
    at = _symbol_offset(data, 'esp_app_desc')
    magic, = struct.unpack_from('<I', data, at)
    if magic != APP_DESC_MAGIC:
        raise ValueError('esp_app_desc at file offset 0x%x has magic 0x%08x' % (at, magic))
    old = tuple(bytes(data[at + o:at + o + FIELD_LEN]).split(b'\0')[0].decode('ascii', 'replace')
                for o in (VERSION_OFF, PROJECT_OFF))
    data[at + VERSION_OFF:at + VERSION_OFF + FIELD_LEN] = _field(version)
    data[at + PROJECT_OFF:at + PROJECT_OFF + FIELD_LEN] = _field(project)
    with open(elf, 'wb') as fh:
        fh.write(data)
    return old


if __name__ == '__main__':
    import sys
    for path in sys.argv[1:]:
        print('%s: version "%s", project "%s"' % ((path,) + read(path)))
