# SPDX-License-Identifier: MIT
#
# Upstream libdrm 2.4.134 for the N32H787 userland.
#
# This is build glue only.  Everything under upstream/ is the unmodified
# release tarball; the cross file is generated in the build directory so the
# source tree contains no machine-specific paths.
#
# Output layout:
#   $(OUT_DIR)/libdrm/build/         Meson/Ninja build tree
#   $(OUT_DIR)/libdrm/rootfs/usr/    staged headers, library and pkg-config file

libdrm_dir := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
upstream   := $(libdrm_dir)/upstream
TOP        := $(abspath $(libdrm_dir)/../../..)

OUT_DIR       ?= $(TOP)/out
LIBDRM_BUILD  ?= $(OUT_DIR)/libdrm/build
LIBDRM_ROOT   ?= $(OUT_DIR)/libdrm/rootfs
ROOTFS_TREE   ?= $(TOP)/rootfs

CROSS_COMPILE ?= $(TOP)/sdk/toolchain/bin/arm-uclinuxfdpiceabi-
CC      := $(CROSS_COMPILE)gcc
AR      := $(CROSS_COMPILE)ar
RANLIB  := $(CROSS_COMPILE)ranlib
STRIP   := $(CROSS_COMPILE)strip
MESON   ?= meson
NINJA   ?= ninja

LIBDRM_LIB    := $(LIBDRM_ROOT)/usr/lib/libdrm.so.2.134.0
LIBDRM_SONAME := $(LIBDRM_ROOT)/usr/lib/libdrm.so.2

.PHONY: all clean install-rootfs
all: $(LIBDRM_SONAME)

$(LIBDRM_BUILD)/.configured: Makefile
	@command -v $(MESON) >/dev/null 2>&1 || { \
		echo "error: meson is required to build libdrm" >&2; \
		echo "       install meson and ninja on the build host, then retry" >&2; \
		exit 1; }
	@command -v $(NINJA) >/dev/null 2>&1 || { \
		echo "error: ninja is required to build libdrm" >&2; \
		echo "       install meson and ninja on the build host, then retry" >&2; \
		exit 1; }
	rm -rf $(LIBDRM_BUILD)
	mkdir -p $(LIBDRM_BUILD)
	{ \
		echo '[binaries]'; \
		echo "c = '$(CC)'"; \
		echo "ar = '$(AR)'"; \
		echo "strip = '$(STRIP)'"; \
		echo "ranlib = '$(RANLIB)'"; \
		echo; \
		echo '[host_machine]'; \
		echo "system = 'linux'"; \
		echo "cpu_family = 'arm'"; \
		echo "cpu = 'arm'"; \
		echo "endian = 'little'"; \
		echo; \
		echo '[properties]'; \
		echo 'needs_exe_wrapper = true'; \
		echo; \
		echo '[built-in options]'; \
		echo "c_args = ['-Os', '-ffunction-sections', '-fdata-sections']"; \
		echo "c_link_args = ['-Wl,--gc-sections']"; \
	} > $(LIBDRM_BUILD)/cross-file.ini
	$(MESON) setup $(LIBDRM_BUILD) $(upstream) \
		--cross-file=$(LIBDRM_BUILD)/cross-file.ini \
		--buildtype=release --wrap-mode=nodownload \
		--prefix=/usr --libdir=lib --includedir=include \
		-Ddefault_library=shared \
		-Dintel=disabled -Dradeon=disabled -Damdgpu=disabled \
		-Dnouveau=disabled -Dvmwgfx=disabled -Domap=disabled \
		-Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled \
		-Dvc4=disabled -Detnaviv=disabled \
		-Dtests=false -Dinstall-test-programs=false \
		-Dman-pages=disabled -Dcairo-tests=disabled \
		-Dvalgrind=disabled -Dudev=false
	@touch $@

$(LIBDRM_BUILD)/.built: $(LIBDRM_BUILD)/.configured
	$(NINJA) -C $(LIBDRM_BUILD)
	@touch $@

$(LIBDRM_SONAME): $(LIBDRM_BUILD)/.built
	DESTDIR=$(LIBDRM_ROOT) $(NINJA) -C $(LIBDRM_BUILD) install
	@test -e $@ || { echo "error: libdrm install did not produce $@" >&2; exit 1; }

# Install only the runtime library into the board rootfs.  Headers and the
# pkg-config file stay in $(LIBDRM_ROOT) for cross-compiling applications.
install-rootfs: $(LIBDRM_SONAME)
	@mkdir -p $(ROOTFS_TREE)/usr/lib
	install -m 0755 $(LIBDRM_LIB) $(ROOTFS_TREE)/usr/lib/libdrm.so.2.134.0
	ln -sfn libdrm.so.2.134.0 $(ROOTFS_TREE)/usr/lib/libdrm.so.2
	ln -sfn libdrm.so.2 $(ROOTFS_TREE)/usr/lib/libdrm.so
	@echo "rootfs: libdrm.so.2 -> $(ROOTFS_TREE)/usr/lib"

clean:
	rm -rf $(OUT_DIR)/libdrm
