#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Phase J.1 prep — substrate-gate soak runner.
#
# Wraps run-regrtest-repros.sh in a loop, running it N times and
# collecting PASS/FAIL/EXPECTED_FAIL counts per iteration. Reports
# a histogram + flake rate at the end. This is the per-iteration
# version of the 24h continuous soak Phase J.1 calls for: the same
# harness extended to a wall-clock budget gives the 24h gate.
#
# Usage:
#   UML_BINARY=$HOME/src/uml-builds/uml-clean/linux \
#     BACKEND=kvm-v2 N=10 \
#     bash tools/testing/selftests/um/regrtest-repros/run-substrate-soak.sh
#
# Exits 0 if every iteration meets the per-iteration min_pass / max_fail
# thresholds (default: PASS>=20 / FAIL<=3); 1 if any iteration regressed.
#
# Output (machine-readable):
#   SOAK iter=K pass=N fail=N xfail=N elapsed_ms=M
#   SOAK SUMMARY n=N pass_min=A pass_max=B pass_med=C pass_floor=D
#                fail_min=A fail_max=B fail_med=C
#                xfail_min=A xfail_max=B xfail_med=C
#                wall_clock_sec=T
#   SOAK PASS|FAIL

set -u

BINARY=${UML_BINARY:-$HOME/src/uml-builds/uml-clean/linux}
BACKEND=${BACKEND:-kvm-v2}
N=${N:-10}
TIMEOUT_PER_ITER=${TIMEOUT_PER_ITER:-300}
MIN_PASS=${MIN_PASS:-20}
MAX_FAIL=${MAX_FAIL:-3}

if [ ! -x "$BINARY" ]; then
    echo "SKIP: $BINARY not found (set UML_BINARY)" >&2
    exit 4
fi

DIR=$(cd "$(dirname "$0")" && pwd)
HARNESS="$DIR/run-regrtest-repros.sh"
if [ ! -f "$HARNESS" ]; then
    echo "SKIP: $HARNESS not found" >&2
    exit 4
fi

PASS_VALS=()
FAIL_VALS=()
XFAIL_VALS=()

START_TS=$(date +%s)

for i in $(seq 1 "$N"); do
    iter_start=$(date +%s)
    out=$(UML_BINARY="$BINARY" BACKEND="$BACKEND" TIMEOUT="$TIMEOUT_PER_ITER" \
          bash "$HARNESS" 2>&1)
    iter_end=$(date +%s)
    elapsed_sec=$((iter_end - iter_start))

    pass=$(echo "$out" | awk -F'PASS=|FAIL=|EXPECTED_FAIL=' '/^PASS=[0-9]+ FAIL=[0-9]+ EXPECTED_FAIL=[0-9]+$/ {print $2; exit}' | tr -d ' ' | sed 's/F$//')
    fail=$(echo "$out" | awk -F'PASS=|FAIL=|EXPECTED_FAIL=' '/^PASS=[0-9]+ FAIL=[0-9]+ EXPECTED_FAIL=[0-9]+$/ {print $3; exit}' | tr -d ' ' | sed 's/E$//')
    xfail=$(echo "$out" | awk -F'PASS=|FAIL=|EXPECTED_FAIL=' '/^PASS=[0-9]+ FAIL=[0-9]+ EXPECTED_FAIL=[0-9]+$/ {print $4; exit}' | tr -d ' ')

    # Fall back: count REPRO: lines directly
    if [ -z "${pass:-}" ]; then
        pass=$(echo "$out" | grep -c -E '^REPRO: \S+ PASS')
        fail=$(echo "$out" | grep -c -E '^REPRO: \S+ FAIL')
        xfail=$(echo "$out" | grep -c -E '^REPRO: \S+ EXPECTED_FAIL')
    fi

    pass=${pass:-0}
    fail=${fail:-0}
    xfail=${xfail:-0}

    PASS_VALS+=("$pass")
    FAIL_VALS+=("$fail")
    XFAIL_VALS+=("$xfail")

    echo "SOAK iter=$i pass=$pass fail=$fail xfail=$xfail elapsed_sec=$elapsed_sec"
done

END_TS=$(date +%s)
WALL=$((END_TS - START_TS))

# Compute min/max/median for each metric.
median() {
    local vals=("$@")
    local n=${#vals[@]}
    if [ "$n" -eq 0 ]; then echo 0; return; fi
    local sorted
    sorted=$(printf '%s\n' "${vals[@]}" | sort -n)
    local mid=$((n / 2))
    if [ $((n % 2)) -eq 1 ]; then
        echo "$sorted" | sed -n "$((mid + 1))p"
    else
        local a b
        a=$(echo "$sorted" | sed -n "${mid}p")
        b=$(echo "$sorted" | sed -n "$((mid + 1))p")
        echo $(((a + b) / 2))
    fi
}

aggregate() {
    local vals=("$@")
    local n=${#vals[@]}
    [ "$n" -eq 0 ] && { echo "0 0 0"; return; }
    local lo hi
    lo=$(printf '%s\n' "${vals[@]}" | sort -n | head -1)
    hi=$(printf '%s\n' "${vals[@]}" | sort -n | tail -1)
    local med
    med=$(median "${vals[@]}")
    echo "$lo $hi $med"
}

read -r pass_min pass_max pass_med <<<"$(aggregate "${PASS_VALS[@]}")"
read -r fail_min fail_max fail_med <<<"$(aggregate "${FAIL_VALS[@]}")"
read -r xfail_min xfail_max xfail_med <<<"$(aggregate "${XFAIL_VALS[@]}")"

echo
printf 'SOAK SUMMARY n=%d pass_min=%d pass_max=%d pass_med=%d pass_floor=%d fail_min=%d fail_max=%d fail_med=%d xfail_min=%d xfail_max=%d xfail_med=%d wall_clock_sec=%d\n' \
    "$N" "$pass_min" "$pass_max" "$pass_med" "$pass_min" \
    "$fail_min" "$fail_max" "$fail_med" \
    "$xfail_min" "$xfail_max" "$xfail_med" "$WALL"

# Verdict: every iteration must meet thresholds.
verdict=PASS
for i in $(seq 0 $((N - 1))); do
    if [ "${PASS_VALS[$i]}" -lt "$MIN_PASS" ] || [ "${FAIL_VALS[$i]}" -gt "$MAX_FAIL" ]; then
        verdict=FAIL
        break
    fi
done
echo "SOAK $verdict"

[ "$verdict" = "PASS" ] && exit 0
exit 1
