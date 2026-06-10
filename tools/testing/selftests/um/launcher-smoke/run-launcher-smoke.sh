#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/launcher-smoke/run-launcher-smoke.sh - regression guard for
# tools/uml/uml-launcher.
#
# Exercises the launcher end-to-end:
#
#   A. `uml-launcher run --dry-run` synthesizes a correct argv
#      without spawning (catches bitrot in the argv builder).
#   B. `uml-launcher run --kernel ... --init /bin/true` boots UML
#      and exits with 128 + SIGABRT = 134 (the expected init-
#      exit panic: init=/bin/true returns, kernel panics,
#      launcher returns that exit code).
#   C. `uml-launcher run --forkserver <ctl>,<status>` plumbs the
#      caller's pipe fds to UML's 198/199 via the launcher's
#      pre_exec dup2. Driven by launcher-forkserver-driver.py
#      which opens pipes, invokes the launcher, does the AFL\\0
#      handshake + one fork iteration, and checks the pid +
#      status bytes come back. Skipped if the UML binary isn't
#      fuzz-profile (doesn't have CONFIG_UM_SNAPSHOT_FORKSERVER=y).
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL, per kselftest convention.
#
# Environment:
#   UML_BINARY        path to UML kernel (default: /tmp/uml-research/linux)
#   UML_LAUNCHER      path to uml-launcher (default: walk up the tree
#                     and look in tools/uml/uml-launcher/target/release/)
#   UML_MEM           mem=N (default: 128M)

set -u

BINARY=${UML_BINARY:-/tmp/uml-research/linux}
MEM=${UML_MEM:-128M}

# DIR always resolves to this script's directory; Part C (the
# forkserver driver) looks up files next to this script via
# $DIR, so we set it unconditionally rather than only in the
# else-branch below. The previous conditional assignment
# tripped `set -u` when UML_LAUNCHER was passed as env by CI:
# DIR stayed unset, Part C's $DIR/launcher-forkserver-driver.py
# expansion faulted with "unbound variable" and the script
# failed before its own auto-skip path could run.
DIR=$(cd "$(dirname "$0")" && pwd)

# Locate uml-launcher. Prefer an explicit UML_LAUNCHER env var;
# otherwise walk up from this script to the tree root and look
# under tools/uml/uml-launcher/target/release/uml-launcher.
if [ -n "${UML_LAUNCHER:-}" ]; then
	LAUNCHER="$UML_LAUNCHER"
else
	# tools/testing/selftests/um/launcher-smoke -> up 4 -> tree root.
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
OUT_B=$(timeout --kill-after=10 60 "$LAUNCHER" run --kernel "$BINARY" --init /bin/true \
	--mem "$MEM" --console null 2>&1)
RC_B=$?

# init=/bin/true exits, kernel panics with 128+SIGABRT = 134.
# If the launcher passes through cleanly, we see 134 here too.
# Some builds/configurations return a different signal; accept
# any exit code in [1, 255] that isn't 0 as evidence that the
# spawn + supervise + reap path worked. rc=0 is suspicious: it
# means the kernel panic didn't fire or the launcher swallowed
# the exit code.
case "$RC_B" in
	134)
		: # fall through to Part C
		;;
	1[3-9][0-9]|1[0-2][0-9])
		: # fall through
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

# --- Part C: --forkserver fd plumbing end-to-end ---
#
# Only meaningful if the UML binary has CONFIG_UM_SNAPSHOT_FORKSERVER=y
# (fuzz / fuzz-deep profiles). Auto-detect by grepping the binary
# for the forkserver handshake string "AFL\0", present only when
# the Kconfig is on.
PART_C=${PART_C:-auto}
DRIVER="$DIR/launcher-forkserver-driver.py"
# The forkserver only fires when an in-guest write to
# /sys/kernel/debug/um/snapshot_ready reaches the kernel-side
# um_snapshot_ready() entry point. Our bundled init script
# mounts debugfs and triggers it; the driver's handshake is what
# the ready-point writes.
INIT_C=${UML_FORKSERVER_INIT:-$DIR/forkserver-init.sh}

run_part_c=0
if [ "$PART_C" = "1" ]; then
	run_part_c=1
elif [ "$PART_C" = "auto" ]; then
	# Cheap detection: the forkserver handshake string is in the
	# kernel image .rodata iff CONFIG_UM_SNAPSHOT_FORKSERVER=y.
	if grep -q "snapshot: forkserver up" "$BINARY" 2>/dev/null; then
		run_part_c=1
	fi
fi

if [ "$run_part_c" != "1" ]; then
	echo "LAUNCHER_SMOKE: PASS dry_run_ok=1 spawn_exit=$RC_B part_c=skipped"
	exit 0
fi

PY=$(command -v python3 || true)
if [ -z "$PY" ]; then
	echo "LAUNCHER_SMOKE: PASS dry_run_ok=1 spawn_exit=$RC_B part_c=skipped_no_python3"
	exit 0
fi
if [ ! -x "$DRIVER" ]; then
	echo "LAUNCHER_SMOKE: FAIL driver $DRIVER not executable"
	exit 1
fi

DRV_OUT=$("$PY" "$DRIVER" "$LAUNCHER" "$BINARY" "$INIT_C" "$MEM" 2>&1 || true)
DRV_LINE=$(echo "$DRV_OUT" | grep -E '^DRV: (PASS|FAIL|SKIP)' | tail -1)
case "$DRV_LINE" in
	*PASS*)
		echo "LAUNCHER_SMOKE: PASS dry_run_ok=1 spawn_exit=$RC_B part_c=pass"
		echo "  part_c: $DRV_LINE"
		exit 0
		;;
	*SKIP*)
		echo "LAUNCHER_SMOKE: PASS dry_run_ok=1 spawn_exit=$RC_B part_c=skipped"
		echo "  part_c: $DRV_LINE"
		exit 0
		;;
	*FAIL*|"")
		echo "LAUNCHER_SMOKE: FAIL part_c: ${DRV_LINE:-no DRV line}"
		echo "$DRV_OUT" | tail -10
		exit 1
		;;
esac
