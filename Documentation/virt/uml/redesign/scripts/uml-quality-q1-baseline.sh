#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# uml-quality-q1-baseline.sh — commit (or overwrite) the Q1 baseline
# from a run-log directory. Use this after uml-quality-q1.sh when
# you've verified the run is the correct expected state.
#
# Usage:
#   uml-quality-q1-baseline.sh <run-log-dir> [profile]
#
# The baseline is a per-profile, per-check text file under
#   Documentation/virt/uml/redesign/scripts/q1-baseline/<profile>/<check>.txt
# containing the sorted, path-normalized warning/error line set.
# uml-quality-q1-diff.sh compares future runs against this baseline.

set -u

RUN_DIR=${1:?usage: uml-quality-q1-baseline.sh <run-log-dir> [profile]}
PROFILE=${2:-research}
SRC=$(cd "$(dirname "$0")/../../../../.." && pwd)
BASELINE_DIR="$SRC/Documentation/virt/uml/redesign/scripts/q1-baseline/$PROFILE"

mkdir -p "$BASELINE_DIR"

for chk in gcc clang sparse smatch; do
	log="$RUN_DIR/$chk.log"
	out="$BASELINE_DIR/$chk.txt"
	if [ ! -f "$log" ]; then
		echo "$chk: no log ($log) — skipping"
		continue
	fi
	grep -E '\bwarning:|\berror:' "$log" |
		sed -E "s|$SRC/||g" |
		LC_ALL=C sort -u > "$out"
	printf '%s: baseline updated (%d lines) -> %s\n' \
		"$chk" "$(wc -l < "$out")" "$out"
done
