#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/kprobes-stress/run-kprobes-stress.sh — host-side launcher for
# the workstream C-04 kprobes/kretprobes stress selftest.
#
# Boots a UML guest built with CONFIG_KPROBES=y + CONFIG_KRETPROBES=y
# (research profile, or any build that selects HAVE_RETHOOK). Runs
# kprobes-stress.sh as init. The guest registers a kretprobe on
# kernel_clone via the in-tree sample module, exercises it with a
# fork-heavy workload, and emits a KPROBES_STRESS: PASS|FAIL line.
#
# Pattern mirrors um/ftrace-smoke/run-ftrace-smoke.sh. The two
# selftests together cover the C-04 + C-05 tracer/probe surface.
#
# Environment:
#   UML_BINARY  path to the UML binary built with CONFIG_KPROBES=y
#               (default: /tmp/uml-research/linux)
#   UML_MEM     mem=N argument (default: 512M — research build is large)
#
# The default binary path matches `make ARCH=um uml/research` when
# the user builds into /tmp/uml-research/. See
# Documentation/virt/uml/profiles/research.rst.

set -u

BINARY=${UML_BINARY:-/tmp/uml-research/linux}
MEM=${UML_MEM:-512M}
DIR=$(cd "$(dirname "$0")" && pwd)
GUEST_SCRIPT="$DIR/kprobes-stress.sh"
MODULE=${UML_KRETPROBE_MODULE:-}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4   # kselftest "skip" exit code
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi

# Locate the kretprobe_example module. Prefer an explicit path via
# UML_KRETPROBE_MODULE, else search the build tree sibling of the
# binary (samples/kprobes/kretprobe_example.ko). The guest script
# will SKIP if it can't find the module at runtime.
if [ -z "$MODULE" ]; then
	SRCROOT=$(dirname "$BINARY")
	if [ -f "$SRCROOT/samples/kprobes/kretprobe_example.ko" ]; then
		MODULE="$SRCROOT/samples/kprobes/kretprobe_example.ko"
	elif [ -f "$PWD/samples/kprobes/kretprobe_example.ko" ]; then
		MODULE="$PWD/samples/kprobes/kretprobe_example.ko"
	fi
fi

OUT=$(UML_KRETPROBE_MODULE="$MODULE" timeout 120 "$BINARY" \
	init="$GUEST_SCRIPT" mem="$MEM" \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw 2>&1)

# The guest emits several KPROBES_STRESS: progress lines and
# exactly one KPROBES_STRESS: {PASS,FAIL,SKIP} ... terminal line.
LINE=$(echo "$OUT" | grep -E '^KPROBES_STRESS: (PASS|FAIL|SKIP) ' | tail -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no KPROBES_STRESS PASS/FAIL/SKIP line in output"
	echo "$OUT" | grep '^KPROBES_STRESS:' | tail -10
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
