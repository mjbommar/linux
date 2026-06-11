#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Guest-side smoke for the KVM v2 private state trace debugfs surface.

set -u

mount -t proc none /proc 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

CTL=/sys/kernel/debug/um/kvm_v2_state_trace_ctl
STATUS=/sys/kernel/debug/um/kvm_v2_state_trace_status
DUMP=/sys/kernel/debug/um/kvm_v2_state_trace_dump

status_value() {
	awk -F': ' -v key="$1" '$1 == key { print $2; exit }' "$STATUS"
}

skip() {
	echo "KVM_STATE_TRACE_SMOKE: SKIP: $1"
	halt -f
	exit 4
}

fail() {
	echo "KVM_STATE_TRACE_SMOKE: FAIL: $1"
	if [ -w "$CTL" ]; then
		echo disable > "$CTL" 2>/dev/null || true
	fi
	halt -f
	exit 1
}

pass() {
	echo "KVM_STATE_TRACE_SMOKE: PASS: $1"
	halt -f
	exit 0
}

[ -w "$CTL" ] || skip "$CTL not writable"
[ -r "$STATUS" ] || fail "$STATUS not readable"
[ -r "$DUMP" ] || fail "$DUMP not readable"

echo disable > "$CTL" || fail "disable command failed"
echo clear > "$CTL" || fail "clear command failed"
[ "$(status_value entries)" = "0" ] || fail "initial entries not zero"

echo enable > "$CTL" || fail "enable command failed"
[ "$(status_value enabled)" = "1" ] || fail "enabled did not become 1"

for i in 1 2 3 4 5; do
	cat /proc/self/stat >/dev/null
	cat /proc/uptime >/dev/null
	/bin/true
	uname -a >/dev/null
done

echo disable > "$CTL" || fail "disable after workload failed"
[ "$(status_value enabled)" = "0" ] || fail "enabled did not become 0"

entries=$(status_value entries)
sequence=$(status_value sequence)
capacity=$(status_value capacity)

[ -n "$entries" ] || fail "entries missing"
[ -n "$sequence" ] || fail "sequence missing"
[ -n "$capacity" ] || fail "capacity missing"

if [ "$entries" -le 0 ] || [ "$sequence" -le 0 ]; then
	fail "no trace entries captured entries=$entries sequence=$sequence"
fi
if [ "$capacity" -lt 64 ]; then
	fail "capacity too small: $capacity"
fi

echo "KVM_STATE_TRACE_DUMP_BEGIN"
cat "$DUMP"
echo "KVM_STATE_TRACE_DUMP_END"

echo clear > "$CTL" || fail "final clear failed"
[ "$(status_value entries)" = "0" ] || fail "entries not zero after clear"
[ "$(status_value sequence)" = "0" ] || fail "sequence not zero after clear"

pass "entries=$entries sequence=$sequence capacity=$capacity"
