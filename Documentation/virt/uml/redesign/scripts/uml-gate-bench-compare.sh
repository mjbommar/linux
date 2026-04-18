#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# uml-gate-bench-compare.sh — compare a fresh run of uml-gate-bench.sh
# against the checked-in baseline.
#
# Usage: uml-gate-bench-compare.sh [baseline.json]
#
# Default baseline:
# Documentation/virt/uml/redesign/02-workstreams/B-static-key-hot-paths/
# notes/bench-baseline.json
#
# Exits 0 if current numbers are within ±ceiling of baseline (default
# 10%); exits 1 otherwise. Intended for CI as a guard against silent
# regressions in gate cost.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BASELINE=${1:-"$DIR/../02-workstreams/B-static-key-hot-paths/notes/bench-baseline.json"}
CEILING_PCT=${UML_BENCH_CEILING_PCT:-10}

if [ ! -f "$BASELINE" ]; then
	echo "error: baseline $BASELINE not found" >&2
	exit 1
fi

CURRENT=$("$DIR/uml-gate-bench.sh")
if [ -z "$CURRENT" ]; then
	echo "error: current run produced no output" >&2
	exit 1
fi

# Use python to parse the JSON and compare — portable enough for CI.
python3 - <<PY
import json
import sys

base = json.loads(open("$BASELINE").read())["results_ns_per_call_x1000"]
cur  = json.loads("""$CURRENT""")["results_ns_per_call_x1000"]
ceil_pct = $CEILING_PCT

fail = 0
for site, bvals in base.items():
    if site not in cur:
        print(f"FAIL {site}: missing in current run")
        fail = 1
        continue
    cvals = cur[site]
    for state in ("off", "on"):
        b = bvals[state]
        c = cvals[state]
        pct = 100.0 * (c - b) / b if b else 0
        status = "OK  "
        if abs(pct) > ceil_pct:
            status = "FAIL"
            fail = 1
        print(f"{status} {site:<18} {state:<4} baseline={b:>6} current={c:>6} delta={pct:+.1f}%")

sys.exit(fail)
PY
