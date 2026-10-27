# SPDX-License-Identifier: BSD-3-Clause
#
# The toolchain path is written relative to *this file*, not to the shell's
# working directory, so the SDK can be unpacked anywhere and the examples can
# be built with `make -C sdk/example`, `make -f <path>/Makefile`, or after
# moving the whole tree.  Nothing here contains an absolute path.
example_dir := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
CROSS_COMPILE ?= $(example_dir)/../toolchain/bin/arm-uclinuxfdpiceabi-
CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
CFLAGS ?= -O2 -Wall -Wextra
CXXFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS += $(if $(SYSROOT),--sysroot="$(SYSROOT)")
LDFLAGS += $(if $(SYSROOT),--sysroot="$(SYSROOT)")
# Where the SDK's shared libraries live on the target: /system/lib, the
# writable userland.  uClibc's ld.so here was built without
# LDSO_CACHE_SUPPORT, so it has no /etc/ld.so.conf and no ld.so.cache and
# would only ever look in /lib and /usr/lib.  Baked-in DT_RPATH makes the
# binary find /system/lib on its own, i.e. also from an ssh non-login command
# or an init script where /etc/profile never ran.  It is used only for this
# binary's own DT_NEEDED entries, so libc.so.0 is still resolved from /lib.
RUNTIME_LIBDIR ?= /system/lib
LDFLAGS += -Wl,-rpath,$(RUNTIME_LIBDIR)
LDLIBS ?=
.PHONY: all
all: hello pthread-app hello-cxx libgreeting.so shared-app leds net-echo \
	gpioctl v4l2-capture camera-matrix

# The examples are sources; the ELF files next to them are build output and are
# not tracked.
.PHONY: clean
clean:
	rm -f hello pthread-app hello-cxx shared-app libgreeting.so leds net-echo
	rm -f v4l2-capture
	rm -f camera-matrix
	rm -f gpioctl
	rm -f *.o
hello: hello.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@
pthread-app: pthread.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread $< $(LDLIBS) -o $@
hello-cxx: hello.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -pthread $< $(LDLIBS) -o $@
libgreeting.so: greeting.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -fPIC -shared -Wl,-soname,libgreeting.so $< -o $@
shared-app: shared.c libgreeting.so
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -L. -lgreeting $(LDLIBS) -o $@

# Board LED marquee on PB3 / PF10 / PI8 through the standard LED class.  The
# active-low polarity is handled by the board DTS, so userspace writes logical
# on/off values to /sys/class/leds/{red,green,blue}/brightness.
leds: leds.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

# General GPIO/LED command.  Exact LED labels use /sys/class/leds, while other
# targets use the Linux GPIO character-device ABI v2.  Both share monotonic
# timers, software PWM and a breathing-light waveform.
gpioctl: gpioctl.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

# TCP echo server: the board side of a network application.  One poll() loop
# multiplexes every client instead of forking one process per connection,
# which is the pattern this no-MMU kernel requires (see doc/sdk.md).  Its -t
# mode serves each connection from its own thread instead; threads share the
# one address space, so that is fine here even though fork() is not.
net-echo: net-echo.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread $< $(LDLIBS) -o $@

# V4L2 frame grabber for the OV5640 on the DVP port: lists the formats the
# capture device supports and saves GREY / YUYV / RGB565 / JPEG frames.
# Header only, so it links against nothing but libc.
v4l2-capture: v4l2-capture.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

# Walks every format x size combination in one run: captures one frame per
# combination, checks it, and writes it out.  The counterpart of
# v4l2-capture for checking what the camera can do rather than for taking a
# picture.
camera-matrix: camera-matrix.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@
