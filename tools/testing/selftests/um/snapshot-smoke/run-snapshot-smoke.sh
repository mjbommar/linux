#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/snapshot-smoke/run-snapshot-smoke.sh — host-side launcher for
# the workstream C-09 snapshot/forkserver smoke selftest.
#
# Runs two checks in sequence:
#
#   A) init=snapshot-smoke.sh to verify the sysfs/debugfs plumbing:
#      /sys/kernel/um/state_version reads "1" and
#      /sys/kernel/debug/um/snapshot_ready is writeable.
#      Always run. FAIL here aborts the selftest.
#
#   B) snapshot-smoke-driver.py drives the AFL-compatible
#      forkserver protocol on fds 198/199: handshake, fork, read
#      the 4-byte pid followed by the 4-byte status, clean
#      disconnect. Per the C-09 v1 ceiling documented in
#      Documentation/virt/uml/snapshot.rst §"v1 ceiling:
#      exit-status semantics" (and the Finding #1 forensic memo
#      at Documentation/virt/uml/redesign/04-risks/
#      signal-reentry-in-fork-window.md), status is a
#      hard-coded 0 rather than the worker's real exit —
#      parent-side waitpid crashed via SIGALRM reentry across
#      four wait4-variant attempts, so this v1 skips the reap
#      and drains zombies non-synchronously at the top of the
#      next iteration. The driver asserts status == 0 so a
#      future real-status fix changing the wire format can't
#      slip past this test. Requires python3 at the host;
#      SKIP'd if python3 isn't found.
#
# Pattern mirrors um/kprobes-stress/run-kprobes-stress.sh. Final
# exit status follows kselftest convention: 0 PASS, 1 FAIL, 4 SKIP.
#
# Environment:
#   UML_BINARY  path to the UML binary built with
#               CONFIG_UM_SNAPSHOT_FORKSERVER=y
#               (default: /tmp/uml-fuzz/linux)
#   UML_MEM     mem=N argument (default: 128M)
#   PART_B      set to 0 to skip the host-driver part

set -u

BINARY=${UML_BINARY:-/tmp/uml-fuzz/linux}
MEM=${UML_MEM:-128M}
PART_B=${PART_B:-1}
DIR=$(cd "$(dirname "$0")" && pwd)
GUEST_SCRIPT="$DIR/snapshot-smoke.sh"
DRIVER="$DIR/snapshot-smoke-driver.py"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi

# --- Part A: guest-side sysfs/debugfs plumbing check. ---
OUT=$(timeout --kill-after=10 60 "$BINARY" \
	init="$GUEST_SCRIPT" mem="$MEM" \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw 2>&1)

LINE=$(echo "$OUT" | grep -E '^SNAPSHOT_SMOKE: (PASS|FAIL|SKIP)' | tail -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no SNAPSHOT_SMOKE PASS/FAIL/SKIP line in output"
	echo "$OUT" | tail -30
	exit 1
fi

echo "$LINE"
case "$LINE" in
*SKIP*) exit 4 ;;
*FAIL*) exit 1 ;;
*PASS*) : ;;  # fall through to Part B
*)      exit 1 ;;
esac

# --- Part B: host-driven AFL forkserver handshake + 1 iteration. ---
if [ "$PART_B" != "1" ]; then
	echo "SNAPSHOT_SMOKE_DRV: SKIP part B disabled via PART_B=0"
	exit 0
fi

PY=$(command -v python3 || true)
if [ -z "$PY" ]; then
	echo "SNAPSHOT_SMOKE_DRV: SKIP python3 not found (host driver requires it)"
	exit 0
fi

DRV_OUT=$("$PY" "$DRIVER" "$BINARY" "$GUEST_SCRIPT" "$MEM" 2>&1 || true)
DRV_LINE=$(echo "$DRV_OUT" | grep -E '^DRV: (PASS|FAIL|SKIP)' | tail -1)

if [ -z "$DRV_LINE" ]; then
	echo "SNAPSHOT_SMOKE_DRV: FAIL no DRV PASS/FAIL/SKIP line"
	echo "$DRV_OUT" | tail -10
	exit 1
fi

echo "$DRV_LINE"
case "$DRV_LINE" in
*PASS*) exit 0 ;;
*SKIP*) exit 4 ;;
*FAIL*) exit 1 ;;
*)      exit 1 ;;
esac
