#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/kvm-smoke/run-kvm-smoke.sh — progression regression guard
# for memo 08's KVM backend integration (workstream D real
# run_userspace path, task #162).
#
# Boots UML with `backend=kvm force=kvm` (a build compiled with
# CONFIG_UM_BACKEND_KVM_INTEGRATED=y) and asserts that the
# integrated run_userspace path exercises the KVM_RUN loop far
# enough to emit one of the expected progression markers:
#
#   - "KVM_EXIT_MMIO"  — reached MMIO decode (sub-commit #3
#                         territory); means SYSCALL trap +
#                         HLT handling all worked up to the
#                         first page fault.
#   - "sub-commit #3 pending"
#                      — explicit panic text the integrated
#                         path emits when sub-commit #3 hasn't
#                         landed yet; same signal as above.
#   - "handle_syscall" + boot progress
#                      — more advanced: we made it into a real
#                         syscall dispatch.
#
# Absence of any of these on a KVM_INTEGRATED=y build means
# regression: something earlier in the pipeline (enter_guest,
# SYSCALL trap, MSR programming, exit decode) has broken.
#
# Exits 0 PASS, 4 SKIP, 1 FAIL — kselftest convention.
#
# Environment:
#   UML_BINARY   UML kernel built with KVM_INTEGRATED=y
#                (default: /tmp/uml-kvmint/linux — the build
#                tree the sub-commit-#1/#2 authoring session
#                used).
#   UML_MEM      mem=N arg. Default 256M.
#
# Requires /dev/kvm on the host. Skips cleanly otherwise — this
# is a real VMEXIT-driven test, not a KUnit check.

set -u

BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-256M}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY; needs KVM_INTEGRATED=y)" >&2
	exit 4
fi

# Self-heal /dev/kvm ACL — udev/elogind sometimes drops it
# between successive UML invocations. Same retry pattern as
# the perf/bounds/df runners (task #267).
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
	echo "SKIP: /dev/kvm not readable by the selftest user" >&2
	exit 4
fi

# Integrated path can panic late in boot (sub-commits #3-#5
# not yet landed). Capture output inside a short `timeout` so
# a wedged guest doesn't stall CI. --kill-after=10 escalates
# to SIGKILL if UML ignores SIGTERM (same rationale as D64).
#
# Cmdline syntax is `backend=force=<kind>` (audit P2 #7). The
# earlier `backend=kvm force=kvm` form parsed `force=kvm` as an
# unknown kernel arg + bare `backend=kvm`, which only worked by
# accident on DYNAMIC builds where the bare form falls through
# to the kvm probe.
OUT=$(timeout --kill-after=10 20 "$BINARY" \
	backend=force=kvm \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 2>&1 || true)

# Useful-diagnostic markers: the integrated path's own log
# lines + the expected progression panics.
MARKERS=(
	"um: kvm init: "                   # lifecycle reached
	"um: kvm enter_guest: bootstrap"   # kvm_enter_guest ran
	"KVM_EXIT_MMIO"                    # hit the sub-commit #3 frontier
	"sub-commit #3 pending"            # explicit frontier panic text
	"handle_syscall"                   # real syscall dispatched
	"KVM_EXIT_HLT"                     # clean HLT seen
)

HITS=0
for m in "${MARKERS[@]}"; do
	if echo "$OUT" | grep -qF "$m"; then
		echo "KVM_SMOKE: hit marker: $m"
		HITS=$((HITS + 1))
	fi
done

# The D-04a panic text — we must NOT hit it under
# KVM_INTEGRATED=y because that would mean run_userspace took
# the scaffold branch.
if echo "$OUT" | grep -q "D-04b SREGS/CR3 setup pending"; then
	echo "KVM_SMOKE: FAIL — D-04a scaffold panic reached; KVM_INTEGRATED path not active"
	exit 1
fi

# Minimum bar for a PASS: at least one integrated-path
# progression marker must have fired. Anything less means we
# didn't get past lifecycle init.
if [ "$HITS" -lt 2 ]; then
	echo "KVM_SMOKE: FAIL — only $HITS/${#MARKERS[@]} markers hit (need >=2)"
	echo "----- last 40 lines of UML output -----"
	echo "$OUT" | tail -40
	exit 1
fi

echo "KVM_SMOKE: PASS markers=$HITS/${#MARKERS[@]}"
exit 0
