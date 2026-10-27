#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause

set -e

here=$(dirname "$0")
board=${BOARD:-}
use=${USE:-adb}
start_ui=${START_UI:-0}

file_sha256()
{
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{ print $1 }'
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | awk '{ print $1 }'
	else
		echo "install.sh: sha256sum or shasum is required" >&2
		exit 2
	fi
}

test -x "$here/human-detect" || {
	echo "install.sh: $here/human-detect is missing" >&2
	exit 2
}
test -x "$here/human-detect-ui" || {
	echo "install.sh: $here/human-detect-ui is missing" >&2
	exit 2
}

stop_detector='
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
'

install_detector='
	chmod 0755 /system/bin/.human-detect.new
	mv /system/bin/.human-detect.new /system/bin/human-detect
	sync
'

start_detector='
	sync
	echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
	setsid sh -c "/system/bin/human-detect \
		--socket /run/human-detect.sock --http-port 8080 \
		>>/tmp/human-detect.log 2>&1 </dev/null &"
	sleep 3
	pidof human-detect >/dev/null
'

start_service="$stop_detector
$install_detector
$start_detector"

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
	chmod 0755 /system/bin/.human-detect-ui.new
	mv /system/bin/.human-detect-ui.new /system/bin/human-detect-ui
	sync
'

start_ui_service='
	setsid sh -c "/system/bin/human-detect-ui \
		-s /run/human-detect.sock -i /dev/input/event0 \
		>>/tmp/human-detect-ui.log 2>&1 </dev/null &"
	sleep 1
	pidof human-detect-ui >/dev/null
'

human_detect_sha=$(file_sha256 "$here/human-detect")
human_detect_ui_sha=$(file_sha256 "$here/human-detect-ui")

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
	$scp "$here/human-detect" "root@$board:/system/bin/.human-detect.new"
	$scp "$here/human-detect-ui" "root@$board:/system/bin/.human-detect-ui.new"
	remote_sha=$($ssh "root@$board" \
		"sha256sum /system/bin/.human-detect.new" | awk '{ print $1 }')
	[ "$remote_sha" = "$human_detect_sha" ] || {
		echo "install.sh: human-detect transfer verification failed" >&2
		exit 1
	}
	remote_sha=$($ssh "root@$board" \
		"sha256sum /system/bin/.human-detect-ui.new" | awk '{ print $1 }')
	[ "$remote_sha" = "$human_detect_ui_sha" ] || {
		echo "install.sh: human-detect-ui transfer verification failed" >&2
		exit 1
	}
	$ssh "root@$board" "$stop_ui"
	remote_sha=$($ssh "root@$board" \
		"sha256sum /system/bin/human-detect-ui" | awk '{ print $1 }')
	[ "$remote_sha" = "$human_detect_ui_sha" ] || {
		echo "install.sh: human-detect-ui installed-file verification failed" >&2
		exit 1
	}
	if [ "$start_ui" = 1 ]; then
		$ssh "root@$board" "$stop_detector"
		$ssh "root@$board" "$install_detector"
	else
		$ssh "root@$board" "$start_service"
	fi
	remote_sha=$($ssh "root@$board" \
		"sha256sum /system/bin/human-detect" | awk '{ print $1 }')
	[ "$remote_sha" = "$human_detect_sha" ] || {
		echo "install.sh: human-detect installed-file verification failed" >&2
		exit 1
	}
	if [ "$start_ui" = 1 ]; then
		$ssh "root@$board" "$start_ui_service" || {
			$ssh "root@$board" 'tail -20 /tmp/human-detect-ui.log 2>/dev/null || true'
			echo "install.sh: human-detect-ui did not start" >&2
			exit 1
		}
		$ssh "root@$board" "$start_detector"
	fi
	;;
adb)
	adb push "$here/human-detect" /system/bin/.human-detect.new
	adb push "$here/human-detect-ui" /system/bin/.human-detect-ui.new
	remote_sha=$(adb shell sha256sum /system/bin/.human-detect.new | awk '{ print $1 }')
	[ "$remote_sha" = "$human_detect_sha" ] || {
		echo "install.sh: human-detect transfer verification failed" >&2
		exit 1
	}
	remote_sha=$(adb shell sha256sum /system/bin/.human-detect-ui.new | awk '{ print $1 }')
	[ "$remote_sha" = "$human_detect_ui_sha" ] || {
		echo "install.sh: human-detect-ui transfer verification failed" >&2
		exit 1
	}
	adb shell "$stop_ui"
	remote_sha=$(adb shell sha256sum /system/bin/human-detect-ui | awk '{ print $1 }')
	[ "$remote_sha" = "$human_detect_ui_sha" ] || {
		echo "install.sh: human-detect-ui installed-file verification failed" >&2
		exit 1
	}
	if [ "$start_ui" = 1 ]; then
		adb shell "$stop_detector"
		adb shell "$install_detector"
	else
		adb shell "$start_service"
	fi
	remote_sha=$(adb shell sha256sum /system/bin/human-detect | awk '{ print $1 }')
	[ "$remote_sha" = "$human_detect_sha" ] || {
		echo "install.sh: human-detect installed-file verification failed" >&2
		exit 1
	}
	if [ "$start_ui" = 1 ]; then
		adb shell "$start_ui_service" || {
			adb shell 'tail -20 /tmp/human-detect-ui.log 2>/dev/null || true'
			echo "install.sh: human-detect-ui did not start" >&2
			exit 1
		}
		adb shell "$start_detector"
	fi
	;;
*)
	echo "install.sh: USE must be ssh or adb" >&2
	exit 2
	;;
esac
