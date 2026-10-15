# Third-party notices

The root BSD-3-Clause license covers project-owned material only. Original Linux, third-party and file-specific licenses and copyrights remain in force.

Nations and NSING are the same company. Optional ns-flash is company-owned and closed-source; its program source is not published. `sdk/example/` is company-written source-only material; local build products are excluded.

Each component's corresponding source - the original tree, the local modifications listed below, the configuration and the build scripts - is delivered in the source package published with the release, as described in [licenses/README.md](licenses/README.md). `tools/` and `sdk/example/` are project material and retain their file-specific licenses.

Use `make legal-info` for the metadata companion and `make legal-info-sources LEGAL_STRICT=1` for the corresponding-source bundle. Materials stay on the host and beside release downloads, outside all firmware images. Evidence results and limitations are described in [licenses/RESOLUTIONS.md](licenses/RESOLUTIONS.md).

| Component | Version / source revision | SPDX / classification |
| --- | --- | --- |
| Project build tools, scripts and SDK examples | repository snapshot | `BSD-3-Clause` |
| Project n32-flash and minimal SFTP server | Company snapshot; distributed files individually identified by SHA-256 | `MIT` |
| Linux kernel and UAPI headers | 7.3.0-rc7 | `GPL-2.0-only` |
| BusyBox (all applet links) | 1.38.0 | `GPL-2.0-only` |
| uClibc-ng loader, shared/static libraries and headers | 1.0.59 | `LGPL-2.1-or-later` |
| GCC compiler, plugins and support | 16.1.0 | `GPL-3.0-or-later` |
| GCC libgcc, libstdc++, crt objects and runtime headers | 16.1.0 / libstdc++.so.6.0.35 | `GPL-3.0-or-later WITH GCC-exception-3.1` |
| GNU Binutils including libdep plugin | 2.46.0.20260210 (host ld --version); source base 2.46.0 | `GPL-3.0-or-later` |
| GCC bundled support libraries | GMP 6.3.0; MPFR 4.2.2; MPC 1.3.1; ISL 0.24; GCC 16.1.0 support libraries | `(LGPL-3.0-or-later OR GPL-2.0-or-later) AND LGPL-3.0-or-later AND LGPL-2.0-or-later AND MIT AND Apache-2.0 AND BSD-3-Clause AND GPL-3.0-or-later` |
| Dropbear SSH server/client/key/scp with bundled crypto | 2026.94 (source declaration) | `MIT AND BSD-2-Clause AND LicenseRef-Dropbear-Mixed` |
| microADB with Linux NOMMU HAL | 85cc7a9 + local port | `Apache-2.0` |
| TinyALSA utilities and static library | 2.0.0 build value; pinned commit | `BSD-3-Clause` |
| Arm CMSIS Core headers | CMSIS_5 5.6.0 (Core(M) 5.3; individual header versions retained) | `Apache-2.0` |
| Nations peripheral/USB/device code and templates | N32H7xx_Library.1.2.0 in representative headers | `LicenseRef-Company-Owned` |
| Nations algorithm archives and flash operation binaries | Company snapshot identified by per-file SHA-256; individual header versions retained | `LicenseRef-Company-Owned` |
| espeak-ng program and voice data | 1.53.0 / ba90c8e | `GPL-3.0-or-later AND Unicode-DFS-2016 AND LicenseRef-CC-CEDICT-Historical` |
| pcaudiolib linked into optional espeak-ng | d6a6b00 | `GPL-3.0-or-later` |
| NSLink optional ns-flash binary | 0.1.4 (README declaration) | `LicenseRef-Company-Proprietary` |
| GNU GCC and Binutils SDK manuals | shipped SDK snapshot | `GFDL-1.3-or-later AND GPL-3.0-or-later` |
| Generated GNU ld linker scripts | SDK snapshot; generator binary 2.46.0.20260210 | `LicenseRef-Binutils-Linker-Script` |

## Project build tools, scripts and SDK examples

