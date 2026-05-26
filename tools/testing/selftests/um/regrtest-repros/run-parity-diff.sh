#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Run the regrtest substrate gate twice — once under backend=seccomp,
# once under backend=kvm-v2 — and diff the per-test results. Mid-Phase
# D, v2 delegates everything HOT to seccomp so the outputs should be
# bit-identical; any divergence is a v2 regression. Post-Phase D.5
# (when v2's .vcpu_run flips), the outputs may legitimately diverge as
# v2 fixes things seccomp can't (itimer_virtual, getrusage_split etc.)
# — but they should never *regress* relative to seccomp.
#
# Usage:
#   UML_BINARY=/path/to/uml/linux \
#     bash tools/testing/selftests/um/regrtest-repros/run-parity-diff.sh
#
# Exit codes:
#   0 — bit-identical PASS/FAIL/EXPECTED_FAIL across both backends
#   1 — divergence (printed unified diff)
#   2 — one or both backends couldn't run the gate at all
#
# Companion to run-regrtest-repros.sh (memo 29 §2.5). Lands per the
# "v2 vs seccomp parity diff wrapper" task in the Phase D test plan.

set -u

BINARY=${UML_BINARY:-$HOME/src/uml-builds/uml-clean/linux}
TIMEOUT=${TIMEOUT:-180}
REPRO_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNNER="$REPRO_DIR/run-regrtest-repros.sh"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found" >&2
	exit 2
fi
if [ ! -x "$RUNNER" ]; then
	echo "SKIP: $RUNNER not executable" >&2
	exit 2
fi

WORK=$(mktemp -d -t parity-diff.XXXXXX)
trap "rm -rf $WORK" EXIT

run_backend() {
	local backend="$1"
	local out="$WORK/$backend.log"
	UML_BINARY="$BINARY" TIMEOUT="$TIMEOUT" BACKEND="$backend" \
		bash "$RUNNER" > "$out" 2>&1
	# Canonical form: <test_name> <verdict>. Strip the per-test
	# trailing details (elapsed=Nms, count=N, fd=N, etc.) which are
	# inherently jittery across runs and would dominate a literal
	# diff. The verdict (PASS / FAIL / EXPECTED_FAIL) is what we
	# care about for parity.
	awk '
		/^REPRO:/ {
			# REPRO: <name> <verdict> [details...]
			print $2, $3
		}
		/^=== SUMMARY/ { in_summary = 1; print; next }
		in_summary && /^PASS=/ { print }
	' "$out" > "$WORK/$backend.results" || true
	echo "$WORK/$backend.results"
}

echo "=== seccomp run ==="
SECCOMP_RESULTS=$(run_backend seccomp)
tail -5 "$WORK/seccomp.results"

echo
echo "=== kvm-v2 run ==="
KVM_V2_RESULTS=$(run_backend kvm-v2)
tail -5 "$WORK/kvm-v2.results"

echo
echo "=== diff (seccomp vs kvm-v2) ==="
if diff -u "$SECCOMP_RESULTS" "$KVM_V2_RESULTS"; then
	echo "PARITY: bit-identical results across both backends"
	exit 0
else
	echo
	echo "PARITY: divergence detected (see diff above)"
	exit 1
fi
