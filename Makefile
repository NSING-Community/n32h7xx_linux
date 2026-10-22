# SPDX-License-Identifier: BSD-3-Clause
# N32H7xx Linux top-level build entry.
#
# Daily use only touches bootloader/, kernel/, rootfs/ and sdk/.  Everything
# needed for that ships in this tree; the component-rebuild targets further down
# additionally use the local sdk-src/ working tree and are not part of `make help`.
#
#   make            build everything and assemble the flash image (same as `all`)
#   make bootloader build the SPI-backed CM7 bootloader
#   make kernel     build and stage the Linux zImage and <board>.dtb
#   make menuconfig change the kernel configuration (see the kernel-* targets)
#   make rootfs     PACK the rootfs into the SquashFS image (nothing is compiled)
#   make flash-image  only the raw SPI NOR image (all already does this)
#
# Every build product goes under OUT_DIR (default ./out); not a single object
# file is written next to the sources, so the checkout stays clean and
# "rm -rf out" is a complete reset:
#
#   out/bootloader/CM7/    CM7 bootloader objects, .elf, .bin, .hex, .map
#   out/kernel/build/      kbuild out-of-tree tree, incl. System.map, vmlinux
#   out/kernel/            staged zImage, <board>.dtb, System.map,
#                          config, SHA256SUMS (flat; no version subdirectory
#                          and no "latest" symlink -- there is only ever one
#                          staged kernel, so a second name for it was noise)
#   out/kernel/modules/    loadable module tree (lib/modules/<release>/);
#                          merged into the rootfs SquashFS image at pack time
#   out/rootfs/            rootfs.squashfs (the userland the board runs)
#   out/work/              userland component builds (busybox, dropbear, ...)
#   out/dist/              the assembled flash images -- the final product
#   out/recover-<stamp>/   per-flash backups from `make flash-bootloader`

SHELL := /bin/bash

TOP       := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
OUT_DIR   ?= $(TOP)/out
JOBS      ?= $(shell nproc 2>/dev/null || echo 4)

BOOT_OUT     := $(OUT_DIR)/bootloader
KERNEL_OUT   := $(OUT_DIR)/kernel
KERNEL_BUILD := $(KERNEL_OUT)/build
ROOTFS_OUT   := $(OUT_DIR)/rootfs
DIST_OUT     := $(OUT_DIR)/dist
WORK_OUT     := $(OUT_DIR)/work

# The sdk-src build scripts (busybox, dropbear, the /sbin tools) keep their
# scratch directories in $OUT_DIR/work rather than next to the sources.
export OUT_DIR

BOOTLOADER_DIR := $(TOP)/bootloader
KERNEL_DIR     := $(TOP)/kernel
ROOTFS_DIR     := $(TOP)/rootfs

# --- bootloader -------------------------------------------------------------
BOOT_STORAGE ?= spi
ifeq ($(BOOT_STORAGE),sdmmc)
BOOT_BUILD_SUBDIR := CM7_sdmmc
else
BOOT_BUILD_SUBDIR := CM7
endif
BOOT_BUILD := $(BOOT_OUT)/$(BOOT_BUILD_SUBDIR)
BOOT_BIN   := $(BOOT_BUILD)/h787_linux_CM7.bin
BOOT_ELF   := $(BOOT_BUILD)/h787_linux_CM7.elf
# The bootloader goes into on-chip flash rather than into the SPI image, so
# without this it would be the one flashable artifact missing from out/dist.
# Only the .bin is staged: that is the file you actually program.
DIST_BOOTLOADER := $(DIST_OUT)/bootloader.bin

# --- kernel -----------------------------------------------------------------
BOARD          ?= n32h787-hmi
# Select the board DTB without changing the user's kernel configuration.
CAMERA         ?= 0
ifneq ($(CAMERA),0)
ifneq ($(CAMERA),1)
$(error CAMERA must be 0 or 1)
endif
endif
LCD            ?= 1
ifneq ($(LCD),0)
ifneq ($(LCD),1)
$(error LCD must be 0 or 1)
endif
endif
KERNEL_DTB_BOARD := $(BOARD)
ifeq ($(CAMERA),0)
ifneq ($(BOARD),n32h787-hmi)
$(error CAMERA=0 is only supported for BOARD=n32h787-hmi)
endif
KERNEL_DTB_BOARD := $(BOARD)-no-camera
endif
ifeq ($(LCD),0)
ifneq ($(BOARD),n32h787-hmi)
$(error LCD=0 is only supported for BOARD=n32h787-hmi)
endif
KERNEL_DTB_BOARD := $(KERNEL_DTB_BOARD)-no-display
endif
# Seed configuration for the kernel: a name under arch/arm/configs/, or a path
# to any defconfig-format file.  It only *seeds* out/kernel/build/.config; see
# the kernel-menuconfig target below.
DEFCONFIG      ?= nsing_n32h7xx_defconfig
# The staged boot artifacts sit directly in out/kernel, next to out/kernel/build
# (the kbuild O= tree).  release.sh is what writes them.
KERNEL_STAGE   := $(KERNEL_OUT)
# The two boot artifacts are separate on purpose: the DTB and the kernel live in
# their own SPI NOR partitions and are loaded to different RAM addresses.  They
# must not be concatenated, or the decompressor rewrites the r2 DTB argument.
KERNEL_ZIMAGE  := $(KERNEL_STAGE)/zImage
KERNEL_DTB     := $(KERNEL_STAGE)/$(KERNEL_DTB_BOARD).dtb
# Where release.sh installs the loadable modules.  The rootfs packaging merges
# this tree into the SquashFS image, so nothing has to be written next to the
# rootfs/ sources.
KERNEL_MODULES := $(KERNEL_STAGE)/modules

