# SPDX-License-Identifier: BSD-3-Clause
#
# The toolchain path is written relative to *this file*, not to the shell's
# working directory, so the SDK can be unpacked anywhere and the examples can
# be built with `make -C sdk/example`, `make -f <path>/Makefile`, or after
# moving the whole tree.  Nothing here contains an absolute path.
example_dir := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
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
all: hello pthread-app hello-cxx libgreeting.so shared-app leds net-echo

# The examples are sources; the ELF files next to them are build output and are
# not tracked.
.PHONY: clean
clean:
	rm -f hello pthread-app hello-cxx shared-app libgreeting.so leds net-echo
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

# Board LED marquee on PB3 / PF10 / PI8 through /sys/class/gpio.  It looks the
# GPIO bank base numbers up at runtime instead of hard-coding them, and
# releases the lines it exported when it exits.
leds: leds.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

# TCP echo server: the board side of a network application.  One poll() loop
# multiplexes every client instead of forking one process per connection,
# which is the pattern this no-MMU kernel requires (see doc/sdk.md).  Its -t
# mode serves each connection from its own thread instead; threads share the
# one address space, so that is fine here even though fork() is not.
net-echo: net-echo.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread $< $(LDLIBS) -o $@
