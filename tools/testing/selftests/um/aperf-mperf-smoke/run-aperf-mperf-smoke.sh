#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/aperf-mperf-smoke/run-aperf-mperf-smoke.sh
#
# Boots a kvm-v2 UML kernel built with
# CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y, runs the
# freestanding demo at Documentation/virt/uml/examples/aperf-mperf/
# as init, and asserts the demo emits its PASS verdict. Direct
# regression guard for the kvm-v2 cap-enable plumbing; proves
# vm_create issues KVM_ENABLE_CAP with the APERFMPERF bit and KVM
# accepts the cap.
#
# Exits 0 PASS, 4 SKIP, 1 FAIL - kselftest convention.
#
# Environment:
#   UML_BINARY    UML kernel built with kvm-v2 + APERFMPERF=y +
#                 DEBUG_FS=y (default: /tmp/uml-aperf/linux)
#   DEMO_BINARY   Built aperf-mperf-demo binary
#                 (default: ../../../../Documentation/virt/uml/
#                  examples/aperf-mperf/aperf-mperf-demo)
#   UML_MEM       mem= argument. Default 128M.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-aperf/linux}
DEMO=${DEMO_BINARY:-$DIR/../../../../../Documentation/virt/uml/examples/aperf-mperf/aperf-mperf-demo}
MEM=${UML_MEM:-128M}

skip() { echo "SKIP: $*" >&2; exit 4; }
fail() { echo "FAIL: $*" >&2; exit 1; }

# ---- preflight ----------------------------------------------------

if [ ! -x "$BINARY" ]; then
	skip "UML binary $BINARY not found; enable APERF/MPERF and set UML_BINARY"
fi

if [ ! -x "$DEMO" ]; then
	skip "demo binary $DEMO not built; run 'make' in Documentation/virt/uml/examples/aperf-mperf/ first"
fi

if ! grep -q aperfmperf /proc/cpuinfo 2>/dev/null; then
	skip "host CPU lacks X86_FEATURE_APERFMPERF; KVM will mask disable exits"
fi

ensure_kvm_readable() {
	local i
	for i in 1 2 3; do
		[ -r /dev/kvm ] && return 0
		sudo -n setfacl -m u:"$(id -un)":rw /dev/kvm 2>/dev/null || true
		[ -r /dev/kvm ] && return 0
		sleep 0.1
	done
	return 1
}
if ! ensure_kvm_readable; then
	skip "/dev/kvm not readable by the selftest user"
fi

# ---- boot + assert ------------------------------------------------

# 30s timeout is plenty: boot + mount + probe + write + reboot.
# kill-after escalates to SIGKILL if UML ignores SIGTERM.
LOG=$(mktemp)
trap "rm -f $LOG" EXIT

# Suppress the bash-side "Aborted (core dumped)" report: UML exits via
# SIGABRT after panic=-1 reboot when init exits, which is expected and
# not a test failure.  setsid detaches into a new session so the SIGCHLD
# delivery doesn't trigger bash's job-control diagnostic.
setsid -w bash -c '
	timeout --kill-after=10 30 \
		"$0" \
		mem="$1" \
		backend=force=kvm-v2 \
		kvm_v2_aperfmperf=on \
		init="$2" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw \
		panic=-1 \
		</dev/null
' "$BINARY" "$MEM" "$DEMO" >"$LOG" 2>&1 || true

if ! grep -q APERF_MPERF_DEMO "$LOG"; then
	echo "---- guest log (last 40 lines) ----" >&2
	tail -40 "$LOG" >&2
	fail "demo did not run - boot wedged or hostfs init exec failed"
fi

VERDICT=$(grep -oE "APERF_MPERF_DEMO: (PASS|FAIL) [^$]*" "$LOG" | tail -1)

if echo "$VERDICT" | grep -q "PASS plumbing_ok=1"; then
	echo "PASS aperf_mperf_smoke"
	echo "  $VERDICT"
	exit 0
fi

echo "---- demo lines ----" >&2
grep APERF_MPERF_DEMO "$LOG" >&2
fail "demo verdict: $VERDICT"
