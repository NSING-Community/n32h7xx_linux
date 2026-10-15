# Evidence closure — 2026-10-15

This supplements the initial [audit and plan](AUDIT.md). Reviewed source/input
digests invalidate the review when selected inputs change. No target device
was accessed. No rootfs, SDK executable/library or firmware image was replaced.

| Finding | Evidence and resolution |
| --- | --- |
| GCC host composition ERROR | Original link records identify static GMP 6.3.0, MPFR 4.2.2, MPC 1.3.1, ISL 0.24 and GCC support libraries. Released cc1/cc1plus/lto1 hashes match the original source-build manifest. External host dependencies distinguished. [Evidence](evidence/host-dependencies.json) |
| Dropbear provenance ERROR | Four released binaries match preserved stripped outputs. 831 source/config inputs compared; local options and mixed grants preserved. [Evidence](evidence/dropbear.json) |
| uClibc incorrect source association | Legacy sdk-src/sources/uclibc is **1.0.57**, not shipped 1.0.59. Fresh isolated build of pinned 1.0.59 tarball + patch/config reproduces libc/loader byte for byte. Correct source selected for delivery. [Evidence](evidence/uclibc.json) |
| Linux corresponding source | Isolated build reproduces DTB. All allocated ELF bytes match after normalizing only exact GNU build-ID bytes and minimal built-in cpio mtimes. Different output paths affect build-ID; cpio generation records wall clock time. **zImage is not claimed byte-identical.** Normalizations and section hashes recorded. Dirty/untracked source retained; ignored artifacts excluded. [Evidence](evidence/linux.json) |
| BusyBox source association | Published binary matches preserved runtime build. Source digest, configuration and five patches retained. Existing-build association verified; no fresh runtime rebuild claimed. Applet links remain one component. [Evidence](evidence/busybox.json) |
| Binutils / GCC runtime | Preserved original build outputs reproduce current files after standard stripping. Complete sources and recipes retained. [Binutils](evidence/binutils.json), [runtime](evidence/gcc-runtime.json) |
| CMSIS import | All 24 headers match official CMSIS_5 5.6.0, commit b5f0603d6a584d1724d952fd8b0737458b90d62b, ignoring CRLF only. Arm copyright/version headers unchanged. [Evidence](evidence/cmsis.json) |
| Unicode SPDX | Actual permission/disclaimer body matches Unicode-DFS-2016; original 1991–2018 attribution retained. [Evidence](evidence/espeak.json) |
| Mandarin reconstruction | Dictionary was **not** built by omitting cmn_listx. Restored editable inputs retain 59064 extra-list records and recompile to the same 998551-byte file. Historical selection procedure not asserted. [Inputs and recipe](../sdk/optional-src/espeak-ng/cmn-source/README.md) |
| Closed ns-flash | Original company ARM output matches optional binary. Zig 0.16.0 MIT attribution supplied. Private program source/internal legal tools excluded. [Evidence](evidence/ns-flash.json) |
| SDK manuals | Per-document grants retained, including GCC manuals and GPL CTF/SFrame specifications. GFDL invariant sections/cover texts preserved. [Evidence](evidence/sdk-manuals.json) |
| microADB / TinyALSA | Current daemon matches its preserved stripped build. A fresh isolated TinyALSA build reproduces all four published utilities byte for byte; no identity claim for the static archive container. [microADB](evidence/microadb.json), [TinyALSA](evidence/tinyalsa.json) |

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
