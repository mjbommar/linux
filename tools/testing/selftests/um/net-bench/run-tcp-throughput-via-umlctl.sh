#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# guest->host TCP throughput via the production
# umlctl fd-handoff path.
#
# The earlier standalone wrapper (run-tcp-throughput.sh +
# exec-uml-fd.py) only got a clean number for the vector2 inproc
# path (CONFIG_UML_NET_VECTOR_V2_INPROC).  The production path is
# fd-handoff: umlctl pre-opens /dev/net/tun, dups it onto a known
# fd, and exec's the kernel with `vec2.0:transport=fd,mode=fd,fd=N`.
# This script drives the production path by invoking `umlctl gate
# loop` with --network-driver vector / vector2.
#
# Usage:
#   tools/testing/selftests/um/net-bench/run-tcp-throughput-via-umlctl.sh \
#       [--kernel PATH] [--duration SEC] [--reps N] [--backend BE]

set -euo pipefail

UMLCTL=${UMLCTL:-$HOME/bench-bundle/bin/umlctl}
KERNEL=${KERNEL:-$HOME/src/uml-builds/uml-smp-t41fix/linux}
DURATION=${DURATION:-8}
REPS=${REPS:-3}
BACKEND=${BACKEND:-seccomp}
BENCH_PORT=${BENCH_PORT:-5302}
TAP=${TAP:-tcpbench-tap1}
HOST_IP_PLAIN=${HOST_IP_PLAIN:-192.168.45.1}
GUEST_IP_PLAIN=${GUEST_IP_PLAIN:-192.168.45.2}
OUT=${OUT:-$HOME/src/tcp-throughput-via-umlctl}

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel) KERNEL="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --reps) REPS="$2"; shift 2 ;;
        --backend) BACKEND="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT"
SRCDIR="$(dirname "$(readlink -f "$0")")"
TEMPLATE="$SRCDIR/tcp-throughput.toml.template"

cleanup() {
    if [ -n "${SRV_PID:-}" ] && kill -0 "$SRV_PID" 2>/dev/null; then
        kill "$SRV_PID" 2>/dev/null || true
    fi
    # umlctl owns tap cleanup on stop; nothing to do.
}
trap cleanup EXIT

# ----- start host TCP sink (binds 0.0.0.0; works across reps) -----
cat > "$OUT/sink.py" <<PY
import socket, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", $BENCH_PORT))
s.listen(16)
while True:
    c, addr = s.accept()
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    total = 0
    t0 = time.monotonic()
    while True:
        d = c.recv(1 << 20)
        if not d:
            break
        total += len(d)
    dt = time.monotonic() - t0
    print(f"sink rx={total} dt={dt:.2f} peer={addr}", flush=True)
    c.close()
PY
python3 "$OUT/sink.py" >"$OUT/sink.log" 2>&1 &
SRV_PID=$!
sleep 0.3

# ----- resolve template (one .toml per driver) --------------------
resolve_template() {
    local drv=$1 out=$2
    sed -e "s|{{KERNEL}}|$KERNEL|g" \
        -e "s|{{BACKEND}}|$BACKEND|g" \
        -e "s|{{TAP_NAME}}|$TAP|g" \
        -e "s|{{HOST_IP_PLAIN}}|$HOST_IP_PLAIN|g" \
        -e "s|{{GUEST_IP_PLAIN}}|$GUEST_IP_PLAIN|g" \
        -e "s|{{HOST_IP}}|$HOST_IP_PLAIN/30|g" \
        -e "s|{{GUEST_IP}}|$GUEST_IP_PLAIN/30|g" \
        -e "s|{{NETWORK_DRIVER}}|$drv|g" \
        -e "s|{{DURATION_SEC}}|$DURATION|g" \
        -e "s|{{BENCH_PORT}}|$BENCH_PORT|g" \
        "$TEMPLATE" > "$out"
}

run_driver() {
    local drv=$1
    local toml="$OUT/tcp-bench-$drv.toml"
    local out_dir="$OUT/loop-$drv"
    mkdir -p "$out_dir"

    resolve_template "$drv" "$toml"

    "$UMLCTL" gate loop \
        -f "$toml" \
        --network-driver "$drv" \
        -W 1 -M "$REPS" \
        --timeout 90 \
        --out "$out_dir" \
        >"$OUT/loop-$drv.log" 2>&1 || true

    # Extract BENCH_RESULT mbps from each per-iter init.log.
    local results=()
    local log mbps
    for log in "$out_dir"/p0_default/w0/run-*.log; do
        [ -f "$log" ] || continue
        mbps=$(grep "BENCH_RESULT" "$log" | sed -E 's/.*mbps=([0-9.]+).*/\1/' | head -1)
        if [ -n "$mbps" ]; then
            results+=("$mbps")
        fi
    done
    if [ ${#results[@]} -eq 0 ]; then
        echo "no $drv results captured"
        return 1
    fi
    printf '%s\n' "${results[@]}"
}

echo "[bench] kernel=$KERNEL backend=$BACKEND duration=${DURATION}s reps=$REPS"
echo "[bench] tap=$TAP host=$HOST_IP_PLAIN guest=$GUEST_IP_PLAIN port=$BENCH_PORT"
echo

echo "=== driver=vector ==="
mapfile -t VEC < <(run_driver vector)
printf 'vector: %s\n' "${VEC[@]}"
echo

echo "=== driver=vector2 ==="
mapfile -t V2 < <(run_driver vector2)
printf 'vector2: %s\n' "${V2[@]}"
echo

# ----- verdict ----------------------------------------------------
median() {
    local arr=("$@")
    local sorted
    IFS=$'\n' sorted=($(printf '%s\n' "${arr[@]}" | sort -g))
    unset IFS
    local n=${#sorted[@]}
    echo "${sorted[$((n / 2))]}"
}

VEC_MED=$(median "${VEC[@]:-0}")
V2_MED=$(median "${V2[@]:-0}")

RATIO=$(python3 -c "v=$V2_MED; l=$VEC_MED; print(f'{v/l:.3f}' if l > 0 else 'inf')")
PASS=$(python3 -c "v=$V2_MED; l=$VEC_MED; print('PASS' if l > 0 and v/l >= 0.85 else 'FAIL')")

cat > "$OUT/verdict.txt" <<EOF
guest->host TCP throughput via umlctl fd-handoff

Kernel:    $KERNEL
Backend:   $BACKEND
Duration:  ${DURATION}s per iteration, $REPS reps
Tap:       $TAP
Host IP:   $HOST_IP_PLAIN
Guest IP:  $GUEST_IP_PLAIN
Port:      $BENCH_PORT

vector  per-rep Mbps: ${VEC[*]:-(none)}
vector2 per-rep Mbps: ${V2[*]:-(none)}

vector  median Mbps: $VEC_MED
vector2 median Mbps: $V2_MED
ratio (v2 / legacy): $RATIO   (gate >= 0.85)

VERDICT: $PASS
EOF
cat "$OUT/verdict.txt"
[ "$PASS" = "PASS" ]
