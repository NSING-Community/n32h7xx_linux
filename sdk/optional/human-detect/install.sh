#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause

set -e

here=$(dirname "$0")
board=${BOARD:-}
use=${USE:-adb}
start_ui=${START_UI:-0}

test -x "$here/human-detect" || {
	echo "install.sh: $here/human-detect is missing" >&2
	exit 2
}
test -x "$here/human-detect-ui" || {
	echo "install.sh: $here/human-detect-ui is missing" >&2
	exit 2
}

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

stop_ui='
	killall human-detect-ui 2>/dev/null || true
	n=5
	while pidof human-detect-ui >/dev/null 2>&1 && [ "$n" -gt 0 ]; do
		sleep 1
		n=$((n - 1))
	done
	if pidof human-detect-ui >/dev/null 2>&1; then
		killall -9 human-detect-ui 2>/dev/null || true
		sleep 1
	fi
	chmod 0755 /tmp/human-detect-ui.new
	mv /tmp/human-detect-ui.new /system/bin/human-detect-ui
	sync
'

start_ui_service='
	setsid /system/bin/human-detect-ui \
		-s /run/human-detect.sock -i /dev/input/event0 \
		>>/tmp/human-detect-ui.log 2>&1 </dev/null &
	sleep 1
	pidof human-detect-ui >/dev/null
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
	$scp "$here/human-detect-ui" "root@$board:/tmp/human-detect-ui.new"
	$ssh "root@$board" "$stop_ui"
	$ssh "root@$board" "$start_service"
	if [ "$start_ui" = 1 ]; then
		$ssh "root@$board" "$start_ui_service" || {
			$ssh "root@$board" 'tail -20 /tmp/human-detect-ui.log 2>/dev/null || true'
			echo "install.sh: human-detect-ui did not start" >&2
			exit 1
		}
	fi
	;;
adb)
	adb push "$here/human-detect" /tmp/human-detect.new
	adb push "$here/human-detect-ui" /tmp/human-detect-ui.new
	adb shell "$stop_ui"
	adb shell "$start_service"
	if [ "$start_ui" = 1 ]; then
		adb shell "$start_ui_service" || {
			adb shell 'tail -20 /tmp/human-detect-ui.log 2>/dev/null || true'
			echo "install.sh: human-detect-ui did not start" >&2
			exit 1
		}
	fi
	;;
*)
	echo "install.sh: USE must be ssh or adb" >&2
	exit 2
	;;
esac
