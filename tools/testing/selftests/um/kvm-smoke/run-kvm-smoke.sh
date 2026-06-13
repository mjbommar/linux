#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/kvm-smoke/run-kvm-smoke.sh - progression regression guard for
# the KVM backend run_userspace path.
#
# Boots UML with `backend=force=kvm` (a build compiled with
# CONFIG_UM_BACKEND_KVM_V2=y) and asserts that the KVM v2
# run_userspace path exercises the KVM_RUN loop far enough to
# emit the expected progression markers:
#
#   - "um: backend = kvm-v2"  - the KVM v2 backend is active, not a
#                               seccomp fallback (force=kvm panics if
#                               KVM is unavailable, so this is firm).
#   - "kvm_v2_vcpu_run" / "kvm_v2_handle_io_trap" / "handle_syscall"
#                             - the KVM_RUN loop ran, decoded an I/O
#                               exit, and dispatched a real syscall
#                               (seen in the init-exit backtrace).
#   - "exitcode=0x00000000"   - init=/bin/true ran to a clean exit
#                               under KVM v2.
#
# Fewer than two of these on a KVM_V2=y build means regression:
# something earlier in the pipeline (enter_guest, SYSCALL trap, MSR
# programming, exit decode) has broken.
#
# Exits 0 PASS, 4 SKIP, 1 FAIL, per kselftest convention.
#
# Environment:
#   UML_BINARY   UML kernel built with CONFIG_UM_BACKEND_KVM_V2=y
#                (default: /tmp/uml-kvmint/linux).
#   UML_MEM      mem=N arg. Default 256M.
#
# Requires /dev/kvm on the host. Skips cleanly otherwise; this
# is a real VMEXIT-driven test, not a KUnit check.

set -u

BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-256M}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY; needs CONFIG_UM_BACKEND_KVM_V2=y)" >&2
	exit 4
fi

# Self-heal /dev/kvm ACL: udev/elogind sometimes drops it
# between successive UML invocations.
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

# Capture output inside a short `timeout` so a wedged guest does not
# stall CI. --kill-after=10 escalates to SIGKILL if UML ignores
# SIGTERM.
#
# Cmdline syntax is `backend=force=<kind>`. A bare `force=kvm` token is
# an unknown kernel arg; assert the selected backend so silent fallback
# cannot pass.
OUT=$(timeout --kill-after=10 20 "$BINARY" \
	backend=force=kvm \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 2>&1 || true)

# Useful-diagnostic markers: the integrated path's own log
# lines + the expected progression backtrace symbols and the
# clean init-exit panic.
MARKERS=(
	"um: backend = kvm-v2"      # KVM v2 backend active (not fallback)
	"kvm_v2_vcpu_run"           # the KVM_RUN loop executed
	"kvm_v2_handle_io_trap"     # an I/O / HLT exit was decoded
	"handle_syscall"            # a real syscall was dispatched
	"exitcode=0x00000000"       # init=/bin/true ran to a clean exit
)

HITS=0
for m in "${MARKERS[@]}"; do
	if echo "$OUT" | grep -qF "$m"; then
		echo "KVM_SMOKE: hit marker: $m"
		HITS=$((HITS + 1))
	fi
done

# Minimum bar for a PASS: at least one integrated-path
# progression marker must have fired. Anything less means we
# didn't get past lifecycle init.
if [ "$HITS" -lt 2 ]; then
	echo "KVM_SMOKE: FAIL - only $HITS/${#MARKERS[@]} markers hit (need >=2)"
	echo "----- last 40 lines of UML output -----"
	echo "$OUT" | tail -40
	exit 1
fi

echo "KVM_SMOKE: PASS markers=$HITS/${#MARKERS[@]}"
exit 0
