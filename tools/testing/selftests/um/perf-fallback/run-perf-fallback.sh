#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Fallback-path syscall round-trip bookend (task #241).
#
# Boots UML with the freestanding fallback-loop binary as
# init= under each backend available, captures the
# PERF_FALLBACK: line from stdout, and prints a comparison
# table. Sibling to run-perf-getpid.sh — uses __NR_getsid
# instead of __NR_getpid so the gadget kernel still VMEXITs
# (getsid is Class A passthrough, not Class E gadget).
#
# Used as the regression-and-progress gate for the fallback-
# lever series. Each lever's commit appends a row to
# Documentation/virt/uml/redesign/02-workstreams/D-kvm-
# backend/measurements.md showing the before/after numbers.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.
#
# Environment:
#   UML_BINARY         UML kernel (default /tmp/uml-kvmint/linux).
#   UML_GADGET_BINARY  optional second kernel with
#                      CONFIG_UM_BACKEND_KVM_GADGET=y. When set,
#                      the runner additionally measures the
#                      gadget-kernel fallback path so we can
#                      track both kvm-fallback (gadget=n) and
#                      kvm-gadget-fallback (gadget=y, but
#                      getsid still takes the fallback because
#                      it isn't in Class E).
#   UML_MEM            mem= argument. Default 256M.
#   BACKENDS           backend list. Default "ptrace seccomp kvm".
#   MAX_KVM_RATIO      kvm-fallback:seccomp ratio ceiling for
#                      the PASS gate. Default 4.0 — set high
#                      enough to absorb pre-#238 numbers, will
#                      be tightened to 1.1 (seccomp parity)
#                      after #238 lands.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
GADGET_BINARY=${UML_GADGET_BINARY:-}
MEM=${UML_MEM:-256M}
LOOP=${PERF_FALLBACK_LOOP:-$DIR/fallback-loop}
BACKENDS=${BACKENDS:-ptrace seccomp kvm}
MAX_KVM_RATIO=${MAX_KVM_RATIO:-4.0}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$LOOP" ]; then
	echo "SKIP: $LOOP not built; run 'make' in this dir" >&2
	exit 4
fi

