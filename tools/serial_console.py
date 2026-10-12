#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Read, write, and pattern-match the board's USART1 console.

The port auto-detects: it prefers a NSLink / CMSIS-DAP console in
/dev/serial/by-id/ and refuses to guess between several candidates.  Pin it with
--device or H787_SERIAL when there is more than one.  Defaults to 115200 8N1.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import select
import sys
import termios
import time


# Empty means "auto-detect".  A hard-coded /dev/ttyACM0 would silently pick
# whatever USB serial happens to enumerate first on someone else's machine, so
# the device is only pinned when H787_SERIAL (or --device) says so explicitly.
DEFAULT_DEVICE = os.environ.get("H787_SERIAL", "")
DEFAULT_BAUD = int(os.environ.get("H787_SERIAL_BAUD", "115200"), 0)


def find_device() -> str:
    by_id = Path("/dev/serial/by-id")
    candidates = []
    if by_id.is_dir():
        candidates = sorted(
            str(path) for path in by_id.iterdir()
            if "CMSIS-DAP" in path.name or "NSLink" in path.name
        )
    if not candidates:
        candidates = sorted(str(path) for path in Path("/dev").glob("ttyACM*"))
    if len(candidates) != 1:
        if not candidates:
            raise SystemExit("no serial device found; pass --device")
        raise SystemExit(
            "multiple serial devices found; pass --device: " + ", ".join(candidates)
        )
    return candidates[0]


def configure_device(fd: int, baud: int) -> None:
    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag = attrs[:4]

    iflag &= ~(
        termios.IGNBRK
        | termios.BRKINT
        | termios.PARMRK
        | termios.ISTRIP
        | termios.INLCR
        | termios.IGNCR
        | termios.ICRNL
        | termios.IXON
        | termios.IXOFF
        | termios.IXANY
    )
    oflag &= ~termios.OPOST
    lflag &= ~(
        termios.ECHO
        | termios.ECHONL
        | termios.ICANON
        | termios.ISIG
        | termios.IEXTEN
    )
    cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
    cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL

    speed = getattr(termios, f"B{baud}", None)
    if speed is None:
        raise SystemExit(f"unsupported baud rate: {baud}")

    attrs[0], attrs[1], attrs[2], attrs[3] = iflag, oflag, cflag, lflag
    attrs[4] = attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def open_serial(device: str, baud: int) -> int:
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure_device(fd, baud)
        os.set_blocking(fd, False)
    except Exception:
        os.close(fd)
        raise
    return fd


def write_text(fd: int, text: str, newline: bool = True) -> None:
    payload = text.encode("utf-8") + (b"\n" if newline else b"")
    view = memoryview(payload)
    while view:
        written = os.write(fd, view)
        view = view[written:]


def read_for(fd: int, seconds: float, output: str | None = None) -> None:
    deadline = time.monotonic() + seconds
    out = open(output, "wb") if output else None
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            ready, _, _ = select.select([fd], [], [], min(remaining, 0.25))
            if not ready:
                continue
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if not data:
                continue
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            if out:
                out.write(data)
                out.flush()
    finally:
        if out:
            out.close()


def expect(fd: int, pattern: str, timeout: float, output: str | None = None) -> int:
    regex = re.compile(pattern.encode("utf-8"))
    deadline = time.monotonic() + timeout
    seen = bytearray()
    out = open(output, "wb") if output else None
    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], min(0.25, deadline - time.monotonic()))
            if not ready:
                continue
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if not data:
                continue
            seen.extend(data)
            if len(seen) > 2 * 1024 * 1024:
                del seen[:-1024 * 1024]
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            if out:
                out.write(data)
                out.flush()
            if regex.search(seen):
                return 0
    finally:
        if out:
            out.close()
    print(f"timeout waiting for pattern: {pattern}", file=sys.stderr)
    return 1


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--device", default=DEFAULT_DEVICE,
                        help="serial port; default auto-detects the CMSIS-DAP console")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)

    capture = sub.add_parser("capture", help="capture output for a fixed time")
    add_common(capture)
    capture.add_argument("--seconds", type=float, default=10.0)
    capture.add_argument("-o", "--output")

    send = sub.add_parser("send", help="send text to the console")
    add_common(send)
    send.add_argument("text")
    send.add_argument("--no-newline", action="store_true")

    run = sub.add_parser("run", help="send one command and capture its response")
    add_common(run)
    run.add_argument("text")
    run.add_argument("--seconds", type=float, default=3.0)
    run.add_argument("-o", "--output")

    expect_parser = sub.add_parser("expect", help="wait for a regular expression")
    add_common(expect_parser)
    expect_parser.add_argument("pattern")
    expect_parser.add_argument("--timeout", type=float, default=30.0)
    expect_parser.add_argument("-o", "--output")

    args = parser.parse_args()
    device = args.device
    # Auto-detect unless the user pinned a device, or the environment pinned one
    # that no longer exists (a stale by-id symlink should fall back, not fail).
    if not device or not Path(device).exists():
        device = find_device()

    fd = open_serial(device, args.baud)
    try:
        if args.action == "capture":
            read_for(fd, args.seconds, args.output)
            return 0
        if args.action == "send":
            write_text(fd, args.text, not args.no_newline)
            return 0
        if args.action == "run":
            write_text(fd, args.text)
            read_for(fd, args.seconds, args.output)
            return 0
        return expect(fd, args.pattern, args.timeout, args.output)
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())
