#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Assemble a raw SPI NOR image for the N32H787 board.

The image packs every fixed partition and deliberately leaves the system
partition erased:

    dtb      0x000000    64 KiB   flattened device tree
    kernel   0x010000     4 MiB   Linux zImage (no appended DTB)
    rootfs   0x410000     3 MiB   SquashFS
    system   0x710000    remainder

system is not packed because the kernel formats it as JFFS2 on first mount.  Its
size is whatever the NOR device leaves after the fixed partitions, so one build
works on a larger flash without touching the layout; only a floor is enforced so
a typo in --size cannot produce a system partition too small to be useful.

The bootloader reads just the dtb and kernel partitions and hands the DTB
address to Linux in r2; rootfs and system are reached through MTD.  The layout
mirrors the BOOT_PART_* constants in bootloader/CM7/src/boot_app.c and the DTS
fixed-partitions node (kernel/arch/arm/boot/dts/nsing/n32h787-hmi.dts).

Two artifacts come out of one run:

    --output       the whole flash, fixed partitions plus the erased system tail
    --prefix-out   everything except system, i.e. bytes [0, 0x710000)

The prefix is what an over-USB update writes: it carries dtb + kernel + rootfs
and stops exactly where the system partition starts, so a firmware update can
never reach the user's writable data.  system is sized by the medium itself
(flash size minus 0x710000), which is why the same build works on a larger part
and why --min-system only has to catch an unusably small remainder.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys


MIB = 1024 * 1024
KIB = 1024

DTB_OFFSET = 0x000000
DTB_SIZE = 64 * KIB
KERNEL_OFFSET = 0x010000
KERNEL_SIZE = 4 * MIB
ROOTFS_OFFSET = 0x410000
ROOTFS_SIZE = 3 * MIB
SYSTEM_OFFSET = 0x710000

# The system partition holds the whole writable userland, so refuse to build an
# image that would shrink it below a usable floor.
MIN_SYSTEM_SIZE = 8 * MIB

FLASH_BYTES_DEFAULT = 16 * MIB
ERASE_BYTE_DEFAULT = 0xFF

FDT_MAGIC = 0xD00DFEED
FDT_TOTALSIZE_OFFSET = 4

ZIMAGE_MAGIC = 0x016F2818
ZIMAGE_MAGIC_OFFSET = 0x24
ZIMAGE_END_OFFSET = 0x2C


_SUFFIXES = (
    ("kib", 1024),
    ("mib", 1024 ** 2),
    ("gib", 1024 ** 3),
    ("kb", 1000),
    ("mb", 1000 ** 2),
    ("gb", 1000 ** 3),
    ("k", 1024),
    ("m", 1024 ** 2),
    ("g", 1024 ** 3),
)


def parse_int(text: str) -> int:
    """Accept plain literals and IEC/SI byte suffixes such as 16M, 8MiB."""
    value = text.strip().lower()
    scale = 1
    for suffix, factor in _SUFFIXES:
        if value.endswith(suffix):
            value = value[: -len(suffix)]
            scale = factor
            break
    try:
        return int(value, 0) * scale
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"not a size: {text!r}") from exc