ensure_kvm_readable() {
	# Self-heal /dev/kvm ACL — udev / elogind sometimes drops
	# the user ACL between successive UML invocations. Retry
	# up to 3 times with a brief settle delay before giving
	# up. Used both at runner entry and before every kvm-side
	# spawn so an in-loop ACL drop doesn't surface as a
	# spurious SKIP / FAIL (task #267).
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

measure_one() {
	local backend=$1
	local binary=${2:-$BINARY}
	local log

	if [ "$backend" = "kvm" ] && [ -e /dev/kvm ]; then
		if ! ensure_kvm_readable; then
			printf 'PERF_FALLBACK: backend=%s FAIL (lost /dev/kvm ACL)\n' \
				"$backend"
			return
		fi
	fi
	log=$(timeout --kill-after=5 30 "$binary" \
		backend="force=$backend" \
		init="$LOOP" mem="$MEM" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw \
		panic=-1 </dev/null 2>&1 || true)
	local observed
	observed=$(echo "$log" | sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
	if [ "$observed" != "$backend" ]; then
		printf 'PERF_FALLBACK: backend=%s FAIL (observed=%s; check um: backend line)\n' \
			"$backend" "$observed"
		return
	fi
	echo "$log" | grep -E '^PERF_FALLBACK: ' | head -1
}

declare -A NS_PER_CALL
declare -A CYC_PER_CALL

for B in $BACKENDS; do
	if [ "$B" = "kvm" ] && [ -e /dev/kvm ] && \
	   ! ensure_kvm_readable; then
		echo "PERF_FALLBACK: backend=$B SKIP (no /dev/kvm)"
		continue
	fi
	if [ "$B" = "kvm" ] && [ ! -e /dev/kvm ]; then
		echo "PERF_FALLBACK: backend=$B SKIP (no /dev/kvm)"
		continue
	fi
	LINE=$(measure_one "$B")
	if [ -z "$LINE" ]; then
		echo "PERF_FALLBACK: backend=$B FAIL (no PERF_FALLBACK line emitted)"
		continue
	fi
	# When the primary $BINARY is the no-gadget kernel AND a
	# UML_GADGET_BINARY is ALSO set, rename the primary kvm
	# row to kvm-fallback so the labels stay honest.
	label=$B
	if [ "$B" = "kvm" ] && [ -n "$GADGET_BINARY" ] && \
	   [ -x "$GADGET_BINARY" ]; then
		label=kvm-fallback
	fi
	echo "PERF_FALLBACK: backend=$label $LINE" | sed 's/PERF_FALLBACK: //2'
	NS=$(echo "$LINE" | sed -n 's/.*ns_per_call=\([0-9]*\).*/\1/p')
	CYC=$(echo "$LINE" | sed -n 's/.*cyc_per_call=\([0-9]*\).*/\1/p')
	NS_PER_CALL[$label]=${NS:-0}
	CYC_PER_CALL[$label]=${CYC:-0}
done

# Optional gadget-kernel measurement: even though getsid is
# Class A, running it under the gadget kernel exercises the
# same fallback path with whatever caching the gadget build
# adds (vvar refresh, gadget_state_refresh, etc.). The number
# should be very close to the no-gadget number.
# measure_one calls ensure_kvm_readable for kvm-side spawns,
# so the gadget pass below doesn't need its own self-heal.
if [ -n "$GADGET_BINARY" ] && [ -x "$GADGET_BINARY" ] && [ -e /dev/kvm ]; then
	LINE=$(measure_one kvm "$GADGET_BINARY")
	if [ -n "$LINE" ]; then
		echo "PERF_FALLBACK: backend=kvm-gadget-fallback $LINE" | sed 's/PERF_FALLBACK: //2'
		NS=$(echo "$LINE" | sed -n 's/.*ns_per_call=\([0-9]*\).*/\1/p')
		CYC=$(echo "$LINE" | sed -n 's/.*cyc_per_call=\([0-9]*\).*/\1/p')
		NS_PER_CALL[kvm-gadget-fallback]=${NS:-0}
		CYC_PER_CALL[kvm-gadget-fallback]=${CYC:-0}
	else
		printf 'PERF_FALLBACK: backend=kvm-gadget-fallback FAIL '
		printf '(no PERF_FALLBACK line emitted)\n'
	fi
fi

# Summary + gate.
SECCOMP_CYC=${CYC_PER_CALL[seccomp]:-0}
KVM_CYC=${CYC_PER_CALL[kvm-fallback]:-${CYC_PER_CALL[kvm]:-0}}
RATIO="n/a"
if [ "$SECCOMP_CYC" != "0" ] && [ "$KVM_CYC" != "0" ]; then
	RATIO=$(awk -v k="$KVM_CYC" -v s="$SECCOMP_CYC" 'BEGIN { printf "%.3f", k / s }')
fi

ALL=""
for k in "${!CYC_PER_CALL[@]}"; do
	ALL="$ALL $k"
done

printf 'PERF_FALLBACK: SUMMARY backends=[%s] kvm_cyc=%s seccomp_cyc=%s ' \
	"$(echo "$ALL" | xargs)" "$KVM_CYC" "$SECCOMP_CYC"
printf 'ratio_kvm_over_seccomp=%s max_allowed=%s\n' \
	"$RATIO" "$MAX_KVM_RATIO"

if [ "$SECCOMP_CYC" = "0" ] || [ "$KVM_CYC" = "0" ]; then
	echo "PERF_FALLBACK: SKIP (missing seccomp or kvm-fallback measurement; gate not evaluable)"
	exit 4
fi
if [ "$MAX_KVM_RATIO" = "0" ]; then
	echo "PERF_FALLBACK: PASS (gate disabled via MAX_KVM_RATIO=0)"
	exit 0
fi
GATE=$(awk -v r="$RATIO" -v m="$MAX_KVM_RATIO" 'BEGIN { print (r <= m) ? "pass" : "fail" }')
if [ "$GATE" = "pass" ]; then
	echo "PERF_FALLBACK: PASS"
	exit 0
fi
echo "PERF_FALLBACK: FAIL (ratio $RATIO > $MAX_KVM_RATIO)"
exit 1
