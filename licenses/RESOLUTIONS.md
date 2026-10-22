# Evidence closure — 2026-10-15

This supplements the initial [audit and plan](AUDIT.md). Reviewed source/input
digests invalidate the review when selected inputs change. No target device
was accessed. No rootfs, SDK executable/library or firmware image was replaced.

| Finding | Evidence and resolution |
| --- | --- |
| GCC host composition ERROR | Original link records identify static GMP 6.3.0, MPFR 4.2.2, MPC 1.3.1, ISL 0.24 and GCC support libraries. Released cc1/cc1plus/lto1 hashes match the original source-build manifest. External host dependencies distinguished. [Evidence](evidence/host-dependencies.json) |
| Dropbear provenance ERROR | Four released binaries match preserved stripped outputs. 831 source/config inputs compared; local options and mixed grants preserved. [Evidence](evidence/dropbear.json) |
| uClibc incorrect source association | A legacy local libc tree is **1.0.57**, not the shipped 1.0.59. Fresh isolated build of the pinned 1.0.59 tarball plus patch/config reproduces libc/loader byte for byte. Correct source selected for delivery. [Evidence](evidence/uclibc.json) |
| Linux corresponding source | Isolated build reproduces DTB. All allocated ELF bytes match after normalizing only exact GNU build-ID bytes and minimal built-in cpio mtimes. Different output paths affect build-ID; cpio generation records wall clock time. **zImage is not claimed byte-identical.** Normalizations and section hashes recorded. Dirty/untracked source retained; ignored artifacts excluded. [Evidence](evidence/linux.json) |
| BusyBox source association | Published binary matches preserved runtime build. Source digest, configuration and five patches retained. Existing-build association verified; no fresh runtime rebuild claimed. Applet links remain one component. [Evidence](evidence/busybox.json) |
| Binutils / GCC runtime | Preserved original build outputs reproduce current files after standard stripping. Complete sources and recipes retained. [Binutils](evidence/binutils.json), [runtime](evidence/gcc-runtime.json) |
| CMSIS import | All 24 headers match official CMSIS_5 5.6.0, commit b5f0603d6a584d1724d952fd8b0737458b90d62b, ignoring CRLF only. Arm copyright/version headers unchanged. [Evidence](evidence/cmsis.json) |
| Unicode SPDX | Actual permission/disclaimer body matches Unicode-DFS-2016; original 1991–2018 attribution retained. [Evidence](evidence/espeak.json) |
| Mandarin reconstruction | Dictionary was **not** built by omitting cmn_listx. Restored editable inputs retain 59064 extra-list records and recompile to the same 998551-byte file. Historical selection procedure not asserted. [Inputs and recipe](../sdk/optional-src/espeak-ng/cmn-source/README.md) |
| Closed ns-flash | Original company ARM output matches the optional binary. Zig 0.16.0 MIT attribution supplied. Private program source stays unpublished. [Evidence](evidence/ns-flash.json) |
| SDK manuals | Per-document grants retained, including GCC manuals and GPL CTF/SFrame specifications. GFDL invariant sections/cover texts preserved. [Evidence](evidence/sdk-manuals.json) |
| microADB / TinyALSA | Current daemon matches its preserved stripped build. A fresh isolated TinyALSA build reproduces all four published utilities byte for byte; no identity claim for the static archive container. [microADB](evidence/microadb.json), [TinyALSA](evidence/tinyalsa.json) |
| curl / Mbed TLS / CA bundle | Published build scripts reproduce the curl binary and all three Mbed TLS shared objects byte for byte from the pinned upstream releases; no source changes. Mbed TLS is dual Apache-2.0 OR GPL-2.0-or-later and this build is taken under Apache-2.0. The CA bundle is the published curl.se file, hash-matched against the staged copy and the rootfs copy, with its Mozilla certdata.txt revision recorded in the file header. Cross-compiled configure probes cannot run, so the curl build forces `ac_cv_func_eventfd=no` for this kernel. [curl](evidence/curl.json), [Mbed TLS](evidence/mbedtls.json), [CA bundle](evidence/ca-certificates.json) |

## Evidence refresh — 2026-10-16

Later work moved reviewed inputs again, so the record follows the edits instead
of reading them as drift. `tools-internal/refresh_uclibc_evidence.py` and
`tools-internal/refresh_release_evidence.py` re-record a component's hashes only
after re-checking that the deployed files still come from the reviewed inputs;
neither one edits a firmware input.

`refresh_release_evidence.py --source-digest COMPONENT` re-derives the
provenance digest of a source tree that was edited on purpose; it accepts only a
component that already records a digest, so it can follow an edit to a reviewed
tree but never start vouching for an unaudited one.

