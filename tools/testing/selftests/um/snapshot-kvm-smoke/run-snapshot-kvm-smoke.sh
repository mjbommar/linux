#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/snapshot-kvm-smoke/run-snapshot-kvm-smoke.sh — host-side launcher
# for the workstream Phase 3 task #251 KVM-snapshot smoke test.
#
# Boots UML with `backend=force=kvm` and asserts that the in-tree
# KUnit case kvm_snapshot_basic_test (added in commit b6e5bceab7a2)
# reports "ok 36 kvm_snapshot_basic_test" against the kvm-backend
# build. That case exercises the kvm_snapshot_capture / restore_full
# / destroy primitives shipped in commit 040bdb2f6b04 (memo 12
# steps 1+2). Pattern mirrors um/dyn-loader/run-dyn-loader.sh.
#
# Future scope (memo 12 steps 3+4): once um_snapshot_ready under the
# KVM backend stops returning -EBUSY (today guarded by commit
# 7f79b35e1531), this selftest extends with a per-iteration round-
# trip via the AFL forkserver protocol, mirroring snapshot-smoke's
# Part B driver. For now this is a build/boot/KUnit-shape regression
# guard — a kvm-backend regression that breaks the snapshot API
# surface fails this test before it can reach the AFL path.
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

# Self-heal /dev/kvm ACL — same retry pattern as the perf / kvm-bounds
# / kvm-smoke runners (task #267).
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

# Confirm the kernel actually picked the kvm backend; SKIP if the
# probe demoted to seccomp/ptrace at runtime (e.g. nested-KVM
# unavailable on the host).
OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm)" >&2
	exit 4
fi

# KUnit line we're matching on: "ok 36 kvm_snapshot_basic_test".
# Number may shift if cases are reordered; match the case name not
# the index.
KU_LINE=$(echo "$OUT" | grep -E '^[[:space:]]+ok [0-9]+ kvm_snapshot_basic_test' | head -1)
if [ -z "$KU_LINE" ]; then
	# Either the test didn't run or it didn't pass. FAIL with
	# diagnostics for triage.
	echo "SNAPSHOT_KVM_SMOKE: FAIL (no 'ok N kvm_snapshot_basic_test' KUnit line)"
	echo "$OUT" | grep -E 'kvm_snapshot|backend = ' | head -5
	exit 1
fi

# Capture the diagnostic kunit_info line so logs show the rc the
# test observed (acceptable values: 0 from a successful capture or
# -19 from the lazy-memslot bailout).
KU_INFO=$(echo "$OUT" | grep -E 'kvm_snapshot_capture rc=' | head -1)

echo "SNAPSHOT_KVM_SMOKE: PASS ${KU_LINE# *}"
if [ -n "$KU_INFO" ]; then
	echo "SNAPSHOT_KVM_SMOKE: info ${KU_INFO# *}"
fi
exit 0
