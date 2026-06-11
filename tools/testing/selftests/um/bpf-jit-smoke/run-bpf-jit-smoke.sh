#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side launcher for the UML BPF JIT smoke test.
#
# Boots a BPF-enabled UML guest, runs bpf-jit-smoke.sh as init, and
# expects the guest helper to load a minimal eBPF program and report a
# nonzero JITed program length.
#
# Environment:
#   UML_BINARY  path to a research-profile UML binary
#               (default: /tmp/uml-research/linux)
#   UML_MEM     mem=N argument (default: 512M)
#   BPF_JIT_SMOKE_HELPER  optional path to bpf-jit-smoke helper

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-/tmp/uml-research/linux}
MEM=${UML_MEM:-512M}
GUEST_SCRIPT="$DIR/bpf-jit-smoke.sh"

if [ -n "${BPF_JIT_SMOKE_HELPER:-}" ]; then
	HELPER=$BPF_JIT_SMOKE_HELPER
elif [ -n "${OUTPUT:-}" ] && [ -x "$OUTPUT/bpf-jit-smoke" ]; then
	HELPER="$OUTPUT/bpf-jit-smoke"
else
	HELPER="$DIR/bpf-jit-smoke"
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
	echo "SKIP: BPF JIT helper $HELPER not built; run 'make' in this dir" >&2
	exit 4
fi

OUT=$(timeout --kill-after=10 90 "$BINARY" \
	init="$GUEST_SCRIPT" bpf_jit_helper="$HELPER" mem="$MEM" \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1)

LINE=$(echo "$OUT" | grep -E '^BPF_JIT_SMOKE: (PASS|FAIL|SKIP) ' | tail -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no BPF_JIT_SMOKE PASS/FAIL/SKIP line in output"
	echo "$OUT" | grep '^BPF_JIT_SMOKE:' | tail -10
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
