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
# verify strict replay installs the replay signal policy, replays raw-time
# payloads, and kills mismatched, RDTSC/RDTSCP, randomness, and external-I/O
# live events instead of falling back.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-./linux}
MEM=${UML_MEM:-256M}
GUEST_SCRIPT="$DIR/kvm-record-smoke.sh"
TASK_HELPER=${KVM_RECORD_TASK_HELPER:-$DIR/kvm-record-task}
NEGATIVE_HELPER=${KVM_RECORD_NEGATIVE_HELPER:-$DIR/kvm-record-negative}
NEGATIVE_OPENAT_HELPER=${KVM_RECORD_NEGATIVE_OPENAT_HELPER:-$DIR/kvm-record-negative-openat}
MISMATCH_HELPER=${KVM_RECORD_MISMATCH_HELPER:-$DIR/kvm-record-mismatch}
TIME_HELPER=${KVM_RECORD_TIME_HELPER:-$DIR/kvm-record-time}
RDTSC_HELPER=${KVM_RECORD_RDTSC_HELPER:-$DIR/kvm-record-rdtsc}
RDTSCP_HELPER=${KVM_RECORD_RDTSCP_HELPER:-$DIR/kvm-record-rdtscp}
SIGNAL_HELPER=${KVM_RECORD_SIGNAL_HELPER:-$DIR/kvm-record-signal}

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
if [ ! -x "$NEGATIVE_OPENAT_HELPER" ]; then
	echo "SKIP: $NEGATIVE_OPENAT_HELPER not built; run 'make' in this dir" >&2
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
if [ ! -x "$RDTSC_HELPER" ]; then
	echo "SKIP: $RDTSC_HELPER not built; run 'make' in this dir" >&2
	exit 4
fi
if [ ! -x "$RDTSCP_HELPER" ]; then
	echo "SKIP: $RDTSCP_HELPER not built; run 'make' in this dir" >&2
	exit 4
fi
if [ ! -x "$SIGNAL_HELPER" ]; then
	echo "SKIP: $SIGNAL_HELPER not built; run 'make' in this dir" >&2
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
	"test_record_syscall_payload_clock_gettime_fifo"
	"test_record_syscall_payload_gettimeofday_fifo"
	"test_record_syscall_payload_time_fifo"
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

SIGNAL_SUMMARY=live-signal=1
if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable before signal-policy smoke" >&2
	exit 4
fi

SIGNAL_OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	init="$SIGNAL_HELPER" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

SIGNAL_LINE=$(echo "$SIGNAL_OUT" |
	grep -E '^KVM_RECORD_SIGNAL: (PASS|FAIL|SKIP)' | tail -1)
if [ -z "$SIGNAL_LINE" ]; then
	echo "KVM_RECORD_SMOKE: FAIL (no signal-policy smoke result line)"
	echo "$SIGNAL_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_SIGNAL|UML: fatal|panic' |
		tail -80
	exit 1
fi

echo "$SIGNAL_LINE"
case "$SIGNAL_LINE" in
*SKIP\ trace_event=*)
	SIGNAL_SUMMARY=live-signal=skip
	;;
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
*PASS*) ;;
*)
	echo "KVM_RECORD_SMOKE: FAIL (raw-time helper did not pass)"
	echo "$TIME_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_TIME|UML: fatal|panic' |
		tail -80
	exit 1
	;;
esac
if echo "$TIME_OUT" | grep -q 'KVM_RECORD_TIME: FAIL'; then
	echo "KVM_RECORD_SMOKE: FAIL (raw-time helper reported failure)"
	echo "$TIME_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_TIME|UML: fatal|panic' |
		tail -80
	exit 1
fi

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable before RDTSC smoke" >&2
	exit 4
fi

RDTSC_OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	init="$RDTSC_HELPER" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

RDTSC_LINE=$(echo "$RDTSC_OUT" | grep '^KVM_RECORD_RDTSC:' | head -1)
if [ -z "$RDTSC_LINE" ]; then
	echo "KVM_RECORD_SMOKE: FAIL (no RDTSC smoke result line)"
	echo "$RDTSC_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_RDTSC|UML: fatal|panic' |
		tail -80
	exit 1
fi

echo "$RDTSC_LINE"
case "$RDTSC_LINE" in
*armed\ instruction=rdtsc*) ;;
*)
	echo "KVM_RECORD_SMOKE: FAIL (RDTSC helper did not arm)"
	echo "$RDTSC_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_RDTSC|UML: fatal|panic' |
		tail -80
	exit 1
	;;
esac

if echo "$RDTSC_OUT" | grep -q 'KVM_RECORD_RDTSC: FAIL'; then
	echo "KVM_RECORD_SMOKE: FAIL (RDTSC returned under replay)"
	echo "$RDTSC_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_RDTSC|UML: fatal|panic' |
		tail -80
	exit 1
fi
if ! echo "$RDTSC_OUT" |
	grep -q "Kernel panic - not syncing: Attempted to kill init"; then
	echo "KVM_RECORD_SMOKE: FAIL (RDTSC replay fault did not kill init)"
	echo "$RDTSC_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_RDTSC|UML: fatal|panic' |
		tail -80
	exit 1