- Version: repository snapshot
- SPDX/classification: `BSD-3-Clause`
- Source: https://github.com/NSING-Community/n32h7xx_linux
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: `tools` (ships in this repository)
- Modifications: Top-level BSD applies only to project-owned material; preserve prior MIT grants. Owner confirms sdk/example sources are company-written; local compiled outputs are not distributed.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [LICENSE](LICENSE)
  - [licenses/project-MIT.txt](licenses/project-MIT.txt)

## Project n32-flash and minimal SFTP server

- Version: Company snapshot; distributed files individually identified by SHA-256
- SPDX/classification: `MIT`
- Source: Company snapshot; no immutable public URL confirmed
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: Source SPDX says MIT; do not relabel BSD.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [LICENSE](LICENSE)
  - [licenses/project-MIT.txt](licenses/project-MIT.txt)

## Linux kernel and UAPI headers

- Version: 7.3.0-rc7
- SPDX/classification: `GPL-2.0-only`
- Source: https://github.com/NSING-Community/linux.git ; branch n32h7xx
- Source revision: a894130780a157e2f5a69a8a948e52b4f5bf227e plus the local NSING changes in this repository, as reviewed; licenses/evidence/linux.json was regenerated for this revision from an isolated rebuild of the same configuration
- Corresponding source: `kernel` (ships in this repository)
- Modifications: Merge of upstream 7.3.0-rc7 plus the local NSING changes carried in this repository. UAPI: per-file licenses, including GPL-2.0-only WITH Linux-syscall-note.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [kernel/COPYING](kernel/COPYING)
  - [kernel/LICENSES/deprecated/0BSD](kernel/LICENSES/deprecated/0BSD)
  - [kernel/LICENSES/deprecated/CC0-1.0](kernel/LICENSES/deprecated/CC0-1.0)
  - [kernel/LICENSES/deprecated/GFDL-1.1](kernel/LICENSES/deprecated/GFDL-1.1)
  - [kernel/LICENSES/deprecated/GFDL-1.2](kernel/LICENSES/deprecated/GFDL-1.2)
  - [kernel/LICENSES/deprecated/GPL-1.0](kernel/LICENSES/deprecated/GPL-1.0)
  - [kernel/LICENSES/deprecated/ISC](kernel/LICENSES/deprecated/ISC)
  - [kernel/LICENSES/deprecated/Linux-OpenIB](kernel/LICENSES/deprecated/Linux-OpenIB)
  - [kernel/LICENSES/deprecated/X11](kernel/LICENSES/deprecated/X11)
  - [kernel/LICENSES/deprecated/Zlib](kernel/LICENSES/deprecated/Zlib)
  - [kernel/LICENSES/dual/Apache-2.0](kernel/LICENSES/dual/Apache-2.0)
  - [kernel/LICENSES/dual/CC-BY-4.0](kernel/LICENSES/dual/CC-BY-4.0)
  - [kernel/LICENSES/dual/CDDL-1.0](kernel/LICENSES/dual/CDDL-1.0)
  - [kernel/LICENSES/dual/MPL-1.1](kernel/LICENSES/dual/MPL-1.1)
  - [kernel/LICENSES/dual/copyleft-next-0.3.1](kernel/LICENSES/dual/copyleft-next-0.3.1)
  - [kernel/LICENSES/exceptions/GCC-exception-2.0](kernel/LICENSES/exceptions/GCC-exception-2.0)
  - [kernel/LICENSES/exceptions/Linux-syscall-note](kernel/LICENSES/exceptions/Linux-syscall-note)
  - [kernel/LICENSES/preferred/BSD-2-Clause](kernel/LICENSES/preferred/BSD-2-Clause)
  - [kernel/LICENSES/preferred/BSD-3-Clause](kernel/LICENSES/preferred/BSD-3-Clause)
  - [kernel/LICENSES/preferred/BSD-3-Clause-Clear](kernel/LICENSES/preferred/BSD-3-Clause-Clear)
  - [kernel/LICENSES/preferred/GPL-2.0](kernel/LICENSES/preferred/GPL-2.0)
  - [kernel/LICENSES/preferred/LGPL-2.0](kernel/LICENSES/preferred/LGPL-2.0)
  - [kernel/LICENSES/preferred/LGPL-2.1](kernel/LICENSES/preferred/LGPL-2.1)
  - [kernel/LICENSES/preferred/MIT](kernel/LICENSES/preferred/MIT)
  - [licenses/evidence/linux.json](licenses/evidence/linux.json)

