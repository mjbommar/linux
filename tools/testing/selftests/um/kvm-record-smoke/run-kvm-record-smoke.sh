#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side smoke for the experimental KVM v2 record/replay core.
#
# The first leg boots the KUnit suite for the in-memory state machine and FIFO
# replay primitive. The second boots a guest script that drives the debugfs
# control surface and verifies that a real KVM v2 workload records syscall
# entries. The final legs boot static helpers that start recording from the
# same task that runs the scalar workload, replay the task-owned log, and
# verify strict replay kills mismatched, raw-time, and unsupported live
# syscalls instead of falling back.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-./linux}
MEM=${UML_MEM:-256M}
GUEST_SCRIPT="$DIR/kvm-record-smoke.sh"
TASK_HELPER=${KVM_RECORD_TASK_HELPER:-$DIR/kvm-record-task}
NEGATIVE_HELPER=${KVM_RECORD_NEGATIVE_HELPER:-$DIR/kvm-record-negative}
MISMATCH_HELPER=${KVM_RECORD_MISMATCH_HELPER:-$DIR/kvm-record-mismatch}
TIME_HELPER=${KVM_RECORD_TIME_HELPER:-$DIR/kvm-record-time}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi
if [ ! -x "$TASK_HELPER" ]; then
	echo "SKIP: $TASK_HELPER not built; run 'make' in this dir" >&2
	exit 4
fi
if [ ! -x "$NEGATIVE_HELPER" ]; then
	echo "SKIP: $NEGATIVE_HELPER not built; run 'make' in this dir" >&2
	exit 4
fi
if [ ! -x "$MISMATCH_HELPER" ]; then
	echo "SKIP: $MISMATCH_HELPER not built; run 'make' in this dir" >&2
	exit 4
fi
if [ ! -x "$TIME_HELPER" ]; then
	echo "SKIP: $TIME_HELPER not built; run 'make' in this dir" >&2
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
	"test_record_strict_syscall_policy"
	"test_record_observe_syscall"
	"test_record_entry_format_contract"
	"test_record_replay_syscall_fifo"
	"test_record_syscall_payload_fifo"
	"test_record_syscall_payload_getcwd_fifo"
	"test_record_syscall_payload_short_buffer"
	"test_record_syscall_payload_arg_mismatch"
	"test_record_syscall_payload_overflow"
	"test_record_replay_divergence_preserves_cursor"
	"test_record_strict_replay_failure_is_counted"
	"test_record_strict_syscall_gate"
	"test_record_strict_time_policy"
	"test_record_strict_external_io_policy"
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
*PASS*) ;;
*FAIL*)
	exit 1
	;;
*)
	exit 1
	;;
esac

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable before task-owned smoke" >&2
	exit 4
fi

TASK_OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	init="$TASK_HELPER" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

TASK_LINE=$(echo "$TASK_OUT" | grep '^KVM_RECORD_TASK:' | head -1)
if [ -z "$TASK_LINE" ]; then
	echo "KVM_RECORD_SMOKE: FAIL (no task-owned smoke result line)"
	echo "$TASK_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_TASK|UML: fatal|panic' |
		tail -80
	exit 1
fi

echo "$TASK_LINE"
case "$TASK_LINE" in
*PASS*) ;;
*FAIL*)
	exit 1
	;;
*)
	exit 1
	;;
esac

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable before mismatch smoke" >&2
	exit 4
fi

MISMATCH_OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	init="$MISMATCH_HELPER" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

MISMATCH_LINE=$(echo "$MISMATCH_OUT" | grep '^KVM_RECORD_MISMATCH:' | head -1)
if [ -z "$MISMATCH_LINE" ]; then
	echo "KVM_RECORD_SMOKE: FAIL (no mismatch smoke result line)"
	echo "$MISMATCH_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_MISMATCH|UML: fatal|panic' |
		tail -80
	exit 1
fi

echo "$MISMATCH_LINE"
case "$MISMATCH_LINE" in
*armed\ syscall=*) ;;
*)
	echo "KVM_RECORD_SMOKE: FAIL (mismatch helper did not arm)"
	echo "$MISMATCH_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_MISMATCH|UML: fatal|panic' |
		tail -80
	exit 1
	;;
esac

MISMATCH_NR=$(echo "$MISMATCH_LINE" |
	sed -n 's/.*armed syscall=\([0-9][0-9]*\).*/\1/p')
