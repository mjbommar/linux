#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# uml-quality-q1-diff.sh — diff a fresh Q1 run against a checked-in
# baseline of expected warnings.
#
# The baseline lives at
#   Documentation/virt/uml/redesign/scripts/q1-baseline/<profile>/<check>.txt
# Each baseline file is the expected `warning:` / `error:` line set
# (sorted, dedup'd) for that profile×check combination. A run is
# "clean" if its produced warning set is a subset of the baseline.
# A "regression" is any new warning not present in the baseline; an
# "improvement" is a baseline warning that's now absent.
#
# Usage:
#   uml-quality-q1-diff.sh <run-log-dir> [profile]
#
# Exit codes:
#   0 — run matches baseline
#   1 — regressions detected
#   2 — improvements detected (baseline can be lowered; not failure)
#   3 — both regressions and improvements

set -u

RUN_DIR=${1:?usage: uml-quality-q1-diff.sh <run-log-dir> [profile]}
PROFILE=${2:-research}
SRC=$(cd "$(dirname "$0")/../../../../.." && pwd)
BASELINE_DIR="$SRC/Documentation/virt/uml/redesign/scripts/q1-baseline/$PROFILE"

if [ ! -d "$BASELINE_DIR" ]; then
	echo "no baseline at $BASELINE_DIR — first time?" >&2
	echo "create one with: uml-quality-q1-baseline.sh $RUN_DIR $PROFILE" >&2
	exit 1
fi

# Extract the warning/error lines from a build log, normalize away
# absolute source paths so the diff is portable. Case-insensitive
# so modpost's uppercase `WARNING:` lines are in the baseline too
# (see uml-quality-q1.sh note).
extract() {
	local log=$1
	[ -f "$log" ] || return 0
	grep -iE '\bwarning:|\berror:' "$log" |
		sed -E "s|$SRC/||g" |
		LC_ALL=C sort -u
}

regressions=0
improvements=0

for chk in gcc clang sparse smatch; do
	cur=$(extract "$RUN_DIR/$chk.log")
	base=""
	[ -f "$BASELINE_DIR/$chk.txt" ] && base=$(cat "$BASELINE_DIR/$chk.txt")

	new=$(LC_ALL=C comm -23 <(echo "$cur") <(echo "$base"))
	gone=$(LC_ALL=C comm -13 <(echo "$cur") <(echo "$base"))

	if [ -z "$new" ] && [ -z "$gone" ]; then
		echo "$chk: clean (matches baseline)"
		continue
	fi

	if [ -n "$new" ]; then
		printf '%s: REGRESSION (%d new):\n' "$chk" "$(echo "$new" | wc -l)"
		echo "$new" | sed 's/^/    + /'
		regressions=1
	fi
	if [ -n "$gone" ]; then
		printf '%s: improvement (%d gone):\n' "$chk" "$(echo "$gone" | wc -l)"
		echo "$gone" | sed 's/^/    - /'
		improvements=1
	fi
done

case "$regressions$improvements" in
	00) exit 0 ;;
	10) exit 1 ;;
	01) exit 2 ;;
	11) exit 3 ;;
esac
