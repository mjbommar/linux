#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side launcher for the UML KCOV smoke test.
#
# Boots a KCOV-enabled UML guest, runs kcov-smoke.sh as init, and
# expects the guest helper to open /sys/kernel/debug/kcov, initialize
# and mmap the trace buffer, enable PC tracing, and collect coverage
# from a syscall.
#
# Environment:
#   UML_BINARY  path to a fuzz-profile UML binary
#               (default: /tmp/uml-fuzz/linux)
#   UML_MEM     mem=N argument (default: 512M)
#   KCOV_SMOKE_HELPER  optional path to kcov-smoke helper

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-fuzz/linux}
MEM=${UML_MEM:-512M}
GUEST_SCRIPT="$DIR/kcov-smoke.sh"

if [ -n "${KCOV_SMOKE_HELPER:-}" ]; then
	HELPER=$KCOV_SMOKE_HELPER
elif [ -n "${OUTPUT:-}" ] && [ -x "$OUTPUT/kcov-smoke" ]; then
	HELPER="$OUTPUT/kcov-smoke"
else
	HELPER="$DIR/kcov-smoke"
fi

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi
if [ ! -x "$HELPER" ]; then
	echo "SKIP: KCOV helper $HELPER not built; run 'make' in this dir" >&2
	exit 4
fi

OUT=$(timeout --kill-after=10 90 "$BINARY" \
	init="$GUEST_SCRIPT" kcov_smoke_helper="$HELPER" mem="$MEM" \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1)

LINE=$(echo "$OUT" | grep -E '^KCOV_SMOKE: (PASS|FAIL|SKIP) ' | tail -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no KCOV_SMOKE PASS/FAIL/SKIP line in output"
	echo "$OUT" | grep '^KCOV_SMOKE:' | tail -10
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
