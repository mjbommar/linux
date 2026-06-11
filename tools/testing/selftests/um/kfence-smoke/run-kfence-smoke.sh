#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side launcher for the UML KFENCE report-generation smoke test.
#
# Boots a KFENCE-enabled UML guest, runs kfence-smoke.sh as init, and
# loads mm/kfence/kfence_test.ko. The guest requires at least one
# "BUG: KFENCE:" report in dmesg.
#
# Environment:
#   UML_BINARY  path to a research-profile UML binary
#               (default: /tmp/uml-research/linux)
#   UML_MEM     mem=N argument (default: 512M)
#   UML_KFENCE_MODULE  explicit kfence_test.ko path; else the
#               launcher searches common build-tree locations.
#   KFENCE_SMOKE_FILTER_GLOB  KUnit filter passed on the UML cmdline
#               (default: kfence.test_double_free)

set -u

BINARY=${UML_BINARY:-/tmp/uml-research/linux}
MEM=${UML_MEM:-512M}
DIR=$(cd "$(dirname "$0")" && pwd)
GUEST_SCRIPT="$DIR/kfence-smoke.sh"
MODULE=${UML_KFENCE_MODULE:-}
FILTER_GLOB=${KFENCE_SMOKE_FILTER_GLOB:-kfence.test_double_free}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi

if [ -z "$MODULE" ]; then
	SRCROOT=$(dirname "$BINARY")
	for cand in \
		"$SRCROOT/mm/kfence/kfence_test.ko" \
		"$PWD/mm/kfence/kfence_test.ko"; do
		if [ -f "$cand" ]; then
			MODULE="$cand"
			break
		fi
	done
fi

CMDLINE_MODULE_ARG=""
if [ -n "$MODULE" ]; then
	CMDLINE_MODULE_ARG="kfence_test_module=$MODULE"
fi

OUT=$(timeout --kill-after=10 90 "$BINARY" \
	init="$GUEST_SCRIPT" mem="$MEM" \
	$CMDLINE_MODULE_ARG \
	kunit.filter_glob="$FILTER_GLOB" \
	kfence.sample_interval=1 kfence.fault=report \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1)

LINE=$(echo "$OUT" | grep -E '^KFENCE_SMOKE: (PASS|FAIL|SKIP) ' | tail -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no KFENCE_SMOKE PASS/FAIL/SKIP line in output"
	echo "$OUT" | grep '^KFENCE_SMOKE:' | tail -10
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
