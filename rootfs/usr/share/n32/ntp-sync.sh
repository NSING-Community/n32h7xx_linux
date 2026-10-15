#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# One-shot NTP step, backgrounded by /etc/init.d/ntp at boot.
#
# Waits for udhcpc to leave a name server and a default route, then runs
# "ntpd -n -q" (ntpdate semantics: poll once, step, exit) and stores the clock
# in the on-chip RTC, which still has no battery, so a power cycle loses it.
#
# no-MMU hush: real program invocations only, never a "( ... ) &" compound.
export PATH=/bin:/sbin

CONF=/etc/ntp.conf
# How long to wait for the DHCP lease to appear before trying anyway.
WAIT_MAX=60
# Backstop for ntpd itself; with -q it normally exits within a minute.
NTPD_TIMEOUT=90

# Backgrounded, so stdout never reaches the console: log through the kernel log
# ("tools/h787ctl klog"), since the serial link drops bytes board->host.
log_msg()
{
	echo "$*"
	echo "$*" >/dev/kmsg 2>/dev/null
	return 0
}

# Lease is in once udhcpc wrote a nameserver and installed a default route;
# ntpd resolves the peer names itself, so both are prerequisites.
lease_ready()
{
	grep -q '^nameserver ' /etc/resolv.conf 2>/dev/null || return 1
	ip route show 2>/dev/null | grep -q '^default'
}

wait_until()
{
	tries=0
	while [ "$tries" -lt "$WAIT_MAX" ]; do
		if "$@"; then
			return 0
		fi
		sleep 1
		tries=$((tries + 1))
	done
	return 1
}

if [ ! -s "$CONF" ]; then
	log_msg 'ntp-sync: no NTP peers configured, skipping'
	exit 0
fi

# BusyBox ntpd takes peers as repeated -p options, so the chosen file is read
# here instead of relying on its hardcoded /etc/ntp.conf.  Comment and blank
# lines are skipped; the "server" keyword is the only one ntpd understands.
peers=$(sed -n 's/^[[:space:]]*server[[:space:]]\{1,\}\([^[:space:]]\{1,\}\).*/-p \1/p' "$CONF")
if [ -z "$peers" ]; then
	log_msg "ntp-sync: $CONF lists no usable 'server' lines, skipping"
	exit 0
fi

if ! wait_until lease_ready; then
	log_msg "ntp-sync: still no name server/default route after ${WAIT_MAX}s"
fi

# An epoch-dated line followed by a real date proves the step happened.
log_msg "ntp-sync: before: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"

# Bound ntpd by hand rather than with busybox "timeout": the rc below decides
# whether the clock gets stored in the RTC, and "timeout" reports a timeout and
# a self-inflicted SIGTERM as the same 143.
# Unquoted on purpose: $peers is a list of "-p HOST" words built above.
ntpd -n -q $peers &
ntpd_pid=$!
ntpd_left=$NTPD_TIMEOUT
while [ "$ntpd_left" -gt 0 ] && kill -0 "$ntpd_pid" 2>/dev/null; do
	sleep 1
	ntpd_left=$((ntpd_left - 1))
done
rc=1
if kill -0 "$ntpd_pid" 2>/dev/null; then
	log_msg "ntp-sync: ntpd still running after ${NTPD_TIMEOUT}s, killing it"
	kill -9 "$ntpd_pid" 2>/dev/null
	wait "$ntpd_pid" 2>/dev/null
else
	wait "$ntpd_pid" 2>/dev/null
	rc=$?
fi

if [ "$rc" -eq 0 ]; then
	log_msg "ntp-sync: after:  $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
	# -u: the system clock and rtc_hctosys both treat the RTC as UTC.
	if [ -c /dev/rtc0 ]; then
		if hwclock -w -u -f /dev/rtc0 2>/dev/null; then
			log_msg 'ntp-sync: clock stored in the RTC'
		else
			log_msg 'ntp-sync: WARNING: could not store the clock in the RTC'
		fi
	fi
else
	log_msg "ntp-sync: failed, rc=$rc"
fi
exit "$rc"
