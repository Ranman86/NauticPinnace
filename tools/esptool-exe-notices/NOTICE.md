# Notices for the bundled `esptool.exe`

The Windows release package of NauticPinnace contains `esptool.exe`, so that
`flash.bat` works without installing anything. This folder holds the notices
and licence texts for everything inside that executable. In the repository it
lives at `tools/esptool-exe-notices/`; `tools/make_release.py` copies it into
the release package as `esptool-4.8.1/notices/`.

## What the executable is

`esptool.exe` is **esptool 4.8.1** by Espressif Systems, exactly as published
in Espressif's release archive
<https://github.com/espressif/esptool/releases/download/v4.8.1/esptool-v4.8.1-win64.zip>.
It is redistributed **unmodified**: 6,760,840 bytes, SHA-256
`069db0f1e4d8f929f36d0f60243499eb670626c03a686376728998aaaad2f664`, still
carrying Espressif's valid Authenticode signature. NauticPinnace only runs it
to flash the board — it is not linked into the firmware.

The exe is a PyInstaller "one-file" build, made by esptool's own CI with
PyInstaller 5.13.2 on Python 3.8. It is the PyInstaller bootloader plus an
embedded archive holding a complete Python runtime, the esptool package and
its two third-party Python dependencies; at start the bootloader unpacks the
archive into a temporary folder and runs it. Redistributing the exe therefore
redistributes every component listed below.

## Where the sources are

esptool is GPL-2.0-or-later. GPLv2 section 3 requires the complete
corresponding source of the executable to accompany it, and that includes the
Python modules compiled into it — not only esptool's own code. The release
package therefore carries, next to the exe:

```
esptool.exe                              the unmodified executable
esptool-4.8.1/esptool-4.8.1-source.zip   esptool 4.8.1, complete source
esptool-4.8.1/pyserial-3.5.tar.gz        pyserial 3.5 source distribution (PyPI)
esptool-4.8.1/intelhex-2.3.0.tar.gz      intelhex 2.3.0 source distribution (PyPI)
esptool-4.8.1/LICENSE, README.md         esptool's own notices, unmodified
esptool-4.8.1/notices/                   this folder
LICENSES/GPL-2.0.txt                     the GPL-2.0 text
```

The two source distributions are listed in `sources.json` with their PyPI
download URL and the SHA-256 the download must have; `tools/make_release.py`
fetches them from there. Each component below also names its exact upstream
source.

## Components

### esptool 4.8.1 — GPL-2.0-or-later
```
SPDX-FileCopyrightText: 2014-2022 Fredrik Ahlberg, Angus Gratton,
Espressif Systems (Shanghai) CO LTD, other contributors as noted.
```
(header of `esptool/__init__.py`; other source files carry years up to 2024.)
The `esptool` package in the embedded archive, plus esptool's flasher stubs
(JSON files under `esptool/targets/stub_flasher/`). Stub set `1` is
GPL-2.0-or-later, built from
<https://github.com/espressif/esptool-legacy-flasher-stub/tree/v1.3.0>. Stub
set `2` is Apache-2.0 OR MIT, built from
<https://github.com/esp-rs/esp-flasher-stub/tree/v0.3.0>; its two licence
files are inside `esptool-4.8.1-source.zip` at
`esptool/targets/stub_flasher/2/`.
Licence text: `../LICENSE` (esptool's own copy, in `esptool-4.8.1/`) and
`LICENSES/GPL-2.0.txt` at the top of the release package.
Source: `../esptool-4.8.1-source.zip`, upstream
<https://github.com/espressif/esptool/tree/v4.8.1>.

### Python 3.8.10 — PSF-2.0 (SPDX `Python-2.0`, which includes the historical BeOpen, CNRI and CWI licences)
```
Copyright (c) 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010,
2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019, 2020, 2021 Python Software Foundation;
All Rights Reserved
Copyright (c) 2000 BeOpen.com. All Rights Reserved.
Copyright (c) 1995-2001 Corporation for National Research Initiatives. All Rights Reserved.
Copyright (c) 1991-1995 Stichting Mathematisch Centrum, Amsterdam. All Rights Reserved.
```
`python38.dll`; the extension modules `_bz2.pyd`, `_ctypes.pyd`,
`_hashlib.pyd`, `_lzma.pyd`, `_socket.pyd`, `_ssl.pyd`, `select.pyd` and
`unicodedata.pyd`; `base_library.zip` and the standard-library modules in the
embedded archive. All are from the official CPython 3.8.10 Windows build
(the DLL and `.pyd` files are byte-identical to the python.org NuGet package
`python` 3.8.10).
- `PSF-2.0-Python-3.8.txt` — the Python licence (CPython `LICENSE`).
- `Python-3.8.10-Doc-license.rst` — CPython's "History and License" page. Its
  section "Licenses and Acknowledgements for Incorporated Software" holds the
  notices for third-party code inside CPython, among them the Mersenne Twister
  (`random`), SipHash24, `strtod`/`dtoa`, cfuhash (`tracemalloc`), zlib 1.2.11
  (built into `python38.dll`) and the UUencode functions (`uu`).
- `Python-3.8.10-crtlicense.txt` — CPython's "Additional Conditions for this
  Windows binary build", covering the Microsoft code that the linker embeds in
  every `.exe`, `.dll` and `.pyd` (see *Microsoft runtime DLLs* below).

`unicodedata.pyd` also contains data from the Unicode Character Database
12.1.0, "© 2019 Unicode®, Inc."; terms of use:
<https://www.unicode.org/terms_of_use.html>.
Source: <https://github.com/python/cpython/tree/v3.8.10> (commit
`3d8993a744813c5144851da5347d7b4b1885f234`).

