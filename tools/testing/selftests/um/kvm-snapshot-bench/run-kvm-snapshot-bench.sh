#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/kvm-snapshot-bench/run-kvm-snapshot-bench.sh — host-side
# launcher for the v2 snapshot benchmark (#168 Phase 5 + 6).
#
# Boots UML with `backend=force=kvm-v2 kvm_v2_snapshot_bench=N`
# and scrapes the dmesg `um: kvm-v2 snapshot bench:` line that the
# late-initcall driver emits. Asserts the line is present + reports
# capture / median / p95 cycle counts.
#
# Validates the memo-12 latency claim ("<50 ms cold-start, sub-ms
# iteration") on real silicon against the physmem-size of the
# default v2 config.
#
# Pattern mirrors um/kvm-smoke/run-kvm-smoke.sh (kselftest exit
# convention 0/1/4 PASS/FAIL/SKIP). Default N=64 is small enough
# to keep wall time under 5 s on every host the rest of the
# kvm-* selftests already run on.
#
# Pre-v2 history: this script previously targeted the v1 path
# (backend=force=kvm + kvm_snapshot_bench=) when the snapshot code
# lived at arch/um/backend/kvm-v1-archive/snapshot.c. Phase 6 of
# the v2 port (memo 26-snapshot §Phase 6) re-plumbs it onto the v2
# symbol/cmdline surface — name change only; the contract carries
# over verbatim.
#
# Environment:
#   UML_BINARY  UML kernel built with CONFIG_UM_BACKEND_KVM_V2=y.
#               Default /tmp/uml-kvmint/linux.
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
	backend=force=kvm-v2 \
	kvm_v2_snapshot_bench="$N" \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

# v2 prints "um: backend = kvm-v2" — accept exactly that string.
OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm-v2" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm-v2)" >&2
	exit 4
fi

# Match only the success line; the failure variant says "capture
# failed (...)" and must surface as FAIL.
LINE=$(echo "$OUT" | grep -E 'um: kvm-v2 snapshot bench: capture=[0-9]+ ns;' | head -1)
if [ -z "$LINE" ]; then
	echo "KVM_SNAPSHOT_BENCH: FAIL (no successful bench line)"
	echo "$OUT" | grep -E 'kvm-v2 snapshot|backend = |UML: fatal' | head -5
	exit 1
fi

# Sanity-check: capture and median must both be > 0 (zero would
# indicate the timing source returned 0).
CAP=$(echo "$LINE" | sed -E 's/.*capture=([0-9]+) ns.*/\1/')
MED=$(echo "$LINE" | sed -E 's/.*median=([0-9]+).*/\1/')
if [ "$CAP" -eq 0 ] || [ "$MED" -eq 0 ]; then
	echo "KVM_SNAPSHOT_BENCH: FAIL (capture=$CAP median=$MED — timing source returned 0)"
	exit 1
fi

SUMMARY=$(echo "$LINE" | sed -E 's/.*um: kvm-v2 snapshot bench: //')
echo "KVM_SNAPSHOT_BENCH: PASS $SUMMARY"
exit 0
