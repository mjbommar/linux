#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# kvm-fallback-stats.sh — statistical hygiene wrapper for the
# fallback-lever series (#238 + Phase 2 hardening).
#
# Runs tools/testing/selftests/um/perf-fallback/run-perf-
# fallback.sh N times, extracts cyc_per_call for the
# kvm-fallback row, and emits median + IQR + min/max.
#
# Why a wrapper rather than baking into the runner: the
# kselftest runners stay simple (single-shot, kselftest
# convention). This wrapper is for in-tree development —
# verify that a perf change is real, not noise.
#
# Per A-07 (uml-perf.sh shape): perf-event counters not wall
# time, median + IQR over N runs (N=11 default — odd so the
# median is a single sample), drop the top + bottom outlier.
#
# Output (one line, machine-parseable):
#
#   KVM_FALLBACK_STATS: n=11 median=<cyc> iqr=<cyc> p5=<cyc>
#                       p95=<cyc> samples=<csv>
#
# Optional pinning for reduced variance:
#
#   UML_TASKSET_CPU=<cpu>     pin UML to one core via taskset
#                             (recommended: an isolcpus core,
#                             else any single non-housekeeping
#                             core).
#   UML_FREQ_PIN=performance  if set + cpupower available, pre-
#                             flight `cpupower frequency-set -g
#                             performance` (requires sudo -n).
#                             Restore prior governor on exit.
#
# Requires: an existing pair of UML kernels at
#   UML_BINARY=/tmp/uml-kvmint/linux
#   UML_GADGET_BINARY=/tmp/uml-kvmbench/linux

set -u

SRC=$(cd "$(dirname "$0")/../../../../.." && pwd)
RUNNER=$SRC/tools/testing/selftests/um/perf-fallback/run-perf-fallback.sh
N=${KVM_FALLBACK_STATS_N:-11}
CPU=${UML_TASKSET_CPU:-}
FREQ=${UML_FREQ_PIN:-}
RESTORE_GOV=

if [ ! -x "$RUNNER" ]; then
	echo "SKIP: runner $RUNNER not executable" >&2
	exit 4
fi

cleanup() {
	if [ -n "$RESTORE_GOV" ] && command -v cpupower >/dev/null; then
		sudo -n cpupower frequency-set -g "$RESTORE_GOV" \
			>/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT

if [ -n "$FREQ" ] && command -v cpupower >/dev/null; then
	RESTORE_GOV=$(cpupower frequency-info -p 2>/dev/null |
		      sed -nE 's/.*"([^"]+)".*/\1/p' | head -1)
	sudo -n cpupower frequency-set -g "$FREQ" \
		>/dev/null 2>&1 || true
fi

run_one() {
	local cmd="$RUNNER"
	# taskset wraps the inner UML invocation, but the runner
	# itself doesn't take a taskset arg. Easiest: prefix the
	# whole runner with taskset; UML inherits the CPU mask
	# via fork.
	if [ -n "$CPU" ]; then
		cmd="taskset -c $CPU $RUNNER"
	fi
	UML_BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux} \
	UML_GADGET_BINARY=${UML_GADGET_BINARY:-/tmp/uml-kvmbench/linux} \
		$cmd 2>&1 |
		grep -E '^PERF_FALLBACK: backend=kvm-fallback ' |
		head -1 |
		sed -nE 's/.*cyc_per_call=([0-9]+).*/\1/p'
}

samples=()
for i in $(seq 1 "$N"); do
	v=$(run_one)
	if [ -n "$v" ]; then
		samples+=("$v")
	fi
done

if [ "${#samples[@]}" -lt 5 ]; then
	echo "FAIL: only ${#samples[@]} valid samples (need >=5)" >&2
	exit 1
fi

# Sort numerically, compute median, IQR (Q3-Q1), p5, p95.
# All in pure bash + sort/awk to keep deps minimal.
sorted=$(printf '%s\n' "${samples[@]}" | sort -n)
n=$(echo "$sorted" | wc -l)
median=$(echo "$sorted" | awk -v n="$n" 'NR==int((n+1)/2)')
q1=$(echo "$sorted" | awk -v n="$n" 'NR==int((n+3)/4)')
q3=$(echo "$sorted" | awk -v n="$n" 'NR==int((3*n+1)/4)')
iqr=$((q3 - q1))
p5=$(echo "$sorted" | awk -v n="$n" 'NR==int(n*5/100)+1')
p95=$(echo "$sorted" | awk -v n="$n" 'NR==int(n*95/100)+1')
csv=$(printf '%s\n' "${samples[@]}" | paste -sd, -)

printf 'KVM_FALLBACK_STATS: n=%d median=%d iqr=%d p5=%d p95=%d samples=%s\n' \
	"$n" "$median" "$iqr" "$p5" "$p95" "$csv"