# The detector is an optional application.  CAMERA=1 only selects the camera
# device tree and kernel memory pool; building and installing the detector is a
# separate action so ordinary camera firmware does not rebuild it every time.
HUMAN_DETECT_DIR           := $(TOP)/sdk/optional-src/human-detect
HUMAN_DETECT_BIN           := $(HUMAN_DETECT_DIR)/human-detect
HUMAN_DETECT_OPTIONAL_DIR  := $(TOP)/sdk/optional/human-detect
HUMAN_DETECT_UI_BIN        := $(OUT_DIR)/lvgl/human-detect-ui
HUMAN_DETECT_CROSS_COMPILE := $(if $(CROSS_COMPILE),$(CROSS_COMPILE),$(TOP)/sdk/toolchain/bin/arm-uclinuxfdpiceabi-)

INPUT_EVENTS_DIR := $(TOP)/sdk/optional-src/input-events

# Graphics userland.  libdrm is built with upstream Meson and staged under
# out/libdrm/rootfs; LVGL uses the standard libdrm headers and shared library
# from that staging tree.  These targets are not dependencies of `make all`,
# but the libdrm runtime committed under rootfs/usr/lib is part of the rootfs.
LIBDRM_DIR := $(TOP)/sdk/optional-src/libdrm
LVGL_DIR   := $(TOP)/sdk/optional-src/lvgl
GRAPHICS_CROSS_COMPILE ?= $(if $(CROSS_COMPILE),$(CROSS_COMPILE),$(TOP)/sdk/toolchain/bin/arm-uclinuxfdpiceabi-)

# --- rootfs (packaging only) ------------------------------------------------
# The tree that becomes the userland: the rootfs/ directory itself, which is the
# one and only userland tree.  It is packed into SquashFS and lives in its own
# flash partition; the kernel no longer embeds a copy (see the defconfig).
ROOTFS_TREE     ?= $(ROOTFS_DIR)
ROOTFS_SQUASHFS := $(ROOTFS_OUT)/rootfs.squashfs
# mksquashfs takes one source tree, so the rootfs and the kernel modules are
# merged into a staging copy first.  The copy lives under out/ so the shipped
# rootfs/ tree stays exactly what it is in git.
ROOTFS_STAGE    := $(ROOTFS_OUT)/stage
# Empty directories have no representation in git, so the mount points and
# working directories the userland needs must be recreated at pack time: a
# freshly cloned tree would otherwise produce a rooted image with no /proc,
# /sys, /dev, /tmp or /system.  rcS cannot create them itself -- it runs from
# the read-only SquashFS, where mkdir fails -- so this is not optional.
ROOTFS_DIRS     := dev dev/pts mnt mnt/sd proc root run sys system tmp var/log

# --- flash image ------------------------------------------------------------
# The image carries every fixed partition -- dtb + kernel + rootfs -- and leaves
# the system partition erased.  The kernel formats system as JFFS2 on first
# mount, and its size is whatever the flash leaves after the fixed partitions,
# so the same build works on a larger device: only FLASH_SIZE is a knob, and the
# tool refuses to shrink system below MIN_SYSTEM.
#   dtb      0x000000    64 KiB
#   kernel   0x010000     4 MiB
#   rootfs   0x410000     3 MiB
#   system   0x710000    the rest of FLASH_SIZE (>= MIN_SYSTEM)
FLASH_SIZE     ?= 16M
MIN_SYSTEM     ?= 8M
FLASH_IMAGE    := $(DIST_OUT)/spi-nor-$(FLASH_SIZE).img
# Everything except system, packed into one image: dtb + kernel + rootfs, i.e.
# bytes [0, 0x710000).  This is what an over-USB update writes, so it stops
# exactly where the user's writable system partition begins.  No size in the
# name: the fixed partitions all end at 0x710000, so this file is byte for byte
# the same whatever FLASH_SIZE is (only spi-nor-<size>.img depends on it).
FLASH_PREFIX   := $(DIST_OUT)/spi-nor-boot.img
TOOLS_DIR      := $(TOP)/tools
SPI_IMAGE_TOOL := $(TOOLS_DIR)/spi_image.py

.PHONY: all help bootloader kernel human-detect publish-human-detect \
        human-detect-ui publish-human-detect-ui input-events \
        libdrm install-libdrm lvgl-demo lvgl-touch-demo \
        rootfs rootfs-squashfs busybox dropbear \
        tools tinyalsa pcaudiolib sftp-server mbedtls curl ca-certificates \
        flash-image flash-kernel ota-check \
        ota-rootfs ota-kernel ota-dtb publish-bin \
        flash-bootloader doctor \
        menuconfig kernel-menuconfig kernel-nconfig kernel-xconfig \
        kernel-oldconfig kernel-savedefconfig kernel-defconfig \
        clean clean-bootloader clean-kernel clean-rootfs clean-flash

