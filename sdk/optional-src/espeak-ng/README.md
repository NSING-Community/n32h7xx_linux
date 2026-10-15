# espeak-ng on the N32H787 (ARMv7-M, no-MMU)

Port of [espeak-ng](https://github.com/espeak-ng/espeak-ng) 1.53.0 to this
board, built with the SDK toolchain in `sdk/toolchain/`.

**It builds, it runs, and it is bit-exact.** Synthesising the same text on the
board and on an x86-64 host produces byte-identical WAV files — every sample,
not "close enough". See `diagnostics/espeak-ng-2026-10-14/` for the evidence.

There is one operational caveat, and it is a property of this board's libc
rather than of espeak-ng: the 600 KiB `phondata` file has to land in a single
physically contiguous 1 MiB region. Read [the memory
precondition](#the-memory-precondition) before concluding the port "sometimes
fails to start".

## What is here

| Path | What it is |
| --- | --- |
| `Makefile` | Both builds and the voice-data step, driven off upstream's `src/libespeak-ng/CMakeLists.txt` and `src/ucd-tools/CMakeLists.txt` |
| `config.h` | Hand-written stand-in for the `config.h` upstream's CMake generates by probing the *build machine* |
| `compat/` | The `wchar.h` / `wctype.h` / wide-string functions this uClibc does not have |
| `probe/allocprobe.c` | Small diagnostic that measures what the board's allocator can actually satisfy |
| `../../sources/espeak-ng/` | Upstream source, a git checkout of `ba90c8e9f440ad544f674a790bb5f53878b6ffc5` (2026-09-22) |

## Why there are two builds

Upstream's CMake build has a target it calls `NativeBuild`, and it is not
optional. The voice data in `espeak-ng-data/` (`phontab`, `phonindex`,
`phondata`, `intonations`, `<lang>_dict`) is **produced by running espeak-ng
itself** over the rules in `phsource/` and `dictsource/`. The ARM binary cannot
run on the build machine, so:

* **native** — an ordinary x86-64 glibc `espeak-ng`. It is never shipped. Its
  only job is to execute `--compile-intonations`, `--compile-phonemes` and
  `--compile=<lang>`.
* **cross** — the port proper: `arm-uclinuxfdpiceabi-gcc` objects, linked into
  a single stripped binary.

`make all` runs native → data → cross in that order. The data step is
rebuilt from scratch every time rather than tracked file by file; it takes a
few seconds, and a half-written data directory is much more confusing than a
redundant rebuild.

`LANGS` (default `en`) is the one lever that matters for flash usage: every
entry costs its own `<lang>_dict` file and nothing else.

## The compat layer

`UCLIBC_HAS_WCHAR` is **not** set in this SDK's uClibc-ng, so the sysroot ships
no `<wchar.h>`, no `<wctype.h>`, and none of the `wcs*` functions.

Upstream already anticipates this. It carries `src/include/compat/{wchar.h,
wctype.h}`, which redirect the `isw*` classification functions at **ucd-tools** —
the Unicode character database espeak-ng links anyway. That is not a workaround;
ucd-tools answers these questions correctly for every script espeak-ng can read,
where a C-locale `iswalpha()` would not.

Upstream's shims are written to sit in front of a real system header
(`#include_next`, with MSVC's bundled one as the other fallback). With this libc
there is no header underneath, so the port supplies them. Three details are
load-bearing:

* **`-I$(PORT)/compat` must come first** in the include path, or the two shims
  resolve to each other in whichever order the paths happen to be listed. The
  user-visible symptom of getting this wrong is a link error about a duplicate
  `ucd_isalpha`.
* **No `isw*` definition may appear anywhere in the port.** The macro rewrites
  the definition itself into a second definition of the `ucd_` symbol, which
  collides with `ucd-tools/src/ctype.c` at link time. Classification is
  macros-only, on purpose.
* `wchar_shim.c` therefore holds only the string half: `wcslen`, `wcschr`,
  `wcsrchr`, `wcscmp`, `wcsncmp`, `wcscpy`, `wcsncpy`, `wcscat`, and `wcstod`.
  `wchar_t` itself needs nothing — GCC defines it in its own `<stddef.h>`,
  independently of libc — and espeak-ng never calls `mbrtowc`/`wcrtomb`; it does
  its own UTF-8 decoding in `encoding.c`.

`wcstod` is the one function there the library actually reaches at run time
(`ssml.c` parses `<prosody rate="...">`). It copies a bounded prefix, widening
ASCII and turning anything else into `'\1'` so that `strtod` stops there — which
is what a wide `strtod` would do as well.

## config.h

Upstream generates this by probing the build machine; a cross build cannot, and
this workspace has no cmake, so the values are pinned. Upstream writes them with
`#cmakedefine01` and tests them with `#if`, never `#ifdef`, so every symbol must
be defined — an undefined one is a compile error rather than a silently
disabled feature.

* `USE_ASYNC 0` — drops `event.c`/`fifo.c`/`espeak_command.c` and the event
  thread. Nothing here talks to the library while it is speaking.
* `USE_MBROLA 0`, `USE_LIBPCAUDIO 0`, `USE_LIBSONIC 0`,
  `USE_SPEECHPLAYER 0` — none is needed to synthesise into a file, none of the
  libraries is cross-built, and mbrola would drag in `fork()`/`execlp()`, which
  a no-MMU process cannot use for this purpose.
* `USE_KLATT 1` — cheap to keep, and dropping it would change the behaviour of
  any voice that asks for the Klatt formant path.
* `HAVE_MKSTEMP 1`, `HAVE_ISWBLANK 1` — true for the host and, via `compat/`,
  for the target.
* `HAVE_SYS_ENDIAN_H 0`, `HAVE_DECL_OPTRESET 0`, `HAVE_NBTOOL_CONFIG_H 0` —
  `sys/endian.h` is a BSD/musl header and neither glibc nor uClibc-ng has it.

## Building and deploying

Build prerequisites and source-directory overrides are described in
[the optional-source instructions](../README.md#building). The build produces
the native data compiler, ARM executable and voice data under `out/work/espeak-ng/`.
For the selected Mandarin dictionary, use the inputs and recipe in
[cmn-source/](cmn-source/README.md).

The optional component is deployed separately from firmware images. See
[the deployment instructions](../../optional/espeak-ng/README.md) for copying
the executable, data and license materials to the board.

## Running it

```sh
espeak-ng -w out.wav "The quick brown fox jumps over the lazy dog."
espeak-ng -v en -s 150 -w out.wav "..."    # voice, words per minute
```

`ESPEAK_DATA_PATH` overrides the data location; it names the directory
*containing* `espeak-ng-data`, not the data directory itself.

Flash budget, for planning. The installed payload is 1284889 B (1.23 MiB) raw —
294948 B of binary plus 989941 B of data — which compresses to **≈667129 B with
gzip** or ≈544264 B with xz. The rootfs SquashFS is built with
`-comp gzip`, so 667 KB is the number that matters there: the partition has
1589248 B free today, and this would leave about 900 KB. It would also fit
comfortably in `/system` on JFFS2. Note that `espeak-ng` is
**GPL-3.0-or-later** — see [Licensing](#licensing).

Nothing in the port is board-specific at run time: it reads its data, writes a
WAV, exits. No threads, no `fork`, no device access.

## The memory precondition

This is the part that will bite first, and it is worth understanding because it
applies to *any* large `malloc` on this board, not just espeak-ng.

The SDK's uClibc is configured `MALLOC_SIMPLE=y` with
`UCLIBC_UCLINUX_BROKEN_MUNMAP=y`. In
`libc/stdlib/malloc-simple/alloc.c`, the non-MMU branch turns **every** `malloc`
into its own mapping:

```c
result = mmap((void *) 0, size + sizeof(size_t), PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_ANONYMOUS | MAP_UNINITIALIZED, 0, 0);
```

There is no heap and no free-list reuse: `free()` is `munmap`, `realloc()` is
malloc + copy + free. In a no-MMU kernel a mapping must be *physically
contiguous*, so `mm/nommu.c: do_mmap_private()` calls
`alloc_pages_exact(total << PAGE_SHIFT, GFP_KERNEL)`, and **that rounds the
request up to a power-of-two number of pages**.

So `phondata`'s `malloc(600104)` becomes a 602112-byte (147-page) request that
needs one clean **order-8 = 1 MiB** buddy block. When the top orders are gone,
the run dies like this — note that the failure names the *page-aligned* size,
not the 600104 the library asked for:

```text
<4>[  158.430000] espeak-ng: page allocation failure: order:8, mode:0xcc0(GFP_KERNEL), nodemask=(null)
<3>[  158.690000] nommu: Allocation of length 602112 from process 241 (espeak-ng) failed
```

and `espeak-ng` reports `Error: Cannot allocate memory.`

The precondition is therefore just "at least one free order-8 block":

```sh
cat /proc/buddyinfo          # last-but-two column is order 8
```

Page cache is the usual consumer of those blocks. It is worth knowing that
**filling the cache does not always destroy them** — the port kept working
after 200 × 64 KiB reads from the SD card, with two order-8 blocks still free.
The reliable way to get them back is:

```sh
echo 3 > /proc/sys/vm/drop_caches
```

After which the whole sequence (`phontab` 64056, `phonindex` 48856, `phondata`
600104, `intonations` 2584, `en_dict` 182152) allocates cleanly.
`probe/allocprobe.c` is the tool used to establish this; build it with the
cross compiler and pass it the sizes.

A durable fix, if this ever needs to run unattended, would be to stop reading
`phondata` as one 600 KiB block — chunk it, or split the file by phoneme and
load on demand. That has not been done.

## Licencing

espeak-ng is **GPL-3.0-or-later** ([upstream
README](https://github.com/espeak-ng/espeak-ng#license-information), `COPYING`),
with `COPYING.APACHE`, `COPYING.BSD2` and `COPYING.UCD` covering bundled
components. The compiled voice data is part of the same source tree.

The optional binary is GPL-3.0-or-later regardless of its deployment location.
Redistribution requires its corresponding source, dependencies, build materials
and original notices under the applicable GPLv3 distribution option. The port
glue alone does not fulfill the complete corresponding-source requirement.

## Status

Verified on the board (kernel `7.3.0-rc6-01024-g642dfc8ceae0-dirty`):

* `--version` reports `1.53.0` and the data path.
* Three phrases, including a 2.79 s pangram, synthesise with `rc=0` and are
  **byte-identical to the native x86-64 build**.
* Timing: `"hi"` ≈0.54 s, `"Hello world."` ≈0.63 s, the pangram ≈0.88 s
  (cold, first output). A ten-run loop of `"hello world"` averaged 0.47 s.
* Peak cost during a pangram synthesis ≈2.7 MB of `MemFree`.

Not done, and not claimed:

* `libespeak-ng` is not built as a shared library. The port is one static
  binary; nothing else on the board links the library, and a static link avoids
  adding another `.so` for the no-MMU loader to map. Making a `.so` is easy
  (the objects already exist) if an application ever wants to embed it.
* Only English dictionaries were compiled. `LANGS` handles the rest but each
  adds its `_dict` to the shipped data.
* Only the file-output path was exercised (`-w`). Playback deliberately has no
  route on this board yet — see `doc/audio.md`.
* The `double`-precision arithmetic in `wavegen.c` runs in software (this part
  is FPv5-SP), so the timing above already includes that cost; no attempt was
  made to profile where it goes.
