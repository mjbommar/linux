#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# KVM gadget user-pointer bounds-check selftest (audit round-6
# G1, task #240). Boots a freestanding ring-3 binary that calls
# clock_gettime / time / getcpu with a canonical kernel VA
# (0xffff800000001000) and a non-canonical address
# (0x800000000000) for each output pointer, then asserts each
# call returned -EFAULT (errno 14).
#
# A pass requires:
#   - Both gadget kernel (CONFIG_UM_BACKEND_KVM_GADGET=y) and the
#     non-gadget fallback kernel return -EFAULT for every test
#     case. The fallback kernel acts as a baseline showing
#     -EFAULT is the universal expected behaviour, not a
#     gadget-specific quirk; the gadget kernel additionally
#     validates that G1's TASK_SIZE_CAP bounds check correctly
#     routes bad pointers to handle_syscall before the ring-0
#     store happens.
#
# Skips KVM rows if /dev/kvm isn't readable. Exits 0 on PASS, 4
# on SKIP, 1 on FAIL.
#
# Environment:
#   UML_BINARY     UML kernel (default /tmp/uml-kvmint/linux).
#   UML_GADGET_BINARY  optional second kernel with the gadget
#                       enabled. Default unset; runner skips
#                       the kvm-gadget row if not provided.
#   UML_MEM        mem= argument. Default 128M.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
GADGET_BINARY=${UML_GADGET_BINARY:-}
MEM=${UML_MEM:-128M}
LOOP=${KVM_BOUNDS_LOOP:-$DIR/kvm-bounds-loop}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$LOOP" ]; then
	echo "SKIP: $LOOP not built; run 'make' in this dir" >&2
	exit 4
fi
if [ ! -e /dev/kvm ]; then
	echo "SKIP: /dev/kvm not present" >&2
	exit 4
fi

# Self-heal /dev/kvm ACL — udev / elogind sometimes drops the
# user ACL between successive UML invocations. Retry up to 3
# times with a brief settle delay before giving up. Used both
# at runner entry and before every kvm-side spawn so an in-loop
# ACL drop doesn't surface as a spurious "no KVM_BOUNDS line"
# FAIL (the prior pattern, fixed in task #267).
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

run_one() {
	local label=$1 binary=$2
	local log

	if ! ensure_kvm_readable; then
		echo "KVM_BOUNDS: $label FAIL (lost /dev/kvm ACL mid-run)"
		return 1
	fi
	log=$(timeout --kill-after=5 30 "$binary" \
		backend=force=kvm \
		init="$LOOP" mem="$MEM" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw \
		panic=-1 </dev/null 2>&1 || true)
	local observed
	observed=$(echo "$log" | sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
	if [ "$observed" != "kvm" ]; then
		echo "KVM_BOUNDS: $label FAIL (observed=$observed; check um: backend line)"
		return 1
	fi
	local line
	line=$(echo "$log" | grep -E '^KVM_BOUNDS: ' | head -1)
	if [ -z "$line" ]; then
		echo "KVM_BOUNDS: $label FAIL (no KVM_BOUNDS line emitted)"
		return 1
	fi
	echo "KVM_BOUNDS: $label $line" | sed 's/KVM_BOUNDS: //2'
	echo "$line" | grep -q "passed=9/9"
}

FAIL=0
if ! run_one "kvm-fallback" "$BINARY"; then
	FAIL=$((FAIL + 1))
fi

if [ -n "$GADGET_BINARY" ] && [ -x "$GADGET_BINARY" ]; then
	if ! run_one "kvm-gadget" "$GADGET_BINARY"; then
		FAIL=$((FAIL + 1))
	fi
fi

if [ "$FAIL" -gt 0 ]; then
	echo "KVM_BOUNDS: FAIL ($FAIL kernel(s) returned non-EFAULT)"
	exit 1
fi
echo "KVM_BOUNDS: PASS"
exit 0