## BusyBox (all applet links)

- Version: 1.38.0
- SPDX/classification: `GPL-2.0-only`
- Source: https://busybox.net/downloads/ ; busybox-1.38.0.tar.bz2
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: Five local BusyBox patches plus the runtime configuration. Snapshot actual source.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [licenses/busybox/LICENSE](licenses/busybox/LICENSE)
  - [licenses/busybox/AUTHORS](licenses/busybox/AUTHORS)
  - [licenses/evidence/busybox.json](licenses/evidence/busybox.json)

## uClibc-ng loader, shared/static libraries and headers

- Version: 1.0.59
- SPDX/classification: `LGPL-2.1-or-later`
- Source: https://downloads.uclibc-ng.org/releases/1.0.59/uClibc-ng-1.0.59.tar.bz2 plus the NSING FDPIC patch and configuration shipped in the corresponding-source package
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: FDPIC/loader/thread patches; capture working source, config and scripts.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [sdk/toolchain/licenses/uClibc/COPYING.LIB](sdk/toolchain/licenses/uClibc/COPYING.LIB)
  - [sdk/toolchain/licenses/uClibc/COPYING.LIB.boilerplate](sdk/toolchain/licenses/uClibc/COPYING.LIB.boilerplate)
  - [licenses/uclibc-file-notices.txt](licenses/uclibc-file-notices.txt)
  - [licenses/evidence/uclibc.json](licenses/evidence/uclibc.json)

## GCC compiler, plugins and support

- Version: 16.1.0
- SPDX/classification: `GPL-3.0-or-later`
- Source: https://gcc.gnu.org/ ; local non-Git source has no confirmed release archive/commit
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: SDK FDPIC/TLS patches and the toolchain rebuild script.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [sdk/toolchain/licenses/GCC/COPYING](sdk/toolchain/licenses/GCC/COPYING)
  - [sdk/toolchain/licenses/GCC/COPYING3](sdk/toolchain/licenses/GCC/COPYING3)
  - [sdk/toolchain/licenses/GCC/COPYING.LIB](sdk/toolchain/licenses/GCC/COPYING.LIB)
  - [sdk/toolchain/licenses/GCC/COPYING3.LIB](sdk/toolchain/licenses/GCC/COPYING3.LIB)
  - [sdk/toolchain/licenses/GCC/COPYING.RUNTIME](sdk/toolchain/licenses/GCC/COPYING.RUNTIME)
  - [licenses/evidence/gcc.json](licenses/evidence/gcc.json)

## GCC libgcc, libstdc++, crt objects and runtime headers

- Version: 16.1.0 / libstdc++.so.6.0.35
- SPDX/classification: `GPL-3.0-or-later WITH GCC-exception-3.1`
- Source: https://gcc.gnu.org/ ; grant verified in libgcc/libgcc2.c and libstdc++-v3/src/c++11/thread.cc
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: FDPIC/TLS/libstdc++ changes. Runtime exception has eligible compilation conditions; does not waive runtime source distribution.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [sdk/toolchain/licenses/GCC/COPYING](sdk/toolchain/licenses/GCC/COPYING)
  - [sdk/toolchain/licenses/GCC/COPYING3](sdk/toolchain/licenses/GCC/COPYING3)
  - [sdk/toolchain/licenses/GCC/COPYING.LIB](sdk/toolchain/licenses/GCC/COPYING.LIB)
  - [sdk/toolchain/licenses/GCC/COPYING3.LIB](sdk/toolchain/licenses/GCC/COPYING3.LIB)
  - [sdk/toolchain/licenses/GCC/COPYING.RUNTIME](sdk/toolchain/licenses/GCC/COPYING.RUNTIME)
  - [licenses/libgcc2.c.notice](licenses/libgcc2.c.notice)
  - [licenses/thread.cc.notice](licenses/thread.cc.notice)
  - [licenses/evidence/gcc-runtime.json](licenses/evidence/gcc-runtime.json)

