#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/kmsan-smoke/run-kmsan-smoke.sh - host-side launcher
# for the KMSAN regression guard.
#
# Boots a UML binary built with CONFIG_KMSAN=y under clang
# (LLVM=1 research or research-kmsan profile), runs
# kmsan-smoke.sh as init, and asserts that KMSAN is alive:
#   * the KMSAN runtime boot banner is visible in guest dmesg.
#   * At least one kmsan_* kunit testcase or the in-tree
#     kmsan-trivial reproducer prints "BUG: KMSAN:" to
#     dmesg when reading an uninitialized stack variable.
#
# Exits 0 on PASS, 1 on FAIL, 4 on SKIP (kernel lacks
# CONFIG_KMSAN or binary missing). Matches the kselftest
# exit-code convention every other um/ selftest in this tree
# uses.
#
# Environment:
#   UML_BINARY  path to a KMSAN-enabled UML binary.
#               Default: /tmp/uml-research-kmsan/linux
#   UML_MEM     mem=N (default 512M; KMSAN triples RSS for
#               touched pages; 256M boots but OOMs during
#               stack_depot_init on some runners).

set -u

BINARY=${UML_BINARY:-/tmp/uml-research-kmsan/linux}
MEM=${UML_MEM:-512M}
DIR=$(cd "$(dirname "$0")" && pwd)
GUEST_SCRIPT="$DIR/kmsan-smoke.sh"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi

# Quick sanity: was this binary actually built with KMSAN?
# Detect by grepping the image for the "BUG: KMSAN:" report
# string that kmsan_emit_bug_report() emits. If absent, the
# kernel wasn't compiled with -fsanitize=kernel-memory and
# the runtime test below would false-negative.
if ! grep -q "BUG: KMSAN:" "$BINARY" 2>/dev/null; then
	echo "SKIP: $BINARY does not contain KMSAN runtime (rebuild with LLVM=1 CONFIG_KMSAN=y)" >&2
	exit 4
fi

OUT=$(timeout --kill-after=10 90 "$BINARY" \
	init="$GUEST_SCRIPT" mem="$MEM" \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw 2>&1)

LINE=$(echo "$OUT" | grep -E '^KMSAN_SMOKE: (PASS|FAIL|SKIP)' | tail -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no KMSAN_SMOKE PASS/FAIL/SKIP line in output"
	echo "$OUT" | grep -E '^KMSAN_SMOKE:|BUG: KMSAN:' | tail -20
	echo "---"
	echo "$OUT" | tail -40
	exit 1
fi

echo "$LINE"
case "$LINE" in
*PASS*) exit 0 ;;
*SKIP*) exit 4 ;;
*FAIL*) exit 1 ;;
*)      exit 1 ;;
esac
