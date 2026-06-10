#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# bench-stress - concurrency / mm-churn workload. Runs mt-mini
# with strict_memset enabled, T=8 pthread workers x N iterations
# each. Captures wall-clock + STRICT_MEMSET_FAIL count + VERIFY_FAIL
# count.
#
# Tunables:
#   BENCH_SAMPLES   - sample count (default 3 - each takes ~5-15 s)
#   BENCH_WARMUP    - discard first M (default 1)
#   MT_MINI_BIN     - path to mt-mini binary
#   MT_THREADS      - pthread workers (default 8)
#
# Output:
#   BENCH: tier=stress sample=<N> elapsed_ms=<X> rc=<R> \
#          strict_fails=<S> verify_fails=<V>
#   BENCH_MEDIAN: tier=stress samples=<N> elapsed_ms_p50=<X> \
#                 elapsed_ms_p10=<Y> elapsed_ms_p90=<Z> \
#                 strict_fails_total=<S> verify_fails_total=<V>

set -u

MT=${MT_MINI_BIN:-/host/mt-mini}
THREADS=${MT_THREADS:-8}
SAMPLES=${BENCH_SAMPLES:-3}
WARMUP=${BENCH_WARMUP:-1}

if [ ! -x "$MT" ]; then
    echo "BENCH_FAIL: $MT not executable" >&2
    exit 1
fi

# Strict-memset on so we count both byte[0]=0 (STRICT_MEMSET_FAIL)
# and other (VERIFY_FAIL) corruption events.
export MT_STRICT_MEMSET=1

run_once() {
    OUT=$("$MT" "$THREADS" 2>&1)
    RC=$?
    SF=$(printf '%s' "$OUT" | grep -c STRICT_MEMSET_FAIL || true)
    VF=$(printf '%s' "$OUT" | grep -c VERIFY_FAIL       || true)
    printf '%s\n' "$OUT" | tail -2 >&2
    echo "$RC $SF $VF"
}

# Warm-up - discard
i=0
while [ $i -lt $WARMUP ]; do
    run_once >/dev/null 2>&1
    i=$((i + 1))
done

SAMPLES_FILE=$(mktemp)
TOTAL_SF=0
TOTAL_VF=0
i=0
while [ $i -lt $SAMPLES ]; do
    T0=$(date +%s%N)
    RES=$(run_once 2>/dev/null)
    T1=$(date +%s%N)
    set -- $RES
    RC=$1; SF=$2; VF=$3
    ELAPSED_MS=$(( (T1 - T0) / 1000000 ))
    echo "BENCH: tier=stress sample=$i elapsed_ms=$ELAPSED_MS rc=$RC strict_fails=$SF verify_fails=$VF"
    echo "$ELAPSED_MS" >> "$SAMPLES_FILE"
    TOTAL_SF=$((TOTAL_SF + SF))
    TOTAL_VF=$((TOTAL_VF + VF))
    i=$((i + 1))
done

sort -n "$SAMPLES_FILE" -o "$SAMPLES_FILE"
N=$(wc -l < "$SAMPLES_FILE")
P50_IDX=$(( (N + 1) / 2 ))
P10_IDX=1
P90_IDX=$N
# Use sed to grab the Nth line. busybox/awk-less environments are
# common inside minimal UML guests; shell + sed are always available.
P50=$(sed -n "${P50_IDX}p" "$SAMPLES_FILE")
P10=$(sed -n "${P10_IDX}p" "$SAMPLES_FILE")
P90=$(sed -n "${P90_IDX}p" "$SAMPLES_FILE")
rm -f "$SAMPLES_FILE"

echo "BENCH_MEDIAN: tier=stress samples=$N elapsed_ms_p50=$P50 elapsed_ms_p10=$P10 elapsed_ms_p90=$P90 strict_fails_total=$TOTAL_SF verify_fails_total=$TOTAL_VF"
