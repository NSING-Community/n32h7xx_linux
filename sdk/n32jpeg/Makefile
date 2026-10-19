# SPDX-License-Identifier: BSD-3-Clause

dir := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
CROSS_COMPILE ?= $(dir)/../toolchain/bin/arm-uclinuxfdpiceabi-
CC = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar
RANLIB = $(CROSS_COMPILE)ranlib
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS += $(if $(SYSROOT),--sysroot="$(SYSROOT)")
LDFLAGS += $(if $(SYSROOT),--sysroot="$(SYSROOT)")

.PHONY: all clean
all: libn32jpeg.so libn32jpeg.a n32jpeg-test

n32jpeg.o: n32jpeg.c n32jpeg.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

libn32jpeg.so: n32jpeg.o
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libn32jpeg.so $^ -o $@

libn32jpeg.a: n32jpeg.o
	$(AR) rcs $@ $^
	$(RANLIB) $@

n32jpeg-test: n32jpeg-test.c libn32jpeg.a n32jpeg.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) n32jpeg-test.c \
		$(dir)/libn32jpeg.a -o $@

clean:
	rm -f n32jpeg.o libn32jpeg.so libn32jpeg.a n32jpeg-test
