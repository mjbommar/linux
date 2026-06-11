#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# guest->host TCP throughput through the umlctl fd-handoff path.
#
# umlctl pre-opens /dev/net/tun, dups it onto known fds, and execs the
# kernel with the vector2 fd transport.  This wrapper runs the same
# workload with legacy vector and vector2, then gates vector2 against the
# configured legacy throughput threshold.
#
# Usage:
#   tools/testing/selftests/um/net-bench/run-tcp-throughput-via-umlctl.sh \
#       [--kernel PATH] [--duration SEC] [--reps N] [--backend BE]

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/../../../../.." && pwd)

UMLCTL=${UMLCTL:-}
KERNEL=${KERNEL:-${UML_KERNEL:-$ROOT/linux}}
DURATION=${DURATION:-8}
REPS=${REPS:-3}
BACKEND=${BACKEND:-seccomp}
BENCH_PORT=${BENCH_PORT:-5302}
TAP=${TAP:-tcpbench-tap1}
HOST_IP_PLAIN=${HOST_IP_PLAIN:-192.168.45.1}
GUEST_IP_PLAIN=${GUEST_IP_PLAIN:-192.168.45.2}
OUT=${OUT:-$PWD/tcp-throughput-via-umlctl}
TCP_SEND=${TCP_SEND:-$SCRIPT_DIR/tcp-send}

if [ -z "$UMLCTL" ]; then
    for cand in \
        "$ROOT/tools/uml/uml-launcher/target/release/umlctl" \
        "$ROOT/tools/uml/uml-launcher/target/debug/umlctl"; do
        if [ -x "$cand" ]; then
            UMLCTL=$cand
            break
        fi
    done
fi

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

if [ -z "$UMLCTL" ] || [ ! -x "$UMLCTL" ]; then
    echo "error: umlctl not found (set UMLCTL=...)" >&2
    exit 1
fi
if [ ! -x "$KERNEL" ]; then
    echo "error: UML kernel $KERNEL not found (set KERNEL=...)" >&2
    exit 1
fi

mkdir -p "$OUT"
TEMPLATE="$SCRIPT_DIR/tcp-throughput.toml.template"

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
        -e "s|{{TCP_SEND}}|$TCP_SEND|g" \
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
        echo "no $drv results captured" >&2
        return 1
    fi
    printf '%s\n' "${results[@]}"
}

echo "[bench] kernel=$KERNEL backend=$BACKEND duration=${DURATION}s reps=$REPS"
echo "[bench] tap=$TAP host=$HOST_IP_PLAIN guest=$GUEST_IP_PLAIN port=$BENCH_PORT"
echo

echo "=== driver=vector ==="
mapfile -t VEC < <(run_driver vector)
if [ ${#VEC[@]} -eq 0 ]; then
    echo "FAIL: no vector throughput results captured" >&2
    exit 1
fi
printf 'vector: %s\n' "${VEC[@]}"
echo

echo "=== driver=vector2 ==="
mapfile -t V2 < <(run_driver vector2)
if [ ${#V2[@]} -eq 0 ]; then
    echo "FAIL: no vector2 throughput results captured" >&2
    exit 1
fi
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
