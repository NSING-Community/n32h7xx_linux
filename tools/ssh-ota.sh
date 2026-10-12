#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Push a partition image to the board over SSH and flash it with the board's
# own n32-flash.  This is the cable-free counterpart of `make flash-kernel`:
# it talks to nothing but the network, so it works on a board that has Ethernet
# and power and nothing else plugged in.
#
#   ssh-ota.sh IMAGE PARTITION [TARGET] [--no-reboot]
#
# The board does the erase / program / read-back-verify; this script only makes
# the image get there intact and reports what happened.  PARTITION is a
# /proc/mtd label: dtb, kernel, rootfs or system.  n32-flash checks the image
# header against the target before it erases anything, so a zImage cannot land
# in rootfs and a SquashFS image cannot land in kernel.
#
# The image is piped straight into the board's stdin (`n32-flash ota -`), so the
# board programs it as the bytes arrive and never holds more than one 32 KiB
# chunk; the cost does not depend on how big the image is.  Staging it on the
# board first works too (that is what a plain `scp` to /tmp plus `n32-flash ota
# FILE` does, and it is fine for the usual kernel/rootfs sizes), but /tmp there
# is an unquota'd ramfs, so that route spends as much RAM as the image is big.
# SSH already carries a MAC, so the stream cannot arrive silently corrupted, and
# the board compares every chunk against the flash as it programs it.
#
# When the board has only an empty root password, point H787_SSH_ASKPASS at a
# script that echoes it (ssh cannot prompt without a controlling terminal):
#
#   printf '#!/bin/sh\necho ""\n' >/tmp/askpass.sh; chmod +x /tmp/askpass.sh
#   H787_SSH_ASKPASS=/tmp/askpass.sh make ota-rootfs
set -euo pipefail

usage() {
	echo "usage: $0 IMAGE PARTITION [TARGET] [--no-reboot]" >&2
	echo "  TARGET defaults to \${H787_BOARD:-root@192.168.1.85}" >&2
	exit 2
}

[ $# -ge 2 ] || usage

image=$1
partition=$2
target=${H787_BOARD:-root@192.168.1.85}
reboot_after=1
shift 2

for arg in "$@"; do
	case $arg in
	--no-reboot) reboot_after=0 ;;
	--reboot)    reboot_after=1 ;;
	*)           target=$arg ;;
	esac
done

[ -f "$image" ] || { echo "$0: no such image: $image" >&2; exit 1; }

ssh_opts=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o ConnectTimeout=10 -o ServerAliveInterval=5)

# With an askpass helper, ssh has to be detached from our terminal or it will
# refuse to use it.
if [ -n "${H787_SSH_ASKPASS:-}" ]; then
	export SSH_ASKPASS=$H787_SSH_ASKPASS SSH_ASKPASS_REQUIRE=force
	remote_run()  { setsid -w ssh "${ssh_opts[@]}" "$target" "$1"; }
	remote_feed() { setsid -w ssh "${ssh_opts[@]}" "$target" "$1" < "$image"; }
else
	remote_run()  { ssh "${ssh_opts[@]}" "$target" "$1"; }
	remote_feed() { ssh "${ssh_opts[@]}" "$target" "$1" < "$image"; }
fi

local_sha=$(sha256sum "$image" | awk '{print $1}')
local_size=$(stat -c %s "$image")

echo "==> $target: streaming $image ($local_size bytes) into $partition"
echo "    sha256 $local_sha"

tool=$(remote_run 'for p in /sbin/n32-flash /system/bin/n32-flash; do
	[ -x $p ] && { echo $p; break; }; done')
[ -n "$tool" ] || {
	echo "$0: no n32-flash on the board; run 'make tools' and get it there" >&2
	exit 1
}

if [ $reboot_after -eq 1 ]; then
	echo "==> flashing $partition in place and rebooting"
	# The board reboots the moment the image verifies, so a dropped connection
	# here is the success path, not a failure.
	set +e
	remote_feed "$tool ota - $partition --size $local_size"
	rc=$?
	set -e
	if [ $rc -ne 0 ] && [ $rc -ne 255 ]; then
		echo "$0: n32-flash failed (exit $rc) and the board did not reboot" >&2
		echo "    It refuses before erasing on a header/layout mismatch;" >&2
		echo "    anything else is a partial write.  Recover through the" >&2
		echo "    bootloader's USB MSC mode (hold KEY1 while resetting)." >&2
		exit 1
	fi
	echo "    n32-flash/ssh returned $rc (a reset connection is expected)"
else
	echo "==> flashing $partition in place (no reboot)"
	remote_feed "$tool ota - $partition --size $local_size --no-reboot"
fi

host=${target#*@}
host=${host%%:*}
echo "==> waiting for $host to come back"
for _ in $(seq 1 60); do
	if ping -c 1 -W 1 "$host" >/dev/null 2>&1; then
		echo "    $host is up; ${partition} now runs the pushed image"
		exit 0
	fi
	sleep 1
done

echo "$0: $host did not answer within 60 s." >&2
echo "    If it stays silent, recover through the bootloader's USB MSC mode:" >&2
echo "    hold KEY1 (PG3) while resetting, then rewrite the partition." >&2
exit 1
