# SPDX-License-Identifier: BSD-3-Clause
#
# espeak-ng for the N32H787 ARMv7-M no-MMU target, and for the build host.
#
# Two builds come out of this, and the order matters:
#
#   native  an ordinary x86-64 espeak-ng.  It is not shipped anywhere.  Its
#           only job is to run --compile-phonemes / --compile-intonations /
#           --compile=<lang>, because the voice data is produced by *executing*
#           espeak-ng, and the ARM binary cannot run on the build machine.
#           (Upstream's CMake build does exactly the same thing and calls it
#           NativeBuild.)
#
#   cross   the actual port: libucd.a + libespeak-ng.a + the espeak-ng CLI for
#           arm-uclinuxfdpiceabi, plus the compat layer that stands in for the
#           wchar/wctype functions this uClibc build does not have.
#
# Upstream builds with CMake; there is no cmake in this workspace, so the file
# lists below mirror src/libespeak-ng/CMakeLists.txt and src/ucd-tools/CMakeLists.txt
# by hand.  When bumping espeak-ng, diff those two files against these lists.

PORT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
TOP  := $(abspath $(PORT)/../../..)
SRC  ?= $(TOP)/sdk-src/sources/espeak-ng
OUT  ?= $(TOP)/out/work/espeak-ng

# Which languages get a compiled dictionary.  Every entry costs its own
# <lang>_dict in the shipped data directory and nothing else; trimming this
# list is the one lever that matters for flash usage.
LANGS ?= en

# ------------------------------------------------------------ pcaudiolib -----

# espeak-ng's audio output goes through pcaudiolib, which is a port of its own
# with a TinyALSA backend - see sdk-src/project/pcaudiolib/README.md for why
# the board needs a new backend and why USE_LIBPCAUDIO is only on for the
# target.  Both archives are prerequisites the same way TinyALSA is for
# pcaudiolib itself: named here, built elsewhere, and a clear error if missing.
PCAUDIO_PORT ?= $(TOP)/sdk-src/project/pcaudiolib
PCAUDIO_SRC  ?= $(TOP)/sdk-src/sources/pcaudiolib
PCAUDIO_OUT  ?= $(TOP)/out/work/pcaudiolib
PCAUDIO_INC  := $(PCAUDIO_SRC)/src/include
PCAUDIO_LIB  := $(PCAUDIO_OUT)/cross/lib/libpcaudio.a

TINYALSA_SRC ?= $(TOP)/sdk-src/sources/tinyalsa
TINYALSA_LIB := $(TINYALSA_SRC)/src/libtinyalsa.a

# ---------------------------------------------------------------- sources ---

# Every object of upstream's `espeak-ng` library target, minus the optional
# feature files.  Left out, with the config.h switch that would pull them in:
#   mbrowrap.c compilembrola.c synth_mbrola.c  USE_MBROLA (needs the mbrola binary)
#   event.c fifo.c espeak_command.c            USE_ASYNC
#   sPlayer.c                                  USE_SPEECHPLAYER
LIB_SRC := \
	common.c mnemonics.c error.c ieee80.c \
	compiledata.c compiledict.c \
	dictionary.c encoding.c intonation.c langopts.c numbers.c \
	phoneme.c phonemelist.c readclause.c setlengths.c soundicon.c \
	spect.c ssml.c synthdata.c synthesize.c tr_languages.c \
	translate.c translateword.c voices.c wavegen.c speech.c \
	espeak_api.c klatt.c

UCD_SRC := case.c categories.c ctype.c proplist.c scripts.c tostring.c
CLI_SRC := espeak-ng.c

# The wcs* functions uClibc is missing.  Target only: the host build has real
# ones from glibc, and linking this in there would be pointless.
COMPAT_SRC := wchar_shim.c

LIB_INC := -I$(PORT) -I$(SRC)/src/include -I$(SRC)/src/include/compat \
	-I$(SRC)/src/ucd-tools/src/include

# The data directory the binary looks in when ESPEAK_DATA_PATH is not set.
DATA_PATH ?= /usr/share/espeak-ng-data

# ------------------------------------------------------------- native build --

NATIVE_CC      ?= cc
NATIVE_AR      ?= ar
NATIVE_CFLAGS  ?= -O2 -g0 -MMD -MP
NATIVE_CPPFLAGS := $(LIB_INC) -DPATH_ESPEAK_DATA=\"$(DATA_PATH)\"

NATIVE_OUT := $(OUT)/native
NATIVE_OBJ := $(NATIVE_OUT)/obj
NATIVE_BIN := $(NATIVE_OUT)/bin/espeak-ng
NATIVE_LIB := $(NATIVE_OUT)/lib/libespeak-ng.a
NATIVE_UCD := $(NATIVE_OUT)/lib/libucd.a

NATIVE_LIB_OBJS := $(addprefix $(NATIVE_OBJ)/,$(LIB_SRC:.c=.o))
NATIVE_UCD_OBJS := $(addprefix $(NATIVE_OBJ)/,$(UCD_SRC:.c=.o))
NATIVE_CLI_OBJS := $(addprefix $(NATIVE_OBJ)/,$(CLI_SRC:.c=.o))

