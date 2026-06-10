#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run-bench.sh - host-side wrapper for the umlctl bench tiers.
#
# Pattern after run-perf-py-startup.sh: boot UML with the tier's
# umlctl toml, capture the BENCH_MEDIAN line, parse it, compute
# kvm-v2:seccomp ratio, append to scoreboard, exit non-zero on
# regression vs threshold (if set).
#
# Usage:
#   bash tools/testing/selftests/um/bench/run-bench.sh \
#        --tier <micro|py|stress> \
#        --kernel </path/to/uml/linux> \
#        [--backends "kvm-v2 seccomp"]    # default both
#        [--out <scoreboard.jsonl>]
#        [--max-ratio 1.2]                # fail if v2/seccomp > N
#
# Output:
#   - One BENCH_MEDIAN line per (tier, backend) to stdout
#   - One JSON row per (tier, backend) appended to scoreboard
#   - Final BENCH_RATIO line + exit code
#
# Exit codes:
#   0  = PASS (ratio <= max-ratio for all backends; no STRICT fails)
#   1  = FAIL (ratio exceeds threshold OR strict fails > 0)
#   4  = SKIP (kernel binary not found)

set -u

TIER=""
KERNEL=""
BACKENDS="kvm-v2 seccomp"
OUT=""
MAX_RATIO=""
UMLCTL=${UMLCTL:-tools/uml/uml-launcher/target/release/umlctl}

while [ $# -gt 0 ]; do
    case "$1" in
        --tier)        TIER=$2; shift 2 ;;
        --kernel)      KERNEL=$2; shift 2 ;;
        --backends)    BACKENDS=$2; shift 2 ;;
        --out)         OUT=$2; shift 2 ;;
        --max-ratio)   MAX_RATIO=$2; shift 2 ;;
        --help|-h)
            sed -n '4,28p' "$0" | sed 's/^# *//'
            exit 0
            ;;
        *) echo >&2 "unknown arg: $1"; exit 2 ;;
    esac
done

[ -z "$TIER" ]   && { echo >&2 "ERR: --tier required"; exit 2; }
[ -z "$KERNEL" ] && { echo >&2 "ERR: --kernel required"; exit 2; }

if [ ! -x "$KERNEL" ]; then
    echo "BENCH: SKIP kernel=$KERNEL not found"
    exit 4
fi

BENCH_DIR=$(cd "$(dirname "$0")" && pwd)
TOML="$BENCH_DIR/bench-$TIER.toml"

if [ ! -f "$TOML" ]; then
    echo >&2 "ERR: $TOML not found (tier=$TIER)"
    exit 2
fi

if [ ! -x "$UMLCTL" ]; then
    echo >&2 "ERR: umlctl not built ($UMLCTL). Run: cargo build --release -p uml-launcher"
    exit 2
fi

OUT=${OUT:-tools/testing/selftests/um/scoreboard.jsonl}
HOST=$(hostname)
COMMIT=$(git -C "$BENCH_DIR/../../../.." rev-parse --short HEAD 2>/dev/null || echo unknown)
CPU=$(grep "^model name" /proc/cpuinfo | head -1 | sed 's/.*: //')
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
TS=$(date -Iseconds -u)

declare -A MEDIANS

