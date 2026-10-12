#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Safely write a boot artifact to the N32H7xx USB MSC medium.

The medium is the raw SPI NOR, addressed by byte offset, so the same tool has to
accept every shape the bootloader or an update may want:

    boot region   dtb at 0x000000, zImage at 0x010000, rootfs at 0x410000
                  (this is out/dist/spi-nor-boot.img and stops before system)
    dtb           a bare flattened device tree, written at offset 0
    zimage        a bare zImage, written into the kernel partition at 0x010000

The image type is detected from its magic unless --magic says otherwise.  A
write that would reach into the system partition is refused unless
--allow-system is given, because the whole point of the split is that an update
can never touch the writable userland.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import stat
import sys


# Board facts, not host paths.  The model string is what the bootloader reports
# over USB, and 16 MiB is the shipped W25Q128.  A board carrying a larger NOR
# would fail the size guard, so both are overridable; the model is still checked
# so a bigger device is never written by accident.
MSC_MODEL = os.environ.get("H787_MSC_MODEL", "N32H7xx Boot")
MSC_BYTES = int(os.environ.get("H787_MSC_BYTES", str(16 * 1024 * 1024)), 0)

# Partition table; keep in step with spi_image.py, boot_app.c and the DTS.
DTB_OFFSET = 0x000000
DTB_SIZE = 64 * 1024
KERNEL_OFFSET = 0x010000
KERNEL_SIZE = 4 * 1024 * 1024
ROOTFS_OFFSET = 0x410000
ROOTFS_SIZE = 3 * 1024 * 1024
SYSTEM_OFFSET = 0x710000

ZIMAGE_MAGIC = 0x016F2818
ZIMAGE_MAGIC_OFFSET = 0x24
ZIMAGE_END_OFFSET = 0x2C
FDT_MAGIC = 0xD00DFEED
FDT_TOTALSIZE_OFFSET = 4
SQUASHFS_MAGIC = b"hsqs"

MAX_IMAGE_BYTES = 8 * 1024 * 1024
WRITE_CHUNK = 1024 * 1024
# The flash erases and programs 4 KiB sectors, so that is the smallest unit
# worth comparing against the medium before a write.
SECTOR_BYTES = 4096


def differing_runs(fd: int, payload: bytes, base: int) -> list[tuple[int, int]]:
    """Half-open [start, end) spans of @p payload that differ from the medium.

    An update usually changes only the kernel and rootfs, so most of the 7 MiB
    boot region is already correct on the board; rewriting it would turn every
    iteration into a full erase-and-program pass.  Reading the medium first is
    cheap compared with a NOR program cycle.
    """
    runs: list[tuple[int, int]] = []
    start = None
    for off in range(0, len(payload), SECTOR_BYTES):
        chunk = payload[off:off + SECTOR_BYTES]
        if os.pread(fd, len(chunk), base + off) == chunk:
            if start is not None:
                runs.append((start, off))
                start = None
        elif start is None:
            start = off
    if start is not None:
        runs.append((start, len(payload)))
    return runs


def is_mounted(device: Path) -> bool:
    device_real = os.path.realpath(device)
    try:
        lines = Path("/proc/mounts").read_text().splitlines()
    except OSError:
        return False
    for line in lines:
        fields = line.split()
        if not fields:
            continue
        source = fields[0]
        try:
            if os.path.realpath(source) == device_real:
                return True
        except OSError:
            continue
    return False


def describe_candidate(path: Path) -> tuple[Path, str, int]:
    resolved = path.resolve(strict=True)
    mode = resolved.stat().st_mode
    if not stat.S_ISBLK(mode):
        raise ValueError(f"{resolved} is not a block device")

    sysdev = Path("/sys/class/block") / resolved.name
    model_path = sysdev / "device" / "model"
    size_path = sysdev / "size"
    if not model_path.exists() or not size_path.exists():
        raise ValueError(f"no block-device metadata for {resolved}")

    model = model_path.read_text(encoding="utf-8", errors="replace").strip()
    size = int(size_path.read_text().strip()) * 512
    if model != MSC_MODEL:
        raise ValueError(f"{resolved}: model is {model!r}, expected {MSC_MODEL!r}")
    if size != MSC_BYTES:
        raise ValueError(f"{resolved}: size is {size} bytes, expected {MSC_BYTES}")
    if is_mounted(resolved):
        raise ValueError(f"{resolved} is mounted; refusing to write")
    return resolved, model, size


