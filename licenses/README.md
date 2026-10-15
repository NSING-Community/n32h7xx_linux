# License materials (host and release only)

The repository's `LICENSE` remains BSD-3-Clause for project-owned material.
Nothing here replaces any third-party grant, copyright, disclaimer or NOTICE.
`components.json` is the reviewed evidence registry; its version declarations
are not claims that every existing binary was built from those sources.
`NOASSERTION` and `LicenseRef-*` are deliberate unresolved/custom labels.
`LicenseRef-Company-Owned` records the owner's confirmation that Nations and
NSING are the same company; original file terms remain intact.
`LicenseRef-Company-Proprietary` records the closed optional ns-flash binary,
without inventing an open-source grant or requiring publication of company code.
`LicenseRef-Dropbear-Mixed` refers to the preserved custom permissive grants.
`LicenseRef-CC-CEDICT-Historical` marks the explicitly unconfirmed historical
Mandarin data-import license version; it is not a grant inferred from GPL.
Host support-library composition is identified by original link records and
matching compiler hashes; see [evidence closure](RESOLUTIONS.md).

Full standard license texts already in `sdk/toolchain/licenses/`, Linux's
`LICENSES/` and the optional add-on are referenced rather than copied again.
Component-specific grants, attribution and vendor restrictions are preserved
here or in their existing locations. The generated bundle stores byte-identical
texts once by SHA-256 and maps every original path to that object. Different
copyright notices must not be deduplicated merely because the license name matches.

Run `make legal-info` to generate `out/legal-info/` and a companion tarball.
Both targets write only under `out/`; no firmware image, rootfs or source tree
is modified, and building firmware does not depend on them. The generated
reports, inventories and archives are what a release ships as its compliance
materials.
The inventory covers all regular files and links under bootloader/kernel/rootfs/sdk/tools,
without running target programs. Ignored kernel build artifacts are excluded;
dirty and untracked source changes are retained. `sdk/example/`
is company-owned source-only material: its local ELF binaries, object files,
archives, shared libraries and links to those products are excluded from
distribution inventories and source archives. `excluded-files.json` records
these exclusions; local files are left intact. Applet/library
links resolve to their owning component. It includes a component registry,
file hashes, source acquisition notes, local kernel patch, build recipes,
configs, notices and a WARNING/ERROR report. No network request is made.
Check the inventory against the files actually selected for your release.

Run `make legal-info-sources` to additionally archive available working source
trees (including untracked changes, excluding `.git`). This may take several
minutes and considerable host storage. The snapshot preserves the selected
actual local tree. Reviewed correspondence methods and limitations are recorded
in evidence files and checked against source/input digests. Missing sources,
uncertain grants, binary provenance and incomplete link audits remain visible
blockers where applicable.
A source release is a separate artifact: the complete corresponding source,
its modifications, configuration and build scripts must be published beside the
binaries they correspond to. A repository clone is not a substitute for it.

For a release use `make legal-info-sources LEGAL_STRICT=1`. A nonzero exit
still leaves inspectable reports and archives. Resolve all ERRORs and manually
verify binary/source correspondence, build prerequisites, library replacement
or relinking, and any applicable GPLv3 installation-information requirements.
Neither an upstream URL nor the generated acquisition notes are a written offer.
The tooling does not issue an offer on behalf of the distributor.

Ship the legal tarball and verified complete source beside each SDK/firmware
download, on the same release page with clear links and equivalent access as
required by the selected license option. Hardware-only distribution may require
an accompanying valid source offer or source medium; assess that separately.
Do not publish files marked BLOCKED as compliant.

There is no firmware dependency on either target. Legal output is never copied
to rootfs, SquashFS, bootloader or SPI NOR images. Existing rootfs notices are
left untouched. Default builds and boot behavior are unchanged.

Primary references: [Linux licensing rules](https://docs.kernel.org/process/license-rules.html),
[GPLv2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html),
[LGPLv2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html),
[GPLv3](https://www.gnu.org/licenses/gpl-3.0.html),
[GCC exception 3.1](https://www.gnu.org/licenses/gcc-exception-3.1.html).
