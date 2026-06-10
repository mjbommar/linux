#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/perf-pidfam/run-perf-pidfam.sh - mixed pid-family gadget
# perf measurement.
#
# Boots UML with the freestanding pidfam-loop binary as init=
# under each backend (ptrace / seccomp / kvm), captures the
# PERF_PIDFAM: line, computes the kvm/seccomp ratio. Smaller
# scope than perf-getpid: just three rows, one ratio gate.
#
# Useful as the gadget-coverage companion to perf-getpid: the
# single-NR perf-getpid only exercises the head of the LSTAR
# dispatch table, while pidfam round-robins through all seven
# pid-family handlers (getpid / gettid / getppid / getuid /
# geteuid / getgid / getegid). Per-call cost reflects the
# average over the dispatch table, so a regression that hits
# only the second-or-later handler still surfaces here.
#
# Pattern mirrors um/perf-getpid; exits 0 PASS, 4 SKIP, 1 FAIL.
#
# Environment:
#   UML_BINARY         (default /tmp/uml-kvmint/linux)
#   UML_MEM            (default 256M)
#   BACKENDS           "ptrace seccomp kvm" (default)
#   MAX_KVM_RATIO      kvm/seccomp cyc_per_call ceiling for
#                      PASS gate. Default 2.5. Set 0 to skip.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-256M}
BACKENDS=${BACKENDS:-ptrace seccomp kvm}
MAX_KVM_RATIO=${MAX_KVM_RATIO:-2.5}
LOOP=${PIDFAM_LOOP:-$DIR/pidfam-loop}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$LOOP" ]; then
	echo "SKIP: $LOOP not built; run 'make' in this dir" >&2
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

run_one() {
	local backend=$1
	local log

	if [ "$backend" = "kvm" ]; then
		if [ ! -e /dev/kvm ]; then
			echo "PERF_PIDFAM: backend=$backend SKIP (no /dev/kvm)"
			return
		fi
		if ! ensure_kvm_readable; then
			echo "PERF_PIDFAM: backend=$backend SKIP (no /dev/kvm ACL)"
			return
		fi
	fi

	log=$(timeout --kill-after=10 60 "$BINARY" \
		backend="force=$backend" \
		init="$LOOP" mem="$MEM" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw \
		panic=-1 </dev/null 2>&1 || true)

	local observed
	observed=$(echo "$log" | sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
	if [ "$observed" != "$backend" ]; then
		echo "PERF_PIDFAM: backend=$backend FAIL (observed=$observed)"
		return
	fi

	local line
	line=$(echo "$log" | grep -E '^PERF_PIDFAM: ' | head -1)
	if [ -z "$line" ]; then
		echo "PERF_PIDFAM: backend=$backend FAIL (no PERF_PIDFAM line)"
		return
	fi
	echo "${line/backend=??/backend=$backend}"
}

declare -A CYC_PER
for B in $BACKENDS; do
	out=$(run_one "$B")
	echo "$out"
	if echo "$out" | grep -q "FAIL\|SKIP"; then
		continue
	fi
	cyc=$(echo "$out" | sed -E 's/.*cyc_per_call=([0-9]+).*/\1/')
	CYC_PER[$B]=$cyc
done

# Summary + ratio gate.
kvm_cyc=${CYC_PER[kvm]:-0}
seccomp_cyc=${CYC_PER[seccomp]:-0}
if [ "$kvm_cyc" -eq 0 ] || [ "$seccomp_cyc" -eq 0 ]; then
	echo "PERF_PIDFAM: SUMMARY incomplete (kvm=$kvm_cyc seccomp=$seccomp_cyc)"
	exit 4
fi

# Use awk for the float ratio so the script is bash-portable.
ratio=$(awk -v k="$kvm_cyc" -v s="$seccomp_cyc" 'BEGIN{printf "%.3f", k/s}')
echo "PERF_PIDFAM: SUMMARY backends=[${!CYC_PER[*]}] kvm_cyc=$kvm_cyc seccomp_cyc=$seccomp_cyc ratio_kvm_over_seccomp=$ratio max_allowed=$MAX_KVM_RATIO"

if [ "$MAX_KVM_RATIO" = "0" ] || [ "$MAX_KVM_RATIO" = "0.0" ]; then
	echo "PERF_PIDFAM: PASS (gate disabled)"
	exit 0
fi

# awk-based comparison: ratio > MAX_KVM_RATIO => FAIL
fail=$(awk -v r="$ratio" -v m="$MAX_KVM_RATIO" 'BEGIN{print (r > m) ? "1" : "0"}')
if [ "$fail" = "1" ]; then
	echo "PERF_PIDFAM: FAIL (ratio $ratio > $MAX_KVM_RATIO)"
	exit 1
fi
echo "PERF_PIDFAM: PASS"
exit 0