def find_msc(explicit: str | None) -> Path:
    if explicit:
        return describe_candidate(Path(explicit))[0]

    by_id = Path("/dev/disk/by-id")
    candidates = []
    if by_id.is_dir():
        for path in sorted(by_id.iterdir()):
            if not path.name.startswith("usb-") or "-part" in path.name:
                continue
            try:
                candidates.append(describe_candidate(path)[0])
            except (OSError, ValueError):
                continue
    if len(candidates) != 1:
        if not candidates:
            raise SystemExit(
                "N32H7xx MSC device not found; enter MSC mode first, or pass --device"
            )
        raise SystemExit(
            "multiple N32H7xx MSC devices found; pass --device: "
            + ", ".join(str(path) for path in candidates)
        )
    return candidates[0]


def _be32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset:offset + 4], "big")


def _le32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset:offset + 4], "little")


def _check_dtb(data: bytes, offset: int, label: str) -> int:
    """Validate an FDT at @p offset, returning its total size."""
    if len(data) < offset + 8:
        raise SystemExit(f"{label}: too short to hold a device tree header")
    magic = _be32(data, offset)
    if magic != FDT_MAGIC:
        raise SystemExit(
            f"{label}: bad FDT magic 0x{magic:08x} at offset 0x{offset:x}"
        )
    total = _be32(data, offset + FDT_TOTALSIZE_OFFSET)
    if total < 8 or total > DTB_SIZE:
        raise SystemExit(
            f"{label}: FDT totalsize 0x{total:x} exceeds the "
            f"{DTB_SIZE}-byte dtb partition"
        )
    return total


def _check_zimage(data: bytes, offset: int, label: str) -> int:
    """Validate a zImage at @p offset, returning its length field."""
    if len(data) < offset + ZIMAGE_END_OFFSET + 4:
        raise SystemExit(f"{label}: too short to hold a zImage header")
    magic = _le32(data, offset + ZIMAGE_MAGIC_OFFSET)
    if magic != ZIMAGE_MAGIC:
        raise SystemExit(
            f"{label}: bad zImage magic 0x{magic:08x} "
            f"at offset 0x{offset + ZIMAGE_MAGIC_OFFSET:x}"
        )
    end = _le32(data, offset + ZIMAGE_END_OFFSET)
    if end == 0 or end > KERNEL_SIZE or end > len(data) - offset:
        raise SystemExit(
            f"{label}: invalid zImage length 0x{end:x} for a "
            f"{len(data) - offset}-byte payload"
        )
    return end


def classify_image(data: bytes) -> tuple[str, list[str]]:
    """Return (kind, notes) for an image that starts at medium offset 0."""
    notes: list[str] = []
    if len(data) >= KERNEL_OFFSET + ZIMAGE_END_OFFSET + 4 and \
            _be32(data, 0) == FDT_MAGIC:
        total = _check_dtb(data, 0, "dtb")
        _check_zimage(data, KERNEL_OFFSET, "kernel")
        notes.append(f"dtb {total} bytes @0x000000")
        notes.append(f"kernel zImage @0x{KERNEL_OFFSET:06x}")
        if len(data) > ROOTFS_OFFSET + 4:
            if data[ROOTFS_OFFSET:ROOTFS_OFFSET + 4] == SQUASHFS_MAGIC:
                notes.append(f"rootfs squashfs @0x{ROOTFS_OFFSET:06x}")
            else:
                notes.append(f"rootfs @0x{ROOTFS_OFFSET:06x} is erased/unused")
        return "boot-region", notes
    if len(data) >= 8 and _be32(data, 0) == FDT_MAGIC:
        return "dtb", [f"dtb {_check_dtb(data, 0, 'dtb')} bytes"]
    if len(data) >= ZIMAGE_END_OFFSET + 4 and \
            _le32(data, ZIMAGE_MAGIC_OFFSET) == ZIMAGE_MAGIC:
        end = _check_zimage(data, 0, "zImage")
        return "zimage", [f"zImage length field 0x{end:x}"]
    raise SystemExit(
        "unrecognised image: no FDT magic at 0 and no zImage magic at 0x24; "
        "use --magic to force a type"
    )


def validate_image(data: bytes, magic: str) -> tuple[str, list[str]]:
    if not data or len(data) > MAX_IMAGE_BYTES:
        raise SystemExit(f"image size {len(data)} is outside 1..{MAX_IMAGE_BYTES} bytes")
    if magic == "none":
        return "raw", ["magic checks skipped"]
    if magic == "dtb":
        return "dtb", [f"dtb {_check_dtb(data, 0, 'dtb')} bytes"]
    if magic == "zimage":
        end = _check_zimage(data, 0, "zImage")
        return "zimage", [f"zImage length field 0x{end:x}"]
    return classify_image(data)


