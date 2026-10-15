# License audit and implementation plan — 2026-10-15

This records the audit and plan presented before changes. Scope:
`bootloader/`, `kernel/`, `rootfs/`, `sdk/`, `tools/`; ignored maintainer sources
were read as evidence. The working tree already contained many unrelated
changes. They were preserved. This is an evidence audit, not legal certification.

Owner clarifications during implementation supersede the initial authorization
concerns below: Nations and NSING are the same company and their code is
company-owned. Optional NSLink ns-flash is a company-owned closed binary.
Neither case is treated as a missing third-party grant or an obligation to
open that company's source. Original declarations remain intact; distribution
terms and embedded third-party dependencies are recorded separately.

Additional owner clarification: `sdk/example/` contains company-written source.
Its locally compiled binaries are not released. The packager excludes those
build products from release inventories/archives and records the exclusions;
it does not treat them as binaries missing build provenance or delete them.

Owner clarification: the new legal-info generator is internal maintainer tooling.
Its implementation/tests reside in unpublished `tools-internal/`, and are not
included in generated source/legal packages. The Makefile targets remain for
maintainers and give a clear diagnostic when the internal tool is absent.

## Findings

These are the initial findings. Subsequent evidence and corrected source
associations are recorded in [RESOLUTIONS.md](RESOLUTIONS.md); consult the
generated report for current blockers rather than treating this history as
the final status.

* Keep root BSD-3-Clause and Linux's original COPYING/per-file licenses.
  The former cannot license third-party software on its own.
* Linux declares 7.3.0-rc6, pinned at 642dfc8ceae0e80cce8d9fcc92fcf8c992da279d
  with dirty and untracked local changes. GPL-2.0-only; UAPI syscall exception
  does not relax kernel binary distribution requirements.
* BusyBox 1.38.0 LICENSE explicitly restricts distribution to GPL version 2.
  Applets are links to one program, not separate packages. Five local patches
  and runtime configuration are present in ignored `sdk-src/`.
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
* **Initial finding, superseded by owner clarification above:** representative Nations 1.2.0 peripheral, USB and device headers
  expressly restrict duplication/modification/distribution absent permission.
  They are company-owned, not a third-party permission blocker. Algorithm
  binary versions/build provenance remain unconfirmed. Preserve declarations.
* Optional NSLink ns-flash 0.1.4 is company-owned and closed-source. Public
  source is not required merely because it is included in the SDK. Its precise
  external redistribution terms and third-party dependency inventory remain
  to be documented; this is a WARNING, not a demand to open the source.
* **ERROR:** ignored `sdk-src/` does not accompany a public checkout. Source
  version strings and upstream URLs do not prove correspondence to binaries.

## Implementation

1. Evidence registry and third-party notice index, with original attribution.
2. Reuse existing texts; collect missing component-specific grants and NOTICEs.
3. Host-only repeatable legal-info bundle, SHA-256 inventory and link ownership.
4. Full working-source snapshot option, with recipes/configs/patches, and strict
   release checks. Unknowns remain WARNING/ERROR rather than invented metadata.
5. README Licensing explanation and corrections to identified misstatements.
6. Verify unchanged rootfs/LICENSE/kernel COPYING/firmware hashes; repeat bundles
   and exercise negative cases. No firmware rebuild or board write is needed.

## Distribution duties to review

GPLv2 kernel/BusyBox binaries need complete corresponding source (including
modifications and controlling build/install scripts) through a valid section 3
option. A homepage pointer alone is insufficient. GPLv3 add-ons need a valid
section 6 option, including installation information where applicable.

LGPL library distribution includes library source/modifications and notices.
Applications must allow the applicable replacement/relinking and debugging
rights; static or nonshared portions require particular attention. GCC exception
3.1 permits eligible combinations, but does not excuse distributing the runtime
library itself without meeting its source and notice obligations. Do not extend
the runtime exception to the compiler as a whole.

Source files' original declarations remain controlling. This audit identifies
potential top-level BSD scope confusion; it does not assert that third-party
headers were historically rewritten without evidence. No third-party ownership
or license headers are changed by this work.
