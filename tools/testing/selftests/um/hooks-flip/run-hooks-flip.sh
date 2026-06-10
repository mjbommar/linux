#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/hooks-flip/run-hooks-flip.sh - host-side launcher for the runtime
# hook flip smoke test.
#
# Boots a UML guest with hooks-flip.sh as init. The guest runs the
# full flip sequence in a few hundred milliseconds and halts. This
# script captures the guest's PASS/FAIL line and exits 0 / 1.
#
# Environment:
#   UML_BINARY   path to the UML binary  (default: /tmp/uml-matrix-dynamic/linux)
#   UML_MEM      mem=N argument          (default: 128M)

set -u

BINARY=${UML_BINARY:-/tmp/uml-matrix-dynamic/linux}
MEM=${UML_MEM:-128M}
DIR=$(cd "$(dirname "$0")" && pwd)
GUEST_SCRIPT="$DIR/hooks-flip.sh"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found" >&2
	exit 4   # kselftest "skip" exit code
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi

OUT=$(timeout --kill-after=10 20 "$BINARY" init="$GUEST_SCRIPT" mem="$MEM" \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw 2>&1)

LINE=$(echo "$OUT" | grep '^HOOKS_FLIP:' | head -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no HOOKS_FLIP line in output"
	echo "$OUT" | tail -40
	exit 1
fi

echo "$LINE"
case "$LINE" in
*PASS*) exit 0 ;;
*FAIL*) exit 1 ;;
*) exit 1 ;;
esac