# `all` produces the flashable image directly: flash-image pulls in kernel +
# rootfs-squashfs itself, so the only thing to add is the bootloader.  There is
# no separate "now run make flash-image" step any more.
all: bootloader flash-image
	@echo
	@echo "built: bootloader, kernel, rootfs, $(notdir $(FLASH_IMAGE))"
	@echo "dist:  $(DIST_OUT)/  (2 flash images + bootloader.bin)"

# Host-side companion artifacts only. No dependency from any firmware target.
# Default inventory generation succeeds even when the report is BLOCKED;
# LEGAL_STRICT=1 is the explicit release gate (nonzero for unresolved errors).
LEGAL_STRICT ?= 0
LEGAL_INFO_TOOL := $(TOP)/tools-internal/legal_info.py
.PHONY: legal-info legal-info-sources legal-info-tool-check
legal-info-tool-check:
	@test -f "$(LEGAL_INFO_TOOL)" || { \
		echo "error: legal-info is not available in this checkout." >&2; \
		echo "       It is a maintainer-side target; a release ships the" >&2; \
		echo "       generated compliance materials alongside it instead." >&2; \
		exit 1; }

legal-info: legal-info-tool-check
	python3 "$(LEGAL_INFO_TOOL)" --out "$(OUT_DIR)/legal-info" $(if $(filter 1,$(LEGAL_STRICT)),--strict,)

legal-info-sources: legal-info-tool-check
	python3 "$(LEGAL_INFO_TOOL)" --out "$(OUT_DIR)/legal-info-sources" --sources $(if $(filter 1,$(LEGAL_STRICT)),--strict,)

help:
	@echo "N32H7xx Linux top-level targets:"
	@echo
	@echo "  all              bootloader + kernel + rootfs + flash image (default)"
	@echo "  bootloader       build the $(BOOT_STORAGE)-backed CM7 bootloader"
	@echo "  kernel           build and stage the separate zImage + $(KERNEL_DTB_BOARD).dtb"
	@echo "  display-test     build the framebuffer/GPU/page-flip test into out/display/"
	@echo "                   (also builds the loadable modules into out/kernel/modules)"
	@echo "  libdrm           build upstream libdrm into out/libdrm/rootfs/"
	@echo "  install-libdrm   install libdrm.so.2 into rootfs/usr/lib (then repack rootfs)"
	@echo "  lvgl-demo        build LVGL 9.6.0 and its DRM/fbdev demo into out/lvgl/"
	@echo "  lvgl-touch-demo  build the LVGL touchscreen coordinate demo into out/lvgl/"
	@echo "  input-events     build the evdev touchscreen event reader into out/input-events/"
	@echo "  human-detect     build the optional person detector source"
	@echo "  publish-human-detect  refresh the prebuilt sdk/optional/human-detect binary"
	@echo "  human-detect-ui  build the optional LVGL detector monitor"
	@echo "  menuconfig       change the kernel configuration (ncurses; 'kernel-menuconfig')"
	@echo "  kernel-nconfig   same with the newer ncurses UI ('kernel-xconfig' for Qt)"
	@echo "  kernel-oldconfig review the symbols a kernel update added, one by one"
	@echo "  kernel-savedefconfig  write the current configuration as a defconfig"
	@echo "  kernel-defconfig go back to $(DEFCONFIG), dropping your changes"
	@echo "  rootfs           pack $(ROOTFS_TREE) into the SquashFS image (no compilation)"
	@echo "  rootfs-squashfs  same thing; kept for scripts that name it explicitly"
	@echo "  flash-image      assemble the raw SPI NOR image (system left erased)"
	@echo "  flash-kernel     write dtb+kernel+rootfs to the board over USB MSC"
	@echo "  ota-rootfs       push rootfs over SSH and flash it from Linux"
	@echo "  ota-kernel       push $(notdir $(KERNEL_ZIMAGE)) over SSH and flash it from Linux"
	@echo "  ota-dtb          push $(notdir $(KERNEL_DTB)) over SSH and flash it from Linux"
	@echo "  flash-bootloader program the CM7 bootloader into on-chip flash"
	@echo "  publish-bin      refresh bin/ with the two artifacts downloaded from the repository"
	@echo "  doctor           report tool, probe and target state"
	@echo "  legal-info       maintainer-only license bundle, inventory and compliance report"
	@echo "  legal-info-sources  maintainer-only: also snapshot available sources (large)"
	@echo "                   LEGAL_STRICT=1 fails on unresolved release blockers"
	@echo "  clean            remove the whole build tree $(OUT_DIR)"
	@echo
	@echo "Final products land in $(DIST_OUT)/:"
	@echo "  $(notdir $(FLASH_IMAGE))  $(notdir $(FLASH_PREFIX))  bootloader.bin"
	@echo
	@echo "Useful variables:"
	@echo "  JOBS=$(JOBS)  BOOT_STORAGE=$(BOOT_STORAGE)  BOARD=$(BOARD)"
	@echo "  CAMERA=$(CAMERA)  (1: camera + 2 MiB pool; 0: no camera)"
	@echo "  LCD=$(LCD)  (1: LCD/GPU + 3 MiB pool; 0: no LCD or GPU)"
	@echo "  DEFCONFIG=$(DEFCONFIG)  (seed for the kernel configuration)"
	@echo "  OUT_DIR=$(OUT_DIR)"
	@echo "  KERNEL_ZIMAGE=$(KERNEL_ZIMAGE)"
	@echo "  KERNEL_DTB=$(KERNEL_DTB)"
	@echo "  ROOTFS_TREE=$(ROOTFS_TREE)"
	@echo "  FLASH_SIZE=$(FLASH_SIZE)  MIN_SYSTEM=$(MIN_SYSTEM)"
	@echo "    system = FLASH_SIZE - 0x710000, i.e. everything after rootfs"
	@echo "    only spi-nor-\$$FLASH_SIZE.img depends on it; spi-nor-boot.img does not"
	@echo "  H787_BOARD=$(if $(H787_BOARD),$(H787_BOARD),<unset>)  (network OTA target)"

