#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side launcher for the UML KCSAN smoke test.
#
# Boots a race-profile UML guest, runs kcsan-smoke.sh as init, and
# validates the KCSAN boot selftest plus the debugfs enable/disable and
# microbenchmark control paths.
#
# Environment:
#   UML_BINARY  path to a race-profile UML binary
#               (default: /tmp/uml-race/linux)
#   UML_MEM     mem=N argument (default: 512M)

set -u

BINARY=${UML_BINARY:-/tmp/uml-race/linux}
MEM=${UML_MEM:-512M}
DIR=$(cd "$(dirname "$0")" && pwd)
GUEST_SCRIPT="$DIR/kcsan-smoke.sh"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi

OUT=$(timeout --kill-after=10 90 "$BINARY" \
	init="$GUEST_SCRIPT" mem="$MEM" ncpus=2 \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw \
	panic=-1 loglevel=8 </dev/null 2>&1)

LINE=$(echo "$OUT" | grep -E '^KCSAN_SMOKE: (PASS|FAIL|SKIP) ' | tail -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no KCSAN_SMOKE PASS/FAIL/SKIP line in output"
	echo "$OUT" | grep '^KCSAN_SMOKE:' | tail -10
	echo "---"
	echo "$OUT" | tail -80
	exit 1
fi

echo "$LINE"
case "$LINE" in
*PASS*) exit 0 ;;
*SKIP*) exit 4 ;;
*FAIL*) exit 1 ;;
*)      exit 1 ;;
esac
