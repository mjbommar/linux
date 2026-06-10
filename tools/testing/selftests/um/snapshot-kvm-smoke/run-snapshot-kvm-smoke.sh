#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side KUnit smoke for the KVM v2 snapshot suite.
#
# The test boots UML under backend=force=kvm-v2 with the KUnit filter set to
# um_kvm_v2_snapshot, then requires every current snapshot case to report PASS.

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

OUT=$(timeout --kill-after=10 90 "$BINARY" \
	backend=force=kvm-v2 \
	kunit.filter_glob=um_kvm_v2_snapshot \
	kunit_shutdown=halt \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm-v2" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm-v2)" >&2
	echo "$OUT" | grep -E 'backend = |kunit|snapshot|UML: fatal|panic' | head -20
	exit 4
fi

CASES=(
	"test_kvm_v2_snapshot_regs_only"
	"test_kvm_v2_snapshot_full_memslot"
	"test_kvm_v2_snapshot_task_state"
	"test_kvm_v2_snapshot_elf_regs_only"
)

MISSING=()
for case in "${CASES[@]}"; do
	if ! echo "$OUT" | grep -Eq "^[[:space:]]+ok [0-9]+ ${case}\\b"; then
		MISSING+=("$case")
	fi
done

if [ "${#MISSING[@]}" -gt 0 ]; then
	echo "SNAPSHOT_KVM_SMOKE: FAIL (KUnit case(s) not PASS: ${MISSING[*]})"
	echo "$OUT" |
		grep -E 'not ok|ok [0-9]+ test_kvm_v2|um_kvm_v2_snapshot|backend = |snapshot' |
		head -40
	exit 1
fi

if echo "$OUT" | grep -Eq '^[[:space:]]+not ok [0-9]+ test_kvm_v2_snapshot'; then
	echo "SNAPSHOT_KVM_SMOKE: FAIL (snapshot suite reported not ok)"
	echo "$OUT" |
		grep -E 'not ok|ok [0-9]+ test_kvm_v2|um_kvm_v2_snapshot' |
		head -40
	exit 1
fi

echo "SNAPSHOT_KVM_SMOKE: PASS (${#CASES[@]}/${#CASES[@]} snapshot KUnit cases)"
exit 0
