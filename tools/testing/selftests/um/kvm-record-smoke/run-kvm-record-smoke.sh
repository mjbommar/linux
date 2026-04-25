#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/kvm-record-smoke/run-kvm-record-smoke.sh — kselftest for the
# Phase-3 task #253 record/replay primitive skeleton.
#
# Boots UML with `backend=force=kvm` and asserts the in-tree KUnit
# case kvm_record_basic_test (added in commit 56274adfe16b) reports
# "ok N kvm_record_basic_test". That case exercises the kvm_record_
# alloc / start / stop / replay / destroy lifecycle on the freshly-
# booted vCPU.
#
# Pattern mirrors um/snapshot-kvm-smoke/run-snapshot-kvm-smoke.sh:
# build/boot/KUnit-shape regression guard. When subsequent memo-13
# steps land (TSC / syscall / interrupt / random / MMIO recording),
# this selftest extends with a record→KVM_RUN→replay round-trip
# verifying the recorded entries replay byte-identically.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.
#
# Environment:
#   UML_BINARY  UML kernel built with CONFIG_UM_BACKEND_KVM_INTEGRATED=y
#               and CONFIG_UM_BACKEND_CONTRACT_TEST=y (default
#               /tmp/uml-kvmint/linux).
#   UML_MEM     mem= argument. Default 128M.

set -u

BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-128M}

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
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm)" >&2
	exit 4
fi

KU_LINE=$(echo "$OUT" | grep -E '^[[:space:]]+ok [0-9]+ kvm_record_basic_test' | head -1)
if [ -z "$KU_LINE" ]; then
	echo "KVM_RECORD_SMOKE: FAIL (no 'ok N kvm_record_basic_test' KUnit line)"
	echo "$OUT" | grep -E 'kvm_record|backend = ' | head -5
	exit 1
fi

KU_INFO=$(echo "$OUT" | grep -E 'kvm_record_start rc=' | head -1)

echo "KVM_RECORD_SMOKE: PASS ${KU_LINE# *}"
if [ -n "$KU_INFO" ]; then
	echo "KVM_RECORD_SMOKE: info ${KU_INFO# *}"
fi
exit 0
