# SPDX-License-Identifier: BSD-3-Clause
#
# pcaudiolib for the N32H787 ARMv7-M no-MMU target.
#
# Upstream pcaudiolib speaks to PulseAudio, ALSA, QSA and OSS.  This board has
# none of the four - its only ALSA userland is TinyALSA - so the port adds a
# TinyALSA backend and builds the library out of the upstream sources plus that
# one file.  See README.md next to this Makefile.
#
# Upstream builds with autotools; there is no autoconf run here.  What
# autoconf would have generated is config.h in this directory, and the source
# list below mirrors the src_libpcaudio_la_SOURCES line in upstream's
# Makefile.am (minus coreaudio.c and the Windows sources, which are conditional
# or EXTRA_DIST there).  When bumping pcaudiolib, diff that list.
#
# The upstream checkout this builds is *not* pristine: this port applies a
# three-hunk patch so create_audio_device_object() knows about the new backend.
# The patch is recorded at sdk-src/patches/pcaudiolib-tinyalsa-backend.patch.

PORT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
TOP  := $(abspath $(PORT)/../../..)
SRC  ?= $(TOP)/sdk-src/sources/pcaudiolib
OUT  ?= $(TOP)/out/work/pcaudiolib

# TinyALSA is not built here; it is a prerequisite, produced by
# sdk-src/scripts/install-tinyalsa.sh.  Only its header directory and its
# archive are used.
TINYALSA ?= $(TOP)/sdk-src/sources/tinyalsa
TINYALSA_ARCHIVE := $(TINYALSA)/src/libtinyalsa.a
TINYALSA_INC := $(TINYALSA)/include

CROSS      ?= $(TOP)/sdk/toolchain/bin/arm-uclinuxfdpiceabi-
CROSS_CC   := $(CROSS)gcc
CROSS_AR   := $(CROSS)ar
# -Os, not -O2: this library's only consumer on this board so far is an
# espeak-ng binary that statically absorbs it into a 3 MiB rootfs partition.
CROSS_CFLAGS ?= -Os -g0 -Wall -Wextra -MMD -MP
# $(PORT) comes first so this port's config.h is the one that is found; the
# upstream tree has no config.h of its own (autoconf would generate it at the
# checkout root), so a wrong order would silently pick up nothing at all and
# every backend would compile out.  $(SRC)/src carries audio_priv.h.
CROSS_CPPFLAGS := -I$(PORT) -I$(SRC)/src -I$(SRC)/src/include \
	-I$(TINYALSA_INC)
# Same rpath every executable on this board carries; see sdk/example/Makefile.
CROSS_LDFLAGS ?= -Wl,-rpath,/system/lib

CROSS_OUT := $(OUT)/cross
CROSS_OBJ := $(CROSS_OUT)/obj

# Upstream's five unconditional sources, plus the TinyALSA backend.  The four
# vendor backends still have to be compiled even though all four take their
# "return NULL" stub arm under this config.h: audio.c calls all of them
# unconditionally, so their symbols have to resolve.
UPSTREAM_SRC := audio.c alsa.c qsa.c oss.c pulseaudio.c
PORT_SRC     := tinyalsa.c

UPSTREAM_OBJS := $(addprefix $(CROSS_OBJ)/,$(UPSTREAM_SRC:.c=.o))
PORT_OBJS     := $(addprefix $(CROSS_OBJ)/port/,$(PORT_SRC:.c=.o))
ALL_OBJS      := $(UPSTREAM_OBJS) $(PORT_OBJS)

# The shared library needs its own -fPIC objects: the archive's are built
# without it, and a link from those is exactly how a TEXTREL segment appears.
PIC_OBJ      := $(CROSS_OBJ)/pic
UPSTREAM_PIC := $(addprefix $(PIC_OBJ)/,$(UPSTREAM_SRC:.c=.o))
PORT_PIC     := $(addprefix $(PIC_OBJ)/port/,$(PORT_SRC:.c=.o))
ALL_PIC      := $(UPSTREAM_PIC) $(PORT_PIC)

CROSS_LIB := $(CROSS_OUT)/lib/libpcaudio.a
CROSS_SO  := $(CROSS_OUT)/lib/libpcaudio.so

.PHONY: all static shared check clean

all: static

static: $(CROSS_LIB)
shared: $(CROSS_SO)

check: $(CROSS_LIB) $(CROSS_SO)
	@echo "pcaudio: $$( $(CROSS_AR) t $(CROSS_LIB) | tr '\n' ' ')"
	@readelf -d $(CROSS_SO) | grep -q TEXTREL && \
		{ echo "pcaudio: libpcaudio.so has a TEXTREL segment" >&2; exit 1; } \
		|| true
	@echo "pcaudio: no TEXTREL in libpcaudio.so"

$(CROSS_OBJ) $(CROSS_OUT)/lib:
	mkdir -p $@

# Failing here rather than at link time, so the message says what to run.
$(TINYALSA_ARCHIVE):
	@echo "pcaudio: $(TINYALSA_ARCHIVE) is missing; run 'make tinyalsa' first" >&2
	@exit 1

$(UPSTREAM_OBJS): $(CROSS_OBJ)/%.o: $(SRC)/src/%.c | $(CROSS_OBJ)
	$(CROSS_CC) $(CROSS_CPPFLAGS) $(CROSS_CFLAGS) -c $< -o $@

$(PORT_OBJS): $(CROSS_OBJ)/port/%.o: $(PORT)/%.c | $(CROSS_OBJ)
	mkdir -p $(dir $@)
	$(CROSS_CC) $(CROSS_CPPFLAGS) $(CROSS_CFLAGS) -c $< -o $@

$(UPSTREAM_PIC): $(PIC_OBJ)/%.o: $(SRC)/src/%.c
	mkdir -p $(dir $@)
	$(CROSS_CC) $(CROSS_CPPFLAGS) $(CROSS_CFLAGS) -fPIC -c $< -o $@

$(PORT_PIC): $(PIC_OBJ)/port/%.o: $(PORT)/%.c
	mkdir -p $(dir $@)
	$(CROSS_CC) $(CROSS_CPPFLAGS) $(CROSS_CFLAGS) -fPIC -c $< -o $@

# The static form is what espeak-ng links.  Nothing on the board links
# libpcaudio.so yet, and a no-MMU loader mapping one more shared object is
# cost without benefit, so the archive is the default and the .so is optional.
$(CROSS_LIB): $(ALL_OBJS) | $(CROSS_OUT)/lib
	$(CROSS_AR) rcs $@ $^

# The shared form folds TinyALSA in rather than depending on it: the board has
# libtinyalsa.a only, no libtinyalsa.so for the loader to find.
$(CROSS_SO): $(ALL_PIC) $(TINYALSA_ARCHIVE) | $(CROSS_OUT)/lib
	$(CROSS_CC) $(CROSS_CFLAGS) $(CROSS_LDFLAGS) -shared \
		-Wl,-soname,libpcaudio.so \
		$(ALL_PIC) $(TINYALSA_ARCHIVE) -ldl -o $@

clean:
	rm -rf $(OUT)

-include $(shell test -d $(OUT) && find $(OUT) -name '*.d' 2>/dev/null)
