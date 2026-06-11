#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Guest-side live smoke for the experimental KVM v2 record control surface.

set -u

mount -t proc none /proc 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

CTL=/sys/kernel/debug/um/kvm_v2_record_ctl
STATUS=/sys/kernel/debug/um/kvm_v2_record_status

status_value() {
	awk -F': ' -v key="$1" '$1 == key { print $2; exit }' "$STATUS"
}

fail() {
	echo "KVM_RECORD_SMOKE: FAIL: $1"
	if [ -w "$CTL" ]; then
		echo stop > "$CTL" 2>/dev/null || true
		echo destroy > "$CTL" 2>/dev/null || true
	fi
	halt -f
	exit 1
}

pass() {
	echo "KVM_RECORD_SMOKE: PASS: $1"
	halt -f
	exit 0
}

[ -w "$CTL" ] || fail "$CTL not writable"
[ -r "$STATUS" ] || fail "$STATUS not readable"

initial_state=$(status_value state)
[ "$initial_state" = "none" ] || fail "initial state is $initial_state, expected none"

echo "start 1048576" > "$CTL" || fail "start command failed"
[ "$(status_value state)" = "recording" ] || fail "state after start is $(status_value state)"
[ "$(status_value enabled)" = "1" ] || fail "record static key did not enable"
[ "$(status_value snapshot_attempted)" = "1" ] || fail "snapshot was not attempted"
[ "$(status_value snapshot_valid)" = "1" ] || fail "snapshot was not attached"
[ "$(status_value snapshot_rc)" = "0" ] || fail "snapshot_rc=$(status_value snapshot_rc)"
[ "$(status_value snapshot_task_state)" = "1" ] || fail "task state was not captured"

for i in 1 2 3 4 5; do
	cat /proc/self/stat >/dev/null
	cat /proc/uptime >/dev/null
	/bin/true
	echo "record-workload-$i" >/dev/null
done

echo stop > "$CTL" || fail "stop command failed"
[ "$(status_value state)" = "stopped" ] || fail "state after stop is $(status_value state)"
[ "$(status_value enabled)" = "0" ] || fail "record static key did not disable"

entries=$(status_value entries_recorded)
syscalls=$(status_value syscall_count)
used=$(status_value buffer_used)
dropped=$(status_value entries_dropped)

[ -n "$entries" ] || fail "entries_recorded missing"
[ -n "$syscalls" ] || fail "syscall_count missing"
[ -n "$used" ] || fail "buffer_used missing"

if [ "$entries" -le 0 ] || [ "$syscalls" -le 0 ] || [ "$used" -le 0 ]; then
	fail "no live syscall records captured entries=$entries syscalls=$syscalls used=$used"
fi

echo "strict 0" > "$CTL" || fail "strict 0 failed"
[ "$(status_value strict)" = "0" ] || fail "strict did not switch off"
echo "strict 1" > "$CTL" || fail "strict 1 failed"
[ "$(status_value strict)" = "1" ] || fail "strict did not switch on"

echo destroy > "$CTL" || fail "destroy command failed"
[ "$(status_value state)" = "init" ] || fail "state after destroy is $(status_value state)"
[ "$(status_value snapshot_valid)" = "0" ] || fail "snapshot still attached after destroy"
[ "$(status_value buffer_used)" = "0" ] || fail "buffer_used not reset"
[ "$(status_value entries_recorded)" = "0" ] || fail "entries_recorded not reset"

pass "entries=$entries syscalls=$syscalls used=$used dropped=$dropped"