# ---------------------------------------------------------------------------
# bootloader
# ---------------------------------------------------------------------------
bootloader:
	@echo "==> bootloader (STORAGE=$(BOOT_STORAGE))"
	$(MAKE) -C $(BOOTLOADER_DIR) OUT_DIR=$(BOOT_OUT) \
		STORAGE=$(BOOT_STORAGE) J=$(JOBS)
	@test -s $(BOOT_BIN) || { \
		echo "error: no $(BOOT_BIN)." >&2; \
		echo "       Diagnostic variants (BENCH/RX_TEST/SD_COMPARE/SDRAM_TEST," >&2; \
		echo "       MPU_GUARD=0, BOOT_ICACHE=0) get their own out/bootloader/" >&2; \
		echo "       subdirectory; build those with bootloader/Makefile directly." >&2; \
		exit 1; }
	@mkdir -p $(DIST_OUT)
	@cp $(BOOT_BIN) $(DIST_BOOTLOADER)
	@echo "bootloader: $(BOOT_BIN)"
	@echo "          -> $(DIST_BOOTLOADER)"

# ---------------------------------------------------------------------------
# kernel
# ---------------------------------------------------------------------------
kernel:
	@echo "==> kernel ($(BOARD))"
	@# CROSS_COMPILE is forwarded explicitly so it is obvious which compiler the
	@# kernel uses; left empty, release.sh falls back to arm-none-eabi-.  The
	@# SDK ships a soft-float wrapper for this: sdk/toolchain/bin/arm-n32-linux-kernel-
	@# BUILD_DIR/STAGE_DIR keep the kernel build out of the source tree: the
	@# objects go to out/kernel/build and the staged artifacts to out/kernel.
	JOBS=$(JOBS) BOARD=$(BOARD) CAMERA=$(CAMERA) LCD=$(LCD) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		DEFCONFIG=$(DEFCONFIG) \
		BUILD_DIR=$(KERNEL_BUILD) STAGE_DIR=$(KERNEL_STAGE) \
		$(KERNEL_DIR)/scripts/nsing/release.sh
	@test -s $(KERNEL_ZIMAGE)
	@test -s $(KERNEL_DTB)
	@ls -l $(KERNEL_ZIMAGE) $(KERNEL_DTB)
	@echo "kernel: $(KERNEL_ZIMAGE) + $(KERNEL_DTB)"

# Export sanitized UAPI headers locally; leave the SDK sysroot untouched.
DISPLAY_TEST_CC ?= $(TOP)/sdk/toolchain/bin/arm-uclinuxfdpiceabi-gcc
.PHONY: display-test
display-test:
	@mkdir -p $(OUT_DIR)/display
	$(MAKE) -C $(KERNEL_DIR) O=$(KERNEL_BUILD) ARCH=arm \
		INSTALL_HDR_PATH=$(OUT_DIR)/display/headers headers_install
	$(DISPLAY_TEST_CC) -Os -Wall -Wextra -Werror \
		-I$(OUT_DIR)/display/headers/include \
		$(TOP)/sdk/example/n32-display-test.c -o $(OUT_DIR)/display/n32-display-test

# Upstream libdrm and LVGL are optional host-side builds.  They use the SDK
# userspace compiler, not the kernel compiler, and never alter either source
# tree in place.
libdrm:
	$(MAKE) -C $(LIBDRM_DIR) OUT_DIR=$(OUT_DIR) \
		CROSS_COMPILE=$(GRAPHICS_CROSS_COMPILE)

install-libdrm: libdrm
	$(MAKE) -C $(LIBDRM_DIR) OUT_DIR=$(OUT_DIR) ROOTFS_TREE=$(ROOTFS_TREE) \
		CROSS_COMPILE=$(GRAPHICS_CROSS_COMPILE) install-rootfs

lvgl-demo: libdrm
	$(MAKE) -C $(LVGL_DIR) OUT_DIR=$(OUT_DIR) \
		LIBDRM_ROOT=$(OUT_DIR)/libdrm/rootfs \
		CROSS_COMPILE=$(GRAPHICS_CROSS_COMPILE)

