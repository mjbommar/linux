#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side smoke for the experimental KVM v2 record/replay core.
#
# The first half boots the KUnit suite for the in-memory state machine and
# FIFO replay primitive. The second half boots a guest script that drives the
# debugfs control surface and verifies that a real KVM v2 workload records
# syscall entries.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-./linux}
MEM=${UML_MEM:-256M}
GUEST_SCRIPT="$DIR/kvm-record-smoke.sh"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
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

KUNIT_OUT=$(timeout --kill-after=10 90 "$BINARY" \
	backend=force=kvm-v2 \
	kunit.filter_glob=um_kvm_v2_record \
	kunit_shutdown=halt \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED=$(echo "$KUNIT_OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm-v2" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm-v2)" >&2
	echo "$KUNIT_OUT" | grep -E 'backend = |kunit|record|UML: fatal|panic' | head -30
	exit 4
fi

CASES=(
	"test_record_lifecycle"
	"test_record_invalid_transitions"
	"test_record_single_active_owner"
	"test_record_gadget_bypass_page"
	"test_record_observe_syscall"
	"test_record_replay_syscall_fifo"
	"test_record_replay_divergence_preserves_cursor"
	"test_record_time_travel_fifo"
	"test_record_buffer_overflow_is_counted"
	"test_record_reset_releases_snapshot"
)

MISSING=()
for case in "${CASES[@]}"; do
	if ! echo "$KUNIT_OUT" | grep -Eq "^[[:space:]]+ok [0-9]+ ${case}\\b"; then
		MISSING+=("$case")
	fi
done

if [ "${#MISSING[@]}" -gt 0 ]; then
	echo "KVM_RECORD_SMOKE: FAIL (KUnit case(s) not PASS: ${MISSING[*]})"
	echo "$KUNIT_OUT" |
		grep -E 'not ok|ok [0-9]+ test_record|um_kvm_v2_record|backend = |record' |
		head -60
	exit 1
fi

if echo "$KUNIT_OUT" | grep -Eq '^[[:space:]]+not ok [0-9]+ test_record'; then
	echo "KVM_RECORD_SMOKE: FAIL (record suite reported not ok)"
	echo "$KUNIT_OUT" |
		grep -E 'not ok|ok [0-9]+ test_record|um_kvm_v2_record' |
		head -60
	exit 1
fi

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable before live smoke" >&2
	exit 4
fi

LIVE_OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	init="$GUEST_SCRIPT" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

LINE=$(echo "$LIVE_OUT" | grep '^KVM_RECORD_SMOKE:' | head -1)
if [ -z "$LINE" ]; then
	echo "KVM_RECORD_SMOKE: FAIL (no live smoke result line)"
	echo "$LIVE_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_SMOKE|UML: fatal|panic' |
		tail -80
	exit 1
fi

echo "$LINE"
case "$LINE" in
*PASS*)
	echo "KVM_RECORD_SMOKE: PASS (${#CASES[@]}/${#CASES[@]} KUnit cases + live debugfs record)"
	exit 0
	;;
*FAIL*)
	exit 1
	;;
*)
	exit 1
	;;
esac
