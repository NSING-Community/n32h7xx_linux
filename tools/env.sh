#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Common paths and board constants for the H787 project.
#
# Source this after setting H787_ROOT, or source it from the repository root:
#   H787_ROOT=$PWD . ./tools/env.sh

: "${H787_ROOT:?set H787_ROOT before sourcing tools/env.sh}"

# H787_ROOT is the combined project root (the directory holding Makefile,
# bootloader/, kernel/, rootfs/).  tools/h787ctl sets it to its own parent, so
# sourcing this from anywhere inside the checkout gives the same answer.
H787_BOOTLOADER_DIR=${H787_BOOTLOADER_DIR:-$H787_ROOT/bootloader}
# Everything the top-level make builds lands under <project>/out, one
# subdirectory per component.  Override OUT_DIR to sit next to a different tree.
H787_OUT_DIR=${H787_OUT_DIR:-$H787_ROOT/out}
H787_BUILD_DIR=${H787_BUILD_DIR:-$H787_OUT_DIR/bootloader/CM7}
H787_BOOT_BIN=${H787_BOOT_BIN:-$H787_BUILD_DIR/h787_linux_CM7.bin}

# The kernel is a submodule of this project, so it is always a sibling here.
H787_LINUX_ROOT=${H787_LINUX_ROOT:-$H787_ROOT/kernel}
H787_KERNEL_OUT=${H787_KERNEL_OUT:-$H787_OUT_DIR/kernel}
H787_KERNEL_ZIMAGE=${H787_KERNEL_ZIMAGE:-$H787_KERNEL_OUT/zImage}
H787_KERNEL_DTB=${H787_KERNEL_DTB:-$H787_KERNEL_OUT/n32h787-hmi.dtb}

H787_OPENOCD_CFG=${H787_OPENOCD_CFG:-$H787_BOOTLOADER_DIR/OpenocdCfg/n32h7xx_cm7_cmsisdap.cfg}
H787_OPENOCD_HOST=${H787_OPENOCD_HOST:-127.0.0.1}
H787_OPENOCD_TCL_PORT=${H787_OPENOCD_TCL_PORT:-6666}

# Probe serial.  Deliberately EMPTY by default: a hard-coded USB serial would
# be this bench's probe and would make every other board fail to attach.  Leave
# it empty to let OpenOCD pick the only attached CMSIS-DAP; set it only when
# more than one probe is plugged in.
H787_DAP_SERIAL=${H787_DAP_SERIAL:-}

# The NSLink probe exposes a CDC-ACM console whose ttyACM number follows USB
# enumeration order, so it moves between ttyACM0 and ttyACM1.  Prefer the
# stable by-id symlink; pyserial opens it like any other device node.  The
# ttyACM fallback is only a last resort and is reported as such, so a missing
# symlink does not silently pick the wrong adapter.
H787_SERIAL_SOURCE=auto
if [ -z "${H787_SERIAL:-}" ]; then
	for h787_link in /dev/serial/by-id/*CMSIS-DAP*; do
		if [ -e "$h787_link" ]; then
			H787_SERIAL=$h787_link
			H787_SERIAL_SOURCE=by-id
			break
		fi
	done
	if [ -z "${H787_SERIAL:-}" ]; then
		H787_SERIAL=/dev/ttyACM0
		H787_SERIAL_SOURCE=fallback
	fi
else
	H787_SERIAL_SOURCE=explicit
fi
H787_SERIAL_BAUD=${H787_SERIAL_BAUD:-115200}

H787_MSC_FLAG_ADDR=${H787_MSC_FLAG_ADDR:-0x30015f00}
H787_MSC_FLAG_MAGIC=${H787_MSC_FLAG_MAGIC:-0x544f4f42}

export H787_BOOTLOADER_DIR H787_BUILD_DIR H787_BOOT_BIN
export H787_OUT_DIR H787_LINUX_ROOT H787_KERNEL_OUT
export H787_KERNEL_ZIMAGE H787_KERNEL_DTB
export H787_OPENOCD_CFG H787_OPENOCD_HOST H787_OPENOCD_TCL_PORT
export H787_DAP_SERIAL H787_SERIAL H787_SERIAL_BAUD H787_SERIAL_SOURCE
export H787_MSC_FLAG_ADDR H787_MSC_FLAG_MAGIC
