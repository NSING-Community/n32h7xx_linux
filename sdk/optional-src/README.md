# Optional add-ons — source

The source for what is in [`../optional/`](../optional/). Unlike the rest of
`sdk/`, the contents here are **not built or installed by `make`**; they exist
to supply the port code for the prebuilt binaries in `../optional/`. The full
corresponding-source release must also contain upstream sources, dependencies,
selected dictionary inputs, modifications and build materials required by GPLv3.

| Directory | Feeds | Licence |
| --- | --- | --- |
| `espeak-ng/` | `../optional/espeak-ng/` (the binary) | GPL-3.0-or-later |
| `pcaudiolib/` | the same binary (its audio output layer) | GPL-3.0-or-later |

## espeak-ng

`espeak-ng/` is the build glue for the ARM binary next door: a `Makefile` that
mirrors upstream's CMake, a hand-written `config.h`, and the wide-character
`compat/` layer this uClibc does not have.

It is only part of the story. Upstream espeak-ng is **not patched** and is not
duplicated here; check out
<https://github.com/espeak-ng/espeak-ng> at commit
`ba90c8e9f440ad544f674a790bb5f53878b6ffc5` beside it, and you have the whole
corresponding source, including the `phsource/` and `dictsource/` the voice data
was generated from.

`espeak-ng/README.md` describes the native/cross builds and memory requirements.
The selected Mandarin dictionary inputs and reproduction instructions are in
`espeak-ng/cmn-source/`.

## pcaudiolib

`pcaudiolib/` is the audio output layer `espeak-ng/` links. Upstream speaks to
PulseAudio, ALSA, QSA and OSS; this board has none of those four (its only ALSA
userland is TinyALSA), so the port adds a TinyALSA backend in `tinyalsa.c` and
turns `USE_LIBPCAUDIO` on for the target build only.

**This is the one component that is patched**, and the patch is shipped here
rather than described: a backend cannot be added from outside the library,
because `audio.c` decides which backend to use and `audio_priv.h` declares them.
Three hunks — `Makefile.am`, `src/audio.c`, `src/audio_priv.h` — are in

```text
pcaudiolib/pcaudiolib-tinyalsa-backend.patch
```

So the corresponding source for the library is: upstream pcaudiolib at commit
`d6a6b00aa4334b461c1a09c27b6c681eaac8da8a`, plus that patch, plus the glue in
this directory. Unlike espeak-ng, which must stay byte-unmodified, patching
pcaudiolib is fine — it is a separate GPL-3 project and the patch travels in the
same release tree.

TinyALSA itself is BSD-3-Clause. Include its source as a linked dependency in
the complete corresponding source for the combined GPL binary, and preserve
its own copyright, license and disclaimer.

`pcaudiolib/README.md` is the real documentation: why the DAI's exactly-2-channel
rule forces mono→stereo widening, why there is deliberately no resampler, why
the scratch buffer is a fixed array inside the object rather than a write-sized
`malloc`, and why the static archive rather than the `.so` is what espeak-ng
links.

## Building

Fetch both upstreams and point the variables at them. The pcaudiolib glue is
next door, so `PCAUDIO_PORT` is the one path that stays inside this tree:

```sh
git clone https://github.com/espeak-ng/espeak-ng          ~/espeak-ng-src
git -C ~/espeak-ng-src checkout ba90c8e9f440ad544f674a790bb5f53878b6ffc5

git clone https://github.com/tinyalsa/tinyalsa            ~/tinyalsa-src
git clone https://github.com/alsa-project/pcaudiolib      ~/pcaudiolib-src
git -C ~/pcaudiolib-src checkout d6a6b00aa4334b461c1a09c27b6c681eaac8da8a
patch -p1 -d ~/pcaudiolib-src < pcaudiolib/pcaudiolib-tinyalsa-backend.patch

# TinyALSA first: pcaudiolib links its archive, and building it needs the
# kernel's sound UAPI headers (upstream: `make -C src libtinyalsa.a`).

make -C pcaudiolib  SRC=~/pcaudiolib-src TINYALSA=~/tinyalsa-src
make -C espeak-ng   SRC=~/espeak-ng-src \
                    PCAUDIO_SRC=~/pcaudiolib-src \
                    TINYALSA_SRC=~/tinyalsa-src
```

Each `Makefile` writes under `OUT`, which defaults to `<repo>/out/work/<name>`
in both trees, so the archives pcaudiolib produces are the ones espeak-ng picks
up — no extra variable needed. `TINYALSA`/`TINYALSA_SRC` only locate the header
directory and the archive; neither Makefile builds TinyALSA.

Set source-directory variables explicitly, as above. The toolchain and output
paths are derived from the Makefiles; override OUT to use a separate build directory.
