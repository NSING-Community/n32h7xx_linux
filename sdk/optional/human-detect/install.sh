#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause

set -e

here=$(dirname "$0")
board=${BOARD:-}
use=${USE:-adb}

start_service='
	set -e
	killall human-detect 2>/dev/null || true
	n=5
	while pidof human-detect >/dev/null 2>&1 && [ "$n" -gt 0 ]; do
		sleep 1
		n=$((n - 1))
	done
	if pidof human-detect >/dev/null 2>&1; then
		killall -9 human-detect 2>/dev/null || true
		sleep 1
	fi
	if pidof human-detect >/dev/null 2>&1; then
		echo "install.sh: existing human-detect did not stop" >&2
		exit 1
	fi
	chmod 0755 /tmp/human-detect.new
	mv /tmp/human-detect.new /system/bin/human-detect
	sync
	setsid /system/bin/human-detect --socket /run/human-detect.sock \
		--http-port 8080 >>/tmp/human-detect.log 2>&1 </dev/null &
	sleep 1
	pidof human-detect >/dev/null
'

case "$use" in
ssh)
	if [ -z "$board" ]; then
		echo "install.sh: BOARD is required for USE=ssh" >&2
		exit 2
	fi
	askpass=/tmp/human-detect-install-askpass
	printf '#!/bin/sh\necho ""\n' >"$askpass"
	chmod 0700 "$askpass"
	SSH_ASKPASS="$askpass" SSH_ASKPASS_REQUIRE=force
	export SSH_ASKPASS SSH_ASKPASS_REQUIRE
	scp="setsid -w scp -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
	ssh="setsid -w ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
	$scp "$here/human-detect" "root@$board:/tmp/human-detect.new"
	$ssh "root@$board" "$start_service"
	;;
adb)
	adb push "$here/human-detect" /tmp/human-detect.new
	adb shell "$start_service"
	;;
*)
	echo "install.sh: USE must be ssh or adb" >&2
	exit 2
	;;
esac