## GNU Binutils including libdep plugin

- Version: 2.46.0.20260210 (host ld --version); source base 2.46.0
- SPDX/classification: `GPL-3.0-or-later`
- Source: https://sourceware.org/binutils/ ; exact source archive/commit unconfirmed
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: SDK thumb-prel31-plt patch; component-specific GPL/LGPL terms retained.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [sdk/toolchain/licenses/binutils/COPYING3](sdk/toolchain/licenses/binutils/COPYING3)
  - [sdk/toolchain/licenses/GCC/COPYING.LIB](sdk/toolchain/licenses/GCC/COPYING.LIB)
  - [sdk/toolchain/licenses/GCC/COPYING3.LIB](sdk/toolchain/licenses/GCC/COPYING3.LIB)
  - [licenses/evidence/binutils.json](licenses/evidence/binutils.json)
  - [licenses/binutils-zlib-notices.txt](licenses/binutils-zlib-notices.txt)
  - [licenses/binutils-libiberty-notices.txt](licenses/binutils-libiberty-notices.txt)

## GCC bundled support libraries

- Version: GMP 6.3.0; MPFR 4.2.2; MPC 1.3.1; ISL 0.24; GCC 16.1.0 support libraries
- SPDX/classification: `(LGPL-3.0-or-later OR GPL-2.0-or-later) AND LGPL-3.0-or-later AND LGPL-2.0-or-later AND MIT AND Apache-2.0 AND BSD-3-Clause AND GPL-3.0-or-later`
- Source: Bundled in the GCC source tree
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: Static composition established from original compiler link records and matching released compiler hashes. Host compiler: Ubuntu GCC 15.2.0-16ubuntu1; -static-libstdc++/-static-libgcc. System libz/libzstd/libm/libc remain external dynamic host dependencies, not copied into SDK. Per-file copyright and license notices retained.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [sdk/toolchain/licenses/GCC/COPYING3](sdk/toolchain/licenses/GCC/COPYING3)
  - [sdk/toolchain/licenses/GCC/COPYING3.LIB](sdk/toolchain/licenses/GCC/COPYING3.LIB)
  - [licenses/host-dependencies/gmp-6.3.0/COPYINGv2](licenses/host-dependencies/gmp-6.3.0/COPYINGv2)
  - [licenses/host-dependencies/gmp-6.3.0/COPYINGv3](licenses/host-dependencies/gmp-6.3.0/COPYINGv3)
  - [licenses/host-dependencies/mpfr-4.2.2/COPYING](licenses/host-dependencies/mpfr-4.2.2/COPYING)
  - [licenses/host-dependencies/mpfr-4.2.2/COPYING.LESSER](licenses/host-dependencies/mpfr-4.2.2/COPYING.LESSER)
  - [licenses/host-dependencies/mpc/COPYING.LESSER](licenses/host-dependencies/mpc/COPYING.LESSER)
  - [licenses/host-dependencies/isl-0.24/LICENSE](licenses/host-dependencies/isl-0.24/LICENSE)
  - [licenses/host-dependencies/zlib/LICENSE](licenses/host-dependencies/zlib/LICENSE)
  - [licenses/host-dependencies/libcody/LICENSE](licenses/host-dependencies/libcody/LICENSE)
  - [licenses/host-dependencies/libiberty/COPYING.LIB](licenses/host-dependencies/libiberty/COPYING.LIB)
  - [licenses/host-dependencies/gmp-6.3.0/README.notice](licenses/host-dependencies/gmp-6.3.0/README.notice)
  - [licenses/host-dependencies/src/mpfr.h.notice](licenses/host-dependencies/src/mpfr.h.notice)
  - [licenses/host-dependencies/src/mpc.h.notice](licenses/host-dependencies/src/mpc.h.notice)
  - [licenses/host-dependencies/isl/ctx.h.notice](licenses/host-dependencies/isl/ctx.h.notice)
  - [licenses/host-dependencies/zlib/zlib.h.notice](licenses/host-dependencies/zlib/zlib.h.notice)
  - [licenses/host-dependencies/libcody/README.md.notice](licenses/host-dependencies/libcody/README.md.notice)
  - [licenses/host-dependencies/libiberty/README.notice](licenses/host-dependencies/libiberty/README.notice)
  - [licenses/host-dependencies/gmp-6.3.0/file-notices.txt](licenses/host-dependencies/gmp-6.3.0/file-notices.txt)
  - [licenses/host-dependencies/mpfr-4.2.2/file-notices.txt](licenses/host-dependencies/mpfr-4.2.2/file-notices.txt)
  - [licenses/host-dependencies/mpc-1.3.1/file-notices.txt](licenses/host-dependencies/mpc-1.3.1/file-notices.txt)
  - [licenses/host-dependencies/isl-0.24/file-notices.txt](licenses/host-dependencies/isl-0.24/file-notices.txt)
  - [licenses/host-dependencies/libcody/file-notices.txt](licenses/host-dependencies/libcody/file-notices.txt)
  - [licenses/host-dependencies/libbacktrace/file-notices.txt](licenses/host-dependencies/libbacktrace/file-notices.txt)
  - [licenses/host-dependencies/libiberty/file-notices.txt](licenses/host-dependencies/libiberty/file-notices.txt)
  - [licenses/host-dependencies/libcpp/file-notices.txt](licenses/host-dependencies/libcpp/file-notices.txt)
  - [licenses/host-dependencies/libdecnumber/file-notices.txt](licenses/host-dependencies/libdecnumber/file-notices.txt)
  - [licenses/evidence/gcc-link-records.txt](licenses/evidence/gcc-link-records.txt)
  - [licenses/evidence/host-dependencies.json](licenses/evidence/host-dependencies.json)
