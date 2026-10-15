#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# install.sh - copy this directory's add-ons onto a board's /system.
#
#   sh install.sh                    over ssh, to $BOARD
#   BOARD=192.168.1.86 sh install.sh
#   USE=adb sh install.sh            over adb instead
#
# /system is the writable JFFS2 partition and survives kernel and rootfs
# updates.  espeak-ng keeps a directory of its own there (/system/espeak-ng);
# ns-flash is a single file and goes to /system/bin, which is where the board
# looks for programs.  The root password is empty, so the ssh path answers the
# prompt from a helper rather than asking you to press Enter.

set -e

here=$(dirname "$0")
board=${BOARD:-192.168.1.86}
use=${USE:-adb}

case "$use" in
ssh)
	askpass=/tmp/install.sh-askpass
	printf '#!/bin/sh\necho ""\n' >"$askpass"
	chmod +x "$askpass"
	SSH_ASKPASS="$askpass" SSH_ASKPASS_REQUIRE=force
	export SSH_ASKPASS SSH_ASKPASS_REQUIRE
	# setsid: with no terminal, ssh needs the helper above to answer itself.
	scp="setsid scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
	;;
adb)
	;;
*)
	echo "install.sh: USE must be ssh or adb" >&2
	exit 2
	;;
esac

if [ "$use" = adb ]; then
	adb push "$here/espeak-ng" /system/
	adb push "$here/ns-flash/ns-flash" /system/bin/
else
	$scp -r "$here/espeak-ng" "root@$board:/system/"
	$scp "$here/ns-flash/ns-flash" "root@$board:/system/bin/"
fi