def read_dtb(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < 8:
        raise SystemExit(f"{path}: too short to contain a device tree header")
    magic = int.from_bytes(data[0:4], "big")
    if magic != FDT_MAGIC:
        raise SystemExit(f"{path}: bad FDT magic 0x{magic:08x} at offset 0")
    total = int.from_bytes(data[FDT_TOTALSIZE_OFFSET:FDT_TOTALSIZE_OFFSET + 4], "big")
    if total != len(data):
        raise SystemExit(
            f"{path}: FDT totalsize 0x{total:x} disagrees with the "
            f"{len(data)}-byte file"
        )
    if len(data) > DTB_SIZE:
        raise SystemExit(
            f"{path}: DTB is {len(data)} bytes and overflows the "
            f"{DTB_SIZE}-byte dtb partition"
        )
    return data


def read_zimage(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < ZIMAGE_END_OFFSET + 4:
        raise SystemExit(f"{path}: too short to contain a zImage header")
    magic = int.from_bytes(data[ZIMAGE_MAGIC_OFFSET:ZIMAGE_MAGIC_OFFSET + 4], "little")
    end = int.from_bytes(data[ZIMAGE_END_OFFSET:ZIMAGE_END_OFFSET + 4], "little")
    if magic != ZIMAGE_MAGIC:
        raise SystemExit(f"{path}: bad zImage magic 0x{magic:08x} at offset 0x24")
    if end != len(data):
        # Trailing bytes are almost always an appended DTB, and an appended DTB
        # makes the decompressor discard the r2 address in favour of the blob at
        # _edata.  Refuse rather than ship an image that silently ignores the
        # dtb partition.
        raise SystemExit(
            f"{path}: zImage length field 0x{end:x} disagrees with the "
            f"{len(data)}-byte file; do not append a DTB, the dtb partition "
            "is loaded separately"
        )
    if len(data) > KERNEL_SIZE:
        raise SystemExit(
            f"{path}: zImage is {len(data)} bytes and overflows the "
            f"{KERNEL_SIZE}-byte kernel partition"
        )
    return data


def build_image(
    size: int,
    dtb: bytes,
    kernel: bytes,
    rootfs: bytes,
    min_system: int,
    erase_byte: int = ERASE_BYTE_DEFAULT,
) -> bytearray:
    system_size = size - SYSTEM_OFFSET
    if system_size < min_system:
        raise SystemExit(
            f"flash size 0x{size:x} leaves only {system_size} bytes for the "
            f"system partition; at least {min_system} are required "
            f"(fixed partitions end at 0x{SYSTEM_OFFSET:x})"
        )
    if len(rootfs) > ROOTFS_SIZE:
        raise SystemExit(
            f"rootfs is {len(rootfs)} bytes and overflows the "
            f"{ROOTFS_SIZE}-byte rootfs partition"
        )

    image = bytearray(bytes([erase_byte]) * size)
    image[DTB_OFFSET:DTB_OFFSET + len(dtb)] = dtb
    image[KERNEL_OFFSET:KERNEL_OFFSET + len(kernel)] = kernel
    image[ROOTFS_OFFSET:ROOTFS_OFFSET + len(rootfs)] = rootfs
    return image


def describe(name: str, offset: int, size: int, used: int | None) -> str:
    if used is None:
        text = f"(reserved, left erased)"
    else:
        text = f"{used} bytes"
    return (
        f"  {name:<7} @ 0x{offset:06x}  {size:>9} bytes "
        f"({size / MIB:5.2f} MiB)  {text}"
    )


def boot_region(image: bytes | bytearray) -> bytes | bytearray:
    """The fixed partitions only: dtb + kernel + rootfs, no system.

    SYSTEM_OFFSET is the first byte of the system partition, so slicing there
    gives an image that a firmware update can write over USB without touching
    the writable userland.
    """
    return image[:SYSTEM_OFFSET]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="full flash image, including the erased system tail")
    parser.add_argument("--prefix-out", type=Path,
                        help="also write the dtb+kernel+rootfs boot region "
                             f"([0, 0x{SYSTEM_OFFSET:x})) for over-USB updates")
    parser.add_argument("--dtb", type=Path, required=True,
                        help="flattened device tree from the kernel release step")
    parser.add_argument("--kernel", type=Path, required=True,
                        help="zImage from the kernel release step (no appended DTB)")
    parser.add_argument("--rootfs", type=Path, required=True,
                        help="SquashFS rootfs image")
    parser.add_argument("--size", type=parse_int, default=FLASH_BYTES_DEFAULT,
                        help="flash size in bytes (default 16 MiB)")
    parser.add_argument("--min-system", type=parse_int, default=MIN_SYSTEM_SIZE,
                        help="smallest acceptable system partition (default 8 MiB)")
    parser.add_argument("--erase-byte", type=parse_int, default=ERASE_BYTE_DEFAULT,
                        help="fill byte for unused space and the system partition")
    parser.add_argument("--manifest", type=Path,
                        help="write a sha256 manifest next to the image")
    args = parser.parse_args()

    if not 0 <= args.erase_byte <= 0xFF:
        raise SystemExit("--erase-byte must fit in one byte")
    if args.size <= 0:
        raise SystemExit("--size must be positive")

    dtb = read_dtb(args.dtb)
    kernel = read_zimage(args.kernel)
    rootfs = args.rootfs.read_bytes()

    image = build_image(
        args.size, dtb, kernel, rootfs, args.min_system, args.erase_byte
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    digest = hashlib.sha256(image).hexdigest()

    print(f"flash image: {args.output}")
    print(f"  total        {len(image)} bytes ({len(image) / MIB:.2f} MiB)")
    print(describe("dtb", DTB_OFFSET, DTB_SIZE, len(dtb)))
    print(describe("kernel", KERNEL_OFFSET, KERNEL_SIZE, len(kernel)))
    print(describe("rootfs", ROOTFS_OFFSET, ROOTFS_SIZE, len(rootfs)))
    print(describe("system", SYSTEM_OFFSET, args.size - SYSTEM_OFFSET, None))
    print(f"  sha256       {digest}")

    prefix_digest = None
    if args.prefix_out:
        prefix = boot_region(image)
        args.prefix_out.parent.mkdir(parents=True, exist_ok=True)
        args.prefix_out.write_bytes(prefix)
        prefix_digest = hashlib.sha256(prefix).hexdigest()
        print()
        print(f"boot region: {args.prefix_out}  (dtb + kernel + rootfs)")
        print(f"  size         {len(prefix)} bytes ({len(prefix) / MIB:.2f} MiB)")
        print(f"  covers       [0x000000, 0x{SYSTEM_OFFSET:06x})")
        print(f"  system       untouched; starts at 0x{SYSTEM_OFFSET:06x}")
        print(f"  sha256       {prefix_digest}")

    if args.manifest:
        lines = [f"{digest}  {args.output.name}\n"]
        if args.prefix_out and prefix_digest:
            lines.append(f"{prefix_digest}  {args.prefix_out.name}\n")
        args.manifest.write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
