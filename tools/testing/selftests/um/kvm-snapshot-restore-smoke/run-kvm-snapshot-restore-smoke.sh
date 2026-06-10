#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side smoke for KVM v2 snapshot restore. Boots UML with the
# kvm_v2_snapshot_bench=1 cmdline trigger and requires the kernel to emit the
# capture + restore_full timing summary.

set -u

BINARY=${UML_BINARY:-./linux}
MEM=${UML_MEM:-256M}

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
	kvm_v2_snapshot_bench=1 \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm-v2" ]; then
	echo "KVM_SNAPSHOT_RESTORE: FAIL (observed backend='$OBSERVED')"
	echo "$OUT" | grep -E 'backend = |snapshot bench|panic|failed' | head -12
	exit 1
fi

LINE=$(echo "$OUT" | grep -E 'um: kvm-v2 snapshot bench: capture=.*restore_full ns:' | head -1)
if [ -z "$LINE" ]; then
	echo "KVM_SNAPSHOT_RESTORE: FAIL (no snapshot bench restore summary)"
	echo "$OUT" | grep -E 'backend = |snapshot bench|snapshot:|panic|failed' | head -20
	exit 1
fi

echo "KVM_SNAPSHOT_RESTORE: PASS ${LINE#um: kvm-v2 snapshot bench: }"
exit 0
