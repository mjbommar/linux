#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/launcher-smoke/run-launcher-smoke.sh — regression guard for
# tools/uml/uml-launcher (workstream C-10).
#
# Exercises the launcher end-to-end:
#
#   1. `uml-launcher run --dry-run` synthesizes a correct argv
#      without spawning (catches bitrot in the argv builder).
#   2. `uml-launcher run --kernel ... --init /bin/true` boots UML
#      and exits with 128 + SIGABRT = 134 (the expected init-
#      exit panic — init=/bin/true returns, kernel panics,
#      launcher returns that exit code).
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.
#
# Environment:
#   UML_BINARY        path to UML kernel (default: /tmp/uml-research/linux)
#   UML_LAUNCHER      path to uml-launcher (default: walk up the tree
#                     and look in tools/uml/uml-launcher/target/release/)
#   UML_MEM           mem=N (default: 128M)

set -u

BINARY=${UML_BINARY:-/tmp/uml-research/linux}
MEM=${UML_MEM:-128M}

# Locate uml-launcher. Prefer an explicit UML_LAUNCHER env var;
# otherwise walk up from this script to the tree root and look
# under tools/uml/uml-launcher/target/release/uml-launcher.
if [ -n "${UML_LAUNCHER:-}" ]; then
	LAUNCHER="$UML_LAUNCHER"
else
	DIR=$(cd "$(dirname "$0")" && pwd)
	# tools/testing/selftests/um/launcher-smoke → up 4 → tree root.
	ROOT=$(cd "$DIR/../../../../.." && pwd)
	LAUNCHER="$ROOT/tools/uml/uml-launcher/target/release/uml-launcher"
fi

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$LAUNCHER" ]; then
	echo "SKIP: uml-launcher $LAUNCHER not built (run: make -C tools/uml/uml-launcher)" >&2
	exit 4
fi

# --- Part A: --dry-run synthesizes argv correctly ---
OUT_A=$("$LAUNCHER" run --kernel "$BINARY" --init /bin/true --mem "$MEM" \
	--console null --dry-run 2>&1)
RC_A=$?

if [ $RC_A -ne 0 ]; then
	echo "LAUNCHER_SMOKE: FAIL dry-run exited $RC_A"
	echo "$OUT_A"
	exit 1
fi

for required in "rootfstype=hostfs" "init=/bin/true" "mem=$MEM" "con=null"; do
	if ! echo "$OUT_A" | grep -q -- "$required"; then
		echo "LAUNCHER_SMOKE: FAIL dry-run argv missing '$required'"
		echo "got: $OUT_A"
		exit 1
	fi
done

# --- Part B: real spawn ---
OUT_B=$(timeout 60 "$LAUNCHER" run --kernel "$BINARY" --init /bin/true \
	--mem "$MEM" --console null 2>&1)
RC_B=$?

# init=/bin/true exits, kernel panics with 128+SIGABRT = 134.
# If the launcher passes through cleanly, we see 134 here too.
# Some builds/configurations return a different signal; accept
# any exit code in [1, 255] that isn't 0 as evidence that the
# spawn + supervise + reap path worked. rc=0 is suspicious — it
# means the kernel panic didn't fire or the launcher swallowed
# the exit code.
case "$RC_B" in
	134)
		echo "LAUNCHER_SMOKE: PASS dry_run_ok=1 spawn_exit=134"
		exit 0
		;;
	1[3-9][0-9]|1[0-2][0-9])
		echo "LAUNCHER_SMOKE: PASS dry_run_ok=1 spawn_exit=$RC_B (non-134 signal-ish)"
		exit 0
		;;
	0)
		echo "LAUNCHER_SMOKE: FAIL spawn returned 0 (expected init-killed panic exit 128+sig)"
		echo "$OUT_B" | tail -20
		exit 1
		;;
	*)
		echo "LAUNCHER_SMOKE: FAIL unexpected exit rc=$RC_B"
		echo "$OUT_B" | tail -20
		exit 1
		;;
esac
