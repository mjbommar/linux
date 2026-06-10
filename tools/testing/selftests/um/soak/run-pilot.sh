#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# UML pilot soak: multi-workload realistic stress.
#
# For each workload (stress-ng, cpython-soak, kbuild-tiny):
#   - Materialize a per-backend Umlfile from the .template
#   - Run umlctl gate-loop with W workers x M iters
#   - Capture summary
#
# Usage: run-pilot.sh [workload-name] [W] [M] [timeout-sec]
#        ./run-pilot.sh stress-ng 4 20 90
#        ./run-pilot.sh all 4 10 600    # default: all 3 workloads

set -u

KERNEL_DEFAULT="/home/mjbommar/src/uml-builds/uml-smp-t41fix/linux"
KERNEL="${UML_KERNEL:-$KERNEL_DEFAULT}"
UMLCTL="${UMLCTL:-/home/mjbommar/bench-bundle/bin/umlctl}"

WORKLOAD="${1:-all}"
W="${2:-2}"           # default 2 to keep CPU temp under control
M="${3:-10}"
TIMEOUT="${4:-90}"
COOLDOWN="${COOLDOWN:-30}"  # seconds between workloads to let CPU cool

# Read max CPU package temp from /sys/class/thermal. If above
# THERMAL_PAUSE_C, sleep until it drops below THERMAL_RESUME_C.
# Default thresholds chosen for Zen 4 (Tjmax 95C, throttle ~110C).
THERMAL_PAUSE_C="${THERMAL_PAUSE_C:-88}"
THERMAL_RESUME_C="${THERMAL_RESUME_C:-75}"

read_max_temp_c() {
    local maxv=0
    # thermal_zone covers acpitz; misses k10temp/coretemp on AMD/Intel.
    # Walk hwmon for any tempN_input under k10temp/coretemp/zenpower
    # which is where the actual CPU package + Tctl are exposed.
    for nf in /sys/class/hwmon/hwmon*/name; do
        [ -r "$nf" ] || continue
        local n=$(cat "$nf" 2>/dev/null)
        case "$n" in
            k10temp|coretemp|zenpower|cpu_thermal) ;;
            *) continue ;;
        esac
        local d=$(dirname "$nf")
        for t in "$d"/temp*_input; do
            [ -r "$t" ] || continue
            local v=$(cat "$t" 2>/dev/null || echo 0)
            v=$((v / 1000))
            [ "$v" -gt "$maxv" ] && maxv=$v
        done
    done
    # Fallback: thermal_zone if hwmon CPU sensors unavailable.
    if [ "$maxv" -eq 0 ]; then
        for f in /sys/class/thermal/thermal_zone*/temp; do
            [ -r "$f" ] || continue
            local v=$(cat "$f" 2>/dev/null || echo 0)
            v=$((v / 1000))
            [ "$v" -gt "$maxv" ] && maxv=$v
        done
    fi
    echo "$maxv"
}

thermal_check() {
    local t=$(read_max_temp_c)
    if [ "$t" -ge "$THERMAL_PAUSE_C" ]; then
        echo "  [thermal] $t C >= ${THERMAL_PAUSE_C}C - pausing until <= ${THERMAL_RESUME_C}C"
        while [ "$(read_max_temp_c)" -gt "$THERMAL_RESUME_C" ]; do
            sleep 5
        done
        echo "  [thermal] resumed at $(read_max_temp_c)C"
    fi
}

OUT_BASE="/tmp/soak-pilot-$(date +%s)"
mkdir -p "$OUT_BASE"

SOAK_DIR="$(cd "$(dirname "$0")" && pwd)"

run_one() {
    local workload="$1"
    local backend="$2"
    local timeout_sec="$3"

    local tmpl="$SOAK_DIR/${workload}.toml.template"
    [ -f "$tmpl" ] || { echo "ERR: no template $tmpl"; return 2; }

    local toml="/tmp/soak-${workload}-${backend}.toml"
    sed -e "s|{{KERNEL}}|$KERNEL|g" -e "s|{{BACKEND}}|$backend|g" \
        -e "s|{{SOAK_DIR}}|$SOAK_DIR|g" \
        "$tmpl" > "$toml"

    local out_dir="$OUT_BASE/${workload}-${backend}"
    mkdir -p "$out_dir"

    thermal_check
    local t_pre=$(read_max_temp_c)
    echo "===== $workload backend=$backend W=$W M=$M timeout=${timeout_sec}s temp=${t_pre}C ====="
    local t0=$(date +%s)
    "$UMLCTL" gate loop -f "$toml" -W "$W" -M "$M" \
        --timeout "$timeout_sec" --out "$out_dir" 2>&1 | tee "$out_dir/_loop.log" | tail -20
    local t1=$(date +%s)
    local elapsed=$((t1 - t0))
    local t_post=$(read_max_temp_c)
    echo "  [thermal] post-run ${t_post}C; cooling ${COOLDOWN}s"
    sleep "$COOLDOWN"

    # Categorize
    local total=0 pass=0 nonpass=0
    for f in "$out_dir"/p0_default/*/run-*.log; do
        [ -f "$f" ] || continue
        total=$((total + 1))
        if grep -q "REPRO_DONE rc=0" "$f"; then
            pass=$((pass + 1))
        else
            nonpass=$((nonpass + 1))
        fi
    done

    echo "----- $workload/$backend: $pass/$total PASS (elapsed=${elapsed}s) -----"
    echo "$workload,$backend,$total,$pass,$nonpass,$elapsed" \
        >> "$OUT_BASE/_summary.csv"
}

case "$WORKLOAD" in
    all)
        WORKLOADS=(memcheck iocheck stress-ng cpython-soak kbuild-tiny)
        TIMEOUTS=(90 90 120 360 600)
        ;;
    short)  # the 3 fast ones; for smoke testing the harness
        WORKLOADS=(memcheck iocheck stress-ng)
        TIMEOUTS=(90 90 120)
        ;;
    stress-ng) WORKLOADS=(stress-ng); TIMEOUTS=("$TIMEOUT") ;;
    cpython-soak) WORKLOADS=(cpython-soak); TIMEOUTS=("$TIMEOUT") ;;
    django-loopback-none) WORKLOADS=(django-loopback-none); TIMEOUTS=("$TIMEOUT") ;;
    kbuild-tiny) WORKLOADS=(kbuild-tiny); TIMEOUTS=("$TIMEOUT") ;;
    memcheck) WORKLOADS=(memcheck); TIMEOUTS=("$TIMEOUT") ;;
    iocheck) WORKLOADS=(iocheck); TIMEOUTS=("$TIMEOUT") ;;
    *) echo "unknown workload: $WORKLOAD"; exit 2 ;;
esac

echo "workload,backend,total,pass,nonpass,elapsed_sec" > "$OUT_BASE/_summary.csv"

for i in "${!WORKLOADS[@]}"; do
    wl="${WORKLOADS[$i]}"
    to="${TIMEOUTS[$i]}"
    for backend in kvm-v2 seccomp; do
        run_one "$wl" "$backend" "$to"
    done
done

echo
echo "==================== SOAK PILOT SUMMARY ===================="
column -t -s, "$OUT_BASE/_summary.csv"
echo
echo "Output: $OUT_BASE"