| Finding | Evidence and resolution |
| --- | --- |
| uClibc configuration and libc changed | Seven switches the vendor configuration had silently turned off upstream defaults (including `MALLOC_SIMPLE` earlier) were restored. `sdk-src/configs/uclibc.config` and the deployed `libuClibc-1.0.59.so` were re-recorded; the loader is unchanged. The corresponding-source tree under `out/legal-proof/uclibc-1.0.59/src` was rebuilt from the pinned tarball, the FDPIC patch and the new configuration, and reproduces both deployed shared objects byte for byte; the static archives match member by member. [Evidence](evidence/uclibc.json) |
| CA bundle installed one certificate per file | The installer now also writes `/etc/ssl/certs/roots`, one file per certificate, because mbedTLS parses a PEM file with a single allocation as large as the whole bundle and the no-MMU path cannot always find 185 KiB contiguously. The split is regenerated from the same checked `cacert.pem` on every install, and the script hash was re-recorded. [CA bundle](evidence/ca-certificates.json) |
| WeniClaw add-on has no evidence record | The agent's project code is BSD-3-Clause, with libcurl, Mbed TLS and the Zig runtime covered by their own component records and notices. [Evidence](evidence/weniclaw.json) |
| Zig build caches read as distributed binaries | `tools-internal/legal_info.py` now excludes `.zig-cache/` and `zig-out/` by directory name, at any depth, from the inventory, the source archives and the tree digests. They are regenerable build output that the tree's own `.gitignore` already excludes, so they are recorded in `excluded-files.json` rather than reported as binaries missing provenance. |
| WeniClaw collapsed every failed config open into `OpenFailed` | A board with no `--config` file read exactly like a file the user may not open, and the hint sent them looking for permissions. The port now classifies the failed open (`FileNotFound` / `AccessDenied` / other), writes a 0600 starting template from `weniclaw.example.json` when the config is simply absent, and stops with instructions instead of running on the placeholder values. The edit moved the reviewed tree, so the component's `source_digest` was re-recorded with `--source-digest`. Still source only, no binary component. [Evidence](evidence/weniclaw.json) |
| Abandoned external add-on withdrawn | The abandoned external add-on was removed from the release and `sdk/optional/install.sh` no longer carries it. `sdk/optional/weniclaw` ships the reviewed ARM FDPIC binary with its README, BSD-3-Clause license, and the Zig, musl and curl notice texts it needs. [Evidence](evidence/weniclaw.json) |
| WeniClaw port source withheld from the published tree | `sdk/optional-src/weniclaw` is maintainer material for now and is not published: the directory is git-ignored, the generated notices mark the WeniClaw corresponding source as not included, and `sdk/optional-src/` documents only the espeak-ng/pcaudiolib sources. The add-on keeps the reviewed binary and its notice texts; the component record retains the reviewed build-input hashes for the audit. [Evidence](evidence/weniclaw.json) |
| WeniClaw binary refresh recorded | The add-on now dynamically links the rootfs `libcurl.so.4`; the binary is 213856 B, its largest mapping is 200704 B and its stack ceiling is 131072 B. The component and evidence records name the final binary, notices and source revision. [Evidence](evidence/weniclaw.json) |
| WeniClaw add-on ships peripheral skills | `sdk/optional/weniclaw/skills/` now carries the four peripheral skill texts (camera, audio, gpio-led, storage) beside the binary, `sdk/optional/install.sh` installs them to `/system/weniclaw/workspace/skills`, and the README documents the layout. The camera skill defaults to 1024x768 so routine captures stay small. [Evidence](evidence/weniclaw.json) |
| WeniClaw think filtering is project code | The `<think>` scanner was independently reimplemented with tests for nested, incomplete, orphan and plain-text inputs. The extra third-party license file and all corresponding notices were removed; WeniClaw, Zig, musl and curl are the remaining attribution set. [Evidence](evidence/weniclaw.json) |

The linux component still pins the kernel built before the `CAMERA` switch
landed. Its evidence is regenerated from an isolated rebuild of the final
revision when the kernel tree is committed for release; until then that
component stays flagged, and the flagged state is not published as compliant.

## Remaining historical data warning

Upstream Mandarin cmn_listx credits Unicode Unihan and CC-CEDICT. Original
CC-CEDICT extraction revision/date and import license version were not retained.
Current publisher licensing and attribution accompany the optional drop, but
are not asserted to prove the historical import version. Recover original
import records or obtain upstream clarification to close this warning. Do not
remove it solely because the enclosing program is GPL. No data was replaced.

References: [CC-CEDICT publisher](https://cc-cedict.org/editor/editor.php?handler=Download),
[SPDX Unicode-DFS-2016](https://spdx.org/licenses/Unicode-DFS-2016.html).

## Release boundary

`make legal-info` emits metadata without complete source; source-bundle ERRORs
identify this incomplete standalone package. Use
`make legal-info-sources LEGAL_STRICT=1` for the full source companion. Strict
mode fails on ERROR and retains WARNINGs visibly; it does not resolve historical
unknowns or publish anything. Deliver reviewed companions with the exact binary
release and satisfy the selected GPL/LGPL distribution option, including
applicable installation information and replacement/relinking requirements.
