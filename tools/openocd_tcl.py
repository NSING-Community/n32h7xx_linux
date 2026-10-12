#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Small, scriptable client for OpenOCD's Tcl-RPC port.

The OpenOCD process used by the board project normally listens on TCP 6666.
This tool is intentionally limited to the operations needed for this board:
target state, reset, memory word access, and the bootloader MSC flag.
"""

from __future__ import annotations

import argparse
import os
import re
import socket
import sys
import time


SEP = b"\x1a"
DEFAULT_HOST = os.environ.get("H787_OPENOCD_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("H787_OPENOCD_TCL_PORT", "6666"), 0)
DEFAULT_TIMEOUT = 30.0

# OpenOCD's Tcl-RPC `capture` command returns command errors as ordinary text.
# Treat the error signatures used by this board's boot-aware reset flow as
# failures so callers do not continue after a reset that never happened.
ERROR_MARKERS = (
    "still disabled",
    "failed to read memory",
    "refusing to touch",
    "target not examined",
    "Runtime Error:",
    "Error:",
    "timed out while waiting for target halted",
    "halt timed out",
    "could not read register",
)

MSC_FLAG_ADDR = int(os.environ.get("H787_MSC_FLAG_ADDR", "0x30015f00"), 0)
MSC_FLAG_MAGIC = int(os.environ.get("H787_MSC_FLAG_MAGIC", "0x544f4f42"), 0)

# The 128-bit UCID is not memory mapped, so DAP cannot simply stop it as a
# block: the vendor CMSIS UCID_BASE (0x1FFFC238) and UID_BASE addresses fault
# on read.  It is only reachable through the OTPC word interface, four words
# starting at OTPC address 0x0238.
OTPC_BASE = int(os.environ.get("H787_OTPC_BASE", "0x51118000"), 0)
OTPC_CTRL = OTPC_BASE + 0x00
OTPC_STS = OTPC_BASE + 0x04
OTPC_ADDR = OTPC_BASE + 0x10
OTPC_RDATA = OTPC_BASE + 0x14
OTPC_CTRL_PRMD = 0x2        # bit1: 1 = write, 0 = read
OTPC_STS_ERR = 0x7C         # BE|OORE|RDPE|WRPE|PGE; sticky, write 1 to clear
UCID_OTP_ADDR = 0x0238
UCID_WORDS = 4

# The DP SELECT register (index 8) selects the AP bank and APBANKSEL.  A dirty
# value can survive a target reset. Clear it before the recovery window, as in
# the vendor reference run (n32h760.dap dpreg 8 0). This sequence has recovered
# the board; it does not by itself establish why AP0 became unavailable.
DP_SELECT = 8


class OpenOCDError(RuntimeError):
    pass


def rpc(host: str, port: int, command: str, timeout: float) -> str:
    """Run one OpenOCD command and return captured text."""
    payload = b"capture {" + command.encode("utf-8") + b"}" + SEP
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(payload)
            data = bytearray()
            while SEP not in data:
                chunk = sock.recv(4096)
                if not chunk:
                    raise OpenOCDError("OpenOCD closed the Tcl-RPC connection")
                data.extend(chunk)
    except OSError as exc:
        raise OpenOCDError(f"cannot talk to OpenOCD at {host}:{port}: {exc}") from exc

    return bytes(data).split(SEP, 1)[0].decode("utf-8", "replace").strip()


def require_args(args: list[str], count: int, usage: str) -> None:
    if len(args) != count:
        raise SystemExit(f"usage: {usage}")


def check_rpc_output(output: str) -> None:
    for marker in ERROR_MARKERS:
        if marker.lower() in output.lower():
            raise OpenOCDError(output)


def rpc_hex_value(host: str, port: int, command: str, timeout: float) -> int:
    output = rpc(host, port, command, timeout)
    check_rpc_output(output)
    for field in reversed(output.replace(":", " ").replace(",", " ").split()):
        if re.fullmatch(r"(?:0x)?[0-9a-fA-F]+", field):
            return int(field, 16)
    raise OpenOCDError(f"no hexadecimal value in OpenOCD output: {output!r}")


def read_ucid(host: str, port: int, timeout: float) -> str:
    """Read the 128-bit chip UCID over DAP and return it as a hex string."""
    def write(address: int, value: int) -> None:
        check_rpc_output(rpc(host, port, f"mww {address:#x} {value:#x}", timeout))

    write(OTPC_CTRL, rpc_hex_value(host, port, f"mdw {OTPC_CTRL:#x} 1", timeout)
          & ~OTPC_CTRL_PRMD)
    # Error flags are sticky and the boot ROM leaves a read-protection error
    # set, so a clean slate is needed before the result can be trusted.
    write(OTPC_STS, OTPC_STS_ERR)

    words = []
    for index in range(UCID_WORDS):
        otp_addr = UCID_OTP_ADDR + index
        write(OTPC_ADDR, otp_addr)
        sts = rpc_hex_value(host, port, f"mdw {OTPC_STS:#x} 1", timeout)
        if sts & OTPC_STS_ERR:
            raise OpenOCDError(
                f"OTPC read error at word {otp_addr:#x}: STS={sts:#x}")
        words.append(rpc_hex_value(host, port, f"mdw {OTPC_RDATA:#x} 1", timeout))

    return "".join(f"{word:08x}" for word in words)


def recover_halt(host: str, port: int, timeout: float, args: list[str]) -> int:
    if len(args) > 1:
        raise SystemExit("usage: openocd_tcl.py recover-halt [DELAY_MS]")

    delay_ms = int(args[0], 0) if args else 700
    if delay_ms < 0 or delay_ms >= 1500:
        raise SystemExit("DELAY_MS must be in the 1.5 s recovery window: 0..1499")

    target = "n32h7xx.cpu"
    dap = "n32h7xx.dap"

    state_output = rpc(host, port, "targets", timeout)
    check_rpc_output(state_output)
    if "halted" in state_output:
        print("target is halted; leaving it in a known running state first...")
        output = rpc(host, port, "reset run", timeout)
        check_rpc_output(output)
        if output:
            print(output)

    # Clear DP SELECT before resetting.  A stale SELECT (AP bank / APBANKSEL) is
    # not cleared by nRESET and is exactly the condition that wedges the boot
    # window until a power cycle.  Writing a DP register is safe while AP0 is
    # disabled, so this must come first.
    try:
        output = rpc(host, port, f"{dap} dpreg {DP_SELECT} 0", timeout)
        if output:
            print(output)
    except OpenOCDError as exc:
        print(f"note: DP SELECT clear failed ({exc}); continuing")

    # NSLink implements CMSIS-DAP DAP_ResetTarget (0x0a) on PB0: it drives the
    # target's nRESET low for 20 ms, releases it, then settles for 10 ms.  This
    # works even though OpenOCD does not expose the adapter's SRST as a generic
    # reset signal.
    output = rpc(host, port, "cmsis-dap cmd 0x0a", timeout)
    check_rpc_output(output)
    if output:
        print(output)

    if delay_ms:
        time.sleep(delay_ms / 1000.0)

    output = rpc(host, port, f"{target} arp_examine", timeout)
    check_rpc_output(output)
    if output:
        print(output)

    output = rpc(host, port, "halt", timeout)
    check_rpc_output(output)
    if output:
        print(output)

    cpuid = rpc_hex_value(host, port, "mdw 0xe000ed00 1", timeout)
    pc = rpc(host, port, "reg pc", timeout).strip()
    print(f"recovered: CPUID={cpuid:#010x} PC={pc}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument(
        "action",
        choices=[
            "targets",
            "halt",
            "resume",
            "reset-halt",
            "reset-run",
            "reset-init",
            "recover-halt",
            "mdw",
            "mww",
            "set-msc-flag",
            "clear-msc-flag",
            "read-msc-flag",
            "ucid",
            "raw",
        ],
    )
    parser.add_argument("args", nargs="*")
    args = parser.parse_args()

    if args.action == "recover-halt":
        return recover_halt(args.host, args.port, args.timeout, args.args)
    if args.action == "ucid":
        print(read_ucid(args.host, args.port, args.timeout))
        return 0
    if args.action == "targets":
        command = "targets"
    elif args.action == "halt":
        command = "halt"
    elif args.action == "resume":
        command = "resume"
    elif args.action == "reset-halt":
        command = "reset halt"
    elif args.action == "reset-run":
        command = "reset run"
    elif args.action == "reset-init":
        command = "reset init"
    elif args.action == "mdw":
        if len(args.args) not in (1, 2):
            raise SystemExit("usage: openocd_tcl.py mdw ADDRESS [COUNT]")
        command = "mdw " + " ".join(args.args)
    elif args.action == "mww":
        require_args(args.args, 2, "openocd_tcl.py mww ADDRESS VALUE")
        command = "mww " + " ".join(args.args)
    elif args.action == "set-msc-flag":
        outputs = []
        outputs.append(rpc(
            args.host,
            args.port,
            f"mww {MSC_FLAG_ADDR:#x} {MSC_FLAG_MAGIC:#x}",
            args.timeout,
        ))
        outputs.append(rpc(
            args.host,
            args.port,
            f"mww {MSC_FLAG_ADDR + 4:#x} {(~MSC_FLAG_MAGIC) & 0xffffffff:#x}",
            args.timeout,
        ))
        for output in outputs:
            check_rpc_output(output)
        print("MSC flag written at 0x{:08x}".format(MSC_FLAG_ADDR))
        for output in outputs:
            if output:
                print(output)
        return 0
    elif args.action == "clear-msc-flag":
        outputs = [
            rpc(args.host, args.port, f"mww {MSC_FLAG_ADDR:#x} 0", args.timeout),
            rpc(args.host, args.port, f"mww {MSC_FLAG_ADDR + 4:#x} 0", args.timeout),
        ]
        for output in outputs:
            check_rpc_output(output)
        print("MSC flag cleared")
        return 0
    elif args.action == "read-msc-flag":
        output = rpc(
            args.host,
            args.port,
            f"mdw {MSC_FLAG_ADDR:#x} 2",
            args.timeout,
        )
        check_rpc_output(output)
        print(output)
        return 0
    else:
        if not args.args:
            raise SystemExit("usage: openocd_tcl.py raw COMMAND [ARGS...]")
        command = " ".join(args.args)

    output = rpc(args.host, args.port, command, args.timeout)
    check_rpc_output(output)
    if output:
        print(output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OpenOCDError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
