#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/kvm-record-clock-bench/run-kvm-record-clock-bench.sh — memo 04
# §"Acceptance criteria" integration gate (post-2026-05-19 sprint).
#
# Boots UML with `backend=force=kvm-v2 kvm_v2_record_clock_bench=N`,
# then greps dmesg for the single-line bench verdict that
# arch/um/backend/kvm-v2/record.c::kvm_v2_record_clock_bench_run emits:
#
#   um: kvm-v2 record clock bench: N=%u observed=%llu replayed=%llu
#       mismatches=%llu verdict=%s
#
# verdict=PASS iff the full hook chain (um_on_clock_read →
# __um_record_event_clock → kvm_v2_record_observe_time_travel) plus the
# symmetric replay-side path (um_time_travel_consume_replay →
# kvm_v2_record_consume_time_travel) round-trips N monotonic clock
# advances byte-identical.  Closes HONEST-AUDIT §1's integration-test
# gap: KUnit (kvm_v2_record_smoke 8/8) exercises the observe/consume
# API surface with synthetic events; this bench exercises the runtime
# wiring with the same static-key gates a production
# `umlctl record start --engage-hooks` flow flips.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.
#
# Environment:
#   UML_BINARY   UML kernel built with CONFIG_UM_BACKEND_KVM_V2=y.
#                Default /tmp/uml-kvmint/linux.
#   UML_MEM      mem= argument.  Default 128M.
#   BENCH_N      N value for kvm_v2_record_clock_bench=N. Default 100
#                (matches memo 04's 100-advance example).

set -u

BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-128M}
BENCH_N=${BENCH_N:-100}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi

if [ ! -e /dev/kvm ]; then
	echo "SKIP: /dev/kvm not present" >&2
	exit 4
fi

ensure_kvm_readable() {
	local i
	for i in 1 2 3; do
		if [ -r /dev/kvm ]; then
			return 0
		fi
		sudo -n setfacl -m u:"$(id -un)":rw /dev/kvm 2>/dev/null || true
		[ -r /dev/kvm ] && return 0
		sleep 0.1
	done
	return 1
}

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable" >&2
	exit 4
fi

OUT=$(timeout --kill-after=10 30 "$BINARY" \
	backend=force=kvm-v2 \
	kvm_v2_record_clock_bench="$BENCH_N" \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED_BACKEND=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED_BACKEND" != "kvm-v2" ]; then
	echo "SKIP: backend probed to '$OBSERVED_BACKEND' (need kvm-v2)" >&2
	exit 4
fi

LINE=$(echo "$OUT" | grep -E '^um: kvm-v2 record clock bench: ' | head -1)
if [ -z "$LINE" ]; then
	echo "KVM_RECORD_CLOCK_BENCH: FAIL (no bench line in dmesg)"
	echo "$OUT" | grep -Ei 'backend = |record|panic|bug|warn' | head -10
	exit 1
fi

# Parse the verdict.
if ! echo "$LINE" | grep -q 'verdict=PASS'; then
	echo "KVM_RECORD_CLOCK_BENCH: FAIL (verdict != PASS)"
	echo "$LINE"
	exit 1
fi

# Cross-check the counters: observed and replayed must both equal N,
# mismatches must be zero.  verdict=PASS already implies these, but
# the explicit assertion guards against future refactors that loosen
# the verdict string.
N_OBSERVED=$(echo "$LINE" | sed -n 's/.*observed=\([0-9]\+\).*/\1/p')
N_REPLAYED=$(echo "$LINE" | sed -n 's/.*replayed=\([0-9]\+\).*/\1/p')
N_MISMATCH=$(echo "$LINE" | sed -n 's/.*mismatches=\([0-9]\+\).*/\1/p')

if [ "$N_OBSERVED" != "$BENCH_N" ] || \
   [ "$N_REPLAYED" != "$BENCH_N" ] || \
   [ "$N_MISMATCH" != "0" ]; then
	echo "KVM_RECORD_CLOCK_BENCH: FAIL (counter cross-check) observed=$N_OBSERVED replayed=$N_REPLAYED mismatches=$N_MISMATCH expected_N=$BENCH_N"
	echo "$LINE"
	exit 1
fi

echo "KVM_RECORD_CLOCK_BENCH: PASS (N=$BENCH_N observed=$N_OBSERVED replayed=$N_REPLAYED mismatches=$N_MISMATCH)"
exit 0
