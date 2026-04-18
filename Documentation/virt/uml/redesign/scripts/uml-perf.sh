#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# uml-perf.sh — UML backend perf benchmark runner (A-05.S3 + A-07).
#
# Measures host CPU cycles, instructions, and task-clock per UML
# `init=/bin/true` boot using `perf stat`. Cycles is the right
# metric for cross-host-stable comparison; task-clock catches
# wall-time changes; their ratio (CPI) is informative.
#
# Per A-07 Q1: perf-event counters not wall time.
# Per A-07 Q2: median + IQR over N runs (N=10 default), not point
# estimate. Total runtime ~5 min for 4 backends × 10 iters × ~3 s.
#
# Output: machine-readable JSON line per backend (consumed by
# uml-perf-capture.sh and uml-perf-compare.sh).
#
# Requires: perf with hw counter access. Run
#   sudo sysctl kernel.perf_event_paranoid=1
# first if needed.

set -u

SRC=$(cd "$(dirname "$0")/../../../../.." && pwd)
PTRACE_BUILD=/tmp/uml-matrix-ptrace_only
SECCOMP_BUILD=/tmp/uml-matrix-seccomp_only
DYNAMIC_BUILD=/tmp/uml-matrix-dynamic
ITERS=${UML_PERF_ITERS:-10}
EVENTS=cycles,instructions,task-clock

if [ ! -x "$PTRACE_BUILD/linux" ] || [ ! -x "$SECCOMP_BUILD/linux" ]; then
	echo "Need both $PTRACE_BUILD/linux and $SECCOMP_BUILD/linux."
	echo "Run: $(dirname "$0")/uml-boot-matrix.sh first."
	exit 1
fi

if ! perf stat -e cycles /bin/true 2>&1 | grep -q cycles; then
	echo "perf can't read hw counters."
	echo "Run: sudo sysctl kernel.perf_event_paranoid=1"
	exit 1
fi

# Compute median, p25, p75 of integers from stdin.
percentiles() {
	mapfile -t values
	local n=${#values[@]}
	[ "$n" -eq 0 ] && { echo "0 0 0"; return; }
	local sorted
	sorted=($(printf '%s\n' "${values[@]}" | sort -n))
	local p25=$(( n / 4 ))
	local p50=$(( n / 2 ))
	local p75=$(( (3 * n) / 4 ))
	echo "${sorted[$p50]} ${sorted[$p25]} ${sorted[$p75]}"
}

# Run one boot under perf stat; emit "<cycles> <instructions> <us>".
one_iter() {
	local binary=$1; shift
	local stat_out
	stat_out=$(perf stat -x',' -e "$EVENTS" -- \
		timeout 30 "$binary" \
		init=/bin/true mem=64M con=null \
		con0=fd:0,fd:1 root=/dev/root \
		rootfstype=hostfs "$@" 2>&1 1>/dev/null)
	local cycles instructions task_clock
	cycles=$(echo "$stat_out" | awk -F, '$3=="cycles" {print $1; exit}')
	instructions=$(echo "$stat_out" | awk -F, '$3=="instructions" {print $1; exit}')
	# task-clock is in ms; convert to integer µs
	task_clock=$(echo "$stat_out" | awk -F, '$3=="task-clock" {printf "%d\n", $1 * 1000; exit}')
	echo "${cycles:-0} ${instructions:-0} ${task_clock:-0}"
}

run_backend() {
	local label=$1 binary=$2; shift 2
	local cycles_list=() instr_list=() clock_list=()
	local i

	for i in $(seq 1 "$ITERS"); do
		read -r c ins tc < <(one_iter "$binary" "$@")
		cycles_list+=("$c")
		instr_list+=("$ins")
		clock_list+=("$tc")
	done

	local cycles_p50 cycles_p25 cycles_p75
	read -r cycles_p50 cycles_p25 cycles_p75 < <(printf '%s\n' "${cycles_list[@]}" | percentiles)
	local instr_p50 instr_p25 instr_p75
	read -r instr_p50 instr_p25 instr_p75 < <(printf '%s\n' "${instr_list[@]}" | percentiles)
	local clock_p50 clock_p25 clock_p75
	read -r clock_p50 clock_p25 clock_p75 < <(printf '%s\n' "${clock_list[@]}" | percentiles)

	printf '{"backend":"%s","iters":%d,"cycles":{"p25":%s,"p50":%s,"p75":%s},"instructions":{"p25":%s,"p50":%s,"p75":%s},"task_clock_us":{"p25":%s,"p50":%s,"p75":%s}}\n' \
		"$label" "$ITERS" \
		"$cycles_p25" "$cycles_p50" "$cycles_p75" \
		"$instr_p25" "$instr_p50" "$instr_p75" \
		"$clock_p25" "$clock_p50" "$clock_p75"
}

run_backend PTRACE_ONLY  "$PTRACE_BUILD/linux"
run_backend SECCOMP_ONLY "$SECCOMP_BUILD/linux" seccomp=on
if [ -x "$DYNAMIC_BUILD/linux" ]; then
	run_backend DYN_ptrace  "$DYNAMIC_BUILD/linux"
	run_backend DYN_seccomp "$DYNAMIC_BUILD/linux" seccomp=on
fi
