#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side benchmark smoke for KVM v2 snapshot capture and restore.
#
# The test boots UML with kvm_v2_snapshot_bench=N, requires the KVM v2
# backend to run, and parses the kernel timing line. It is a regression
# guard for the snapshot benchmark control surface rather than a strict
# cross-machine performance assertion.

set -u

BINARY=${UML_BINARY:-./linux}
MEM=${UML_MEM:-256M}
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

OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	kvm_v2_snapshot_bench="$N" \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm-v2" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm-v2)" >&2
	echo "$OUT" | grep -E 'backend = |snapshot bench|UML: fatal|panic' | head -12
	exit 4
fi

LINE=$(echo "$OUT" | grep -E 'um: kvm-v2 snapshot bench: capture=[0-9]+ ns;' | head -1)
if [ -z "$LINE" ]; then
	echo "KVM_SNAPSHOT_BENCH: FAIL (no successful benchmark line)"
	echo "$OUT" | grep -E 'backend = |snapshot bench|snapshot:|panic|failed' | head -20
	exit 1
fi

CAP=$(echo "$LINE" | sed -E 's/.*capture=([0-9]+) ns.*/\1/')
MED=$(echo "$LINE" | sed -E 's/.*median=([0-9]+).*/\1/')
if [ -z "$CAP" ] || [ -z "$MED" ] || [ "$CAP" -eq 0 ] || [ "$MED" -eq 0 ]; then
	echo "KVM_SNAPSHOT_BENCH: FAIL (invalid timing values: capture=$CAP median=$MED)"
	exit 1
fi

SUMMARY=$(echo "$LINE" | sed -E 's/.*um: kvm-v2 snapshot bench: //')
echo "KVM_SNAPSHOT_BENCH: PASS $SUMMARY"
exit 0
