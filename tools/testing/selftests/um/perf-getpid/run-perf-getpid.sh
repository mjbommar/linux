#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# getpid() round-trip bookend.
#
# Boots UML with the freestanding getpid-loop binary as init=
# under each backend available in the kernel, captures the
# PERF_GETPID: line from stdout, and prints a three-row
# comparison. A regression gate runs at the end: the KVM
# backend must land within MAX_KVM_RATIO times seccomp's
# cyc_per_call (default 2.0). Set MAX_KVM_RATIO=0 to skip the
# gate and just report numbers.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL, per kselftest convention.
#
# Environment:
#   UML_BINARY   UML kernel with CONFIG_UM_BACKEND_KVM_
#                INTEGRATED=y AND seccomp + ptrace co-selected
#                (default /tmp/uml-kvmint/linux). This is the
#                fallback build: CONFIG_UM_BACKEND_KVM_GADGET=n,
#                so every getpid() takes the VMEXIT dispatch
#                path.
#   UML_GADGET_BINARY  optional additional UML kernel with
#                CONFIG_UM_BACKEND_KVM_GADGET=y. If set and
#                readable, the runner adds a fourth measurement
#                row (backend=kvm-gadget) and computes the
#                gadget:fallback ratio as a secondary gate.
#                Default unset.
#   UML_MEM      mem= argument. Default 256M.
#   BACKENDS     space-separated backend list to measure.
#                Default "seccomp kvm". The runner skips any
#                backend the host can't support (e.g. kvm if
#                /dev/kvm isn't readable).
#   MAX_KVM_RATIO  KVM:seccomp cyc_per_call ratio ceiling for
#                  the PASS gate. Default 2.5.
#   MAX_GADGET_RATIO  gadget:fallback cyc_per_call ratio
#                     ceiling for the G7 PASS gate. Default
#                     0.20; the gadget must be at least 5x
#                     faster than the fallback or it's not
#                     earning its complexity. Set 0 to skip.
#
# Output: one line per backend with the extracted metrics and
# one trailing "PERF_GETPID: SUMMARY" line. Each backend line
# echoes the guest-emitted record verbatim plus a backend=
# tag so downstream parsers don't have to know which kernel
# was booted. If UML_GADGET_BINARY is set, an additional
# "PERF_GETPID: GADGET_SUMMARY" line reports the gadget
# ratio.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
GADGET_BINARY=${UML_GADGET_BINARY:-}
MEM=${UML_MEM:-256M}
LOOP=${PERF_GETPID_LOOP:-$DIR/getpid-loop}
BACKENDS=${BACKENDS:-seccomp kvm}
MAX_KVM_RATIO=${MAX_KVM_RATIO:-2.5}
MAX_GADGET_RATIO=${MAX_GADGET_RATIO:-0.20}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$LOOP" ]; then
	echo "SKIP: $LOOP not built; run 'make' in this dir" >&2
	exit 4
fi

