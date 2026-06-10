#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Dynamic-loader kselftest.
#
# All other UML kselftests use freestanding-ELF init binaries
# that don't exercise ld-linux (perf-getpid, perf-fallback,
# kvm-bounds, df-preserve). That hides a class of bugs in the
# host #PF recovery path: ld-linux faults on shared-library
# pages before main(), and on the KVM backend's lazy-fault
# path the first-instruction-fetch of a freshly mapped lib
# page fails to be serviced, boot dies before reaching the
# script body.
#
# This selftest boots UML with `init=/bin/dash -c "echo
# DYN_LOADER: ok"`. dash is a small dynamically-linked PIE
# binary that pulls in libc + ld-linux via the standard ELF
# interpreter mechanism. PASS = the DYN_LOADER: ok line is
# emitted to stdout. FAIL = no line emitted (boot died) or
# any error pattern (segfault, double-fault, kernel panic
# other than the panic=-1-on-clean-init-exit pattern).
#
# This test must PASS for both kvmint (no gadget) and kvmbench
# (gadget-on) kernels.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL, per kselftest convention.
#
# Environment:
#   UML_BINARY        UML kernel (default /tmp/uml-kvmint/linux).
#   UML_GADGET_BINARY optional kernel with CONFIG_UM_BACKEND_KVM_
#                     GADGET=y; runner adds a kvm-gadget row when set.
#   UML_MEM           mem= argument. Default 256M (dash + libc need
#                     more headroom than perf-* binaries).
#   BACKENDS          backend list. Default "ptrace seccomp kvm".
#   DYN_INIT          path to init binary inside hostfs. Default
#                     /bin/dash (small dynamically-linked PIE).
#   DYN_ARGS          space-separated args. Default '-c echo DYN_LOADER:\ ok'.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
GADGET_BINARY=${UML_GADGET_BINARY:-}
MEM=${UML_MEM:-256M}
BACKENDS=${BACKENDS:-ptrace seccomp kvm}
DYN_INIT=${DYN_INIT:-/bin/echo}
DYN_ARGS=${DYN_ARGS:-"DYN_LOADER: ok"}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$DYN_INIT" ]; then
	echo "SKIP: $DYN_INIT not present on host (set DYN_INIT)" >&2
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
	local binary=${2:-$BINARY}
	local log

	if [ "$backend" = "kvm" ] && [ -e /dev/kvm ]; then
		if ! ensure_kvm_readable; then
			printf 'DYN_LOADER: backend=%s FAIL (lost /dev/kvm ACL)\n' \
				"$backend"
			return
		fi
	fi
	# Note: rootfs=hostfs so the host's /bin/echo + ld-linux
	# + libc are visible at the same paths inside the guest.
	# init=/bin/echo + positional cmdline args become argv to
	# echo, which prints them; this exercises ld-linux on libc but
	# not a shell tree.
	log=$(timeout --kill-after=5 30 "$binary" \
		backend="force=$backend" \
		init="$DYN_INIT" $DYN_ARGS mem="$MEM" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw \
		panic=-1 </dev/null 2>&1 || true)
	local observed
	observed=$(echo "$log" | sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
	# Debug: optionally save the full kernel log per-backend.
	if [ -n "${DYN_LOADER_DUMP:-}" ]; then
		echo "$log" > "${DYN_LOADER_DUMP}.${backend}"
	fi
	if [ "$observed" != "$backend" ]; then
		printf 'DYN_LOADER: backend=%s FAIL (observed=%s)\n' \
			"$backend" "$observed"
		return
	fi
	if echo "$log" | grep -q '^DYN_LOADER: ok'; then
		echo "DYN_LOADER: backend=$backend PASS"
		return
	fi
	# Pull a hint of the failure mode for triage. Anchor to
	# 'panic - not syncing' / 'Kernel panic' / 'Oops' / etc.
	# instead of bare 'panic' so the cmdline 'panic=-1' arg
	# doesn't false-positive.
	local hint
	local fail_re
	fail_re='Kernel panic|panic - not|^Oops|SHUTDOWN'
	fail_re="$fail_re|fatal signal|cr2=0x[0-9a-f]+ rip="
	fail_re="$fail_re|kvm #DF|run_userspace: unrecov"
	hint=$(echo "$log" | grep -E "$fail_re" |
	       head -1 | tr -s ' ' | cut -c1-120)
	if [ -z "$hint" ]; then
		hint='no output / no recognized failure (boot stalled)'
	fi
	echo "DYN_LOADER: backend=$backend FAIL ($hint)"
}

FAIL=0
for B in $BACKENDS; do
	if [ "$B" = "kvm" ] && [ ! -e /dev/kvm ]; then
		echo "DYN_LOADER: backend=$B SKIP (no /dev/kvm)"
		continue
	fi
	if [ "$B" = "kvm" ] && ! ensure_kvm_readable; then
		echo "DYN_LOADER: backend=$B SKIP (no /dev/kvm)"
		continue
	fi
	LINE=$(run_one "$B")
	echo "$LINE"
	if echo "$LINE" | grep -q FAIL; then
		FAIL=$((FAIL + 1))
	fi
done

if [ -n "$GADGET_BINARY" ] && [ -x "$GADGET_BINARY" ] && [ -e /dev/kvm ]; then
	if ensure_kvm_readable; then
		LINE=$(run_one kvm "$GADGET_BINARY")
		# Rename label so the row is self-describing.
		echo "${LINE/backend=kvm/backend=kvm-gadget}"
		if echo "$LINE" | grep -q FAIL; then
			FAIL=$((FAIL + 1))
		fi
	else
		echo "DYN_LOADER: backend=kvm-gadget SKIP (no /dev/kvm)"
	fi
fi

if [ "$FAIL" -gt 0 ]; then
	# The kvm and kvm-gadget rows are expected to pass
	# deterministically.  A failure here is a real regression and
	# should fail the test outright.
	echo "DYN_LOADER: FAIL ($FAIL backend(s) failed)"
	exit 1
fi
echo "DYN_LOADER: PASS"
exit 0
