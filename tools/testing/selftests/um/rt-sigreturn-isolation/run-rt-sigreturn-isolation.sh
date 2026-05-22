#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Path A from Documentation/virt/uml/redesign/06-sequencing/
# post-2026-05-21-three-test-paths.md §1.
#
# Runs the host-only atomic stack-pivot primitive test.  Validates
# that we can leave a deep C call chain behind and resume on a
# fresh stack at a clean entry — the v1-ceiling escape mechanism
# the UML pool-member work needs.
#
# Exits 0 on PASS, 1 on FAIL — kselftest convention.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
TEST=$DIR/rt_sigreturn_test

if [ ! -x "$TEST" ]; then
	echo "SKIP: $TEST not built — run make first"
	exit 4
fi

OUT=$("$TEST" 2>&1)
RC=$?

EXPECTED_LINES=(
	"main entered"
	"recursion depth=20"
	"about to pivot stack"
	"STACK_PIVOT: arrived"
)
for line in "${EXPECTED_LINES[@]}"; do
	if ! echo "$OUT" | grep -qF -- "$line"; then
		echo "RT_SIGRETURN_ISOLATION: FAIL (missing line: $line)"
		echo "$OUT"
		exit 1
	fi
done

if [ "$RC" -ne 0 ]; then
	echo "RT_SIGRETURN_ISOLATION: FAIL (exit=$RC)"
	echo "$OUT"
	exit 1
fi

echo "RT_SIGRETURN_ISOLATION: PASS (atomic stack-pivot primitive works on this host)"
exit 0