- Bundled GMP 6.3.0: `LGPL-3.0-or-later OR GPL-2.0-or-later`.
- Bundled MPFR 4.2.2: `LGPL-3.0-or-later`.
- Bundled MPC 1.3.1: `LGPL-3.0-or-later`.
- Bundled ISL 0.24: `MIT`.
- Bundled libcody bundled with GCC 16.1.0: `Apache-2.0`.
- Bundled libbacktrace bundled with GCC 16.1.0: `BSD-3-Clause`.
- Bundled libiberty bundled with GCC 16.1.0: `LGPL-2.0-or-later; individual permissive files also retained`.
- Bundled libcpp bundled with GCC 16.1.0: `GPL-3.0-or-later`.
- Bundled libdecnumber bundled with GCC 16.1.0: `GPL-3.0-or-later WITH GCC-exception-3.1`.

## Dropbear SSH server/client/key/scp with bundled crypto

- Version: 2026.94 (source declaration)
- SPDX/classification: `MIT AND BSD-2-Clause AND LicenseRef-Dropbear-Mixed`
- Source: https://github.com/mkj/dropbear ; https://matt.ucc.asn.au/dropbear/releases/
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: Localoptions configuration. OpenSSH/PuTTY/LibTomCrypt/LibTomMath/TweetNaCl declarations retained verbatim.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [licenses/dropbear/LICENSE](licenses/dropbear/LICENSE)
  - [licenses/dropbear/libtomcrypt/LICENSE](licenses/dropbear/libtomcrypt/LICENSE)
  - [licenses/dropbear/libtommath/LICENSE](licenses/dropbear/libtommath/LICENSE)
  - [licenses/dropbear-file-notices.txt](licenses/dropbear-file-notices.txt)
  - [licenses/evidence/dropbear.json](licenses/evidence/dropbear.json)

## microADB with Linux NOMMU HAL

- Version: 85cc7a9 + local port
- SPDX/classification: `Apache-2.0`
- Source: https://github.com/spiriou/microADB.git
- Source revision: 85cc7a90101b2ae76ee4112ba285141c8fba5012
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: Copyright (C) 2020 Simon Piriou; NSING NOMMU HAL and sync patch.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [rootfs/usr/share/licenses/microadb/LICENSE](rootfs/usr/share/licenses/microadb/LICENSE)
  - [rootfs/usr/share/licenses/microadb/NOTICE](rootfs/usr/share/licenses/microadb/NOTICE)
  - [licenses/evidence/microadb.json](licenses/evidence/microadb.json)

