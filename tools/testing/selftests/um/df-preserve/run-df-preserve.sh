#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Direction-flag preservation selftest (audit round-4 F3 follow-on,
# task #222). Boots a freestanding ring-3 binary that sets DF=1,
# invokes a class-A SYSCALL (forces the F2 helper round-trip)
# and a recoverable #PF, then
# checks DF survived both round-trips. F2 (decisions-log D75) is
# the fix this validates; F2 KUnit covers the pure-data helper,
# this selftest covers the live path.
#
# Default: tests under each of {ptrace, seccomp, kvm}; KVM is the
# only backend whose RFLAGS round-trip went through a custom
# helper (the others use ptrace/seccomp signal-frame restore and
# inherit DF correctness from the host kernel). The non-KVM rows
# act as a baseline showing DF=PASS is the universal expected
# behaviour, not a KVM-specific quirk.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.
#
# Environment:
#   UML_BINARY     UML kernel (default /tmp/uml-kvmint/linux).
#   UML_GADGET_BINARY  optional second kernel with
#                  CONFIG_UM_BACKEND_KVM_GADGET=y. When set, an
#                  additional kvm-gadget row uses this binary.
#   UML_MEM        mem= argument. Default 128M.
#   BACKENDS       backend list. Default "ptrace seccomp kvm".

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
GADGET_BINARY=${UML_GADGET_BINARY:-}
MEM=${UML_MEM:-128M}
LOOP=${DF_PRESERVE_LOOP:-$DIR/df-preserve-loop}
BACKENDS=${BACKENDS:-ptrace seccomp kvm}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$LOOP" ]; then
	echo "SKIP: $LOOP not built; run 'make' in this dir" >&2
	exit 4
fi

ensure_kvm_readable() {
	# Self-heal /dev/kvm ACL — udev / elogind sometimes drops
	# the user ACL between successive UML invocations. Retry
	# up to 3 times with a brief settle delay (task #267).
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

run_one() {
	local backend=$1 binary=${2:-$BINARY}
	local log

	if [ "$backend" = "kvm" ] && [ -e /dev/kvm ]; then
		if ! ensure_kvm_readable; then
			echo "FAIL (lost /dev/kvm ACL)"
			return
		fi
	fi
	log=$(timeout --kill-after=5 30 "$binary" \
		backend="force=$backend" \
		init="$LOOP" mem="$MEM" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw \
		panic=-1 </dev/null 2>&1 || true)
	local observed
	observed=$(echo "$log" | sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
	if [ "$observed" != "$backend" ]; then
		echo "FAIL (observed=$observed)"
		return
	fi
	echo "$log" | grep -E '^DF_PRESERVE: ' | head -1
}

FAIL=0
for B in $BACKENDS; do
	if [ "$B" = "kvm" ] && [ -e /dev/kvm ] && \
	   ! ensure_kvm_readable; then
		echo "DF_PRESERVE: backend=$B SKIP (no /dev/kvm)"
		continue
	fi
	if [ "$B" = "kvm" ] && [ ! -e /dev/kvm ]; then
		echo "DF_PRESERVE: backend=$B SKIP (no /dev/kvm)"
		continue
	fi
	LINE=$(run_one "$B")
	echo "DF_PRESERVE: backend=$B $LINE"
	if echo "$LINE" | grep -q "syscall=FAIL"; then FAIL=$((FAIL+1)); fi
	if echo "$LINE" | grep -q "pf=FAIL"; then FAIL=$((FAIL+1)); fi
done

if [ -n "$GADGET_BINARY" ] && [ -x "$GADGET_BINARY" ] && [ -e /dev/kvm ]; then
	if ensure_kvm_readable; then
		LINE=$(run_one kvm "$GADGET_BINARY")
		echo "DF_PRESERVE: backend=kvm-gadget $LINE"
		if echo "$LINE" | grep -q "syscall=FAIL"; then FAIL=$((FAIL+1)); fi
		if echo "$LINE" | grep -q "pf=FAIL"; then FAIL=$((FAIL+1)); fi
	fi
fi

if [ "$FAIL" -gt 0 ]; then
	echo "DF_PRESERVE: FAIL ($FAIL backend(s) lost DF across round-trip)"
	exit 1
fi
echo "DF_PRESERVE: PASS"
exit 0