fi

RDTSCP_SUMMARY=live-rdtscp=1
if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable before RDTSCP smoke" >&2
	exit 4
fi

RDTSCP_OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	init="$RDTSCP_HELPER" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

RDTSCP_LINE=$(echo "$RDTSCP_OUT" | grep '^KVM_RECORD_RDTSCP:' | head -1)
if [ -z "$RDTSCP_LINE" ]; then
	echo "KVM_RECORD_SMOKE: FAIL (no RDTSCP smoke result line)"
	echo "$RDTSCP_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_RDTSCP|UML: fatal|panic' |
		tail -80
	exit 1
fi

echo "$RDTSCP_LINE"
case "$RDTSCP_LINE" in
*SKIP\ instruction=rdtscp\ unsupported*)
	RDTSCP_SUMMARY=live-rdtscp=skip
	;;
*armed\ instruction=rdtscp*)
	if echo "$RDTSCP_OUT" | grep -q 'KVM_RECORD_RDTSCP: FAIL'; then
		echo "KVM_RECORD_SMOKE: FAIL (RDTSCP returned under replay)"
		echo "$RDTSCP_OUT" |
			grep -E 'backend = |kvm-v2 record|KVM_RECORD_RDTSCP|UML: fatal|panic' |
			tail -80
		exit 1
	fi
	if ! echo "$RDTSCP_OUT" |
		grep -q "Kernel panic - not syncing: Attempted to kill init"; then
		echo "KVM_RECORD_SMOKE: FAIL (RDTSCP replay fault did not kill init)"
		echo "$RDTSCP_OUT" |
			grep -E 'backend = |kvm-v2 record|KVM_RECORD_RDTSCP|UML: fatal|panic' |
			tail -80
		exit 1
	fi
	;;
*)
	echo "KVM_RECORD_SMOKE: FAIL (RDTSCP helper did not arm or skip)"
	echo "$RDTSCP_OUT" |
		grep -E 'backend = |kvm-v2 record|KVM_RECORD_RDTSCP|UML: fatal|panic' |
		tail -80
	exit 1
	;;
esac

run_negative_helper() {
	local helper=$1
	local tag=$2
	local out line nr

	if ! ensure_kvm_readable; then
		echo "SKIP: /dev/kvm not readable before $tag smoke" >&2
		exit 4
	fi

	out=$(timeout --kill-after=10 45 "$BINARY" \
		backend=force=kvm-v2 \
		init="$helper" mem="$MEM" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw \
		panic=-1 </dev/null 2>&1 || true)

	line=$(echo "$out" | grep '^KVM_RECORD_NEGATIVE:' | head -1)
	if [ -z "$line" ]; then
		echo "KVM_RECORD_SMOKE: FAIL (no $tag result line)"
		echo "$out" |
			grep -E 'backend = |kvm-v2 record|KVM_RECORD_NEGATIVE|UML: fatal|panic' |
			tail -80
		exit 1
	fi

	echo "$line"
	case "$line" in
	*armed\ syscall=*) ;;
	*)
		echo "KVM_RECORD_SMOKE: FAIL ($tag helper did not arm)"
		echo "$out" |
			grep -E 'backend = |kvm-v2 record|KVM_RECORD_NEGATIVE|UML: fatal|panic' |
			tail -80
		exit 1
		;;
	esac

	nr=$(echo "$line" |
		sed -n 's/.*armed syscall=\([0-9][0-9]*\).*/\1/p')
	if [ -z "$nr" ] ||
	   ! echo "$out" |
		grep -q "kvm-v2 record: strict replay unsupported nr=$nr"; then
		echo "KVM_RECORD_SMOKE: FAIL ($tag strict rejection missing)"
		echo "$out" |
			grep -E 'backend = |kvm-v2 record|KVM_RECORD_NEGATIVE|UML: fatal|panic' |
			tail -80
		exit 1
	fi
	if echo "$out" | grep -q 'KVM_RECORD_NEGATIVE: FAIL'; then
		echo "KVM_RECORD_SMOKE: FAIL ($tag helper returned from unsupported syscall)"
		echo "$out" |
			grep -E 'backend = |kvm-v2 record|KVM_RECORD_NEGATIVE|UML: fatal|panic' |
			tail -80
		exit 1
	fi
}

run_negative_helper "$NEGATIVE_HELPER" "negative"
run_negative_helper "$NEGATIVE_OPENAT_HELPER" "external-I/O negative"

SUMMARY="KVM_RECORD_SMOKE: PASS (KUnit=${#CASES[@]}/${#CASES[@]}"
SUMMARY="$SUMMARY live-debugfs=1 task-owned=1 live-mismatch=1"
SUMMARY="$SUMMARY $SIGNAL_SUMMARY live-time=1 live-rdtsc=1"
SUMMARY="$SUMMARY $RDTSCP_SUMMARY live-negative=1 live-external-io=1)"
echo "$SUMMARY"
exit 0