### OpenSSL 1.1.1k — OpenSSL License and original SSLeay License (SPDX `OpenSSL`)
```
Copyright (c) 1998-2019 The OpenSSL Project.  All rights reserved.
Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)
All rights reserved.
```
`libcrypto-1_1.dll` and `libssl-1_1.dll`, used by `_ssl.pyd` and
`_hashlib.pyd`. They are CPython's Windows build of OpenSSL, byte-identical to
`amd64/` in tag `openssl-bin-1.1.1k-1` of
<https://github.com/python/cpython-bin-deps>.
Licence text: `OpenSSL-SSLeay-1.1.1.txt`.
Source: <https://github.com/openssl/openssl/tree/OpenSSL_1_1_1k>.

Acknowledgements required by these licences:

> This product includes software developed by the OpenSSL Project for use in
> the OpenSSL Toolkit (http://www.openssl.org/)

> This product includes cryptographic software written by Eric Young
> (eay@cryptsoft.com)

The OpenSSL License header further states: "This product includes software
written by Tim Hudson (tjh@cryptsoft.com)."

### libffi 3.3-rc0 — MIT
```
libffi - Copyright (c) 1996-2014  Anthony Green, Red Hat, Inc and others.
```
`libffi-7.dll`, used by `_ctypes.pyd`. Byte-identical to
`amd64/libffi-7.dll` in tag `libffi-3.3.0` of
<https://github.com/python/cpython-bin-deps>, which CPython 3.8.10 builds from
its source mirror tag `libffi-3.3.0-rc0-r1` (upstream libffi 3.3-rc0).
Licence text: `MIT-libffi.txt`.
Source: <https://github.com/libffi/libffi/tree/v3.3-rc0>.

### bzip2 1.0.6 — SPDX `bzip2-1.0.6`
```
copyright (C) 1996-2010 Julian R Seward.  All rights reserved.
```
Statically linked into `_bz2.pyd`.
Licence text: `bzip2.txt`.
Source: <https://sourceware.org/git/?p=bzip2.git;a=tree;hb=bzip2-1.0.6>.

### liblzma from XZ Utils 5.2.2 — public domain
Statically linked into `_lzma.pyd`. liblzma 5.2.2 is in the public domain and
carries no copyright notice; `xz-COPYING.txt` is the XZ Utils licensing
summary that says so.
Source: <https://github.com/tukaani-project/xz/tree/v5.2.2>.

### zlib 1.2.11 and 1.2.13 — Zlib
```
(C) 1995-2022 Jean-loup Gailly and Mark Adler
```
zlib 1.2.11 is built into `python38.dll` (its notice, "(C) 1995-2017", is
also in the "zlib" section of `Python-3.8.10-Doc-license.rst`); the
PyInstaller bootloader contains the inflate part of zlib 1.2.13.
Licence text: `Zlib-zlib-1.2.13.txt` (the terms of 1.2.11 are the same).
Source: <https://github.com/madler/zlib/tree/v1.2.11>,
<https://github.com/madler/zlib/tree/v1.2.13>.

### pyserial 3.5 — BSD-3-Clause
```
Copyright (c) 2001-2020 Chris Liechti <cliechti@gmx.net>
All Rights Reserved.
```
The `serial` package in the embedded archive.
Licence text: `BSD-3-Clause-pyserial.txt`.
Source: `../pyserial-3.5.tar.gz`, upstream
<https://github.com/pyserial/pyserial/tree/v3.5>.

### intelhex 2.3.0 — BSD-3-Clause
```
Copyright (c) 2005-2018, Alexander Belchenko
All rights reserved.
```
The `intelhex` package in the embedded archive.
Licence text: `BSD-3-Clause-intelhex.txt`.
Source: `../intelhex-2.3.0.tar.gz`, upstream
<https://github.com/python-intelhex/intelhex/tree/2.3.0>.

### PyInstaller 5.13.2 — GPL-2.0-or-later WITH Bootloader-exception; run-time hooks Apache-2.0
```
Copyright (c) 2010-2023, PyInstaller Development Team
Copyright (c) 2005-2009, Giovanni Bajo
Based on previous work under copyright (c) 2002 McMillan Enterprises, Inc.
```
The bootloader (the executable part of `esptool.exe` that unpacks and starts
the archive) and the loader modules `pyiboot01_bootstrap`, `pyimod01_archive`,
`pyimod02_importers`, `pyimod03_ctypes` and `pyimod04_pywin32` are
GPL-2.0-or-later with the following exception, quoted from PyInstaller's
`COPYING.txt`:

> In addition to the permissions in the GNU General Public License, the
> authors give you unlimited permission to link or embed compiled bootloader
> and related files into combinations with other programs, and to distribute
> those combinations without any restriction coming from the use of those
> files. (The General Public License restrictions do apply in other respects;
> for example, they cover modification of the files, and distribution when
> not linked into a combined executable.)

The run-time hook `pyi_rth_inspect` is Apache-2.0.
Licence text: `GPL-2.0-with-bootloader-exception-PyInstaller.txt` (PyInstaller's
`COPYING.txt`: the terms above plus the full GPL-2.0, Apache-2.0 and MIT texts).
Source: <https://github.com/pyinstaller/pyinstaller/tree/v5.13.2>.

### Microsoft runtime DLLs — Microsoft Distributable Code
```
© Microsoft Corporation. All rights reserved.
```
- `VCRUNTIME140.dll` 14.28.29914.0 — Visual C++ 2019 runtime.
- `ucrtbase.dll` and 38 `api-ms-win-*.dll`, all 10.0.17134.12 — the
  Universal C Runtime, as redistributed with the Windows 10 SDK.

These are not open source. They are Microsoft "Distributable Code": the
Visual Studio and Windows SDK licence terms allow them to be redistributed,
unmodified, as part of a program built with those tools, and allow that
program's distributors to pass them on with it. Here they are part of the
unmodified `esptool.exe`. No licence text is reproduced in this folder;
Microsoft's own pages are:
- <https://learn.microsoft.com/en-us/visualstudio/releases/2019/redistribution>
  (Distributable Code for Visual Studio 2019)
- <https://learn.microsoft.com/en-us/cpp/windows/universal-crt-deployment>
  (local deployment of the Universal CRT)
- <https://learn.microsoft.com/en-us/legal/windows-sdk/license>
  (Windows SDK licence terms)

## Licence texts in this folder

Every file is a byte-identical copy of the upstream file at the exact version
(`.gitattributes` here keeps git from changing its line endings).

| File | Upstream file | SHA-256 |
|---|---|---|
| `PSF-2.0-Python-3.8.txt` | <https://raw.githubusercontent.com/python/cpython/v3.8.10/LICENSE> | `599826df92bfdcd2702eac691072498bb096c55af04ee984cf90f70ed77b5a70` |
| `Python-3.8.10-Doc-license.rst` | <https://raw.githubusercontent.com/python/cpython/v3.8.10/Doc/license.rst> | `ccf85ebc8cd82a6a540ac4781dadb3f9b98036c8efb33c5e9985c69a6c2eb5ec` |
| `Python-3.8.10-crtlicense.txt` | <https://raw.githubusercontent.com/python/cpython/v3.8.10/PC/crtlicense.txt> | `3c71b3ae243121f6f227374cd1ef7c59ed07b1969b330d0e947f91a3a1b3fd73` |
| `OpenSSL-SSLeay-1.1.1.txt` | <https://raw.githubusercontent.com/openssl/openssl/OpenSSL_1_1_1k/LICENSE> | `c32913b33252e71190af2066f08115c69bc9fddadf3bf29296e20c835389841c` |
| `MIT-libffi.txt` | <https://raw.githubusercontent.com/libffi/libffi/v3.3-rc0/LICENSE> | `0f4d7a0bfb83c37465d42dc305f124189196cc0cc2cc8d6f8461103682aebbc5` |
| `bzip2.txt` | <https://sourceware.org/git/?p=bzip2.git;a=blob_plain;f=LICENSE;hb=bzip2-1.0.6> | `4919cfb14a73cd64fcef67b107613970cf1659a09aa675dba31314f373bc7204` |
| `xz-COPYING.txt` | <https://raw.githubusercontent.com/tukaani-project/xz/v5.2.2/COPYING> | `c4f8e14fafe458d84808a4cd8b69f94673ebe2bf8fc992291629a69ac12218f8` |
| `Zlib-zlib-1.2.13.txt` | <https://raw.githubusercontent.com/madler/zlib/v1.2.13/LICENSE> | `845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243` |
| `BSD-3-Clause-pyserial.txt` | <https://raw.githubusercontent.com/pyserial/pyserial/v3.5/LICENSE.txt> | `f91cb9813de6a5b142b8f7f2dede630b5134160aedaeaf55f4d6a7e2593ca3f3` |
| `BSD-3-Clause-intelhex.txt` | <https://raw.githubusercontent.com/python-intelhex/intelhex/2.3.0/LICENSE.txt> | `1b2eb032ab8a1b0266f7995c76e44509f89ca9c0e90ec507763b5e735aca7de4` |
| `GPL-2.0-with-bootloader-exception-PyInstaller.txt` | <https://raw.githubusercontent.com/pyinstaller/pyinstaller/v5.13.2/COPYING.txt> | `dcf75fdb959db1e3b41c0f8505069d2ece781b5ec6b3d0a4d30975cfc6580245` |

## How the versions were determined

From the exe itself, read with a PyInstaller archive reader:

- **Contents.** The archive table lists `python38.dll` (Python version field
  308), the DLLs and `.pyd` files named above, `base_library.zip`, esptool's
  stub JSON files and one module archive. Its top-level packages are the
  standard library plus `esptool`, `serial` and `intelhex` — no other
  third-party package.
- **Python.** Version resource 3.8.10 on `python38.dll` and every `.pyd`;
  build strings `tags/v3.8.10`, `3d8993a`, `[MSC v.1928 64 bit (AMD64)]`.
  `python38.dll`, all eight `.pyd` files and the OpenSSL and libffi DLLs are
  byte-identical to the files in the NuGet package `python` 3.8.10
  (<https://www.nuget.org/packages/python/3.8.10>).
- **OpenSSL.** `OpenSSL 1.1.1k  25 Mar 2021` in both DLLs; both are
  byte-identical to `cpython-bin-deps` tag `openssl-bin-1.1.1k-1`.
- **libffi.** No version string. The DLL is byte-identical to
  `cpython-bin-deps` tag `libffi-3.3.0` (last changed 2019-11-13, before the
  libffi 3.3 release). CPython 3.8.10's `PCbuild/get_externals.bat` names the
  source `libffi-3.3.0-rc0-r1`, whose `configure.ac` says `3.3-rc0`. The file
  name `libffi-7` is the libffi 3.3 ABI.
- **bzip2, xz.** `1.0.6, 6-Sept-2010` in `_bz2.pyd`, `5.2.2` in `_lzma.pyd`;
  `get_externals.bat` names `bzip2-1.0.6` and `xz-5.2.2`.
- **zlib.** `inflate 1.2.11 Copyright 1995-2017 Mark Adler` in `python38.dll`,
  `inflate 1.2.13 Copyright 1995-2022 Mark Adler` in the bootloader.
- **pyserial.** `serial/__init__` sets `__version__ = '3.5'`, and every
  function and class of the 14 bundled `serial` modules starts on the same
  line as in the `pyserial-3.5` source distribution.
- **intelhex.** No version constant is bundled. All 98 functions and classes
  of the 3 bundled modules match the `intelhex-2.3.0` source distribution
  line for line; against 2.2.1, only 26 of the 79 in `intelhex/__init__` do.
- **esptool.** `__version__ = '4.8.1'`; all 25 bundled modules match the 4.8.1
  source line for line, and the stub JSON files are identical apart from line
  endings.
- **PyInstaller.** esptool's `.github/workflows/build_esptool.yml` pins
  `pyinstaller==5.13.2`. The bootloader's code and data sections are identical
  to `run.exe` (Windows-64bit-intel) from the PyInstaller 5.13.2 wheel; its
  `.rdata` differs only in a 4-byte timestamp. The loader modules match the
  5.13.2 sources line for line.
- **Microsoft DLLs.** Version resources as listed above.

When `ESPTOOL_VERSION` in `tools/make_release.py` changes, all of this has to
be checked again: a different esptool build bundles different versions.
