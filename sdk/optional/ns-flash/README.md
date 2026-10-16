# ns-flash — optional add-on

A prebuilt `ns-flash` 0.1.4 for the N32H787, cross-built for this board's
Cortex-M7 no-MMU Linux (uClibc, ELF FDPIC). It is the **host side** of the
NSLink probe: it drives an external CMSIS-DAP probe plugged into the board's
USB **host** port, and talks SWD to whatever that probe is wired to.

**This is not part of any image.** It is not in `rootfs/`, nothing in `make`
builds or installs it, and `make flash-image` will not put it on the board.
Where it goes on your system is your call.

| Path | What it is |
| --- | --- |
| `ns-flash` | The ARM binary, stripped, 265944 B |

It is **not** a way to flash the board's own internal flash. The target it
reaches is the chip on the other end of the probe's SWD cable; the board it
runs on is only the host.

## Kernel requirements — read this first

The binary itself needs nothing but this SDK's uClibc (it links `libc.so.0`
with an rpath of `/system/lib`). What it *does* need is a kernel with the USB
host stack and the right transport for the probe in front of it. **A probe
that enumerates but cannot be opened is nearly always this, not the tool.**

NSLink ships two firmwares and this one binary speaks to both. It picks the
backend from the interface class the probe answers on, so nothing has to be
configured:

| Probe firmware | Transport | Kernel must provide | Node it opens |
| --- | --- | --- | --- |
| CMSIS-DAP **v2** (bulk) | usbfs | `CONFIG_USB=y` + a host controller; no extra option | `/dev/bus/usb/<bus>/<dev>` |
| CMSIS-DAP **v1** (HID) | hidraw | `CONFIG_INPUT=y`, `CONFIG_HID_SUPPORT=y`, `CONFIG_HID=y`, `CONFIG_HID_GENERIC=y`, **`CONFIG_HIDRAW=y`**, `CONFIG_USB_HID=y` | `/dev/hidrawN` |

Both also need `/sys/bus/usb/devices` and a mounted `devtmpfs`, and the board's
USB port has to actually be in host role (`/sys/class/usb_role/`) — with the
right cable and, on USB1, external VBUS.

**`CONFIG_HIDRAW=y` is the one that matters, and only `CONFIG_USB_HID` is not
enough.** hidraw is what creates `/dev/hidrawN`; without it a HID probe still
enumerates in `lsusb` and still answers `19f5:3106`, and `ns-flash` still finds
the device — it just has no node to talk through, and reports:

```text
not_a_probe: the device matched by vid/pid is not a CMSIS-DAP v2 probe
  detail   bus 2 dev 2 interface 0 is class 0x02
           interface 2 of that device is class 0x03 with endpoints 0x03/0x83
```

That is one probe described three times. `19f5:3106` carries
`1.0 = class 0x02` (the bulk backend), `1.1 = class 0x0a` (CDC), and
`1.2 = class 0x03` (the HID backend). The tool is telling you it matched the
vendor interface and found the HID one next to it, and could not open it.

With HID in the kernel, the same run answers:

```text
probe    : /dev/hidraw0  serial 0001A0000002
idcode   : 0x2BA01477
core     : Cortex-M7, CPUID 0x411FC272
dbg_id   : layout h76x, model 0x760 (N32H760), flash 0x200000
model    : n32h76x_i_2m
```

`--list-dap` naming an interface "USB HID v1" and then saying
`a run would use the old firmware's profile: the table's interface is not on
the bus` is **not an error**: it means the bulk profile is not present, so a
run will use the HID one, which is correct for a v1 probe.

## Install

`../install.sh` is the one-click script for this directory: choose the transport
with `USE` (`adb` by default; add `BOARD=<board-ip>` when you use `ssh`) and it
copies the add-ons onto the board's `/system`. It builds nothing, creates no
directory and needs no `mkdir`. `BOARD` has no default because the board takes a
DHCP lease — read the address on the board with `ifconfig eth0`. Or just put the
binary where you want it:

```sh
scp sdk/optional/ns-flash/ns-flash root@<board-ip>:/system/bin/
ssh root@<board-ip> 'chmod +x /system/bin/ns-flash'
```

`/system` is the writable JFFS2 partition, so the binary survives kernel and
rootfs updates (those only write below `0x710000`).

**Write the path out in full.** A non-interactive `ssh`/`adb` shell has
`PATH=/bin:/sbin:/usr/sbin` and does **not** include `/system/bin`; only the
serial login shell reads `/etc/profile` and gets it. So `/system/bin/ns-flash`
works from a script and `ns-flash` alone does not.

## Use

```sh
/system/bin/ns-flash --help          # the full option list
/system/bin/ns-flash --version
/system/bin/ns-flash --list-dap      # every DAP interface on the bus
/system/bin/ns-flash --detect        # ask the target which part it is
/system/bin/ns-flash --id            # IDCODE only
```

`--detect` and `--id` read and halt nothing, so they are safe to run against a
board that is already running. Flashing is a positional argument:

```sh
/system/bin/ns-flash firmware.bin
```

`--dap <index>` picks between probes when more than one is plugged in, and
`--speed <khz>` overrides the clock (the HID profile defaults to 2000 kHz, the
bulk one higher).

## Limits

- The stack request is 256 KiB, and the program needs it as **one contiguous
  run** — this is a no-MMU target, so a fragmented heap can fail an allocation
  that would succeed on a smaller `free`. Check `free` before a long run.
- Only `--detect`, `--id` and `--list-dap` have been run on this board so far.
  Those go through the same HID report layer a flash run uses, but a complete
  write has not been exercised here.

## Provenance

Built from the NSLink project's `tools/build-m7-linux.sh`, which generates C
from the Zig sources and links it with this package's toolchain
(`sdk/toolchain`, `arm-uclinuxfdpiceabi-gcc`). The source is that project's,
and is not included in this package.

```text
265944 B
md5     d6bfe8f3b44129966e33003b68166ca4
sha256  9cbf1bedfe9d5f454c6c2ef3de706403d2b9c8b0ad5605cb5c1707d88bbd5226
```