lvgl-touch-demo: libdrm
	$(MAKE) -C $(LVGL_DIR) touch-demo OUT_DIR=$(OUT_DIR) \
		LIBDRM_ROOT=$(OUT_DIR)/libdrm/rootfs \
		CROSS_COMPILE=$(GRAPHICS_CROSS_COMPILE)

input-events:
	$(MAKE) -C $(INPUT_EVENTS_DIR) OUT_DIR=$(OUT_DIR) \
		CROSS_COMPILE=$(GRAPHICS_CROSS_COMPILE)

# The detector source keeps its SDK-friendly in-tree output. It links only libc and
# embeds the N32 JPEG helper statically. Firmware targets do not depend on it.
human-detect:
	$(MAKE) -C $(HUMAN_DETECT_DIR) CROSS_COMPILE="$(HUMAN_DETECT_CROSS_COMPILE)"

publish-human-detect: human-detect
	@test -x $(HUMAN_DETECT_BIN)
	@mkdir -p $(HUMAN_DETECT_OPTIONAL_DIR)
	@install -m 0755 $(HUMAN_DETECT_BIN) $(HUMAN_DETECT_OPTIONAL_DIR)/human-detect
	@echo "optional: $(HUMAN_DETECT_OPTIONAL_DIR)/human-detect"

human-detect-ui: libdrm
	$(MAKE) -C $(LVGL_DIR) human-detect-ui OUT_DIR=$(OUT_DIR) \
		LIBDRM_ROOT=$(OUT_DIR)/libdrm/rootfs \
		CROSS_COMPILE=$(GRAPHICS_CROSS_COMPILE)

publish-human-detect-ui: human-detect-ui
	@test -x $(HUMAN_DETECT_UI_BIN)
	@mkdir -p $(HUMAN_DETECT_OPTIONAL_DIR)
	@install -m 0755 $(HUMAN_DETECT_UI_BIN) \
		$(HUMAN_DETECT_OPTIONAL_DIR)/human-detect-ui
	@echo "optional: $(HUMAN_DETECT_OPTIONAL_DIR)/human-detect-ui"

# --- kernel configuration ---------------------------------------------------
# The customer-facing way to change the kernel.  The working configuration is
# out/kernel/build/.config: `make kernel` seeds it from $(DEFCONFIG) the first
# time and then keeps it, so what is changed here survives the next build.
# scripts/nsing/kconfig.sh holds the rules (including what happens when the
# released defconfig moves on and the .config was edited).
KCONFIG_TOOL := ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) DEFCONFIG=$(DEFCONFIG) \
	BUILD_DIR=$(KERNEL_BUILD) $(KERNEL_DIR)/scripts/nsing/kconfig.sh

menuconfig: kernel-menuconfig

kernel-menuconfig:
	@$(KCONFIG_TOOL) menuconfig

kernel-nconfig:
	@$(KCONFIG_TOOL) nconfig

kernel-xconfig:
	@$(KCONFIG_TOOL) xconfig

kernel-oldconfig:
	@$(KCONFIG_TOOL) oldconfig

kernel-savedefconfig:
	@$(KCONFIG_TOOL) savedefconfig

kernel-defconfig:
	@$(KCONFIG_TOOL) reset

# ---------------------------------------------------------------------------
# rootfs - packaging only, no compiler is invoked on the rootfs itself
# ---------------------------------------------------------------------------
$(ROOTFS_OUT)/.dir:
	@mkdir -p $(ROOTFS_OUT)
	@touch $@

# The rootfs targets used to depend only on a "mkdir happened" marker, so a
# change to the tree did not repack it.  This manifest is regenerated on every
# run (FORCE) and is only rewritten when the tree really changed, which gives
# make the dependency it needs without hashing every file on every build.
ROOTFS_MANIFEST := $(ROOTFS_OUT)/.tree-manifest

$(ROOTFS_MANIFEST): FORCE | $(ROOTFS_OUT)/.dir
	@find $(ROOTFS_TREE) -printf '%p %y %m %s %T@\n' | sort >$@.new
	@# The module tree is part of the image, so a rebuilt .ko has to repack it.
	@if [ -d $(KERNEL_MODULES) ]; then \
		find $(KERNEL_MODULES) -printf 'mod %P %y %m %s %T@\n' | sort >>$@.new; \
	fi
	@cmp -s $@.new $@ || mv $@.new $@
	@rm -f $@.new

.PHONY: FORCE
FORCE:

