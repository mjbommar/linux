#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/hooks-flip/hooks-flip.sh — workstream B-06 first-flip demo.
#
# Verifies the killer property of the UML Layer 2 gate design:
# a gate can be flipped at runtime, observed in stats, and flipped
# back, all without restarting the kernel.
#
# This script runs *inside* a UML guest. It is invoked by
# run-hooks-flip.sh (from the host) via the init= boot parameter.
#
# Test flow:
#   1. mount proc + debugfs
#   2. read baseline: hooks/trace_syscalls == 0, stats/trace_syscalls hits == N0
#   3. run a small syscall-heavy workload; confirm hits unchanged
#   4. write 1 to hooks/trace_syscalls
#   5. run the workload again; confirm hits increased
#   6. write 0 to hooks/trace_syscalls
#   7. confirm hits stop increasing
#   8. emit a single PASS/FAIL line and halt
#
# Exit code is conveyed via the halt reason on the host side (see
# run-hooks-flip.sh).

set -u

mount -t proc proc /proc 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

HOOK=/sys/kernel/debug/um/hooks/trace_syscalls
STATS=/sys/kernel/debug/um/stats

read_hits() {
	awk '/^trace_syscalls/{print $3; exit}' "$STATS" | sed 's/hits=//'
}

workload() {
	# A handful of syscalls per iteration: stat + read + write.
	local i
	for i in 1 2 3 4 5; do
		ls /proc >/dev/null 2>&1
		ls /sys >/dev/null 2>&1
		echo x > /dev/null
	done
}

fail() {
	echo "HOOKS_FLIP: FAIL: $1"
	echo 0 > "$HOOK" 2>/dev/null
	halt -f
	exit 1
}

pass() {
	echo "HOOKS_FLIP: PASS: $1"
	halt -f
	exit 0
}

[ -r "$HOOK" ] || fail "$HOOK not readable (debugfs not mounted, or sandbox profile)"

# Step 1: baseline state
off_state=$(cat "$HOOK")
baseline_hits=$(read_hits)
if [ "$off_state" != "0" ]; then
	fail "default gate state is $off_state, expected 0"
fi

# Step 2: workload with gate off — hits should stay at baseline
workload
hits_after_off=$(read_hits)
off_delta=$((hits_after_off - baseline_hits))
if [ "$off_delta" -gt 0 ]; then
	fail "gate off but $off_delta hits recorded — static-key machinery broken"
fi

# Step 3: flip on
echo 1 > "$HOOK" || fail "write 1 to $HOOK failed"
on_state=$(cat "$HOOK")
if [ "$on_state" != "1" ]; then
	fail "after write 1, reads $on_state"
fi

# Step 4: workload with gate on — hits should increase
workload
hits_after_on=$(read_hits)
on_delta=$((hits_after_on - hits_after_off))
if [ "$on_delta" -lt 10 ]; then
	fail "gate on but only $on_delta hits (expected >10 from workload)"
fi

# Step 5: flip off
echo 0 > "$HOOK" || fail "write 0 to $HOOK failed"
settled_hits=$(read_hits)
workload
final_hits=$(read_hits)
# Allow small drift from kernel-internal syscalls; require strict
# increase to be much smaller than the on-state increase.
settle_delta=$((final_hits - settled_hits))
if [ "$settle_delta" -gt "$on_delta" ]; then
	fail "after flip-off, still seeing $settle_delta hits (on-phase was $on_delta)"
fi

pass "baseline=$baseline_hits off_delta=$off_delta on_delta=$on_delta settle_delta=$settle_delta"
