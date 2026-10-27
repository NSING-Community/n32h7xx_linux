#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# install.sh - copy this directory's add-ons onto a board's /system.
#
#   sh install.sh                       over adb (the default)
#   BOARD=<board-ip> USE=ssh sh install.sh
#
# /system is the writable JFFS2 partition and survives kernel and rootfs
# updates.  espeak-ng keeps a directory of its own there (/system/espeak-ng);
# ns-flash and weniclaw are single files and go to /system/bin, which is where
# the board looks for programs. WeniClaw's peripheral skills go to the
# workspace the agent reads (/system/weniclaw/workspace/skills).  Note that the
# whole partition is only a few MiB on a 16 MiB NOR part: check "df /system"
# before installing every component.
# The root password is empty, so the ssh path answers the prompt from a helper
# rather than asking you to press Enter.
#
# BOARD has no default: the board takes a DHCP lease, so a baked-in address
# would be someone else's bench and would just hang on an ssh timeout.  Read
# the current one on the board ("ifconfig eth0") and pass it for this run.

set -e

here=$(dirname "$0")
board=${BOARD:-}
use=${USE:-adb}
gpioctl="$here/../example/gpioctl"

if [ ! -x "$gpioctl" ]; then
	echo "install.sh: gpioctl not built; run: make -C sdk/example gpioctl" >&2
	exit 2
fi

case "$use" in
ssh)
	if [ -z "$board" ]; then
		echo "install.sh: BOARD is not set." >&2
		echo "            BOARD is the board's address, e.g." >&2
		echo "            BOARD=<board-ip> USE=ssh sh install.sh" >&2
		echo "            The address comes from DHCP; read it on the board" >&2
		echo "            with 'ifconfig eth0'." >&2
		exit 2
	fi
	askpass=/tmp/install.sh-askpass
	printf '#!/bin/sh\necho ""\n' >"$askpass"
	chmod +x "$askpass"
	SSH_ASKPASS="$askpass" SSH_ASKPASS_REQUIRE=force
	export SSH_ASKPASS SSH_ASKPASS_REQUIRE
	# setsid: with no terminal, ssh needs the helper above to answer itself.
	scp="setsid scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
	ssh="setsid ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
	;;
adb)
	;;
*)
	echo "install.sh: USE must be ssh or adb" >&2
	exit 2
	;;
esac

if [ "$use" = adb ]; then
	weniclaw_was_running=false
	if adb shell 'pidof weniclaw >/dev/null 2>&1'; then
		weniclaw_was_running=true
	fi
	adb push "$here/espeak-ng" /system/
	adb push "$here/ns-flash/ns-flash" /system/bin/
	adb push "$here/weniclaw/weniclaw" /system/bin/.weniclaw.new
	adb push "$gpioctl" /system/bin/.gpioctl.new
	adb shell mkdir -p /system/weniclaw/workspace
	adb shell 'rm -f /system/weniclaw/workspace/skills/gpio-led/SKILL.md; rmdir /system/weniclaw/workspace/skills/gpio-led 2>/dev/null || true'
	adb push "$here/weniclaw/skills" /system/weniclaw/workspace/
	adb shell 'killall weniclaw 2>/dev/null || true; killall gpioctl 2>/dev/null || true'
	adb shell 'chmod 0755 /system/bin/.weniclaw.new /system/bin/.gpioctl.new; mv -f /system/bin/.weniclaw.new /system/bin/weniclaw; mv -f /system/bin/.gpioctl.new /system/bin/gpioctl'
	adb shell 'test -x /system/bin/gpioctl && test -r /system/weniclaw/workspace/skills/gpioctl/SKILL.md'
	if [ "$weniclaw_was_running" = true ]; then
		adb shell 'setsid nice -n 10 /system/bin/weniclaw --config /system/weniclaw/weniclaw.json wechat run >> /system/weniclaw/wechat.log 2>&1 </dev/null &'
		sleep 1
		adb shell 'pidof weniclaw >/dev/null'
	fi
else
	weniclaw_was_running=false
	if $ssh "root@$board" 'pidof weniclaw >/dev/null 2>&1'; then
		weniclaw_was_running=true
	fi
	$scp -r "$here/espeak-ng" "root@$board:/system/"
	$scp "$here/ns-flash/ns-flash" "root@$board:/system/bin/"
	$scp "$here/weniclaw/weniclaw" "root@$board:/system/bin/.weniclaw.new"
	$scp "$gpioctl" "root@$board:/system/bin/.gpioctl.new"
	$ssh "root@$board" "mkdir -p /system/weniclaw/workspace"
	$ssh "root@$board" "rm -f /system/weniclaw/workspace/skills/gpio-led/SKILL.md; rmdir /system/weniclaw/workspace/skills/gpio-led 2>/dev/null || true"
	$scp -r "$here/weniclaw/skills" "root@$board:/system/weniclaw/workspace/"
	$ssh "root@$board" "killall weniclaw 2>/dev/null || true; killall gpioctl 2>/dev/null || true"
	$ssh "root@$board" "chmod 0755 /system/bin/.weniclaw.new /system/bin/.gpioctl.new; mv -f /system/bin/.weniclaw.new /system/bin/weniclaw; mv -f /system/bin/.gpioctl.new /system/bin/gpioctl"
	$ssh "root@$board" "test -x /system/bin/gpioctl && test -r /system/weniclaw/workspace/skills/gpioctl/SKILL.md"
	if [ "$weniclaw_was_running" = true ]; then
		$ssh "root@$board" "setsid nice -n 10 /system/bin/weniclaw --config /system/weniclaw/weniclaw.json wechat run >> /system/weniclaw/wechat.log 2>&1 </dev/null &"
		sleep 1
		$ssh "root@$board" "pidof weniclaw >/dev/null"
	fi
fi

echo "install.sh: WeniClaw and gpioctl skill installed" >&2

USE="$use" BOARD="$board" sh "$here/human-detect/install.sh"