if [ -z "$MISMATCH_NR" ] ||
   ! echo "$MISMATCH_OUT" |
	grep -q "kvm-v2 record: strict replay divergence nr=$MISMATCH_NR"; then
	echo "KVM_RECORD_SMOKE: FAIL (mismatch strict divergence missing)"
	echo "$MISMATCH_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_MISMATCH|UML: fatal|panic' |
		tail -80
	exit 1
fi
if echo "$MISMATCH_OUT" | grep -q 'KVM_RECORD_MISMATCH: FAIL'; then
	echo "KVM_RECORD_SMOKE: FAIL (mismatch helper returned from replay divergence)"
	echo "$MISMATCH_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_MISMATCH|UML: fatal|panic' |
		tail -80
	exit 1
fi

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable before raw-time smoke" >&2
	exit 4
fi

TIME_OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	init="$TIME_HELPER" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

TIME_LINE=$(echo "$TIME_OUT" | grep '^KVM_RECORD_TIME:' | head -1)
if [ -z "$TIME_LINE" ]; then
	echo "KVM_RECORD_SMOKE: FAIL (no raw-time smoke result line)"
	echo "$TIME_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_TIME|UML: fatal|panic' |
		tail -80
	exit 1
fi

echo "$TIME_LINE"
case "$TIME_LINE" in
*armed\ syscall=*) ;;
*)
	echo "KVM_RECORD_SMOKE: FAIL (raw-time helper did not arm)"
	echo "$TIME_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_TIME|UML: fatal|panic' |
		tail -80
	exit 1
	;;
esac

TIME_NR=$(echo "$TIME_LINE" |
	sed -n 's/.*armed syscall=\([0-9][0-9]*\).*/\1/p')
if [ -z "$TIME_NR" ] ||
   ! echo "$TIME_OUT" |
	grep -q "kvm-v2 record: strict replay unsupported nr=$TIME_NR"; then
	echo "KVM_RECORD_SMOKE: FAIL (raw-time strict rejection missing)"
	echo "$TIME_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_TIME|UML: fatal|panic' |
		tail -80
	exit 1
fi
if echo "$TIME_OUT" | grep -q 'KVM_RECORD_TIME: FAIL'; then
	echo "KVM_RECORD_SMOKE: FAIL (raw-time helper returned from unsupported syscall)"
	echo "$TIME_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_TIME|UML: fatal|panic' |
		tail -80
	exit 1
fi

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable before negative smoke" >&2
	exit 4
fi

NEGATIVE_OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	init="$NEGATIVE_HELPER" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

NEGATIVE_LINE=$(echo "$NEGATIVE_OUT" | grep '^KVM_RECORD_NEGATIVE:' | head -1)
if [ -z "$NEGATIVE_LINE" ]; then
	echo "KVM_RECORD_SMOKE: FAIL (no negative smoke result line)"
	echo "$NEGATIVE_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_NEGATIVE|UML: fatal|panic' |
		tail -80
	exit 1
fi

echo "$NEGATIVE_LINE"
case "$NEGATIVE_LINE" in
*armed\ syscall=*) ;;
*)
	echo "KVM_RECORD_SMOKE: FAIL (negative helper did not arm)"
	echo "$NEGATIVE_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_NEGATIVE|UML: fatal|panic' |
		tail -80
	exit 1
	;;
esac

NEGATIVE_NR=$(echo "$NEGATIVE_LINE" |
	sed -n 's/.*armed syscall=\([0-9][0-9]*\).*/\1/p')
if [ -z "$NEGATIVE_NR" ] ||
   ! echo "$NEGATIVE_OUT" |
	grep -q "kvm-v2 record: strict replay unsupported nr=$NEGATIVE_NR"; then
	echo "KVM_RECORD_SMOKE: FAIL (negative strict rejection missing)"
	echo "$NEGATIVE_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_NEGATIVE|UML: fatal|panic' |
		tail -80
	exit 1
fi
if echo "$NEGATIVE_OUT" | grep -q 'KVM_RECORD_NEGATIVE: FAIL'; then
	echo "KVM_RECORD_SMOKE: FAIL (negative helper returned from unsupported syscall)"
	echo "$NEGATIVE_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_NEGATIVE|UML: fatal|panic' |
		tail -80
	exit 1
fi

SUMMARY="KVM_RECORD_SMOKE: PASS (KUnit=${#CASES[@]}/${#CASES[@]}"
SUMMARY="$SUMMARY live-debugfs=1 task-owned=1 live-mismatch=1"
SUMMARY="$SUMMARY live-time=1 live-negative=1)"
echo "$SUMMARY"
exit 0
