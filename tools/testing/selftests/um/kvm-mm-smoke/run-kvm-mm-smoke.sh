#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/kvm-mm-smoke/run-kvm-mm-smoke.sh - host-side launcher for the
# mm-mutation KVM regression guard.
#
# Boots UML with `backend=force=kvm`, runs the freestanding
# mm-smoke-loop binary as init, scrapes the "MM_SMOKE: PASS"
# line from stdout. The guest binary exercises mmap, mprotect,
# mremap, munmap, brk in sequence and validates each step's
# memory contents post-syscall.
#
# This guards the correctness invariant that mm-mutating syscalls flow
# through UML's mm_map / mm_unmap callbacks, which already invalidate
# the shadow page tables, so kvm_decode_syscall does not need an
# unconditional post-handle_syscall shadow refill. If a future syscall
# mutates mm without going through those callbacks, this test fails by
# either SIGSEGV-on-stale-mapping or silent data corruption on
# readback.
#
# Pattern mirrors um/kvm-bounds/run-kvm-bounds.sh. Exits 0 on
# PASS, 4 on SKIP, 1 on FAIL.
#
# Environment:
#   UML_BINARY      UML kernel (default /tmp/uml-kvmint/linux).
#   UML_MEM         mem= argument. Default 128M.
#   MM_SMOKE_LOOP   path to the mm-smoke-loop binary. Default
#                   built alongside the runner.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-128M}
LOOP=${MM_SMOKE_LOOP:-$DIR/mm-smoke-loop}

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
	init="$LOOP" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm)" >&2
	exit 4
fi

LINE=$(echo "$OUT" | grep -E '^MM_SMOKE: ' | head -1)
if [ -z "$LINE" ]; then
	echo "KVM_MM_SMOKE: FAIL (no 'MM_SMOKE:' line; guest crashed before reporting)"
	echo "$OUT" | grep -E 'MM_SMOKE|backend = |panic|fatal|cr2=' | head -8
	exit 1
fi

case "$LINE" in
*PASS*) echo "KVM_MM_SMOKE: PASS ${LINE#MM_SMOKE: }"; exit 0 ;;
*FAIL*) echo "KVM_MM_SMOKE: FAIL ${LINE#MM_SMOKE: }"; exit 1 ;;
*)      echo "KVM_MM_SMOKE: FAIL (unexpected line: $LINE)"; exit 1 ;;
esac
