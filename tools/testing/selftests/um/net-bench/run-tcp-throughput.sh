#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# guest->host TCP throughput, vector vs vector2.
#
# Boots a UML guest twice (legacy vector + vector2), runs a Python
# TCP sender for $DURATION seconds against a host-side sink, and
# reports the throughput each side delivered.  Verdict: vector2 must
# be >= 0.85 * vector.
#
# Usage:
#   tools/testing/selftests/um/net-bench/run-tcp-throughput.sh \
#       [--kernel PATH] [--duration SEC] [--reps N]
#
# Requires passwordless sudo (for tap setup) and a UML kernel built
# with both CONFIG_UML_NET_VECTOR and CONFIG_UML_NET_VECTOR_V2.

set -euo pipefail

KERNEL=${KERNEL:-$HOME/src/uml-builds/uml-smp-t41fix/linux}
TAP=${TAP:-tcpbench-tap0}
HOST_IP=${HOST_IP:-192.168.43.1}
GUEST_IP=${GUEST_IP:-192.168.43.2}
PORT=${PORT:-5301}
DURATION=${DURATION:-10}
REPS=${REPS:-3}
OUT=${OUT:-$HOME/src/tcp-throughput-bench}

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel) KERNEL="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --reps) REPS="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT"

if ! sudo -n true >/dev/null 2>&1; then
    echo "error: need passwordless sudo for tap setup" >&2
    exit 1
fi

# -------------- helpers ---------------------------------------------

cleanup() {
    if [ -n "${SRV_PID:-}" ] && kill -0 "$SRV_PID" 2>/dev/null; then
        kill "$SRV_PID" 2>/dev/null || true
    fi
    sudo ip link delete "$TAP" 2>/dev/null || true
}
trap cleanup EXIT

setup_tap() {
    sudo ip link delete "$TAP" 2>/dev/null || true
    sudo ip tuntap add dev "$TAP" mode tap user "$USER"
    sudo ip addr add "$HOST_IP/30" dev "$TAP"
    sudo ip link set "$TAP" up
}

start_sink() {
    cat > "$OUT/sink.py" <<PY
import socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", $PORT))
s.listen(8)
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
    print(f"sink rx={total} dt={dt:.2f}", flush=True)
    c.close()
PY
    python3 "$OUT/sink.py" >"$OUT/sink.log" 2>&1 &
    SRV_PID=$!
    # Give the listener a moment to bind.
    sleep 0.3
}

run_bench_iter() {
    local DRV=$1
    local REP=$2
    local CMDLINE_NET DEV
    if [ "$DRV" = "vector" ]; then
        CMDLINE_NET="vec0:transport=tap,ifname=$TAP,depth=128"
        DEV="vec0"
    else
        # Use production fd-handoff path (matches umlctl deploy).
        CMDLINE_NET="vec2.0:transport=fd,mode=fd,fd=200,depth=128"
        DEV="vec2.0"
    fi

    cat > "$OUT/init-$DRV.sh" <<EOF
#!/bin/sh
set -x
exec 2>&1
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
ip link show
ip link set lo up
ip addr add $GUEST_IP/30 dev $DEV
ip link set $DEV up
ip route add default via $HOST_IP 2>/dev/null || true
ip addr show $DEV
ip route show
sleep 0.5
python3 - <<PY
import socket, time
s = socket.socket()
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
s.connect(("$HOST_IP", $PORT))
buf = b"X" * 65536
t0 = time.monotonic()
n = 0
while time.monotonic() - t0 < $DURATION:
    n += s.send(buf)
dt = time.monotonic() - t0
mbps = n * 8.0 / dt / 1e6
print("BENCH_RESULT mbps=%.1f bytes=%d dt=%.2f" % (mbps, n, dt))
s.close()
PY
poweroff -f
EOF
    chmod +x "$OUT/init-$DRV.sh"

    if [ "$DRV" = "vector2" ]; then
        # Open tap fd in pre-exec wrapper, then exec UML with fd=200.
# backend=seccomp (not kvm-v2): kvm-v2 maps host fds into the
        # vCPU pool and trips a fatal signal on the inherited tap fd.
        # seccomp uses the legacy ptrace shape that lets the fd live
        # in the init process's table.
        timeout $((DURATION + 60)) python3 \
            "$(dirname "$0")/exec-uml-fd.py" "$TAP" 200 \
            "$KERNEL" \
            mem=512M rootfstype=hostfs rootflags=/ root=/dev/root rw \
            backend=seccomp ncpus=2 \
            $CMDLINE_NET \
            init="$OUT/init-$DRV.sh" >"$OUT/boot-$DRV-rep$REP.log" 2>&1
    else
        timeout $((DURATION + 60)) "$KERNEL" \
            mem=512M rootfstype=hostfs rootflags=/ root=/dev/root rw \
            backend=seccomp ncpus=2 \
            $CMDLINE_NET \
            init="$OUT/init-$DRV.sh" >"$OUT/boot-$DRV-rep$REP.log" 2>&1
    fi
    local res
    res=$(grep "BENCH_RESULT" "$OUT/boot-$DRV-rep$REP.log" | tail -1)
    if [ -n "$res" ]; then
        local mbps
        mbps=$(echo "$res" | sed -E 's/.*mbps=([0-9.]+).*/\1/')
        echo "$DRV rep $REP: $res"
        printf '%s' "$mbps"
    else
        echo "$DRV rep $REP: MISSING"
        printf '%s' "0"
    fi
}

# -------------- run -------------------------------------------------

setup_tap
start_sink

declare -a VEC_RESULTS=()
declare -a V2_RESULTS=()

echo "[bench] kernel=$KERNEL duration=${DURATION}s reps=$REPS"
echo "[bench] tap=$TAP host_ip=$HOST_IP guest_ip=$GUEST_IP port=$PORT"
echo

for rep in $(seq 1 "$REPS"); do
    echo "--- rep $rep ---"
    val=$(run_bench_iter vector "$rep")
    VEC_RESULTS+=("$val")
    val=$(run_bench_iter vector2 "$rep")
    V2_RESULTS+=("$val")
    echo
done

# Median helper (works on a small array).
median() {
    local arr=("$@")
    local sorted
    IFS=$'\n' sorted=($(printf '%s\n' "${arr[@]}" | sort -g))
    unset IFS
    local n=${#sorted[@]}
    echo "${sorted[$((n / 2))]}"
}

VEC_MED=$(median "${VEC_RESULTS[@]}")
V2_MED=$(median "${V2_RESULTS[@]}")

RATIO=$(python3 -c "v=$V2_MED; l=$VEC_MED; print(f'{v/l:.3f}' if l > 0 else 'inf')")
PASS=$(python3 -c "v=$V2_MED; l=$VEC_MED; print('PASS' if l > 0 and v/l >= 0.85 else 'FAIL')")

cat > "$OUT/verdict.txt" <<EOF
guest->host TCP throughput verdict

Kernel:    $KERNEL
Duration:  ${DURATION}s per iteration, $REPS reps
Tap:       $TAP
Host IP:   $HOST_IP
Guest IP:  $GUEST_IP

vector  per-rep Mbps: ${VEC_RESULTS[*]}
vector2 per-rep Mbps: ${V2_RESULTS[*]}

vector  median Mbps: $VEC_MED
vector2 median Mbps: $V2_MED
ratio (v2 / legacy): $RATIO   (gate >= 0.85)

VERDICT: $PASS
EOF

cat "$OUT/verdict.txt"

[ "$PASS" = "PASS" ]