$(ROOTFS_SQUASHFS): $(ROOTFS_MANIFEST)
	@test -d $(ROOTFS_TREE) || { \
		echo "error: ROOTFS_TREE $(ROOTFS_TREE) does not exist" >&2; exit 1; }
	@echo "==> staging rootfs from $(ROOTFS_TREE)"
	@rm -rf $(ROOTFS_STAGE)
	@mkdir -p $(ROOTFS_STAGE)
	@cp -a $(ROOTFS_TREE)/. $(ROOTFS_STAGE)/
	@for d in $(ROOTFS_DIRS); do mkdir -p $(ROOTFS_STAGE)/$$d; done
	@if [ -d $(KERNEL_MODULES)/lib/modules ]; then \
		echo "==> adding kernel modules from $(KERNEL_MODULES)"; \
		mkdir -p $(ROOTFS_STAGE)/lib; \
		cp -a $(KERNEL_MODULES)/lib/. $(ROOTFS_STAGE)/lib/; \
	else \
		echo "note: $(KERNEL_MODULES) is not staged yet, so this rootfs carries" >&2; \
		echo "      no /lib/modules; run 'make kernel' first if you want it" >&2; \
	fi
	@echo "==> packing SquashFS"
	mksquashfs $(ROOTFS_STAGE) $@ -noappend -comp gzip -b 65536 \
		-processors 1 -all-root
	@echo "rootfs: $@"

# SquashFS is the only userland format that ships: it is what the flash image
# carries and what OTA replaces.  There is deliberately no ramfs/cpio path --
# the kernel no longer embeds a userland, so nothing consumes an archive.  The
# kernel's own usr/gen_init_cpio is still built inside out/kernel/build for its
# tiny built-in cpio list; that is kbuild's business, not ours.
rootfs: $(ROOTFS_SQUASHFS)
rootfs-squashfs: $(ROOTFS_SQUASHFS)

# ---------------------------------------------------------------------------
# busybox - the one place applets are added or removed
# ---------------------------------------------------------------------------
# The userland component rebuild targets below are the only ones that
# need sdk-src/.  They check for it first and explain the situation instead of
# failing with a bare "No such file or directory" from bash.  Like the other
# sdk-src targets they stay out of `make help`, which lists only what a
# published checkout can actually build.
SDK_SRC := $(TOP)/sdk-src

define require-sdk-src
	@test -d $(SDK_SRC)/scripts || { \
		echo "error: the $(1) rebuild target is not available in this checkout." >&2; \
		echo "       bootloader/, kernel/, rootfs/ and flash-image build from" >&2; \
		echo "       the sources present here and do not need it." >&2; \
		exit 1; }
endef

# Edit sdk-src/configs/busybox-runtime.config, run this, then repack the rootfs
# image.  Every applet is a symlink to one binary, so only the applet *code*
# costs space.
busybox:
	$(call require-sdk-src,busybox)
	@echo "==> busybox (sdk-src/configs/busybox-runtime.config)"
	bash $(TOP)/sdk-src/scripts/install-busybox.sh $(ROOTFS_TREE)
	@echo "now rerun 'make rootfs-squashfs' to repack the rootfs partition"

# ---------------------------------------------------------------------------
# dropbear and the in-project /sbin tools
# ---------------------------------------------------------------------------
# These are separate from busybox because they are separate programs with their
# own sources.  Like busybox they must be rebuilt after any libc ABI change.
dropbear:
	$(call require-sdk-src,dropbear)
	@echo "==> dropbear (sdk-src/configs/dropbear-localoptions.h)"
	bash $(TOP)/sdk-src/scripts/install-dropbear.sh $(ROOTFS_TREE)
	@echo "now rerun 'make rootfs-squashfs' to repack the rootfs partition"

tools:
	$(call require-sdk-src,tools)
	@echo "==> board tools (n32-flash)"
	bash $(TOP)/sdk-src/scripts/install-n32-tools.sh $(ROOTFS_TREE)
	@echo "now rerun 'make rootfs-squashfs' to repack the rootfs partition"

# BusyBox ships no audio applets.  The ALSA user space on this board is the
# upstream TinyALSA utilities: tinyplay (playback), tinycap (capture),
# tinymix (mixer controls) and tinypcminfo (PCM capabilities).
tinyalsa:
	$(call require-sdk-src,tinyalsa)
	@echo "==> tinyalsa (sdk-src/sources/tinyalsa)"
	bash $(TOP)/sdk-src/scripts/install-tinyalsa.sh $(ROOTFS_TREE)
	@echo "now rerun 'make rootfs-squashfs' to repack the rootfs partition"

# pcaudiolib is espeak-ng's audio sink and has no ALSA/PulseAudio/OSS/QSA to
# talk to here, so the port at sdk-src/project/pcaudiolib/ adds a TinyALSA
# backend.  Unlike the targets above this installs nothing: it produces a
# static library under out/work/, which is what the espeak-ng cross build links.
# Run 'make tinyalsa' first - the backend links TinyALSA's archive.
pcaudiolib:
	$(call require-sdk-src,pcaudiolib)
	@echo "==> pcaudiolib (sdk-src/sources/pcaudiolib + TinyALSA backend)"
	$(MAKE) -C $(TOP)/sdk-src/project/pcaudiolib

# Dropbear execs SFTPSERVER_PATH for the SSH "sftp" subsystem (used by "sftp"
# and by "scp" in its default mode); nothing in busybox or dropbear provides it.
sftp-server:
	$(call require-sdk-src,sftp-server)
	@echo "==> sftp-server (sdk-src/project/src/sftp-server.c)"
	bash $(TOP)/sdk-src/scripts/install-sftp-server.sh $(ROOTFS_TREE)
	@echo "now rerun 'make rootfs-squashfs' to repack the rootfs partition"

