#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/snapshot-kvm-smoke/run-snapshot-kvm-smoke.sh — host-side launcher
# for the v2 snapshot KUnit smoke (#168 Phase 6).
#
# Boots UML with `backend=force=kvm-v2` and asserts the in-tree
# KUnit cases for the v2 snapshot suite all report PASS:
#
#   - test_kvm_v2_snapshot_basic — regs-only round-trip.
#   - test_kvm_v2_snapshot_full  — full capture + memslot round-trip
#                                  (Phase 3).
#   - test_kvm_v2_snapshot_task  — cross-task semantics: iotrap_fpu +
#                                  iotrap_events round-trip plus
#                                  -EINVAL gate (Phase 4).
#
# Build/boot/KUnit-shape regression guard: a kvm-v2 backend regression
# that breaks any of the snapshot primitives fails here.
#
# Pre-v2 history: this script previously targeted the v1 path
# (backend=force=kvm + kvm_snapshot_basic_test KUnit case name) when
# the snapshot code lived at arch/um/backend/kvm-v1-archive/snapshot.c.
# Phase 6 of the v2 port re-plumbs against the v2 symbol surface.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.
#
# Environment:
#   UML_BINARY  UML kernel built with CONFIG_UM_BACKEND_KVM_V2=y +
#               CONFIG_UM_BACKEND_KVM_V2_KUNIT=y. Default
#               /tmp/uml-kvmint/linux.
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
	backend=force=kvm-v2 \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

# Confirm the kernel actually picked the kvm-v2 backend; SKIP if
# the probe demoted to seccomp at runtime (e.g. /dev/kvm permission
# or nested-KVM unavailable on the host).
OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm-v2" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm-v2)" >&2
	exit 4
fi

# KUnit cases we're matching on: the three Phase 1/3/4 snapshot
# cases must all PASS. KUnit prints the case name verbatim so we
# match by name, not index.
CASES=(
	"test_kvm_v2_snapshot_basic"
	"test_kvm_v2_snapshot_full"
	"test_kvm_v2_snapshot_task"
)
MISSING=()
for case in "${CASES[@]}"; do
	if ! echo "$OUT" | grep -Eq "^[[:space:]]+ok [0-9]+ ${case}\b"; then
		MISSING+=("$case")
	fi
done
if [ "${#MISSING[@]}" -gt 0 ]; then
	echo "SNAPSHOT_KVM_SMOKE: FAIL (KUnit case(s) not PASS: ${MISSING[*]})"
	echo "$OUT" | grep -E 'kvm_v2|backend = ' | head -10
	exit 1
fi

# Capture the post-restore diagnostic the snapshot layer emits so
# the kselftest log shows the bytes-restored counts in addition to
# the bare KUnit ok line.
KU_INFO=$(echo "$OUT" | grep -E 'um: kvm-v2 snapshot: (captured|restored)' | head -3)

echo "SNAPSHOT_KVM_SMOKE: PASS (3/3 snapshot cases — basic, full, task)"
if [ -n "$KU_INFO" ]; then
	echo "SNAPSHOT_KVM_SMOKE: info ${KU_INFO}"
fi
exit 0
