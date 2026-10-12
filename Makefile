# SPDX-License-Identifier: BSD-3-Clause
# N32H7xx Linux top-level build entry.
#
# Daily use only touches bootloader/, kernel/, rootfs/ and sdk/.  Everything
# needed for that ships in this tree; the maintainer-only sdk-src/ rebuild
# environment is used by four optional userland-rebuild targets (see `make help`).
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
KERNEL_DTB     := $(KERNEL_STAGE)/$(BOARD).dtb

# --- rootfs (packaging only) ------------------------------------------------
# The tree that becomes the userland: the rootfs/ directory itself, which is the
# one and only userland tree.  It is packed into SquashFS and lives in its own
# flash partition; the kernel no longer embeds a copy (see the defconfig).
ROOTFS_TREE     ?= $(ROOTFS_DIR)
ROOTFS_SQUASHFS := $(ROOTFS_OUT)/rootfs.squashfs

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

.PHONY: all help bootloader kernel rootfs rootfs-squashfs busybox dropbear \
        tools sftp-server flash-image flash-kernel ota-check \
        ota-rootfs ota-kernel ota-dtb \
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

help:
	@echo "N32H7xx Linux top-level targets:"
	@echo
	@echo "  all              bootloader + kernel + rootfs + flash image (default)"
	@echo "  bootloader       build the $(BOOT_STORAGE)-backed CM7 bootloader"
	@echo "  kernel           build and stage the separate zImage + $(BOARD).dtb"
	@echo "  menuconfig       change the kernel configuration (ncurses; 'kernel-menuconfig')"
	@echo "  kernel-nconfig   same with the newer ncurses UI ('kernel-xconfig' for Qt)"
	@echo "  kernel-oldconfig review the symbols a kernel update added, one by one"
	@echo "  kernel-savedefconfig  write the current configuration as a defconfig"
	@echo "  kernel-defconfig go back to $(DEFCONFIG), dropping your changes"
	@echo "  rootfs           pack $(ROOTFS_TREE) into the SquashFS image (no compilation)"
	@echo "  rootfs-squashfs  same thing; kept for scripts that name it explicitly"
	@echo "  busybox          rebuild BusyBox and install it into the rootfs tree"
	@echo "  dropbear         rebuild dropbear/dropbearkey and install them"
	@echo "  tools            rebuild n32-flash and install it"
	@echo "  sftp-server      build the SSH 'sftp' subsystem and install it"
	@echo "    (the four component-rebuild targets above need sdk-src/, the local"
	@echo "     source-rebuild environment, which is not part of this tree."
	@echo "     Everything else here is self-contained.)"
	@echo "  flash-image      assemble the raw SPI NOR image (system left erased)"
	@echo "  flash-kernel     write dtb+kernel+rootfs to the board over USB MSC"
	@echo "  ota-rootfs       push rootfs over SSH and flash it from Linux"
	@echo "  ota-kernel       push $(notdir $(KERNEL_ZIMAGE)) over SSH and flash it from Linux"
	@echo "  ota-dtb          push $(notdir $(KERNEL_DTB)) over SSH and flash it from Linux"
	@echo "  flash-bootloader program the CM7 bootloader into on-chip flash"
	@echo "  doctor           report tool, probe and target state"
	@echo "  clean            remove the whole build tree $(OUT_DIR)"
	@echo
	@echo "Final products land in $(DIST_OUT)/:"
	@echo "  $(notdir $(FLASH_IMAGE))  $(notdir $(FLASH_PREFIX))  bootloader.bin"
	@echo
	@echo "Useful variables:"
	@echo "  JOBS=$(JOBS)  BOOT_STORAGE=$(BOOT_STORAGE)  BOARD=$(BOARD)"
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
	JOBS=$(JOBS) BOARD=$(BOARD) CROSS_COMPILE=$(CROSS_COMPILE) \
		DEFCONFIG=$(DEFCONFIG) \
		BUILD_DIR=$(KERNEL_BUILD) STAGE_DIR=$(KERNEL_STAGE) \
		$(KERNEL_DIR)/scripts/nsing/release.sh
	@test -s $(KERNEL_ZIMAGE)
	@test -s $(KERNEL_DTB)
	@ls -l $(KERNEL_ZIMAGE) $(KERNEL_DTB)
	@echo "kernel: $(KERNEL_ZIMAGE) + $(KERNEL_DTB)"

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
	@find $(ROOTFS_TREE) -printf '%p %y %s %T@\n' | sort >$@.new
	@cmp -s $@.new $@ || mv $@.new $@
	@rm -f $@.new

.PHONY: FORCE
FORCE:

$(ROOTFS_SQUASHFS): $(ROOTFS_MANIFEST)
	@test -d $(ROOTFS_TREE) || { \
		echo "error: ROOTFS_TREE $(ROOTFS_TREE) does not exist" >&2; exit 1; }
	@echo "==> packing SquashFS from $(ROOTFS_TREE)"
	mksquashfs $(ROOTFS_TREE) $@ -noappend -comp gzip -b 65536 \
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
# The four "rebuild a userland component" targets below are the only ones that
# touch sdk-src/.  sdk-src/ is the local source-rebuild environment and is NOT
# part of the open-source release, so they check for it and say so instead of
# failing with a bare "No such file or directory" from bash.
SDK_SRC := $(TOP)/sdk-src

define require-sdk-src
	@test -d $(SDK_SRC)/scripts || { \
		echo "error: $(1) needs $(SDK_SRC)/scripts, which is not part of the" >&2; \
		echo "       open-source tree (it is the local source-rebuild env)." >&2; \
		echo "       Nothing else in this Makefile needs it: bootloader/kernel/" >&2; \
		echo "       rootfs/flash-image all build from the shipped sources." >&2; \
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

# Dropbear execs SFTPSERVER_PATH for the SSH "sftp" subsystem (used by "sftp"
# and by "scp" in its default mode); nothing in busybox or dropbear provides it.
sftp-server:
	$(call require-sdk-src,sftp-server)
	@echo "==> sftp-server (sdk-src/project/src/sftp-server.c)"
	bash $(TOP)/sdk-src/scripts/install-sftp-server.sh $(ROOTFS_TREE)
	@echo "now rerun 'make rootfs-squashfs' to repack the rootfs partition"

# ---------------------------------------------------------------------------
# flash images
# ---------------------------------------------------------------------------
flash-image: kernel rootfs-squashfs
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
# back byte for byte, then reboots into it.  The address is DHCP-assigned
# unless the board is configured otherwise, so it can change between boots.
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
#   H787_SSH_ASKPASS=/tmp/askpass.sh make ota-rootfs H787_BOARD=root@192.168.1.85
H787_BOARD    ?=
SSH_OTA_TOOL  := $(TOOLS_DIR)/ssh-ota.sh

# Shared precondition for every ota-<part> target.
ota-check:
	@test -n "$(H787_BOARD)" || { \
		echo "error: H787_BOARD is not set." >&2; \
		echo "       It is the board's SSH target, e.g. root@192.168.1.85." >&2; \
		echo "       The address is DHCP-assigned and changes between boots, so" >&2; \
		echo "       look it up on the console (ifconfig eth0) and pass it:" >&2; \
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
