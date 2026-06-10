#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side smoke for KVM v2 snapshot restore. Boots a supported UP guest with
# kvm_v2_snapshot_bench=1 and requires the kernel to emit the capture +
# restore_full timing summary. When the tested UML binary has CONFIG_SMP=y,
# also boots an SMP guest and requires snapshot capture to fail with the
# documented -EOPNOTSUPP policy.

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

if ! "$BINARY" --showconfig 2>/dev/null | grep -q '^CONFIG_SMP=y$'; then
	echo "KVM_SNAPSHOT_RESTORE: PASS ${LINE#um: kvm-v2 snapshot bench: }; smp_gate=not-built"
	exit 0
fi

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable before SMP gate check" >&2
	exit 4
fi

OUT_SMP=$(timeout --kill-after=10 30 "$BINARY" \
	backend=force=kvm-v2 \
	ncpus=2 \
	kvm_v2_snapshot_bench=1 \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED_SMP=$(echo "$OUT_SMP" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED_SMP" != "kvm-v2" ]; then
	echo "KVM_SNAPSHOT_RESTORE: FAIL (SMP gate observed backend='$OBSERVED_SMP')"
	echo "$OUT_SMP" | grep -E 'backend = |snapshot bench|snapshot:|panic|failed' | head -12
	exit 1
fi

if ! echo "$OUT_SMP" | grep -q 'refusing SMP snapshot'; then
	echo "KVM_SNAPSHOT_RESTORE: FAIL (SMP guest did not hit snapshot gate)"
	echo "$OUT_SMP" | grep -E 'backend = |snapshot bench|snapshot:|panic|failed' | head -20
	exit 1
fi

if ! echo "$OUT_SMP" | grep -q 'boot run failed (-95)'; then
	echo "KVM_SNAPSHOT_RESTORE: FAIL (SMP gate did not propagate -EOPNOTSUPP)"
	echo "$OUT_SMP" | grep -E 'backend = |snapshot bench|snapshot:|panic|failed' | head -20
	exit 1
fi

echo "KVM_SNAPSHOT_RESTORE: PASS ${LINE#um: kvm-v2 snapshot bench: }; smp_gate=-EOPNOTSUPP"
exit 0
