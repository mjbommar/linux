#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/userspace-smoke/run-userspace-smoke.sh — host-side launcher
# for the baseline UML userspace regression guard.
#
# Boots the UML binary, runs userspace-smoke.sh as init, looks
# for the terminal "USERSPACE_SMOKE: PASS|FAIL|SKIP" line.
# Exits 0 / 1 / 4 per kselftest convention.
#
# Pattern mirrors um/kprobes-stress/run-kprobes-stress.sh and
# um/snapshot-smoke/run-snapshot-smoke.sh.
#
# Environment:
#   UML_BINARY  path to the UML binary (default: /tmp/uml-research/linux)
#   UML_MEM     mem=N argument (default: 256M — python3's heap needs
#               a bit more headroom than kprobes-stress's 64M)

set -u

BINARY=${UML_BINARY:-/tmp/uml-research/linux}
MEM=${UML_MEM:-256M}
DIR=$(cd "$(dirname "$0")" && pwd)
GUEST_SCRIPT="$DIR/userspace-smoke.sh"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi

OUT=$(timeout 60 "$BINARY" \
	init="$GUEST_SCRIPT" mem="$MEM" \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw 2>&1)

LINE=$(echo "$OUT" | grep -E '^USERSPACE_SMOKE: (PASS|FAIL|SKIP)' | tail -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no USERSPACE_SMOKE PASS/FAIL/SKIP line in output"
	echo "$OUT" | grep '^USERSPACE_SMOKE:' | tail -10
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