## TinyALSA utilities and static library

- Version: 2.0.0 build value; pinned commit
- SPDX/classification: `BSD-3-Clause`
- Source: https://github.com/tinyalsa/tinyalsa.git
- Source revision: 961babfe962e71d952ae734074cc89576b28a9e7
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: Copyright 2011 Android Open Source Project; Copyright (c) 2019 Linux Foundation. Actual text has three conditions.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [licenses/tinyalsa/LICENSE](licenses/tinyalsa/LICENSE)
  - [licenses/evidence/tinyalsa.json](licenses/evidence/tinyalsa.json)

## Arm CMSIS Core headers

- Version: CMSIS_5 5.6.0 (Core(M) 5.3; individual header versions retained)
- SPDX/classification: `Apache-2.0`
- Source: https://github.com/ARM-software/CMSIS_5 ; exact vendor import unconfirmed
- Source revision: b5f0603d6a584d1724d952fd8b0737458b90d62b
- Corresponding source: `bootloader/Driver/CMSIS/core` (ships in this repository)
- Modifications: All 24 vendor-imported core headers match official CMSIS_5 5.6.0, ignoring CRLF only. Arm original copyrights retained.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [licenses/boot-file-notices.txt](licenses/boot-file-notices.txt)
  - [rootfs/usr/share/licenses/microadb/LICENSE](rootfs/usr/share/licenses/microadb/LICENSE)
  - [licenses/evidence/cmsis.json](licenses/evidence/cmsis.json)

## Nations peripheral/USB/device code and templates

- Version: N32H7xx_Library.1.2.0 in representative headers
- SPDX/classification: `LicenseRef-Company-Owned`
- Source: Nations / NSING are the same company and this code is company-owned, confirmed by repository owner on 2026-10-15.
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: `bootloader` (ships in this repository)
- Modifications: Original Nations copyright/headers retained. Company-owned scope is not treated as a missing third-party grant.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [licenses/boot-file-notices.txt](licenses/boot-file-notices.txt)
  - [rootfs/usr/share/licenses/microadb/LICENSE](rootfs/usr/share/licenses/microadb/LICENSE)

## Nations algorithm archives and flash operation binaries

- Version: Company snapshot identified by per-file SHA-256; individual header versions retained
- SPDX/classification: `LicenseRef-Company-Owned`
- Source: Company-owned Nations / NSING code, confirmed by repository owner on 2026-10-15.
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: Not included; see the ownership/source statement above
- Modifications: Original headers preserved; no open-source duty inferred solely from precompiled company code.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [licenses/boot-file-notices.txt](licenses/boot-file-notices.txt)
  - [rootfs/usr/share/licenses/microadb/LICENSE](rootfs/usr/share/licenses/microadb/LICENSE)

## espeak-ng program and voice data

- Version: 1.53.0 / ba90c8e
- SPDX/classification: `GPL-3.0-or-later AND Unicode-DFS-2016 AND LicenseRef-CC-CEDICT-Historical`
- Source: https://github.com/espeak-ng/espeak-ng.git
- Source revision: ba90c8e9f440ad544f674a790bb5f53878b6ffc5
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: Target port glue and TinyALSA backend preserved; pinned upstream source and generated-data build preserved. Restored editable Mandarin subset reproduces the published dictionary exactly; historical CC-CEDICT extraction/version remains a documented warning. See licenses/evidence/espeak.json.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [sdk/optional/espeak-ng/licenses/COPYING](sdk/optional/espeak-ng/licenses/COPYING)
  - [sdk/optional/espeak-ng/licenses/COPYING.APACHE](sdk/optional/espeak-ng/licenses/COPYING.APACHE)
  - [sdk/optional/espeak-ng/licenses/COPYING.BSD2](sdk/optional/espeak-ng/licenses/COPYING.BSD2)
  - [sdk/optional/espeak-ng/licenses/COPYING.PCAUDIOLIB](sdk/optional/espeak-ng/licenses/COPYING.PCAUDIOLIB)
  - [sdk/optional/espeak-ng/licenses/COPYING.UCD](sdk/optional/espeak-ng/licenses/COPYING.UCD)
  - [sdk/optional/espeak-ng/licenses/NOTICE](sdk/optional/espeak-ng/licenses/NOTICE)
  - [licenses/espeak-file-notices.txt](licenses/espeak-file-notices.txt)
  - [sdk/optional/espeak-ng/licenses/TinyALSA-LICENSE](sdk/optional/espeak-ng/licenses/TinyALSA-LICENSE)
  - [sdk/optional/espeak-ng/licenses/Mandarin-NOTICE.txt](sdk/optional/espeak-ng/licenses/Mandarin-NOTICE.txt)
  - [licenses/CC-BY-SA-4.0.txt](licenses/CC-BY-SA-4.0.txt)
  - [licenses/evidence/espeak.json](licenses/evidence/espeak.json)