# ---------------------------------------------------------------------------
# curl, Mbed TLS and the CA bundle it validates against
# ---------------------------------------------------------------------------
# This BusyBox has no curl applet and its wget is built without https, so curl
# is the board's only HTTPS client.  Mbed TLS is built as shared libraries
# rather than statically linked so that curl - and anything added later - uses
# one copy on the board.  'curl' therefore depends on 'mbedtls'.
mbedtls:
	$(call require-sdk-src,mbedtls)
	@echo "==> mbedtls (sdk-src/configs/mbedtls-config.h)"
	bash $(TOP)/sdk-src/scripts/install-mbedtls.sh $(ROOTFS_TREE)
	@echo "now rerun 'make rootfs-squashfs' to repack the rootfs partition"

curl: mbedtls
	$(call require-sdk-src,curl)
	@echo "==> curl (sdk-src/sources/curl)"
	bash $(TOP)/sdk-src/scripts/install-curl.sh $(ROOTFS_TREE)
	@echo "now rerun 'make rootfs-squashfs' to repack the rootfs partition"

# curl is built with --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt, so
# without this the https:// URLs fail with "error reading CA cert file".  Skip
# it only if the product supplies its own trust store.
ca-certificates:
	$(call require-sdk-src,ca-certificates)
	@echo "==> CA certificates (sdk-src/sources/ca-certificates)"
	bash $(TOP)/sdk-src/scripts/install-ca-certificates.sh $(ROOTFS_TREE)
	@echo "now rerun 'make rootfs-squashfs' to repack the rootfs partition"

# ---------------------------------------------------------------------------
# flash images
# ---------------------------------------------------------------------------
# The two build steps are sequenced in the recipe rather than listed as
# prerequisites: make builds prerequisites in parallel under -j, and packing the
# rootfs before the kernel had staged its loadable modules silently produced an
# image without /lib/modules.  Both targets still work on their own.
flash-image:
	@$(MAKE) --no-print-directory kernel
	@$(MAKE) --no-print-directory rootfs-squashfs
	@echo "==> SPI NOR image ($(FLASH_SIZE): dtb + kernel + rootfs, system erased)"
	python3 $(SPI_IMAGE_TOOL) \
		--output $(FLASH_IMAGE) \
		--prefix-out $(FLASH_PREFIX) \
		--dtb $(KERNEL_DTB) \
		--kernel $(KERNEL_ZIMAGE) \
		--rootfs $(ROOTFS_SQUASHFS) \
		--size $(FLASH_SIZE) \
		--min-system $(MIN_SYSTEM) \
		--manifest $(FLASH_IMAGE).sha256
	@echo "flash image: $(FLASH_IMAGE)"
	@echo "boot region: $(FLASH_PREFIX)  (dtb + kernel + rootfs; no system)"

# Write the fixed partitions onto the board's SPI flash through its own USB MSC
# device.  Only the boot region is written, so the system partition survives an
# update.
flash-kernel: flash-image
	@echo "==> writing dtb + kernel + rootfs to the board over USB MSC"
	$(TOOLS_DIR)/h787ctl msc-write $(FLASH_PREFIX)

# The on-chip flash programmer lives in the NSLink repository, which is a
# separate checkout: point H787_RECOVER_TOOL at its tools/h787_recover_flash.py.
# There is no default because the path is machine-specific -- note this is NOT
# our own top-level tools/ directory, which holds only host-side helpers.
H787_RECOVER_TOOL ?=
# Leave empty to let the debug probe be auto-detected; set it when more than
# one CMSIS-DAP probe is attached.
H787_DAP_SERIAL   ?=

flash-bootloader: bootloader
	@echo "==> programming CM7 bootloader into on-chip flash"
	@test -n "$(H787_RECOVER_TOOL)" || { \
		echo "error: H787_RECOVER_TOOL is not set." >&2; \
		echo "       It must point at the NSLink checkout's" >&2; \
		echo "       tools/h787_recover_flash.py, e.g." >&2; \
		echo "       make flash-bootloader H787_RECOVER_TOOL=/path/to/ns-link/tools/h787_recover_flash.py" >&2; \
		exit 1; }
	@test -f $(H787_RECOVER_TOOL) || { \
		echo "error: $(H787_RECOVER_TOOL) not found" >&2; exit 1; }
	python3 $(H787_RECOVER_TOOL) $(BOOT_BIN) \
		$(if $(H787_DAP_SERIAL),--serial $(H787_DAP_SERIAL),) \
		--elf $(BOOT_ELF) \
		--output $(OUT_DIR)/recover-$(shell date +%Y%m%d-%H%M%S)