# -------------------------------------------------------------- cross build --

CROSS      ?= $(TOP)/sdk/toolchain/bin/arm-uclinuxfdpiceabi-
CROSS_CC   := $(CROSS)gcc
CROSS_AR   := $(CROSS)ar
CROSS_STRIP:= $(CROSS)strip
# -Os because this ends up in a 3 MiB rootfs partition; no -fPIC/-fno-pic --
# the FDPIC toolchain already emits the ABI the kernel loader expects.
CROSS_CFLAGS ?= -Os -g0 -Wall -Wextra -ffunction-sections -fdata-sections -MMD -MP
# $(PORT)/compat comes first, and that ordering is load-bearing.  Upstream also
# ships wchar.h and wctype.h in src/include/compat, written as `#include_next`
# wrappers around the system headers a normal libc has.  With this libc those
# resolve to each other in whichever order the paths happen to be listed, which
# is how a definition of iswalpha() here once ended up macro-rewritten into a
# duplicate ucd_isalpha().  Naming the port's directory first makes the target
# use the port's headers, while everything upstream's shim covers and the port
# does not (endian.h getopt.h math.h stdint.h stdio.h string.h strings.h sys/
# unistd.h) still resolves through it, unchanged.
# ESPEAK_NG_TARGET is what turns on USE_LIBPCAUDIO in config.h; see the comment
# there.  Only this build gets it - the host build must not depend on an audio
# library.  -I$(PCAUDIO_INC) is only needed because speech.c includes
# <pcaudiolib/audio.h> once that switch is on.
CROSS_CPPFLAGS := -I$(PORT)/compat $(LIB_INC) -I$(PCAUDIO_INC) \
	-DPATH_ESPEAK_DATA=\"$(DATA_PATH)\" -DESPEAK_NG_TARGET=1
# Every executable on this board links against /system/lib, which is where the
# SDK's shared libraries live; see sdk/example/Makefile for the same setting.
CROSS_LDFLAGS ?= -Wl,-rpath,/system/lib -Wl,--gc-sections

CROSS_OUT := $(OUT)/cross
CROSS_OBJ := $(CROSS_OUT)/obj
CROSS_BIN := $(CROSS_OUT)/bin/espeak-ng

CROSS_LIB_OBJS := $(addprefix $(CROSS_OBJ)/,$(LIB_SRC:.c=.o))
CROSS_UCD_OBJS := $(addprefix $(CROSS_OBJ)/,$(UCD_SRC:.c=.o))
CROSS_CLI_OBJS := $(addprefix $(CROSS_OBJ)/,$(CLI_SRC:.c=.o))
CROSS_COMPAT_OBJS := $(addprefix $(CROSS_OBJ)/,$(COMPAT_SRC:.c=.o))

# ------------------------------------------------------------------ data ----

# Layout copied from upstream cmake/data.cmake: DATA_ROOT is the directory
# ESPEAK_DATA_PATH points at; the compiled data lands in
# $(DATA_ROOT)/espeak-ng-data, and phsource/ and dictsource/ sit next to it as
# the working directories the --compile-* commands expect to be run from.
DATA_ROOT := $(OUT)/data
DATA_DIR  := $(DATA_ROOT)/espeak-ng-data
DATA_ENV  := ESPEAK_DATA_PATH=$(DATA_ROOT)
DATA_BIN  := $(NATIVE_BIN)

DATA_STAMP := $(OUT)/.data-stamp

# --------------------------------------------------------------- targets ----

.PHONY: all native cross data clean
all: native cross data

native: $(NATIVE_BIN)
cross:  $(CROSS_BIN)

data: $(DATA_STAMP)

$(NATIVE_OUT)/lib $(NATIVE_OBJ) $(CROSS_OUT)/lib $(CROSS_OBJ) $(DATA_ROOT):
	mkdir -p $@

# -- native objects -----------------------------------------------------------

$(NATIVE_LIB_OBJS): $(NATIVE_OBJ)/%.o: $(SRC)/src/libespeak-ng/%.c | $(NATIVE_OBJ)
	$(NATIVE_CC) $(NATIVE_CPPFLAGS) $(NATIVE_CFLAGS) -c $< -o $@

$(NATIVE_UCD_OBJS): $(NATIVE_OBJ)/%.o: $(SRC)/src/ucd-tools/src/%.c | $(NATIVE_OBJ)
	$(NATIVE_CC) $(NATIVE_CPPFLAGS) $(NATIVE_CFLAGS) -c $< -o $@

$(NATIVE_CLI_OBJS): $(NATIVE_OBJ)/%.o: $(SRC)/src/%.c | $(NATIVE_OBJ)
	$(NATIVE_CC) $(NATIVE_CPPFLAGS) $(NATIVE_CFLAGS) -c $< -o $@