- **WARNING**: Mandarin cmn_listx credits CC-CEDICT, but the historical extraction revision/date and original import license version were not retained; current publisher CC-BY-SA-4.0 text and attribution preserved without asserting historical version

## pcaudiolib linked into optional espeak-ng

- Version: d6a6b00
- SPDX/classification: `GPL-3.0-or-later`
- Source: https://github.com/alsa-project/pcaudiolib.git
- Source revision: d6a6b00aa4334b461c1a09c27b6c681eaac8da8a
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: TinyALSA backend patch and glue in sdk/optional-src/pcaudiolib.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [sdk/optional/espeak-ng/licenses/COPYING](sdk/optional/espeak-ng/licenses/COPYING)
  - [sdk/optional/espeak-ng/licenses/COPYING.APACHE](sdk/optional/espeak-ng/licenses/COPYING.APACHE)
  - [sdk/optional/espeak-ng/licenses/COPYING.BSD2](sdk/optional/espeak-ng/licenses/COPYING.BSD2)
  - [sdk/optional/espeak-ng/licenses/COPYING.PCAUDIOLIB](sdk/optional/espeak-ng/licenses/COPYING.PCAUDIOLIB)
  - [sdk/optional/espeak-ng/licenses/COPYING.UCD](sdk/optional/espeak-ng/licenses/COPYING.UCD)
  - [sdk/optional/espeak-ng/licenses/NOTICE](sdk/optional/espeak-ng/licenses/NOTICE)
  - [licenses/pcaudio-file-notices.txt](licenses/pcaudio-file-notices.txt)
  - [sdk/optional/espeak-ng/licenses/TinyALSA-LICENSE](sdk/optional/espeak-ng/licenses/TinyALSA-LICENSE)
  - [licenses/evidence/pcaudio.json](licenses/evidence/pcaudio.json)

## NSLink optional ns-flash binary

- Version: 0.1.4 (README declaration)
- SPDX/classification: `LicenseRef-Company-Proprietary`
- Source: Company-owned NSLink project; closed-source binary, as confirmed by repository owner on 2026-10-15.
- Source revision: Company-owned closed source; no publication required
- Corresponding source: Not included; see the ownership/source statement above
- Modifications: Company-owned closed binary. No open-source obligation is inferred from absence of its source. Third-party linked code still follows its own terms.
- Copyright: Nations/NSING company-owned program; Zig contributors for bundled Zig support.
- Full licenses, NOTICEs and evidence:
  - [sdk/optional/ns-flash/README.md](sdk/optional/ns-flash/README.md)
  - [licenses/Zig-MIT.txt](licenses/Zig-MIT.txt)
  - [licenses/evidence/ns-flash.json](licenses/evidence/ns-flash.json)
  - [sdk/optional/ns-flash/THIRD_PARTY_NOTICES.txt](sdk/optional/ns-flash/THIRD_PARTY_NOTICES.txt)
  - [sdk/optional/ns-flash/Zig-MIT.txt](sdk/optional/ns-flash/Zig-MIT.txt)