def main() -> int:
    global MSC_BYTES

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", nargs="?")
    parser.add_argument("--device", help="explicit MSC block device")
    parser.add_argument("--expect-bytes", type=lambda t: int(t, 0),
                        help="expected MSC capacity (default 16 MiB; raise it "
                             "for a board with a larger SPI NOR)")
    parser.add_argument("--offset", type=lambda t: int(t, 0),
                        help="destination byte offset (default: kernel at "
                             "0x10000, other images at 0)")
    parser.add_argument("--magic", choices=("auto", "dtb", "zimage", "none"),
                        default="auto",
                        help="image type to validate against (default: detect)")
    parser.add_argument("--allow-system", action="store_true",
                        help="permit a write that reaches into the system "
                             "partition (0x710000 and above)")
    parser.add_argument("--find-only", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-verify", action="store_true")
    parser.add_argument("--no-skip", action="store_true",
                        help="write every sector even when the medium already "
                             "holds the same bytes")
    args = parser.parse_args()
    # The capacity guard is what keeps a wrong device from being written; the
    # default matches the shipped 16 MiB NOR, and a bigger board raises it here
    # (or with H787_MSC_BYTES) rather than the check being removed.
    if args.expect_bytes:
        MSC_BYTES = args.expect_bytes

    image = None
    image_path = None
    kind = None
    notes: list[str] = []
    if args.image:
        image_path = Path(args.image)
        image = image_path.read_bytes()
        kind, notes = validate_image(image, args.magic)
        expected_offset = KERNEL_OFFSET if kind == "zimage" else DTB_OFFSET
        if args.offset is None:
            args.offset = expected_offset
        if kind in ("boot-region", "dtb", "zimage") and args.offset != expected_offset:
            raise SystemExit(f"a {kind} image belongs at offset 0x{expected_offset:x}")
    if args.offset is None:
        args.offset = 0
    if args.check_only:
        if image is None or image_path is None:
            raise SystemExit("image path is required with --check-only")
        print(f"IMAGE_OK {image_path} {len(image)} bytes kind={kind}")
        for note in notes:
            print(f"  {note}")
        return 0

    device = find_msc(args.device)
    if args.find_only:
        print(device)
        return 0
    if image is None or image_path is None:
        raise SystemExit("image path is required unless --find-only is used")

    end = args.offset + len(image)
    if args.offset < 0 or end > MSC_BYTES:
        raise SystemExit(
            f"write range 0x{args.offset:x}..0x{end:x} does not fit the "
            f"{MSC_BYTES}-byte medium"
        )

    pad = (-len(image)) % 4096
    payload = image + (b"\xff" * pad)
    print(f"TARGET {device}")
    print(f"IMAGE {image_path} {len(image)} bytes kind={kind}")
    for note in notes:
        print(f"  {note}")
    print(f"WRITE 0x{args.offset:x}..0x{end:x} ({len(payload)} bytes with padding)")
    if end > SYSTEM_OFFSET and not args.allow_system:
        raise SystemExit(
            f"refusing to write past 0x{SYSTEM_OFFSET:x}: the range reaches the "
            "system partition; pass --allow-system to override"
        )
    if end <= SYSTEM_OFFSET:
        print(f"SYSTEM 0x{SYSTEM_OFFSET:x}..0x{MSC_BYTES:x} untouched")
    if args.dry_run:
        return 0

    fd = os.open(device, os.O_RDWR | os.O_SYNC)
    try:
        runs = [(0, len(payload))]
        if not args.no_skip and args.offset % SECTOR_BYTES == 0:
            runs = differing_runs(fd, payload, args.offset)
            changed = sum(end - start for start, end in runs)
            print(f"SKIP   {len(payload) - changed} bytes already match; "
                  f"programming {changed} bytes in {len(runs)} span(s)")
        for start, end in runs:
            for off in range(start, end, WRITE_CHUNK):
                chunk = payload[off:min(off + WRITE_CHUNK, end)]
                if os.pwrite(fd, chunk, args.offset + off) != len(chunk):
                    raise OSError(f"short write at offset 0x{args.offset + off:x}")
        os.fsync(fd)

        if args.no_verify:
            return 0

        digest = hashlib.sha256()
        os.lseek(fd, args.offset, os.SEEK_SET)
        for offset in range(0, len(image), WRITE_CHUNK):
            expected = image[offset:offset + WRITE_CHUNK]
            actual = os.read(fd, len(expected))
            if actual != expected:
                raise SystemExit(
                    f"readback mismatch at offset 0x{args.offset + offset:x}"
                )
            digest.update(actual)
    finally:
        os.close(fd)

    print(f"READBACK_OK sha256={digest.hexdigest()} kind={kind}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
