#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# uml-cross-backend.sh — A-05.S2 cross-backend equivalence test.
#
# Boots the same minimal init under both PTRACE_ONLY and SECCOMP_ONLY
# kernels, diffs the observable dmesg output, and passes when the
# difference is purely backend-identification (the `um: backend = ...`
# line itself plus addresses/timestamps that legitimately differ).
#
# Per Documentation/virt/uml/redesign/02-workstreams/
# A-backend-abstraction/05-contract.md: the eventual home is
# tools/testing/selftests/uml/cross-backend/ with a Python harness
# driving LTP. This shell version is the skeleton.

set -u

SRC=$(cd "$(dirname "$0")/../../../../.." && pwd)
PTRACE_BUILD=/tmp/uml-matrix-ptrace_only
SECCOMP_BUILD=/tmp/uml-matrix-seccomp_only

if [ ! -x "$PTRACE_BUILD/linux" ] || [ ! -x "$SECCOMP_BUILD/linux" ]; then
	echo "Need both $PTRACE_BUILD/linux and $SECCOMP_BUILD/linux."
	echo "Run: $(dirname "$0")/uml-boot-matrix.sh first."
	exit 1
fi

# Common boot args.
COMMON="init=/bin/true mem=64M con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs"

# Boot each kernel and capture dmesg.
boot_one() {
	local label=$1 binary=$2; shift 2
	local out=/tmp/uml-cross-${label}.log
	timeout 12 "$binary" $COMMON "$@" > "$out" 2>&1 || true
	echo "$out"
}

# Normalize dmesg for diff:
#   - Strip leading [   12.345678] timestamps if present
#   - Strip the `um: backend = ...` line itself (will always differ)
#   - Strip kernel-version line (build hash differs)
#   - Strip kernel-text addresses in stack traces ([<60022845>])
#   - Strip absolute paths
normalize() {
	sed -E \
		-e 's/^\[ *[0-9]+\.[0-9]+\] //' \
		-e '/^um: backend = /d' \
		-e '/^Linux version /d' \
		-e 's/\[<[0-9a-f]+>\]/[<HEX>]/g' \
		-e 's|/home/[^ ]+|<PATH>|g' \
		-e 's/pid = [0-9]+/pid = <PID>/g' \
		-e 's/, errno = [0-9]+/, errno = <ERRNO>/g' \
		-e 's|/[0-9a-zA-Z_-]+/uml/[0-9a-zA-Z]+/|/<UMID>/|g' \
		"$1"
}

PTRACE_LOG=$(boot_one ptrace "$PTRACE_BUILD/linux")
SECCOMP_LOG=$(boot_one seccomp "$SECCOMP_BUILD/linux" seccomp=on)

normalize "$PTRACE_LOG" > /tmp/uml-cross-ptrace.norm
normalize "$SECCOMP_LOG" > /tmp/uml-cross-seccomp.norm

echo "===== ptrace dmesg backend line ====="
grep "^um: backend = " "$PTRACE_LOG" || echo "(missing)"
echo "===== seccomp dmesg backend line ====="
grep "^um: backend = " "$SECCOMP_LOG" || echo "(missing)"
echo
echo "===== diff (normalized) ====="
diff -u /tmp/uml-cross-ptrace.norm /tmp/uml-cross-seccomp.norm > /tmp/uml-cross.diff || true
diff_lines=$(grep -cE '^[+-]' /tmp/uml-cross.diff | head -1)
diff_lines=${diff_lines:-0}

# Show first 20 lines of diff for inspection.
head -40 /tmp/uml-cross.diff || true
echo
echo "Diff size: $diff_lines changed lines (after normalization)."
echo
echo "Per A-05.S2: this skeleton just reports the diff; the eventual"
echo "harness should run an identical syscall workload under both"
echo "backends and assert byte-equivalent observable behavior."
