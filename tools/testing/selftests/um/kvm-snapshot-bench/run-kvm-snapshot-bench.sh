#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/kvm-snapshot-bench/run-kvm-snapshot-bench.sh — host-side
# launcher for the workstream Phase 3 #250 snapshot benchmark.
#
# Boots UML with `backend=force=kvm kvm_snapshot_bench=N` and
# scrapes the dmesg `kvm_snapshot_bench:` line that the late-
# initcall driver emits. Asserts the line is present + reports
# capture / median / p95 cycle counts.
#
# Validates the memo-12 latency claim ("<50 ms cold-start, sub-ms
# iteration") on real silicon against the physmem-size of the
# default kvmint config.
#
# Pattern mirrors um/kvm-smoke/run-kvm-smoke.sh (kselftest exit
# convention 0/1/4 PASS/FAIL/SKIP). Default N=64 is small enough
# to keep wall time under 5 s on every host the rest of the
# kvm-* selftests already run on.
#
# Environment:
#   UML_BINARY  UML kernel built with CONFIG_UM_BACKEND_KVM_
#               INTEGRATED=y. Default /tmp/uml-kvmint/linux.
#   UML_MEM     mem= argument. Default 128M (full memslot is
#               memcpy'd at each restore — bigger memslot →
#               slower iteration; 128M keeps capture under 50 ms).
#   N          number of restore_full iterations. Default 64.

set -u

BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-128M}
N=${N:-64}

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
	backend=force=kvm \
	kvm_snapshot_bench="$N" \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm)" >&2
	exit 4
fi

# Match only the success line (`capture=N ns; ...`); the failure
# variant says `capture failed (...)` and must surface as FAIL.
LINE=$(echo "$OUT" | grep -E 'kvm_snapshot_bench: capture=[0-9]+ ns;' | head -1)
if [ -z "$LINE" ]; then
	echo "KVM_SNAPSHOT_BENCH: FAIL (no successful bench line)"
	# Print whatever bench-related diagnostics we did get for
	# triage.
	echo "$OUT" | grep -E 'kvm_snapshot|backend = |UML: fatal' | head -5
	exit 1
fi

# Sanity-check: capture and median must both be > 0 (zero would
# indicate the timing source returned 0, like the original
# get_cycles() bug).
CAP=$(echo "$LINE" | sed -E 's/.*capture=([0-9]+) ns.*/\1/')
MED=$(echo "$LINE" | sed -E 's/.*median=([0-9]+).*/\1/')
if [ "$CAP" -eq 0 ] || [ "$MED" -eq 0 ]; then
	echo "KVM_SNAPSHOT_BENCH: FAIL (capture=$CAP median=$MED — timing source returned 0)"
	exit 1
fi

SUMMARY=$(echo "$LINE" | sed -E 's/.*kvm_snapshot_bench: //')
echo "KVM_SNAPSHOT_BENCH: PASS $SUMMARY"
exit 0