## GNU GCC and Binutils SDK manuals

- Version: shipped SDK snapshot
- SPDX/classification: `GFDL-1.3-or-later AND GPL-3.0-or-later`
- Source: https://gcc.gnu.org/ and https://sourceware.org/binutils/ ; document sources in the bundled GCC and Binutils source trees
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: published with the release; see [licenses/README.md](licenses/README.md)
- Modifications: Per-document grants inspected. CTF/SFrame info specs are GPL-3.0-or-later; other GNU manuals are GFDL with document-specific invariant sections and cover texts preserved. Manuals are shipped unmodified; complete GCC and Binutils doc sources included.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [licenses/GFDL-1.3.texi](licenses/GFDL-1.3.texi)
  - [licenses/sdk-manual-notices.txt](licenses/sdk-manual-notices.txt)
  - [sdk/toolchain/licenses/GCC/COPYING3](sdk/toolchain/licenses/GCC/COPYING3)
  - [licenses/evidence/sdk-manuals.json](licenses/evidence/sdk-manuals.json)

## Generated GNU ld linker scripts

- Version: SDK snapshot; generator binary 2.46.0.20260210
- SPDX/classification: `LicenseRef-Binutils-Linker-Script`
- Source: Generated by GNU Binutils ld; original scripts shipped in SDK
- Source revision: Per-file SHA-256 / source snapshot; no upstream commit asserted
- Corresponding source: `sdk/toolchain/arm-uclinuxfdpiceabi/lib/ldscripts` (ships in this repository)
- Modifications: Copyright (C) 2014-2026 Free Software Foundation, Inc. Copying/distribution with or without modification permitted if copyright and notice are preserved.
- Copyright: Original component/file copyright holders; complete declarations in the referenced materials.
- Full licenses, NOTICEs and evidence:
  - [licenses/binutils-linker-script-notices.txt](licenses/binutils-linker-script-notices.txt)

## Binary distribution duties

Linux and BusyBox: GPL-2.0-only; supply complete modified source, configurations
and controlling scripts under a valid GPLv2 section 3 option. The Linux syscall
note concerns UAPI use; it is not an exception for distributing the kernel itself.

uClibc-ng: preserve individual LGPL/BSD/public-domain terms. Supply library
source and modifications, applicable notices, and the required ability to replace
or relink the library and debug modifications. Assess static/nonshared objects
separately; dynamic linking does not remove library source obligations.

GCC runtime: GPL-3.0-or-later WITH GCC-exception-3.1. Eligible compilation can
permit independent applications under other terms. Distribution of runtime
libraries themselves still requires their corresponding source and notices.

espeak-ng/pcaudiolib: GPLv3 section 6 applies, including voice-data source and
statically linked dependencies, build glue, patches and applicable installation
information. Port glue or upstream URLs alone are not a complete source release.

Preserve Apache-2.0 licenses, relevant NOTICE attribution and modification
notices. Permissive components still require their own copyright/grant/disclaimer.
GFDL manuals have separate document-specific conditions. Closed company code
does not become open source through aggregation, and must honor any third-party
dependencies. See [audit](licenses/AUDIT.md) and [release workflow](licenses/README.md).

## Source provenance and data attribution

The Unicode permission/disclaimer text was matched to Unicode-DFS-2016; original Copyright 1991-2018 Unicode, Inc. remains intact. Ucd-tools is included at the espeak-ng source revision. TinyALSA remains BSD-3-Clause.

Mandarin data credits Unicode Unihan and CC-CEDICT contributors. The current CC-CEDICT license text and attribution are preserved, while its historical extraction revision and original import license version remain unconfirmed. This limitation is reported, not inferred from the project license.

Host static dependency composition is backed by the original compiler link records and matching compiler binary hashes. External dynamic system libz/libzstd/libm/libc are not copied into the SDK. Full support-library sources and file-specific notices accompany the source bundle.

Generated SDK linker scripts retain their separate permissive grant (`LicenseRef-Binutils-Linker-Script`). SDK UAPI header origin is recorded with the kernel source and per-file grants; the syscall exception does not change the kernel license.