$(NATIVE_UCD): $(NATIVE_UCD_OBJS) | $(NATIVE_OUT)/lib
	$(NATIVE_AR) rcs $@ $^

$(NATIVE_LIB): $(NATIVE_LIB_OBJS) | $(NATIVE_OUT)/lib
	$(NATIVE_AR) rcs $@ $^

$(NATIVE_BIN): $(NATIVE_CLI_OBJS) $(NATIVE_LIB) $(NATIVE_UCD)
	mkdir -p $(dir $@)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $^ -lm -o $@

# -- cross objects ------------------------------------------------------------

$(CROSS_LIB_OBJS): $(CROSS_OBJ)/%.o: $(SRC)/src/libespeak-ng/%.c | $(CROSS_OBJ)
	$(CROSS_CC) $(CROSS_CPPFLAGS) $(CROSS_CFLAGS) -c $< -o $@

$(CROSS_UCD_OBJS): $(CROSS_OBJ)/%.o: $(SRC)/src/ucd-tools/src/%.c | $(CROSS_OBJ)
	$(CROSS_CC) $(CROSS_CPPFLAGS) $(CROSS_CFLAGS) -c $< -o $@

$(CROSS_CLI_OBJS): $(CROSS_OBJ)/%.o: $(SRC)/src/%.c | $(CROSS_OBJ)
	$(CROSS_CC) $(CROSS_CPPFLAGS) $(CROSS_CFLAGS) -c $< -o $@

$(CROSS_COMPAT_OBJS): $(CROSS_OBJ)/%.o: $(PORT)/compat/%.c | $(CROSS_OBJ)
	$(CROSS_CC) $(CROSS_CPPFLAGS) $(CROSS_CFLAGS) -c $< -o $@

# One binary, statically linked against all four support archives.  Static
# rather than a shared libespeak-ng.so on purpose: this is a single
# self-contained tool, nothing else on the board links the library, and a
# static link avoids adding another .so for the no-MMU loader to map.  The same
# reasoning covers pcaudiolib and TinyALSA, both of which are static-only here.
#
# -ldl is for TinyALSA: pcm_plugin.c/mixer_plugin.c/snd_card_plugin.c dlopen()
# optional plugins.  uClibc's libdl.a is a stub - dlopen and friends live in
# libc.so.0 - so this only satisfies the link.
$(CROSS_BIN): $(CROSS_CLI_OBJS) $(CROSS_COMPAT_OBJS) $(CROSS_LIB_OBJS) $(CROSS_UCD_OBJS) \
		$(PCAUDIO_LIB) $(TINYALSA_LIB)
	mkdir -p $(dir $@)
	$(CROSS_CC) $(CROSS_CFLAGS) $(CROSS_LDFLAGS) $^ -lm -ldl -o $@
	$(CROSS_STRIP) $@

# Failing here rather than at link time, so the message says what to run.
# Both archives are built by their own ports; neither is reimplemented here.
$(PCAUDIO_LIB):
	@echo "espeak-ng: $(PCAUDIO_LIB) is missing" >&2
	@echo "espeak-ng: run 'make pcaudiolib' first" >&2
	@exit 1

$(TINYALSA_LIB):
	@echo "espeak-ng: $(TINYALSA_LIB) is missing" >&2
	@echo "espeak-ng: run 'make tinyalsa' first" >&2
	@exit 1

# -- voice data ---------------------------------------------------------------

# Rebuilt from scratch every time rather than tracked file by file: the
# compile steps are a handful of seconds, and a stale half-compiled data
# directory is far more confusing than a redundant rebuild.
$(DATA_STAMP): $(NATIVE_BIN) $(DATA_ROOT)
	rm -rf $(DATA_DIR) $(DATA_ROOT)/phsource $(DATA_ROOT)/dictsource
	mkdir -p $(DATA_DIR)/voices
	cp -a $(SRC)/espeak-ng-data/lang $(DATA_DIR)/lang
	cp -a $(SRC)/espeak-ng-data/voices/\!v $(DATA_DIR)/voices/
	cp -a $(SRC)/phsource $(DATA_ROOT)/phsource
	cp -a $(SRC)/dictsource $(DATA_ROOT)/dictsource
	cd $(DATA_ROOT)/phsource && $(DATA_ENV) $(DATA_BIN) --compile-intonations
	cd $(DATA_ROOT)/phsource && $(DATA_ENV) $(DATA_BIN) --compile-phonemes
	for lang in $(LANGS); do \
		cd $(DATA_ROOT)/dictsource && $(DATA_ENV) $(DATA_BIN) --compile=$$lang || exit 1; \
	done
	# dictsource/ is only a build scratch: the compiler reads the *rules
	# from there and never needs them again at run time.
	rm -rf $(DATA_ROOT)/dictsource $(DATA_ROOT)/phsource
	touch $@

clean:
	rm -rf $(OUT)

-include $(shell test -d $(OUT) && find $(OUT) -name '*.d' 2>/dev/null)
