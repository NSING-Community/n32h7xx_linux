# Third-party license audit — 2026-10-15

Scope: `bootloader/`, `kernel/`, `rootfs/`, `sdk/`, `tools/`. This is an
evidence audit of the material selected for release, not legal certification.
Every third-party component ships with its original declarations intact; the
audit records distribution terms and embedded dependencies, it does not
relicense anything.

Nations and NSING are the same company and their code is company-owned.
Optional NSLink ns-flash is a company-owned closed binary. Neither is treated as
a missing third-party grant or as an obligation to open that company's source.

`sdk/example/` contains company-written source; its locally compiled binaries
are not released. The packager excludes those build products from release
inventories and archives and records the exclusions, rather than treating them
as binaries missing build provenance or deleting them.

The generated materials come from `make legal-info` and
`make legal-info-sources`. They are host-side artifacts, written under `out/`:
they are never copied into rootfs, SquashFS, the bootloader or an SPI NOR image,
and building firmware does not depend on them.

## Findings

Corrected source associations and the evidence behind them are recorded in
[RESOLUTIONS.md](RESOLUTIONS.md); the generated report carries the current
blockers. Read this list as the findings that shaped the release material, not
as a status report.

* Keep root BSD-3-Clause and Linux's original COPYING/per-file licenses.
  The former cannot license third-party software on its own.
* Linux is **GPL-2.0-only**, and the UAPI syscall exception does not relax the
  source obligations that come with distributing a kernel binary. The recorded
  revision is a894130780a157e2f5a69a8a948e52b4f5bf227e (7.3.0-rc7, merge of
  upstream 7.3.0-rc7) plus the local NSING changes carried in this repository.
  The kernel evidence was regenerated for that revision from an isolated
  rebuild of the published configuration; the recorded revision, version and
  evidence therefore describe the tree that ships.
* BusyBox 1.38.0 LICENSE explicitly restricts distribution to GPL version 2.
  Applets are links to one program, not separate packages. Five local patches
  and the runtime configuration belong to BusyBox's corresponding source and
  must travel with it.
* uClibc-ng 1.0.59 ships LGPL 2.1; printf.c explicitly permits LGPL 2 or later,
  ldso.c has a custom permissive grant, and malloc.c includes public-domain
  declarations. Preserve these, rather than infer all files' terms from a name.
* GCC source says 16.1.0; libgcc2.c and libstdc++ thread.cc explicitly grant
  GPL-3.0-or-later with exception 3.1. Binutils source says 2.46.0. Existing SDK
  licenses can be reused. Host support dependencies need build/link provenance.
* Dropbear source says 2026.94 and carries multiple grants and attributions;
  retaining its top LICENSE alone omits referenced crypto licenses.
* microADB is Apache-2.0, with Simon Piriou's attribution and an NSING HAL.
  Project n32-flash and minimal SFTP sources carry MIT SPDX, despite the root BSD.
* TinyALSA's actual LICENSE has three conditions (BSD-3-Clause), not the
  BSD-2-Clause claimed by optional-package documentation.
* espeak-ng and pcaudiolib are GPL-3.0-or-later. Port glue is not the complete
  corresponding source; upstream data sources, linked code and build scripts
  must be accessible under an appropriate distribution option.
* Nations 1.2.0 peripheral, USB and device headers
  expressly restrict duplication/modification/distribution absent permission.
  They are company-owned, not a third-party permission blocker. Algorithm
  binary versions/build provenance remain unconfirmed. Preserve declarations.
* Optional NSLink ns-flash 0.1.4 is company-owned and closed-source. Public
  source is not required merely because it is included in the SDK. Its precise
  external redistribution terms and third-party dependency inventory remain
  to be documented; this is a WARNING, not a demand to open the source.
* A repository checkout is not a source release. Corresponding source,
  modifications, configuration and build scripts must be published beside the
  binaries they produced; version strings and upstream URLs do not by
  themselves prove that a binary was built from a given source.

## What the materials contain

An evidence registry and third-party notice index with original attribution;
the existing texts reused, plus the component-specific grants and NOTICEs that
were missing; a host-only repeatable `legal-info` bundle with a SHA-256
inventory and link ownership; and an optional full working-source snapshot with
recipes, configurations and patches. Unknowns stay WARNING/ERROR rather than
being filled in with invented metadata. See [README.md](README.md) for how to
generate them and [RESOLUTIONS.md](RESOLUTIONS.md) for the evidence.

## Distribution duties to review

GPLv2 kernel/BusyBox binaries need complete corresponding source (including
modifications and controlling build/install scripts) through a valid section 3
option. A homepage pointer alone is insufficient. GPLv3 add-ons need a valid
section 6 option, including installation information where applicable.

LGPL library distribution includes the library's source, modifications and notices.
Applications must allow the applicable replacement/relinking and debugging
rights; static or nonshared portions require particular attention. GCC exception
3.1 permits eligible combinations, but does not excuse distributing the runtime
library itself without meeting its source and notice obligations. Do not extend
the runtime exception to the compiler as a whole.

Source files' original declarations remain controlling. This audit identifies
potential top-level BSD scope confusion; it does not assert that third-party
headers were historically rewritten without evidence. No third-party ownership
or license headers are changed by this work.
