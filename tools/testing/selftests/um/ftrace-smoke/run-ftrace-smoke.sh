#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/ftrace-smoke/run-ftrace-smoke.sh — host-side launcher for the
# workstream C-05 ftrace smoke test.
#
# Boots a UML guest built with CONFIG_FUNCTION_TRACER=y and runs
# ftrace-smoke.sh as init. The guest enables the function tracer,
# runs a small workload, verifies the trace buffer, disables the
# tracer, and halts. This script captures the guest's
# FTRACE_SMOKE: PASS|FAIL line and exits 0 / 1.
#
# Environment:
#   UML_BINARY   path to the UML binary built with CONFIG_FUNCTION_TRACER=y
#                (default: /tmp/uml-research/linux)
#   UML_MEM      mem=N argument  (default: 512M — research build is large)
#
# The default binary path matches the `make ARCH=um uml/research`
# workflow when the user follows Documentation/virt/uml/profiles/
# research.rst and builds into /tmp/uml-research/.

set -u

BINARY=${UML_BINARY:-/tmp/uml-research/linux}
MEM=${UML_MEM:-512M}
DIR=$(cd "$(dirname "$0")" && pwd)
GUEST_SCRIPT="$DIR/ftrace-smoke.sh"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4   # kselftest "skip" exit code
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi

# --kill-after=10: UML has its own signal plumbing and can
# ignore SIGTERM under some conditions (see D59 Finding #1
# forensic memo). Without --kill-after, a wedged guest
# hangs this harness silently for the full timeout plus
# whatever the CI harness's outer watchdog allows. 10 s
# grace is enough for a clean halt path; if the guest is
# truly wedged, SIGKILL follows and we fail loudly.
OUT=$(timeout --kill-after=10 60 "$BINARY" init="$GUEST_SCRIPT" mem="$MEM" \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw 2>&1)

# The guest script emits several FTRACE_SMOKE: ... progress lines
# and exactly one FTRACE_SMOKE: {PASS,FAIL} ... terminal line at
# the end. Match the terminal line.
LINE=$(echo "$OUT" | grep -E '^FTRACE_SMOKE: (PASS|FAIL) ' | tail -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no FTRACE_SMOKE PASS/FAIL line in output"
	echo "$OUT" | grep '^FTRACE_SMOKE:' | tail -5
	echo "---"
	echo "$OUT" | tail -40
	exit 1
fi

echo "$LINE"
case "$LINE" in
*PASS*) exit 0 ;;
*FAIL*) exit 1 ;;
*)      exit 1 ;;
esac
