#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side smoke for the KVM v2 record/replay clock-event path.

set -u

BINARY=${UML_BINARY:-./linux}
MEM=${UML_MEM:-256M}
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

OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	kvm_v2_record_clock_bench="$BENCH_N" \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm-v2" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm-v2)" >&2
	echo "$OUT" | grep -E 'backend = |record clock bench|UML: fatal|panic' | head -20
	exit 4
fi

LINE=$(echo "$OUT" | grep -E '^um: kvm-v2 record clock bench: ' | head -1)
if [ -z "$LINE" ]; then
	echo "KVM_RECORD_CLOCK_BENCH: FAIL (no benchmark line)"
	echo "$OUT" | grep -E 'backend = |record clock bench|record|panic|failed' | head -40
	exit 1
fi

if ! echo "$LINE" | grep -q 'verdict=PASS'; then
	echo "KVM_RECORD_CLOCK_BENCH: FAIL (verdict != PASS)"
	echo "$LINE"
	exit 1
fi

N_OBSERVED=$(echo "$LINE" | sed -n 's/.*observed=\([0-9]\+\).*/\1/p')
N_REPLAYED=$(echo "$LINE" | sed -n 's/.*replayed=\([0-9]\+\).*/\1/p')
N_MISMATCH=$(echo "$LINE" | sed -n 's/.*mismatches=\([0-9]\+\).*/\1/p')

if [ "$N_OBSERVED" != "$BENCH_N" ] ||
   [ "$N_REPLAYED" != "$BENCH_N" ] ||
   [ "$N_MISMATCH" != "0" ]; then
	printf 'KVM_RECORD_CLOCK_BENCH: FAIL observed=%s replayed=%s mismatches=%s expected=%s\n' \
		"$N_OBSERVED" "$N_REPLAYED" "$N_MISMATCH" "$BENCH_N"
	echo "$LINE"
	exit 1
fi

printf 'KVM_RECORD_CLOCK_BENCH: PASS (N=%s observed=%s replayed=%s mismatches=%s)\n' \
	"$BENCH_N" "$N_OBSERVED" "$N_REPLAYED" "$N_MISMATCH"
exit 0
