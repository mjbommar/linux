#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# H.1 perf gate: minimal Python startup wall-clock under each
# backend, measured via in-kernel printk timestamps.
#
# Boots UML with init=<shell wrapper that runs python3 -c "import
# math; print(math.pi)">, captures the wall-clock between
# "Run <init> as init process" and "Kernel panic - not syncing:
# Attempted to kill init!" lines. That window is init shell +
# python startup + sync; pre-init kernel boot is identical across
# backends and excluded from the comparison.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.
# PASS gate: kvm-v2 / seccomp ratio ≤ MAX_V2_RATIO (default 1.2).
#
# Environment:
#   UML_BINARY     UML kernel built with both backends co-selected.
#                  Default: $HOME/src/uml-builds/uml-clean/linux.
#   SAMPLES        Samples per backend after warm-up. Default 7.
#   UML_MEM        mem= argument. Default 512M.
#   MAX_V2_RATIO   kvm-v2:seccomp wall-clock ratio ceiling. Default
#                  1.2 (matches memo 25 perf gate). Set 0 to skip.
#
# Resolution caveat: printk's CONFIG_PRINTK_TIME timer has 10 ms
# granularity in the UML_BINARY we ship, so individual measurements
# are bucketed to multiples of 10 ms. That's enough to detect
# >2× regressions but not for fine-grained tuning. For tighter
# numbers, instrument with ftrace and use the bench harness in
# tools/testing/selftests/um/perf-getpid (cycle-level).

set -u

BINARY=${UML_BINARY:-$HOME/src/uml-builds/uml-clean/linux}
SAMPLES=${SAMPLES:-7}
MEM=${UML_MEM:-512M}
MAX_V2_RATIO=${MAX_V2_RATIO:-1.2}

if [ ! -x "$BINARY" ]; then
    echo "SKIP: $BINARY not found (set UML_BINARY)" >&2
    exit 4
fi
if [ ! -x /usr/bin/python3 ]; then
    echo "SKIP: /usr/bin/python3 not found on host (visible to UML via hostfs)" >&2
    exit 4
fi

INIT_SCRIPT=$(mktemp /tmp/h1-init.XXXXXX.sh)
chmod +x "$INIT_SCRIPT"
cat > "$INIT_SCRIPT" <<'EOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t tmpfs tmpfs /tmp 2>/dev/null
/usr/bin/python3 -c "import math; print('done', math.pi)" 2>&1
sync
EOF
trap 'rm -f "$INIT_SCRIPT"' EXIT

extract_init_dur() {
    awk '
        /Run [^ ]+ as init process/ {
            if (match($0, /\[ *([0-9]+\.[0-9]+)\]/, m)) start = m[1] + 0
        }
        /Kernel panic - not syncing: Attempted to kill init!/ {
            if (match($0, /\[ *([0-9]+\.[0-9]+)\]/, m)) end = m[1] + 0
            if (start > 0 && end > 0) printf "%.4f\n", end - start
            exit
        }
    '
}

run_one() {
    local backend=$1
    timeout 30 "$BINARY" backend=force="$backend" mem="$MEM" \
        rootfstype=hostfs root=/dev/root rw \
        con=null con0=fd:0,fd:1 panic=-1 printk.time=1 \
        init="$INIT_SCRIPT" </dev/null 2>&1 | extract_init_dur
}

# Warm-up — first run pays page-cache costs.
for backend in seccomp kvm-v2; do run_one "$backend" >/dev/null; done

declare -A SAMPLES_OF
SAMPLES_OF[seccomp]=""
SAMPLES_OF[kvm-v2]=""

for i in $(seq 1 "$SAMPLES"); do
    for backend in seccomp kvm-v2; do
        d=$(run_one "$backend")
        if [ -z "$d" ]; then d="MISS"; fi
        SAMPLES_OF[$backend]="${SAMPLES_OF[$backend]} $d"
    done
done

median() {
    local sorted; sorted=$(printf '%s\n' "$@" | sort -n)
    local n=$#; local mid=$(( (n + 1) / 2 ))
    echo "$sorted" | awk -v m="$mid" 'NR == m { print; exit }'
}

SECC_MED=$(median ${SAMPLES_OF[seccomp]})
KVM_MED=$(median ${SAMPLES_OF[kvm-v2]})

if [ -z "$SECC_MED" ] || [ -z "$KVM_MED" ] || [ "$SECC_MED" = "MISS" ] || [ "$KVM_MED" = "MISS" ]; then
    echo "PERF_PY_STARTUP: SKIP (one or both backends produced no measurement)"
    echo "  seccomp samples: ${SAMPLES_OF[seccomp]}"
    echo "  kvm-v2 samples:  ${SAMPLES_OF[kvm-v2]}"
    exit 4
fi

RATIO=$(awk -v k="$KVM_MED" -v s="$SECC_MED" \
    'BEGIN { if (s+0 == 0) print "n/a"; else printf "%.3f", k / s }')

# One emit-line in the same shape as run-perf-getpid.sh, so a gate
# wrapper can extract numbers with one regex per metric.
echo "PERF_PY_STARTUP: SUMMARY samples=$SAMPLES" \
     "seccomp_med=$SECC_MED kvm_v2_med=$KVM_MED" \
     "ratio_v2_over_seccomp=$RATIO max_allowed=$MAX_V2_RATIO"
echo "PERF_PY_STARTUP: seccomp_samples=${SAMPLES_OF[seccomp]}"
echo "PERF_PY_STARTUP: kvm_v2_samples=${SAMPLES_OF[kvm-v2]}"

if [ "$MAX_V2_RATIO" = "0" ]; then
    echo "PERF_PY_STARTUP: PASS (gate disabled via MAX_V2_RATIO=0)"
    exit 0
fi
if awk -v r="$RATIO" -v m="$MAX_V2_RATIO" \
    'BEGIN { exit !(r <= m + 0) }'; then
    echo "PERF_PY_STARTUP: PASS"
    exit 0
fi
echo "PERF_PY_STARTUP: FAIL ratio=$RATIO exceeds max=$MAX_V2_RATIO"
exit 1