# ---------------------------------------------------------------------------
# network OTA
# ---------------------------------------------------------------------------
# Push an image over SSH and let the board program its own flash.  No probe and
# no USB cable: n32-flash on the board erases, programs and reads the partition
# back byte for byte, then reboots into it.  The address is DHCP-assigned; the
# board's MAC comes from its chip ID, so the lease usually comes back the same,
# but the address is not built into the image and H787_BOARD still has to be
# passed in.
#
# The image is piped into the board's stdin ("n32-flash ota -"), not copied
# there first.  Staging on the board also works and is documented, but /tmp is
# an unquota'd ramfs, so how much of the board's RAM a staged image costs
# depends on its size; streaming needs only a 32 KiB buffer and is therefore
# the one form that behaves the same for every image.
#
# All four partitions are reachable this way: the device tree leaves dtb and
# kernel writable, and n32-flash refuses to write an image whose header does not
# match the target (zImage magic for kernel, DTB magic for dtb).  dtb and kernel
# only take effect after the reboot the tool performs, and a board that will not
# come up is recovered through the bootloader's USB MSC mode.
#
# Deliberately no default address: the board takes a DHCP lease, so a baked-in
# IP would be someone else's bench and would just hang on an SSH timeout.
# Find the current one on the console ("ifconfig eth0") or in the DHCP lease
# table, then pass it for this run or export it.
#
# With an empty root password, ssh cannot prompt in a non-interactive build:
#   printf '#!/bin/sh\necho ""\n' >/tmp/askpass.sh; chmod +x /tmp/askpass.sh
#   H787_SSH_ASKPASS=/tmp/askpass.sh make ota-rootfs H787_BOARD=root@<board-ip>
H787_BOARD    ?=
SSH_OTA_TOOL  := $(TOOLS_DIR)/ssh-ota.sh

# Shared precondition for every ota-<part> target.
ota-check:
	@test -n "$(H787_BOARD)" || { \
		echo "error: H787_BOARD is not set." >&2; \
		echo "       It is the board's SSH target, e.g. root@<board-ip>." >&2; \
		echo "       The address comes from DHCP (the MAC is fixed per board, so" >&2; \
		echo "       the lease is stable, but the image does not pin it).  Read" >&2; \
		echo "       it with 'ifconfig eth0' on the console and pass it:" >&2; \
		echo "         make ota-<part> H787_BOARD=root@<ip>" >&2; \
		exit 1; }
	@test -f $(SSH_OTA_TOOL) || { \
		echo "error: $(SSH_OTA_TOOL) not found." >&2; \
		echo "       The network-OTA helper ships in tools/; restore it and" >&2; \
		echo "       retry.  The board-side half (n32-flash ota) is in rootfs/." >&2; \
		exit 1; }

ota-rootfs: rootfs-squashfs ota-check
	@echo "==> network OTA: rootfs -> $(H787_BOARD)"
	H787_BOARD=$(H787_BOARD) bash $(SSH_OTA_TOOL) $(ROOTFS_SQUASHFS) rootfs

ota-kernel: kernel ota-check
	@echo "==> network OTA: kernel -> $(H787_BOARD)"
	H787_BOARD=$(H787_BOARD) bash $(SSH_OTA_TOOL) $(KERNEL_ZIMAGE) kernel

ota-dtb: kernel ota-check
	@echo "==> network OTA: dtb -> $(H787_BOARD)"
	H787_BOARD=$(H787_BOARD) bash $(SSH_OTA_TOOL) $(KERNEL_DTB) dtb

# ---------------------------------------------------------------------------
# published binaries
# ---------------------------------------------------------------------------
# Copies the two artifacts people download straight out of the repository (via
# raw.githubusercontent.com) into bin/, so a board can be brought up without
# building anything:
#
#   bin/bootloader.bin      CM7 bootloader, programmed into on-chip flash
#   bin/spi-nor-boot.img    dtb + kernel + rootfs, what an over-USB update writes
#
# Deliberately NOT part of `all`: a normal build never writes next to the
# sources, and a bin/ that has drifted from the tree is worse than an empty
# one.  Run this by hand with the tree final, as the last step before a release
# is pushed.
PUBLISHED_BIN_DIR := $(TOP)/bin

publish-bin: bootloader flash-image
	@mkdir -p $(PUBLISHED_BIN_DIR)
	@cp $(DIST_BOOTLOADER) $(PUBLISHED_BIN_DIR)/bootloader.bin
	@cp $(FLASH_PREFIX)    $(PUBLISHED_BIN_DIR)/spi-nor-boot.img
	@chmod 0755 $(PUBLISHED_BIN_DIR)/bootloader.bin
	@chmod 0644 $(PUBLISHED_BIN_DIR)/spi-nor-boot.img
	@echo "==> $(PUBLISHED_BIN_DIR)/ (the two files served by raw.githubusercontent)"
	@cd $(PUBLISHED_BIN_DIR) && sha256sum bootloader.bin spi-nor-boot.img

# ---------------------------------------------------------------------------
# misc
# ---------------------------------------------------------------------------
doctor:
	$(TOOLS_DIR)/h787ctl doctor

# Targeted cleans, for when a full rebuild is not wanted.
clean-bootloader:
	rm -rf $(BOOT_OUT)

clean-kernel:
	rm -rf $(KERNEL_OUT)

clean-rootfs:
	rm -rf $(ROOTFS_OUT)

clean-flash:
	rm -rf $(DIST_OUT)

# Everything make produces lives under $(OUT_DIR), so this is the whole reset.
clean:
	rm -rf $(OUT_DIR)
	@echo "removed $(OUT_DIR); sources untouched"