run_one_backend() {
    local backend=$1
    echo "==> tier=$TIER backend=$backend"

    local instance="bench-$TIER-$backend"
    "$UMLCTL" stop "$instance" >/dev/null 2>&1 || true
    "$UMLCTL" rm   "$instance" >/dev/null 2>&1 || true

    # Materialize a per-backend copy: rewrite both instance.name
    # AND kernel.backend (the backend field is parsed into an enum
    # before env-substitution, so a ${VAR} placeholder there is
    # rejected by umlctl up - sed-substitute the placeholder
    # instead).
    local tmp_toml
    tmp_toml=$(mktemp /tmp/bench-${TIER}-${backend}-XXXXXX.toml)
    sed -e "s/name = \"bench-${TIER}\"/name = \"bench-${TIER}-${backend}\"/" \
        -e "s/backend = \"BENCH_BACKEND_PLACEHOLDER\"/backend = \"${backend}\"/" \
        "$TOML" > "$tmp_toml"

    UML_KERNEL="$KERNEL" UML_BACKEND="$backend" BENCH_DIR="$BENCH_DIR" \
        "$UMLCTL" up --file "$tmp_toml" \
        --wait-for "BENCH_RUN_DONE|panic" --wait-timeout 180 \
        2>&1 | tail -3

    rm -f "$tmp_toml"

    local logdir
    logdir=$(ls -td "$HOME/.local/state/uml/runs/"*/ 2>/dev/null | head -1)
    local logfile="${logdir}init.log"
    [ -f "$logfile" ] || { echo "BENCH_FAIL: no init.log for $backend"; return 1; }

    local median_line
    median_line=$(grep -E "^BENCH_MEDIAN:|^PERF_GETPID:" "$logfile" | head -1)
    [ -z "$median_line" ] && { echo "BENCH_FAIL: no BENCH_MEDIAN/PERF_GETPID for $backend"; return 1; }

    echo "$median_line"

    local p50
    # micro tier emits PERF_GETPID with cyc_per_call
    p50=$(echo "$median_line" | sed -nE 's/.*cyc_per_call=([0-9.]+).*/\1/p')
    # py/stress tiers emit BENCH_MEDIAN with elapsed_ms_p50
    [ -z "$p50" ] && p50=$(echo "$median_line" | sed -nE 's/.*elapsed_ms_p50=([0-9.]+).*/\1/p')
    MEDIANS[$backend]=$p50

    local strict_fails verify_fails
    strict_fails=$(echo "$median_line" | sed -nE 's/.*strict_fails_total=([0-9]+).*/\1/p')
    verify_fails=$(echo "$median_line" | sed -nE 's/.*verify_fails_total=([0-9]+).*/\1/p')
    strict_fails=${strict_fails:-0}
    verify_fails=${verify_fails:-0}

    if [ -n "$OUT" ]; then
        printf '{"ts":"%s","gate":"bench-%s","backend":"%s","commit":"%s","host":"%s","cpu":"%s","governor":"%s","metrics":{"p50":%s,"strict_fails":%s,"verify_fails":%s}}\n' \
            "$TS" "$TIER" "$backend" "$COMMIT" "$HOST" "$CPU" "$GOV" \
            "$p50" "$strict_fails" "$verify_fails" \
            >> "$OUT"
    fi

    if [ "$strict_fails" -gt 0 ] || [ "$verify_fails" -gt 0 ]; then
        echo "BENCH_FAIL: $backend strict=$strict_fails verify=$verify_fails"
        return 1
    fi
    return 0
}

EXIT=0
for backend in $BACKENDS; do
    if ! run_one_backend "$backend"; then
        EXIT=1
    fi
done

# Compute ratios if both kvm-v2 and seccomp were measured
KV=${MEDIANS[kvm-v2]:-}
SC=${MEDIANS[seccomp]:-}
if [ -n "$KV" ] && [ -n "$SC" ]; then
    RATIO=$(awk -v a="$KV" -v b="$SC" 'BEGIN{ if (b>0) printf "%.3f\n", a/b; else print "NaN" }')
    echo "BENCH_RATIO: tier=$TIER kvm_v2_p50=$KV seccomp_p50=$SC ratio_v2_over_seccomp=$RATIO"
    if [ -n "$MAX_RATIO" ]; then
        if awk -v r="$RATIO" -v m="$MAX_RATIO" 'BEGIN{ exit !(r > m) }'; then
            echo "BENCH_FAIL: ratio $RATIO exceeds max $MAX_RATIO"
            EXIT=1
        fi
    fi
fi

exit $EXIT
