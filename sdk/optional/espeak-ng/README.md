# espeak-ng — optional add-on

A prebuilt [espeak-ng](https://github.com/espeak-ng/espeak-ng) 1.53.0 for the
N32H787. **This is not part of any image.** It is not in `rootfs/`, nothing in
`make` builds or installs it, and `make flash-image` will not put it on the
board. Where it goes on your system is your call.

| Path | What it is |
| --- | --- |
| `espeak-ng` | The ARM binary, stripped, 307252 B |
| `espeak-ng-data/` | The voice data (`phontab`, `phonindex`, `phondata`, `intonations`, `en_dict`, `cmn_dict`, `voices/`, `lang/`), 1988492 B |
| `licenses/` | `COPYING` (GPL-3.0-or-later), `COPYING.PCAUDIOLIB` (the same text, for the second GPL component inside the binary), the three bundled-component licences, and a provenance `NOTICE` |

Together that is 2295744 B raw, ≈1060614 B gzipped. The build glue for the
binary is **not** in this directory — it is beside it, in the repository under
[`sdk/optional-src/`](../../optional-src/), which covers both components.

## Licence — read this before you ship anything

espeak-ng is **GPL-3.0-or-later**, and the compiled voice data is under the same
licence. Your own files in this project are BSD-3-Clause. If you redistribute a
board or an image that contains this program, the GPL obligations in
`licenses/COPYING` apply to it — that is the reason it is kept out of the
default image and offered here instead.

**There are two GPL-3 projects in this directory, not one.** The binary also
contains [pcaudiolib](https://github.com/alsa-project/pcaudiolib) — the audio
output layer, which upstream espeak-ng uses to reach PulseAudio, ALSA, QSA or
OSS. This board has none of those, so pcaudiolib was ported to use TinyALSA
instead. Both are GPL-3 and the licence text is identical, which is why
`licenses/COPYING.PCAUDIOLIB` exists next to `COPYING`: the same words, named
after the second component so the two can be told apart in a provenance audit.

## Corresponding source

If you pass these binaries on, GPL-3.0 section 6 requires that the recipients
can get the corresponding source. It is all reachable from here:

* **The build glue for this binary** — the `Makefile`, `config.h`, `compat/` and
  `probe/` that turn upstream sources into the `espeak-ng` beside them — is in
  the repository at
  [`sdk/optional-src/espeak-ng/`](../../optional-src/espeak-ng/). It is small and
  self-contained, and together with pcaudiolib's `tinyalsa.c` it is the whole of
  the corresponding source that is not simply upstream.

* **Upstream espeak-ng**, unmodified, is at
  <https://github.com/espeak-ng/espeak-ng>, commit
  `ba90c8e9f440ad544f674a790bb5f53878b6ffc5`. Not one file of it was patched for
  this board. That repository is also where the voice data comes from: the
  `.wav`-generating data in `espeak-ng-data/` is compiled by running espeak-ng
  over `phsource/` and `dictsource/`, so those trees are part of the source too,
  and they are there.

* **pcaudiolib**, the second GPL-3 component, is at
  <https://github.com/alsa-project/pcaudiolib>, commit
  `d6a6b00aa4334b461c1a09c27b6c681eaac8da8a`. Unlike espeak-ng it **is**
  patched: a new TinyALSA backend. That patch cannot be applied from outside the
  library, because `src/audio.c` decides which backend to use and
  `src/audio_priv.h` declares them — so it is shipped as a file rather than
  described:

  ```sh
  git clone https://github.com/alsa-project/pcaudiolib ~/pcaudiolib-src
  git -C ~/pcaudiolib-src checkout d6a6b00aa4334b461c1a09c27b6c681eaac8da8a
  patch -p1 -d ~/pcaudiolib-src < sdk/optional-src/pcaudiolib/pcaudiolib-tinyalsa-backend.patch
  ```

  Three hunks — `Makefile.am`, `src/audio.c`, `src/audio_priv.h`. The patch was
  checked against a pristine checkout of that commit: it applies with no fuzz
  and reproduces the source tree the shipped library was built from byte for
  byte. The port's `tinyalsa.c` and `Makefile` are in the same directory
  ([`sdk/optional-src/pcaudiolib/`](../../optional-src/pcaudiolib/)), so applying
  the patch and adding those two files gives exactly the library linked into
  this binary. TinyALSA itself is BSD-3-Clause; include its source in the
  combined binary's corresponding-source release and retain its own notices.

If you hand out this directory on its own, provide the complete corresponding
source under the applicable GPLv3 section 6 option. For network distribution,
place a verified source archive and the legal companion beside the binary and
give clear download directions. `sdk/optional-src/` and upstream pointers alone
do not establish compliance. See the repository's `licenses/README.md` and run
`make legal-info-sources` to collect the available working sources and recipes.

## Deploying it

The directory is laid out so it runs from wherever you drop it: `espeak-ng-data`
sits next to the binary, which is the one thing the program needs to find.

**The quick way — one command.** [`../install.sh`](../install.sh), one directory
up, copies both add-on directories onto the board's `/system`. It builds
nothing and translates nothing. Choose the transport with `USE`, and pass
`BOARD` when you use ssh:

```sh
sh install.sh                        # adb (the default)
BOARD=<board-ip> USE=ssh sh install.sh
```

`BOARD` deliberately has no default: the board takes a DHCP lease, so a baked-in
address would only wait for an ssh timeout. Read the current one on the board
with `ifconfig eth0`. `scp -r` keeps the executable bit by itself, so the copy
lands byte-identical to what you downloaded. The manual equivalents follow, in
case you want to see or change what it does.

**Option 1 — run it in place, no installation.** Copy this whole directory to
the board (SD card, `/system`, wherever) and point `ESPEAK_DATA_PATH` at the
directory *containing* `espeak-ng-data`:

```sh
# on the board
ESPEAK_DATA_PATH=/mnt/sd/espeak /mnt/sd/espeak/espeak-ng "Hello world."
```

**Option 2 — install on the board's writable partition.** This is the form the
board was actually tested in. On this board `/` is a read-only SquashFS, so the
binary's compiled-in default (`/usr/share/espeak-ng-data`) cannot exist at
runtime — `/usr/share` is not writable. `/system` is JFFS2, it is writable, and
it survives a rootfs update, so the drop goes there. From the machine holding
this directory, one command:

```sh
scp -r espeak-ng /system/          # or:  adb push espeak-ng /system/
```

The directory keeps its name, so that leaves `/system/espeak-ng/` — 273 files,
byte for byte what you downloaded. No `mkdir`, no `chmod`: `scp -r` creates the
directory and carries the executable bit. Running it again merges into the
directory that is already there.

Then tell the program where its data is. `/system/bin` is on `PATH` for the
**serial login console** (it reads `/etc/profile`) but **not** for
non-interactive `ssh` or `adb shell`, so from those call it by full path:

```sh
ESPEAK_DATA_PATH=/system/espeak-ng /system/espeak-ng/espeak-ng "Hello world."
```

That speaks out of the board's audio output. `-v cmn` picks Mandarin; `-w
/system/out.wav` writes a file instead of speaking.

**If you want a launcher too.** The variable above is the whole story; a
launcher only saves typing it. Drop this 128-byte wrapper in `/system/bin` and
plain `espeak-ng "Hello world."` works on the serial console:

```sh
# on the board, as root
cat > /system/bin/espeak-ng <<'EOF'
#!/bin/sh
ESPEAK_DATA_PATH=${ESPEAK_DATA_PATH:-/system/espeak-ng}
export ESPEAK_DATA_PATH
exec /system/espeak-ng/espeak-ng "$@"
EOF
chmod 0755 /system/bin/espeak-ng    # needed here: the shell creates it 0644
```

It does exactly one thing — point `ESPEAK_DATA_PATH` at the directory
*containing* `espeak-ng-data`. There is no executable-relative lookup in the
program, so without it the binary looks in the compiled-in path and finds
nothing. A caller-set `ESPEAK_DATA_PATH` still wins, so the same drop can live
anywhere and the launcher is only a convenience. It is also the only thing that
is not part of the drop, which is why `/system/espeak-ng/` stays byte-identical
to what you downloaded.

**Option 3 — into a rootfs tree before building an image.** Only if you have
decided to ship it: copy the binary and data to `/usr/bin/espeak-ng` and
`/usr/share/espeak-ng-data` inside `rootfs/`, then `make rootfs-squashfs &&
make flash-image`. This is the one case where the compiled-in default path is
the right one, because the `/usr/share` in question is inside the tree being
packed rather than the read-only one on a running board — so no launcher is
needed and no variable has to be set.

## Output: it speaks directly

espeak-ng plays out of the board's own audio output:

```sh
espeak-ng "The quick brown fox jumps over the lazy dog."
espeak-ng -v en -s 150 "..."        # voice, words per minute
```

No file, no conversion, no second tool. This works because the binary carries
an audio backend the board can actually use — upstream pcaudiolib with a new
TinyALSA backend. (Earlier builds of this drop had none compiled in, and asking
one of them to speak **succeeded silently**: exit status 0, no message, no
sound. Upstream's only real sink is pcaudiolib, and `config.h` had switched it
off.)

Two properties of the board shape that backend, and neither is configurable
away from this side:

* **Exactly two channels.** The DAI declares `.channels_min = .channels_max = 2`
  and rejects anything else, while espeak-ng synthesises **mono**. The backend
  widens each sample to a stereo pair.
* **A fixed set of sample rates**, and 22050 Hz — the rate espeak-ng always
  emits and offers no option to change — is one of them. The backend therefore
  does **not** resample.

What the backend submits is bit-identical to taking the WAV espeak-ng itself
writes for the same input and duplicating every frame. That was checked by
running the real CLI against a recording `pcm_writei` stub on the host:
identical for three utterances with three different partial-period tails.

### Writing a file instead

The file routes still work, and they are what you want when something else is
going to consume the audio:

```sh
espeak-ng -w out.wav "The quick brown fox jumps over the lazy dog."
espeak-ng --stdout "..." > out.wav    # WAV on stdout
```

Output is bit-exact against an x86-64 build of the same version — the same text
produces a byte-identical WAV on both sides.

**Those WAVs will not play on this board as they are.** espeak-ng writes
**22050 Hz mono 16-bit** (`voice_samplerate = 22050` in
`src/libespeak-ng/speech.c` is a fixed default) and the DAI wants **stereo**, so
`tinyplay` refuses the file before playing a sample:

```text
$ tinyplay out.wav -D 0 -d 0
failed to open for pcm 0,0. cannot set hw params: Invalid argument
```

The rate is not the problem — 22050 is one of the DAI's seven rates. The channel
count is: `tinyplay` reads both out of the WAV header and ignores `-c` / `-r` /
`-b`, so the file itself has to be right, and the board has no alsa-lib `plug`
layer to widen it for you. A file has to be widened **before it gets to the
board**. Playing through this drop's own backend, above, needs no file at all.
See `doc/audio.md` for the board side.

## Languages

Two dictionaries are compiled into `espeak-ng-data/`:

| Voice | Dictionary | Size |
| --- | --- | --- |
| `en` — English | `en_dict` | 182152 B |
| `cmn` — Mandarin Chinese | `cmn_dict` | 998551 B |

```sh
espeak-ng -v en  "The quick brown fox jumps over the lazy dog."
espeak-ng -v cmn "你好，欢迎使用语音合成。"
```

`lang/` carries voice *definitions* for around three dozen languages, but a voice
only works once its `<lang>_dict` sits beside the others — the definition alone is
not enough. For example `lang/sit/yue` (Cantonese) is present but there is no
`yue_dict`, so Cantonese is not usable from this drop. To add a language, compile
its dictionary with upstream's `--compile=<lang>` and copy the resulting
`<lang>_dict` into `espeak-ng-data/`; see **Rebuilding** below.

`cmn_dict` is a selected subset of the upstream Mandarin sources. The
published 998551-byte dictionary includes 3860 `cmn_list` records, 2203
`cmn_emoji` records and 59064 selected `cmn_listx` records. Omitting `cmn_listx`
entirely produces a different, 85071-byte dictionary; the previous description
of this drop was incorrect. The full upstream extra list produces 1578875 B.
The restored readable inputs and a verified reconstruction recipe are in
`sdk/optional-src/espeak-ng/cmn-source/`. This is an equivalent reconstruction;
the original historical selection procedure was not retained. The resulting
dictionary matches this drop byte for byte. No published binary/data changed.

Mandarin input comments credit the Unicode Unihan database and CC-CEDICT
contributors (MDBG). See `licenses/Mandarin-NOTICE.txt` for attribution and the
remaining historical extraction-version limitation.

## One operational caveat

This is a property of the board's libc, not of espeak-ng, and it applies to any
large `malloc` here.

uClibc is built `MALLOC_SIMPLE=y`, so every `malloc` becomes its own `mmap`, and
in a no-MMU kernel a mapping must be physically contiguous. espeak-ng reads
`phondata` (600104 B) in one `malloc`, which the kernel rounds up to a
602112-byte request needing a **1 MiB (order-8) contiguous block**. When the
page cache has consumed those blocks the run fails:

```text
Error: Cannot allocate memory.
<3> nommu: Allocation of length 602112 from process 463 (espeak-ng) failed
```

**A failed allocation does not always announce itself.** There are two failure
modes and only one of them is loud:

| what fails | what you get |
| --- | --- |
| `phondata` | `Error: Cannot allocate memory.` on stderr, exit status 1, no file written |
| a dictionary (`cmn_dict`, and `en_dict` when it is large enough to need order 8) | **nothing at all** — exit status **0**, and a well-formed WAV of the correct length whose samples are **pure silence** |

The second row is not a board quirk; it is upstream's own control flow.
`LoadDictionary()` in `src/libespeak-ng/dictionary.c` returns 3 without printing
anything when its `malloc` fails, and its only caller in
`src/libespeak-ng/voices.c` discards the return value — the guard it does test,
`dictionary_name[0] == 0`, has already been set non-zero at the top of
`LoadDictionary()`. So synthesis proceeds with no dictionary loaded and exits
cleanly.

**So do not treat exit status 0 as proof that you got audio.** If a phrase comes
out silent, it may be this. To tell: play the file and listen, or count non-zero
bytes in the `data` payload (a silent one is all zeros).

The dangerous state is not "no memory" — it is **exactly one** free order-8
block. `phondata` is allocated first and takes it; the dictionary is allocated
second and fails silently. Zero free order-8 blocks is the *safe* failure: it
stops at `phondata` and refuses. Check for **two**, not one:

```sh
cat /proc/buddyinfo      # the order-8 count is the ninth number on this board
echo 3 > /proc/sys/vm/drop_caches
```

That usually works, because a large page cache is usually what is holding those
blocks. It is not a guarantee: when memory is plentiful but scattered — `MemFree`
in the megabytes while the largest contiguous run is only 512 KB — there is
nothing for `drop_caches` to release, and this kernel has no page compaction
(no-MMU). If retrying does not restore the ninth number, there is no knob left
but to let memory actually free up, or to reboot. The real fix is to stop loading
`phondata` as a single block.

Synthesis takes ≈0.5–0.9 s per phrase and peaks at ≈2.7 MB of `MemFree`.

**Chinese is stricter than English.** It needs three contiguous blocks at the same
time, where English needs two:

| what | length asked of the kernel | needs |
| --- | --- | --- |
| the program's own read-only text segment | 294912 B | 512 KB, order 7 |
| `phondata` | 602112 B | 1 MiB, order 8 |
| `cmn_dict` | 999424 B | 1 MiB, order 8 |

The two order-8 rows are the ones to watch, and **they fail differently** — see
the two failure modes above. `phondata` is requested first and its failure is
reported; `cmn_dict` is requested second and its failure is silent. A state with
one free order-8 block therefore gets past `phondata` and fails here, quietly.
English has one order-8 request (`phondata`), so it has no silent mode from this
cause.

The first row applies to **every** run, before the program has read anything —
even `--version`. The loader maps the binary's `R E` LOAD segment `MAP_PRIVATE`,
and on this no-MMU kernel a private file mapping is a physical copy rather than a
view of the page cache, so the kernel allocates it in one piece. The segment is
`0x47d0c` = 294156 bytes of file content; `PAGE_ALIGN` turns that into 72 pages =
294912 B, and `get_order()` rounds that to the next power of two pages — 128
pages, order 7. `en_dict` is 182152 B and fits inside a 256 KB block, so English
never asks for order 8 except for `phondata`.

That is why the ninth number in `/proc/buddyinfo` (the order-8 count) is not the
whole story here: check the eighth (order-7) as well. It also works the other
way — a *larger* free block counts, because the buddy allocator splits it, so
two free order-9 (2 MiB) blocks will satisfy both order-8 requests.

## Rebuilding

Everything needed on top of the two upstreams is in the repository under
[`sdk/optional-src/`](../../optional-src/) — `espeak-ng/` for the program,
`pcaudiolib/` for its audio layer. That directory's README has the full recipe;
briefly, three checkouts (espeak-ng, pcaudiolib, TinyALSA), the backend patch,
then two `make` commands, pcaudiolib first because the espeak-ng link absorbs
its archive:

```sh
git clone https://github.com/espeak-ng/espeak-ng     ~/espeak-ng-src
git -C ~/espeak-ng-src checkout ba90c8e9f440ad544f674a790bb5f53878b6ffc5

git clone https://github.com/tinyalsa/tinyalsa       ~/tinyalsa-src
git clone https://github.com/alsa-project/pcaudiolib ~/pcaudiolib-src
git -C ~/pcaudiolib-src checkout d6a6b00aa4334b461c1a09c27b6c681eaac8da8a
patch -p1 -d ~/pcaudiolib-src < pcaudiolib/pcaudiolib-tinyalsa-backend.patch

make -C pcaudiolib SRC=~/pcaudiolib-src TINYALSA=~/tinyalsa-src
make -C espeak-ng  SRC=~/espeak-ng-src \
                   PCAUDIO_SRC=~/pcaudiolib-src TINYALSA_SRC=~/tinyalsa-src
```

The build needs a host x86-64 compiler as well as the ARM toolchain: the voice
data is generated by running espeak-ng itself, which the ARM binary cannot do on
your build machine.

For source prerequisites, build variables and the selected Mandarin dictionary
recipe, see [the optional-source instructions](../../optional-src/README.md).
