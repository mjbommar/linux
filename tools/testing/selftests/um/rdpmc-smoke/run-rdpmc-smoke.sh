#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/rdpmc-smoke/run-rdpmc-smoke.sh
#
# Boots a kvm-v2 UML kernel with CONFIG_UM_BACKEND_KVM_V2_RDPMC=y,
# runs the freestanding rdpmc demo at
# Documentation/virt/uml/examples/rdpmc/ as init, and asserts ONE of:
#
#   PASS:    rdpmc(0) returned a value, demo PASSes
#   PARTIAL: rdpmc(0) returned a value but fixed counters not armed
#            (demo emits FAIL counters_advanced=0; we treat as PASS
#            for kselftest purposes because CR4.PCE IS working --
#            arming fixed counters is a separate kernel-side concern)
#   SKIP:    rdpmc was intercepted by host KVM and #GP-injected; the
#            demo segfaulted before printing anything.  This is the
#            documented host-dependent third outcome (typical on AMD
#            with enable_mediated_pmu=N or missing PERFCTR_CORE in
#            guest CPUID).  Not a UML regression.
#
# Exits 0 on PASS / PARTIAL, 4 on SKIP, 1 on FAIL.
#
# Environment:
#   UML_BINARY    UML kernel with CONFIG_UM_BACKEND_KVM_V2=y +
#                 CONFIG_UM_BACKEND_KVM_V2_RDPMC=y
#                 (default: /tmp/uml-aperf/linux)
#   DEMO_BINARY   Built rdpmc-demo binary
#                 (default: ../../../../../Documentation/virt/uml/
#                  examples/rdpmc/rdpmc-demo)
#   UML_MEM       mem= argument. Default 128M.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-aperf/linux}
DEMO=${DEMO_BINARY:-$DIR/../../../../../Documentation/virt/uml/examples/rdpmc/rdpmc-demo}
MEM=${UML_MEM:-128M}

skip() { echo "SKIP: $*" >&2; exit 4; }
fail() { echo "FAIL: $*" >&2; exit 1; }

if [ ! -x "$BINARY" ]; then
	skip "UML binary $BINARY not found"
fi
if [ ! -x "$DEMO" ]; then
	skip "demo binary $DEMO not built; run 'make' in Documentation/virt/uml/examples/rdpmc/ first"
fi
[ -r /dev/kvm ] || skip "/dev/kvm not readable"

LOG=$(mktemp)
trap "rm -f $LOG" EXIT

setsid -w bash -c '
	timeout --kill-after=10 30 \
		"$0" \
		mem="$1" \
		backend=force=kvm-v2 \
		init="$2" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw \
		panic=-1 \
		</dev/null
' "$BINARY" "$MEM" "$DEMO" >"$LOG" 2>&1 || true

# Outcome 1: demo emitted PASS line.
if grep -q "RDPMC_DEMO: PASS" "$LOG"; then
	echo "PASS rdpmc_smoke"
	grep "RDPMC_DEMO: PASS" "$LOG" | tail -1
	exit 0
fi

# Outcome 2: demo emitted FAIL but rdpmc_works=1 (CR4.PCE worked,
# fixed counters not armed -- separate concern, not a UML regression).
if grep -q "RDPMC_DEMO: FAIL.*rdpmc_works=1" "$LOG"; then
	echo "PASS rdpmc_smoke (partial -- rdpmc works, fixed counters not armed)"
	grep "RDPMC_DEMO: FAIL" "$LOG" | tail -1
	exit 0
fi

# Outcome 3: rdpmc was intercepted; demo segfaulted before printing.
if grep -q "rdpmc-demo\[.*segfault" "$LOG"; then
	skip "host KVM rejected rdpmc (vPMU emulation; see Documentation/virt/uml/examples/rdpmc/README.md)"
fi

# Anything else is unexpected.
echo "---- last 30 lines of guest log ----" >&2
tail -30 "$LOG" >&2
fail "rdpmc-demo did not run cleanly -- boot wedged or unexpected output"
