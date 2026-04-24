#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# D-06 getpid() round-trip bookend (memo 08 sub-commit #6).
#
# Boots UML with the freestanding getpid-loop binary as init=
# under each backend available in the kernel, captures the
# PERF_GETPID: line from stdout, and prints a three-row
# comparison. A regression gate runs at the end: the KVM
# backend must land within MAX_KVM_RATIO × seccomp's
# cyc_per_call (default 2.0). Set MAX_KVM_RATIO=0 to skip the
# gate and just report numbers.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.
#
# Environment:
#   UML_BINARY   UML kernel with CONFIG_UM_BACKEND_KVM_
#                INTEGRATED=y AND seccomp + ptrace co-selected
#                (default /tmp/uml-kvmint/linux).
#   UML_MEM      mem= argument. Default 256M.
#   BACKENDS     space-separated backend list to measure.
#                Default "ptrace seccomp kvm". The runner
#                skips any backend the host can't support
#                (e.g. kvm if /dev/kvm isn't readable).
#   MAX_KVM_RATIO  KVM:seccomp cyc_per_call ratio ceiling for
#                  the PASS gate. Default 2.0.
#
# Output: one line per backend with the extracted metrics and
# one trailing "PERF_GETPID: SUMMARY" line. Each backend line
# echoes the guest-emitted record verbatim plus a backend=
# tag so downstream parsers don't have to know which kernel
# was booted.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-256M}
LOOP=${PERF_GETPID_LOOP:-$DIR/getpid-loop}
BACKENDS=${BACKENDS:-ptrace seccomp kvm}
MAX_KVM_RATIO=${MAX_KVM_RATIO:-2.0}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$LOOP" ]; then
	echo "SKIP: $LOOP not built; run 'make' in this dir" >&2
	exit 4
fi

measure_one() {
	local backend=$1
	local log
	# Note: cmdline token is `backend=force=<kind>`, not
	# `force=<kind>`. An earlier version of this runner used
	# `force=<kind>` which silently parses as an unknown
	# kernel cmdline arg and falls back to the default
	# backend — producing a seccomp-vs-seccomp-vs-seccomp
	# table mislabelled as ptrace/seccomp/kvm. Decisions-log
	# D70 retracts that result. The backend-selection line
	# `um: backend = <kind>` is extracted + asserted below
	# so this class of silent fallback can't recur.
	log=$(timeout --kill-after=5 30 "$BINARY" \
		backend="force=$backend" \
		init="$LOOP" mem="$MEM" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw \
		panic=-1 </dev/null 2>&1 || true)
	local observed
	observed=$(echo "$log" | sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
	if [ "$observed" != "$backend" ]; then
		echo "PERF_GETPID: backend=$backend FAIL (observed=$observed; check um: backend line)"
		return
	fi
	echo "$log" | grep -E '^PERF_GETPID: ' | head -1
}

declare -A NS_PER_CALL
declare -A CYC_PER_CALL

for B in $BACKENDS; do
	if [ "$B" = "kvm" ] && [ ! -r /dev/kvm ]; then
		echo "PERF_GETPID: backend=$B SKIP (no /dev/kvm)"
		continue
	fi
	LINE=$(measure_one "$B")
	if [ -z "$LINE" ]; then
		echo "PERF_GETPID: backend=$B FAIL (no PERF_GETPID line emitted)"
		continue
	fi
	echo "PERF_GETPID: backend=$B $LINE" | sed 's/PERF_GETPID: //2'
	NS=$(echo "$LINE" | sed -n 's/.*ns_per_call=\([0-9]*\).*/\1/p')
	CYC=$(echo "$LINE" | sed -n 's/.*cyc_per_call=\([0-9]*\).*/\1/p')
	NS_PER_CALL[$B]=${NS:-0}
	CYC_PER_CALL[$B]=${CYC:-0}
done

# Summary + gate.
SECCOMP_CYC=${CYC_PER_CALL[seccomp]:-0}
KVM_CYC=${CYC_PER_CALL[kvm]:-0}
RATIO="n/a"
if [ "$SECCOMP_CYC" != "0" ] && [ "$KVM_CYC" != "0" ]; then
	RATIO=$(awk -v k="$KVM_CYC" -v s="$SECCOMP_CYC" 'BEGIN { printf "%.3f", k / s }')
fi

echo "PERF_GETPID: SUMMARY backends=[${!CYC_PER_CALL[@]}]" \
	"kvm_cyc=$KVM_CYC seccomp_cyc=$SECCOMP_CYC" \
	"ratio_kvm_over_seccomp=$RATIO max_allowed=$MAX_KVM_RATIO"

# Regression gate.
if [ "$MAX_KVM_RATIO" = "0" ]; then
	echo "PERF_GETPID: PASS (gate disabled via MAX_KVM_RATIO=0)"
	exit 0
fi
if [ "$SECCOMP_CYC" = "0" ] || [ "$KVM_CYC" = "0" ]; then
	# One of the two we need for the ratio was missing; can't
	# evaluate the gate. SKIP rather than fail — the raw
	# measurements are still useful.
	echo "PERF_GETPID: SKIP (missing seccomp or kvm measurement; gate not evaluable)"
	exit 4
fi
if awk -v r="$RATIO" -v m="$MAX_KVM_RATIO" 'BEGIN { exit !(r <= m) }'; then
	echo "PERF_GETPID: PASS"
	exit 0
fi
echo "PERF_GETPID: FAIL ratio=$RATIO exceeds max=$MAX_KVM_RATIO"
exit 1
