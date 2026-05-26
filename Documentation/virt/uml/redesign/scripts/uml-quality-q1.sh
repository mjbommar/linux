#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# uml-quality-q1.sh — run the Phase Q1 quality bar from
# Documentation/virt/uml/redesign/05-validation/a-plus-quality-plan.md
# against one UML profile.
#
# Q1 says (paraphrased):
#   - GCC build green
#   - Clang (LLVM=1) build green
#   - Sparse clean on touched code
#   - Smatch reviewed on touched files
#   - checkpatch clean (run separately, per-patch — not here)
#
# This script runs the four build/analysis checks. Output:
#   - per-check stdout/stderr in $LOGDIR/<check>.log
#   - one-line "OK" / "FAIL" summary on stdout per check
#   - exit 0 if all four pass, non-zero otherwise
#
# Usage:
#   uml-quality-q1.sh [profile]            # default: research
#   UML_Q1_OUT=/tmp/uml-q1   uml-quality-q1.sh research
#   UML_Q1_SKIP="smatch sparse" uml-quality-q1.sh research
#
# Each check uses its own clean O= tree under $UML_Q1_OUT so the
# tools do not contaminate each other's caches and so any check
# can be rerun in isolation.
#
# Smatch is invoked via the kernel's standard CHECK= mechanism;
# requires $HOME/src/uml-tools/smatch/smatch (see project memory).

set -u

PROFILE=${1:-research}
SRC=$(cd "$(dirname "$0")/../../../../.." && pwd)
LOGDIR=${UML_Q1_OUT:-/tmp/uml-q1}/$(date +%Y%m%d-%H%M%S)-$PROFILE
SMATCH=${SMATCH:-$HOME/src/uml-tools/smatch/smatch}
SKIP=${UML_Q1_SKIP:-}

JOBS=${UML_Q1_JOBS:-4}

mkdir -p "$LOGDIR"
cd "$SRC"

skipped() {
	for s in $SKIP; do
		[ "$s" = "$1" ] && return 0
	done
	return 1
}

# Configure a fresh O= tree for $PROFILE. If a check needs additional
# Kconfig (e.g. clang switches), prepare a separate tree.
prepare() {
	local out=$1
	rm -rf "$out"
	mkdir -p "$out"
	make ARCH=um O="$out" "uml/$PROFILE" >"$LOGDIR/$(basename $out).cfg.log" 2>&1
}

run_check() {
	local name=$1
	local outdir=$2
	shift 2
	local log="$LOGDIR/$name.log"
	local t0=$(date +%s)

	if skipped "$name"; then
		printf '%-10s SKIP\n' "$name"
		return 0
	fi

	"$@" >"$log" 2>&1
	local rc=$?
	local t1=$(date +%s)
	local elapsed=$((t1-t0))

	# Common warning detection. Case-insensitive so modpost's
	# uppercase `WARNING:` lines are counted too — missing those
	# left four init_backend section-mismatch warnings latent in
	# the baseline until a separate review caught them.
	local warns errors
	warns=$(grep -ciE '\bwarning:|\bwarn(ing)?:' "$log")
	errors=$(grep -cE '\berror:|\bError:|\bERROR\b' "$log" | head -1)

	if [ $rc -eq 0 ]; then
		printf '%-10s OK   %4ds   warnings=%-5s errors=%-5s log=%s\n' \
			"$name" "$elapsed" "$warns" "$errors" "$log"
	else
		printf '%-10s FAIL %4ds   warnings=%-5s errors=%-5s log=%s (rc=%d)\n' \
			"$name" "$elapsed" "$warns" "$errors" "$log" "$rc"
	fi
	return $rc
}

# ---------------------------------------------------------------------
# Check 1: GCC baseline build
# ---------------------------------------------------------------------
gcc_build() {
	local out="$LOGDIR/build-gcc"
	prepare "$out"
	make ARCH=um O="$out" -j"$JOBS"
}

# ---------------------------------------------------------------------
# Check 2: Clang (LLVM=1) build
# ---------------------------------------------------------------------
clang_build() {
	local out="$LOGDIR/build-clang"
	prepare "$out"
	make ARCH=um O="$out" LLVM=1 -j"$JOBS"
}

# ---------------------------------------------------------------------
# Check 3: Sparse (CHECK=sparse via C=1)
#
# Sparse runs alongside gcc; we use a dedicated O= tree to avoid
# polluting the gcc-only cache.
# ---------------------------------------------------------------------
sparse_check() {
	local out="$LOGDIR/build-sparse"
	prepare "$out"
	# C=2 = check every file, including unchanged ones (useful for
	# baseline). C=1 = touched only — we use C=2 here for a full sweep.
	make ARCH=um O="$out" CHECK="sparse" C=2 -j"$JOBS"
}

# ---------------------------------------------------------------------
# Check 4: Smatch
#
# Same C= mechanism but pointing CHECK at smatch with kernel mode.
# ---------------------------------------------------------------------
smatch_check() {
	local out="$LOGDIR/build-smatch"
	prepare "$out"
	if [ ! -x "$SMATCH" ]; then
		echo "smatch binary missing: $SMATCH" >&2
		return 2
	fi
	make ARCH=um O="$out" CHECK="$SMATCH -p=kernel" C=2 -j"$JOBS"
}

# ---------------------------------------------------------------------
# Run them all. Continue on failure so we get a complete picture.
# ---------------------------------------------------------------------
echo "Q1 quality bar — profile=$PROFILE  src=$SRC  logs=$LOGDIR"
echo

any_fail=0
run_check gcc    "$LOGDIR/build-gcc"    gcc_build    || any_fail=1
run_check clang  "$LOGDIR/build-clang"  clang_build  || any_fail=1
run_check sparse "$LOGDIR/build-sparse" sparse_check || any_fail=1
run_check smatch "$LOGDIR/build-smatch" smatch_check || any_fail=1

echo
echo "Logs: $LOGDIR"
exit $any_fail