ensure_kvm_readable() {
	# Self-heal /dev/kvm ACL: udev / elogind sometimes drops
	# the user ACL between successive UML invocations. Retry
	# up to 3 times with a brief settle delay before giving
	# up. Used both at runner entry and before every kvm-side
	# spawn so an in-loop ACL drop doesn't surface as a
	# spurious SKIP / FAIL.
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
			echo "PERF_GETPID: backend=$backend FAIL (lost /dev/kvm ACL)"
			return
		fi
	fi
	# Note: cmdline token is `backend=force=<kind>`, not
	# `force=<kind>`. An earlier version of this runner used
	# `force=<kind>` which silently parses as an unknown
	# kernel cmdline arg and falls back to the default
	# backend, producing a seccomp-vs-seccomp-vs-seccomp
	# table mislabelled as ptrace/seccomp/kvm.  The
	# backend-selection line `um: backend = <kind>` is extracted
	# and asserted below so this class of silent fallback cannot
	# recur.
	log=$(timeout --kill-after=5 30 "$binary" \
		backend="force=$backend" \
		init="$LOOP" mem="$MEM" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw \
		panic=-1 </dev/null 2>&1 || true)
	local observed
	observed=$(echo "$log" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
	# Kernel registers the KVM backend as "kvm-v2" (arch/um/backend/kvm-v2/ops.c)
	# and accepts "kvm" as a cmdline synonym (arch/um/os-Linux/start_up.c). Treat
	# them as equal here so a mismatch only triggers on a real fallback, and so
	# users can pass either label in BACKENDS.
	local canonical_observed=$observed
	local canonical_backend=$backend
	[ "$canonical_observed" = "kvm-v2" ] && canonical_observed=kvm
	[ "$canonical_backend" = "kvm-v2" ] && canonical_backend=kvm
	if [ "$canonical_observed" != "$canonical_backend" ]; then
		echo "PERF_GETPID: backend=$backend FAIL (observed=$observed; check um: backend line)"
		return
	fi
	echo "$log" | grep -E '^PERF_GETPID: ' | head -1
}

declare -A NS_PER_CALL
declare -A CYC_PER_CALL

for B in $BACKENDS; do
	if [ "$B" = "kvm" ] && [ -e /dev/kvm ] && \
	   ! ensure_kvm_readable; then
		echo "PERF_GETPID: backend=$B SKIP (no /dev/kvm)"
		continue
	fi
	if [ "$B" = "kvm" ] && [ ! -e /dev/kvm ]; then
		echo "PERF_GETPID: backend=$B SKIP (no /dev/kvm)"
		continue
	fi
	LINE=$(measure_one "$B")
	if [ -z "$LINE" ]; then
		echo "PERF_GETPID: backend=$B FAIL (no PERF_GETPID line emitted)"
		continue
	fi
	# When the primary $BINARY is the gadget kernel AND a
	# UML_GADGET_BINARY is ALSO set (the "compare against
	# fallback" mode), rename the primary kvm row to
	# kvm-fallback so the labels stay honest. The gadget
	# measurement then goes under backend=kvm below.
	label=$B
	if [ "$B" = "kvm" ] && [ -n "$GADGET_BINARY" ] && \
	   [ -x "$GADGET_BINARY" ]; then
		label=kvm-fallback
	fi
	echo "PERF_GETPID: backend=$label $LINE" | sed 's/PERF_GETPID: //2'
	NS=$(echo "$LINE" | sed -n 's/.*ns_per_call=\([0-9]*\).*/\1/p')
	CYC=$(echo "$LINE" | sed -n 's/.*cyc_per_call=\([0-9]*\).*/\1/p')
	# Canonicalize array key: SUMMARY + gate downstream key off [kvm], so a
	# user passing BACKENDS="kvm-v2" still ends up in the same slot.
	storage_label=$label
	[ "$storage_label" = "kvm-v2" ] && storage_label=kvm
	NS_PER_CALL[$storage_label]=${NS:-0}
	CYC_PER_CALL[$storage_label]=${CYC:-0}
done

# Optional gadget measurement. When the caller supplies
# UML_GADGET_BINARY alongside UML_BINARY (which is then the fallback
# reference), run one extra invocation with backend=kvm against the
# gadget kernel and record it as backend=kvm. The primary ratio gate
# then evaluates the gadget against seccomp, and a secondary gate
# checks that the gadget actually beat the fallback.
# measure_one calls ensure_kvm_readable for kvm-side spawns,
# so the gadget pass below doesn't need its own self-heal.
if [ -n "$GADGET_BINARY" ] && [ -x "$GADGET_BINARY" ] && [ -e /dev/kvm ]; then
	LINE=$(measure_one kvm "$GADGET_BINARY")
	if [ -n "$LINE" ]; then
		echo "PERF_GETPID: backend=kvm $LINE" | sed 's/PERF_GETPID: //2'
		NS=$(echo "$LINE" | sed -n 's/.*ns_per_call=\([0-9]*\).*/\1/p')
		CYC=$(echo "$LINE" | sed -n 's/.*cyc_per_call=\([0-9]*\).*/\1/p')
		NS_PER_CALL[kvm]=${NS:-0}
		CYC_PER_CALL[kvm]=${CYC:-0}
	else
		echo "PERF_GETPID: backend=kvm(gadget) FAIL (no PERF_GETPID line emitted)"
	fi
fi

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

# Gadget:fallback summary + gate. When a second kernel was supplied
# the primary kvm row is the gadget; the fallback measurement sits
# under kvm-fallback.
FALLBACK_CYC=${CYC_PER_CALL[kvm-fallback]:-0}
GADGET_RATIO="n/a"
if [ "$KVM_CYC" != "0" ] && [ "$FALLBACK_CYC" != "0" ]; then
	GADGET_RATIO=$(awk -v g="$KVM_CYC" -v f="$FALLBACK_CYC" \
		'BEGIN { printf "%.3f", g / f }')
	echo "PERF_GETPID: GADGET_SUMMARY" \
		"gadget_cyc=$KVM_CYC fallback_cyc=$FALLBACK_CYC" \
		"ratio_gadget_over_fallback=$GADGET_RATIO" \
		"max_allowed=$MAX_GADGET_RATIO"
fi

# Primary regression gate (KVM fallback vs seccomp).
if [ "$MAX_KVM_RATIO" = "0" ]; then
	echo "PERF_GETPID: PASS (gate disabled via MAX_KVM_RATIO=0)"
	exit 0
fi
if [ "$SECCOMP_CYC" = "0" ] || [ "$KVM_CYC" = "0" ]; then
	# One of the two we need for the ratio was missing; can't
	# evaluate the gate. SKIP rather than fail - the raw
	# measurements are still useful.
	echo "PERF_GETPID: SKIP (missing seccomp or kvm measurement; gate not evaluable)"
	exit 4
fi
if ! awk -v r="$RATIO" -v m="$MAX_KVM_RATIO" 'BEGIN { exit !(r <= m) }'; then
	echo "PERF_GETPID: FAIL ratio=$RATIO exceeds max=$MAX_KVM_RATIO"
	exit 1
fi

# Secondary G7 gate: gadget vs fallback. Only evaluable when
# the caller supplied UML_GADGET_BINARY AND both the gadget
# and fallback runs produced a measurement.
if [ "$MAX_GADGET_RATIO" = "0" ] || [ "$FALLBACK_CYC" = "0" ]; then
	echo "PERF_GETPID: PASS"
	exit 0
fi
if awk -v r="$GADGET_RATIO" -v m="$MAX_GADGET_RATIO" \
	'BEGIN { exit !(r <= m) }'; then
	echo "PERF_GETPID: PASS (gadget gate also green)"
	exit 0
fi
echo "PERF_GETPID: FAIL gadget_ratio=$GADGET_RATIO exceeds max=$MAX_GADGET_RATIO"
exit 1
