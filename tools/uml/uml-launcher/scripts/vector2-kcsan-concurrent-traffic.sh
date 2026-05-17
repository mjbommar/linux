#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Run a focused vector2 concurrent TCP/UDP workload under a UML kernel.
#
# The harness is intentionally small and evidence-oriented:
# - starts host-side TCP and UDP sinks for guest-to-host traffic;
# - boots vector2 through umlctl launcher-owned fd handoff;
# - runs concurrent guest TCP and UDP senders pinned across vCPUs;
# - waits for guest TCP and UDP sinks, then sends host-to-guest traffic;
# - captures ethtool per-queue stats and requires TX/RX movement on at
#   least two queues when multiple queues are configured.

set -euo pipefail

usage() {
	cat <<'EOF'
usage: vector2-kcsan-concurrent-traffic.sh [--kernel PATH] [--out DIR]

Environment overrides:
  UML_KERNEL                         UML kernel path when --kernel is omitted
  UMLCTL_BIN                         umlctl binary path; default: existing target/debug/umlctl or cargo run
  UML_VECTOR2_KCSAN_OUT              output directory, default: /tmp/um-vector2-kcsan-traffic
  UML_VECTOR2_KCSAN_NCPUS            UML vCPU count, default: 4
  UML_VECTOR2_KCSAN_QUEUES           network queue spec N|auto, default: auto
  UML_VECTOR2_KCSAN_FLOWS            TCP/UDP flows per direction, default: 4
  UML_VECTOR2_KCSAN_TCP_BYTES        TCP bytes per flow, default: 1048576
  UML_VECTOR2_KCSAN_UDP_PACKETS      UDP packets per flow, default: 256
  UML_VECTOR2_KCSAN_UDP_PAYLOAD      UDP payload bytes, default: 512
  UML_VECTOR2_KCSAN_UDP_DELAY_US     delay after each UDP send, default: 1000
  UML_VECTOR2_KCSAN_READY_HOLD_SECS  seconds to hold init after success marker, default: 300
  UML_VECTOR2_KCSAN_TIMEOUT          outer umlctl timeout seconds, default: 420
  UML_VECTOR2_KCSAN_WAIT_TIMEOUT     umlctl wait timeout seconds, default: 300
  UML_VECTOR2_KCSAN_BACKEND          umlctl backend, default: seccomp
EOF
}

repo_root="$(git rev-parse --show-toplevel)"
kernel="${UML_KERNEL:-}"
out="${UML_VECTOR2_KCSAN_OUT:-/tmp/um-vector2-kcsan-traffic}"
ncpus="${UML_VECTOR2_KCSAN_NCPUS:-4}"
queue_spec="${UML_VECTOR2_KCSAN_QUEUES:-auto}"
flows="${UML_VECTOR2_KCSAN_FLOWS:-4}"
tcp_bytes="${UML_VECTOR2_KCSAN_TCP_BYTES:-1048576}"
udp_packets="${UML_VECTOR2_KCSAN_UDP_PACKETS:-256}"
udp_payload="${UML_VECTOR2_KCSAN_UDP_PAYLOAD:-512}"
udp_delay_us="${UML_VECTOR2_KCSAN_UDP_DELAY_US:-1000}"
ready_hold_secs="${UML_VECTOR2_KCSAN_READY_HOLD_SECS:-300}"
timeout_secs="${UML_VECTOR2_KCSAN_TIMEOUT:-420}"
wait_timeout="${UML_VECTOR2_KCSAN_WAIT_TIMEOUT:-300}"
backend="${UML_VECTOR2_KCSAN_BACKEND:-seccomp}"

g2h_tcp_port="${UML_VECTOR2_KCSAN_G2H_TCP_PORT:-19191}"
g2h_udp_port="${UML_VECTOR2_KCSAN_G2H_UDP_PORT:-19192}"
h2g_tcp_port="${UML_VECTOR2_KCSAN_H2G_TCP_PORT:-19291}"
h2g_udp_port="${UML_VECTOR2_KCSAN_H2G_UDP_PORT:-19292}"

while [[ $# -gt 0 ]]; do
	case "$1" in
	--kernel)
		kernel="$2"
		shift 2
		;;
	--out)
		out="$2"
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		echo "unknown argument: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

if [[ -z "$kernel" ]]; then
	echo "missing UML kernel path; pass --kernel or set UML_KERNEL" >&2
	exit 2
fi
if [[ ! -x "$kernel" ]]; then
	echo "UML kernel is not executable: $kernel" >&2
	exit 2
fi
for n in "$ncpus" "$flows" "$tcp_bytes" "$udp_packets" "$udp_payload" "$udp_delay_us" "$ready_hold_secs" \
	 "$timeout_secs" "$wait_timeout" "$g2h_tcp_port" "$g2h_udp_port" \
	 "$h2g_tcp_port" "$h2g_udp_port"; do
	if [[ "$n" == '' || "$n" == *[!0-9]* ]]; then
		echo "numeric settings must be unsigned integers (got $n)" >&2
		exit 2
	fi
done
for n in "$ncpus" "$flows" "$tcp_bytes" "$udp_packets" "$udp_payload" \
	 "$ready_hold_secs" "$timeout_secs" "$wait_timeout" "$g2h_tcp_port" "$g2h_udp_port" \
	 "$h2g_tcp_port" "$h2g_udp_port"; do
	if [[ "$n" -lt 1 ]]; then
		echo "numeric settings must be positive integers (got $n)" >&2
		exit 2
	fi
done
if [[ "$queue_spec" == "auto" ]]; then
	queue_toml_value='"auto"'
else
	if [[ "$queue_spec" == '' || "$queue_spec" == *[!0-9]* ]]; then
		echo "UML_VECTOR2_KCSAN_QUEUES must be a positive integer or auto (got $queue_spec)" >&2
		exit 2
	fi
	if [[ "$queue_spec" -lt 1 ]]; then
		echo "UML_VECTOR2_KCSAN_QUEUES must be >= 1 (got $queue_spec)" >&2
		exit 2
	fi
	queue_toml_value="$queue_spec"
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "python3 is required on host and guest hostfs" >&2
	exit 2
fi

if [[ -n "${UMLCTL_BIN:-}" ]]; then
	umlctl_cmd=("$UMLCTL_BIN")
elif [[ -x "$repo_root/tools/uml/uml-launcher/target/debug/umlctl" ]]; then
	umlctl_cmd=("$repo_root/tools/uml/uml-launcher/target/debug/umlctl")
else
	umlctl_cmd=(cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" --bin umlctl --)
fi

mkdir -p "$out"
umlf="$out/Umlfile.toml"
up_log="$out/umlctl-up.log"
run_log="$out/run.log"
host_g2h_tcp_log="$out/host-g2h-tcp.log"
host_g2h_udp_log="$out/host-g2h-udp.log"
host_h2g_send_log="$out/host-h2g-send.log"
summary="$out/summary.txt"
name="vector2-kcsan-traffic"
tap="v2kcstraffic0"
guest_ip="10.94.0.2"
guest_cidr="$guest_ip/24"
host_cidr="10.94.0.1/24"
gateway="10.94.0.1"

tcp_sink_pid=""
udp_sink_pid=""

cleanup() {
	if [[ -n "$tcp_sink_pid" ]]; then
		kill "$tcp_sink_pid" 2>/dev/null || true
	fi
	if [[ -n "$udp_sink_pid" ]]; then
		kill "$udp_sink_pid" 2>/dev/null || true
	fi
	if [[ -f "$umlf" ]]; then
		"${umlctl_cmd[@]}" down -f "$umlf" --force --rm >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT

start_g2h_tcp_sink() {
	python3 -u - "$g2h_tcp_port" "$flows" "$tcp_bytes" >"$host_g2h_tcp_log" 2>&1 <<'PY' &
import socket
import sys
import threading
import time

port = int(sys.argv[1])
flows = int(sys.argv[2])
per_flow = int(sys.argv[3])
expected = flows * per_flow
lock = threading.Lock()
total = 0
connections = 0

def handle(conn, addr):
    global total, connections
    got = 0
    with conn:
        while True:
            data = conn.recv(1024 * 1024)
            if not data:
                break
            got += len(data)
    with lock:
        total += got
        connections += 1
    print(f"HOST_G2H_TCP_CONN addr={addr} bytes={got}", flush=True)

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.listen(flows)
    print(f"HOST_G2H_TCP_READY port={port} flows={flows}", flush=True)
    threads = []
    deadline = time.monotonic() + 120
    while len(threads) < flows and time.monotonic() < deadline:
        sock.settimeout(max(0.1, deadline - time.monotonic()))
        try:
            conn, addr = sock.accept()
        except socket.timeout:
            break
        t = threading.Thread(target=handle, args=(conn, addr), daemon=True)
        t.start()
        threads.append(t)
    for t in threads:
        t.join(timeout=120)

print(f"HOST_G2H_TCP_DONE connections={connections} bytes={total} expected={expected}", flush=True)
if connections != flows or total != expected:
    sys.exit(3)
PY
	tcp_sink_pid=$!
}

start_g2h_udp_sink() {
	python3 -u - "$g2h_udp_port" "$flows" "$udp_packets" "$udp_payload" >"$host_g2h_udp_log" 2>&1 <<'PY' &
import socket
import sys
import time

port = int(sys.argv[1])
flows = int(sys.argv[2])
packets_per_flow = int(sys.argv[3])
payload = int(sys.argv[4])
expected_packets = flows * packets_per_flow
expected_bytes = expected_packets * payload
seen = set()
received = 0
bytes_in = 0

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(("0.0.0.0", port))
    sock.settimeout(0.5)
    print(
        f"HOST_G2H_UDP_READY port={port} flows={flows} packets={packets_per_flow}",
        flush=True,
    )
    deadline = time.monotonic() + 120
    while received < expected_packets and time.monotonic() < deadline:
        try:
            data, _addr = sock.recvfrom(max(payload + 256, 2048))
        except socket.timeout:
            continue
        received += 1
        bytes_in += len(data)
        header = data.split(b":", 2)[:2]
        if len(header) == 2:
            try:
                seen.add((int(header[0]), int(header[1])))
            except ValueError:
                pass

print(
    f"HOST_G2H_UDP_DONE packets={received} unique={len(seen)} "
    f"bytes={bytes_in} expected_packets={expected_packets} expected_bytes={expected_bytes}",
    flush=True,
)
if received != expected_packets or bytes_in != expected_bytes:
    sys.exit(3)
PY
	udp_sink_pid=$!
}

wait_log_marker() {
	local log="$1"
	local marker="$2"
	local deadline=$((SECONDS + 30))

	while (( SECONDS < deadline )); do
		if grep -q "$marker" "$log" 2>/dev/null; then
			return 0
		fi
		sleep 0.1
	done

	echo "timed out waiting for $marker in $log" >&2
	cat "$log" >&2 || true
	return 1
}

wait_guest_marker() {
	local marker="$1"
	local deadline=$((SECONDS + wait_timeout))

	while (( SECONDS < deadline )); do
		"${umlctl_cmd[@]}" logs "$name" --require-current --tail 0 \
			>"$run_log" 2>&1 || true
		if grep -q "$marker" "$run_log"; then
			return 0
		fi
		if grep -Eq 'VECTOR2_KCSAN_.*FAIL|phase .* failed|Kernel panic|BUG: KCSAN|data-race|KCSAN:' "$run_log"; then
			echo "detected failure while waiting for $marker in $name" >&2
			return 1
		fi
		sleep 1
	done

	echo "timed out waiting for $marker in $name" >&2
	return 1
}

host_h2g_send() {
	python3 -u - "$guest_ip" "$h2g_tcp_port" "$h2g_udp_port" \
		"$flows" "$tcp_bytes" "$udp_packets" "$udp_payload" \
		"$udp_delay_us" \
		>"$host_h2g_send_log" 2>&1 <<'PY'
import multiprocessing as mp
import os
import socket
import sys
import time

guest = sys.argv[1]
tcp_port = int(sys.argv[2])
udp_port = int(sys.argv[3])
flows = int(sys.argv[4])
tcp_bytes = int(sys.argv[5])
udp_packets = int(sys.argv[6])
udp_payload = int(sys.argv[7])
udp_delay = int(sys.argv[8]) / 1000000.0

def pin(flow):
    try:
        cpus = sorted(os.sched_getaffinity(0))
        if cpus:
            os.sched_setaffinity(0, {cpus[flow % len(cpus)]})
    except (AttributeError, OSError):
        pass

def tcp_send(flow):
    pin(flow)
    chunk = bytes([65 + (flow % 26)]) * 65536
    sent = 0
    with socket.create_connection((guest, tcp_port), timeout=30) as sock:
        while sent < tcp_bytes:
            n = min(len(chunk), tcp_bytes - sent)
            sock.sendall(chunk[:n])
            sent += n
        sock.shutdown(socket.SHUT_WR)
    print(f"HOST_H2G_TCP_SENT flow={flow} bytes={sent}", flush=True)

def udp_send(flow):
    pin(flow)
    pad_len = max(0, udp_payload - 32)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for seq in range(udp_packets):
            header = f"{flow}:{seq}:".encode("ascii")
            packet = (header + (b"u" * pad_len))[:udp_payload]
            if len(packet) < udp_payload:
                packet += b"u" * (udp_payload - len(packet))
            sock.sendto(packet, (guest, udp_port))
            if udp_delay:
                time.sleep(udp_delay)
    print(f"HOST_H2G_UDP_SENT flow={flow} packets={udp_packets}", flush=True)

ctx = mp.get_context("fork")
procs = []
start = time.monotonic()
for flow in range(flows):
    procs.append(ctx.Process(target=tcp_send, args=(flow,)))
    procs.append(ctx.Process(target=udp_send, args=(flow,)))
for proc in procs:
    proc.start()
for proc in procs:
    proc.join(120)
bad = [proc.pid for proc in procs if proc.exitcode != 0]
elapsed = max(time.monotonic() - start, 1e-9)
if bad:
    print(f"HOST_H2G_SEND_FAIL bad={bad}", flush=True)
    sys.exit(3)
print(
    f"HOST_H2G_SEND_DONE flows={flows} tcp_bytes={flows * tcp_bytes} "
    f"udp_packets={flows * udp_packets} seconds={elapsed:.6f}",
    flush=True,
)
PY
}

cat >"$umlf" <<EOF
schema_version = 1

[instance]
name = "$name"
labels = { service = "network-concurrency", driver = "vector2", transport = "fd", queues = "$queue_spec", sanitizer = "kcsan" }

[kernel]
path = "$kernel"
backend = "$backend"
append = []

[runtime]
mem = "1024M"
ncpus = $ncpus

[network]
mode = "tap"
driver = "vector2"
host_mode = "auto"
queues = $queue_toml_value
tap_name = "$tap"
guest_ip = "$guest_cidr"
host_ip = "$host_cidr"
gateway = "$gateway"
nameservers = []
masquerade_via = "auto"
ports = []

[env]
PATH = "/usr/bin:/bin:/sbin:/usr/sbin"
UML_VECTOR2_KCSAN_NCPUS = "$ncpus"
UML_VECTOR2_KCSAN_QUEUES = "$queue_spec"
UML_VECTOR2_KCSAN_FLOWS = "$flows"
UML_VECTOR2_KCSAN_TCP_BYTES = "$tcp_bytes"
UML_VECTOR2_KCSAN_UDP_PACKETS = "$udp_packets"
UML_VECTOR2_KCSAN_UDP_PAYLOAD = "$udp_payload"
UML_VECTOR2_KCSAN_UDP_DELAY_US = "$udp_delay_us"
UML_VECTOR2_KCSAN_READY_HOLD_SECS = "$ready_hold_secs"
UML_VECTOR2_KCSAN_G2H_TCP_PORT = "$g2h_tcp_port"
UML_VECTOR2_KCSAN_G2H_UDP_PORT = "$g2h_udp_port"
UML_VECTOR2_KCSAN_H2G_TCP_PORT = "$h2g_tcp_port"
UML_VECTOR2_KCSAN_H2G_UDP_PORT = "$h2g_udp_port"

[[init.phases]]
name = "network-metadata"
cmd = "env | grep '^UMLCTL_NETWORK_' | sort; echo UMLCTL_NETDEV=\$UMLCTL_NETDEV"

[[init.phases]]
name = "gateway"
cmd = "ping -c 3 \"\$UMLCTL_GATEWAY\""
timeout_secs = 30

[[init.phases]]
name = "stats-before"
cmd = "ethtool -S \"\$UMLCTL_NETDEV\" | tee /tmp/vector2-kcsan-stats-before"
timeout_secs = 20

[[init.phases]]
name = "guest-to-host"
cmd = '''
python3 - "\$UMLCTL_GATEWAY" \
  "\$UML_VECTOR2_KCSAN_G2H_TCP_PORT" "\$UML_VECTOR2_KCSAN_G2H_UDP_PORT" \
  "\$UML_VECTOR2_KCSAN_FLOWS" "\$UML_VECTOR2_KCSAN_TCP_BYTES" \
  "\$UML_VECTOR2_KCSAN_UDP_PACKETS" "\$UML_VECTOR2_KCSAN_UDP_PAYLOAD" \
  "\$UML_VECTOR2_KCSAN_UDP_DELAY_US" <<'PY'
import multiprocessing as mp
import os
import socket
import sys
import time

host = sys.argv[1]
tcp_port = int(sys.argv[2])
udp_port = int(sys.argv[3])
flows = int(sys.argv[4])
tcp_bytes = int(sys.argv[5])
udp_packets = int(sys.argv[6])
udp_payload = int(sys.argv[7])
udp_delay = int(sys.argv[8]) / 1000000.0

def pin(flow):
    try:
        cpus = sorted(os.sched_getaffinity(0))
        if cpus:
            os.sched_setaffinity(0, {cpus[flow % len(cpus)]})
    except (AttributeError, OSError):
        pass

def tcp_send(flow):
    pin(flow)
    chunk = bytes([65 + (flow % 26)]) * 65536
    sent = 0
    with socket.create_connection((host, tcp_port), timeout=30) as sock:
        while sent < tcp_bytes:
            n = min(len(chunk), tcp_bytes - sent)
            sock.sendall(chunk[:n])
            sent += n
        sock.shutdown(socket.SHUT_WR)
    print(f"VECTOR2_KCSAN_G2H_TCP_SENT flow={flow} bytes={sent}", flush=True)

def udp_send(flow):
    pin(flow)
    pad_len = max(0, udp_payload - 32)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for seq in range(udp_packets):
            header = f"{flow}:{seq}:".encode("ascii")
            packet = (header + (b"u" * pad_len))[:udp_payload]
            if len(packet) < udp_payload:
                packet += b"u" * (udp_payload - len(packet))
            sock.sendto(packet, (host, udp_port))
            if udp_delay:
                time.sleep(udp_delay)
    print(f"VECTOR2_KCSAN_G2H_UDP_SENT flow={flow} packets={udp_packets}", flush=True)

ctx = mp.get_context("fork")
procs = []
start = time.monotonic()
for flow in range(flows):
    procs.append(ctx.Process(target=tcp_send, args=(flow,)))
    procs.append(ctx.Process(target=udp_send, args=(flow,)))
for proc in procs:
    proc.start()
for proc in procs:
    proc.join(120)
bad = [proc.pid for proc in procs if proc.exitcode != 0]
elapsed = max(time.monotonic() - start, 1e-9)
if bad:
    print(f"VECTOR2_KCSAN_G2H_GUEST_FAIL bad={bad}", flush=True)
    sys.exit(3)
print(
    f"VECTOR2_KCSAN_G2H_GUEST_OK flows={flows} tcp_bytes={flows * tcp_bytes} "
    f"udp_packets={flows * udp_packets} seconds={elapsed:.6f}",
    flush=True,
)
PY
'''
expect = "VECTOR2_KCSAN_G2H_GUEST_OK"
timeout_secs = 180

[[init.phases]]
name = "host-to-guest"
cmd = '''
python3 - "\$UML_VECTOR2_KCSAN_H2G_TCP_PORT" "\$UML_VECTOR2_KCSAN_H2G_UDP_PORT" \
  "\$UML_VECTOR2_KCSAN_FLOWS" "\$UML_VECTOR2_KCSAN_TCP_BYTES" \
  "\$UML_VECTOR2_KCSAN_UDP_PACKETS" "\$UML_VECTOR2_KCSAN_UDP_PAYLOAD" <<'PY'
import socket
import sys
import threading
import time

tcp_port = int(sys.argv[1])
udp_port = int(sys.argv[2])
flows = int(sys.argv[3])
tcp_bytes = int(sys.argv[4])
udp_packets = int(sys.argv[5])
udp_payload = int(sys.argv[6])
expected_tcp = flows * tcp_bytes
expected_udp_packets = flows * udp_packets
expected_udp_bytes = expected_udp_packets * udp_payload
ready_tcp = threading.Event()
ready_udp = threading.Event()
lock = threading.Lock()
tcp_total = 0
tcp_connections = 0
udp_total = 0
udp_bytes = 0

def tcp_server():
    global tcp_total, tcp_connections
    def handle(conn, addr):
        global tcp_total, tcp_connections
        got = 0
        with conn:
            while True:
                data = conn.recv(1024 * 1024)
                if not data:
                    break
                got += len(data)
        with lock:
            tcp_total += got
            tcp_connections += 1
        print(f"VECTOR2_KCSAN_H2G_TCP_CONN addr={addr} bytes={got}", flush=True)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", tcp_port))
        sock.listen(flows)
        ready_tcp.set()
        threads = []
        deadline = time.monotonic() + 180
        while len(threads) < flows and time.monotonic() < deadline:
            sock.settimeout(max(0.1, deadline - time.monotonic()))
            try:
                conn, addr = sock.accept()
            except socket.timeout:
                break
            t = threading.Thread(target=handle, args=(conn, addr), daemon=True)
            t.start()
            threads.append(t)
        for t in threads:
            t.join(timeout=120)

def udp_server():
    global udp_total, udp_bytes
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        sock.bind(("0.0.0.0", udp_port))
        sock.settimeout(0.5)
        ready_udp.set()
        deadline = time.monotonic() + 180
        while udp_total < expected_udp_packets and time.monotonic() < deadline:
            try:
                data, _addr = sock.recvfrom(max(udp_payload + 256, 2048))
            except socket.timeout:
                continue
            udp_total += 1
            udp_bytes += len(data)

tcp_thread = threading.Thread(target=tcp_server, daemon=True)
udp_thread = threading.Thread(target=udp_server, daemon=True)
tcp_thread.start()
udp_thread.start()
ready_tcp.wait(10)
ready_udp.wait(10)
if not ready_tcp.is_set() or not ready_udp.is_set():
    print("VECTOR2_KCSAN_H2G_READY_FAIL", flush=True)
    sys.exit(3)
print(
    f"VECTOR2_KCSAN_H2G_READY tcp_port={tcp_port} udp_port={udp_port} flows={flows}",
    flush=True,
)
tcp_thread.join(timeout=240)
udp_thread.join(timeout=240)
print(
    f"VECTOR2_KCSAN_H2G_GUEST_DONE tcp_connections={tcp_connections} "
    f"tcp_bytes={tcp_total} expected_tcp={expected_tcp} "
    f"udp_packets={udp_total} expected_udp_packets={expected_udp_packets} "
    f"udp_bytes={udp_bytes} expected_udp_bytes={expected_udp_bytes}",
    flush=True,
)
if (
    tcp_connections != flows
    or tcp_total != expected_tcp
    or udp_total != expected_udp_packets
    or udp_bytes != expected_udp_bytes
):
    print("VECTOR2_KCSAN_H2G_GUEST_FAIL", flush=True)
    sys.exit(3)
print("VECTOR2_KCSAN_H2G_GUEST_OK", flush=True)
PY
'''
expect = "VECTOR2_KCSAN_H2G_GUEST_OK"
timeout_secs = 300

[[init.phases]]
name = "stats-after"
cmd = '''
ethtool -S "\$UMLCTL_NETDEV" | tee /tmp/vector2-kcsan-stats-after
python3 - "\$UMLCTL_NETWORK_QUEUES" /tmp/vector2-kcsan-stats-after <<'PY'
import re
import sys

queues = int(sys.argv[1])
path = sys.argv[2]
stats = {}
for line in open(path, encoding="utf-8", errors="replace"):
    m = re.match(r"\s*([A-Za-z0-9_]+):\s*([0-9]+)\s*$", line)
    if m:
        stats[m.group(1)] = int(m.group(2))

tx = [stats.get(f"queue{i}_tx_ring_enqueued", 0) for i in range(queues)]
rx = [stats.get(f"queue{i}_rx_batch_received_total", 0) for i in range(queues)]
tx_nonzero = sum(1 for value in tx if value > 0)
rx_nonzero = sum(1 for value in rx if value > 0)
print(f"VECTOR2_KCSAN_QUEUE_TX values={','.join(map(str, tx))} nonzero={tx_nonzero}")
print(f"VECTOR2_KCSAN_QUEUE_RX values={','.join(map(str, rx))} nonzero={rx_nonzero}")
if sum(tx) <= 0 or sum(rx) <= 0:
    print("VECTOR2_KCSAN_QUEUE_COUNTER_FAIL")
    sys.exit(3)
if queues > 1 and (tx_nonzero < 2 or rx_nonzero < 2):
    print("VECTOR2_KCSAN_QUEUE_DISTRIBUTION_FAIL")
    sys.exit(3)
print("VECTOR2_KCSAN_QUEUE_DISTRIBUTION_OK")
PY
'''
expect = "VECTOR2_KCSAN_QUEUE_DISTRIBUTION_OK"
timeout_secs = 30

[[init.phases]]
name = "ready"
cmd = "echo VECTOR2_KCSAN_TRAFFIC_OK; echo REPRO_DONE rc=0; sleep \"\$UML_VECTOR2_KCSAN_READY_HOLD_SECS\""
timeout_secs = $((ready_hold_secs + 30))

[debug]
strace = false
gdb = false
gdb_port = 5678
log_dir = ""
keep_running_on_failure = false
EOF

start_g2h_tcp_sink
start_g2h_udp_sink
wait_log_marker "$host_g2h_tcp_log" HOST_G2H_TCP_READY
wait_log_marker "$host_g2h_udp_log" HOST_G2H_UDP_READY

timeout "$timeout_secs"s "${umlctl_cmd[@]}" up -f "$umlf" \
	--wait-for VECTOR2_KCSAN_H2G_READY \
	--wait-timeout "$wait_timeout" >"$up_log" 2>&1

wait "$tcp_sink_pid"
tcp_sink_pid=""
wait "$udp_sink_pid"
udp_sink_pid=""

host_h2g_send
wait_guest_marker VECTOR2_KCSAN_TRAFFIC_OK

"${umlctl_cmd[@]}" down -f "$umlf" --force --rm >/dev/null 2>&1 || true
trap - EXIT

tap_state="TAP_ABSENT"
if [[ -e "/sys/class/net/$tap" ]]; then
	tap_state="TAP_PRESENT"
fi
proc_state="UML_PROCESS_ABSENT"
if "${umlctl_cmd[@]}" ps -q --filter state=running | grep -Fx "$name" >/dev/null 2>&1; then
	proc_state="UML_PROCESS_MATCH"
fi

{
	echo "VECTOR2_KCSAN_CONCURRENT_TRAFFIC_SUMMARY"
	echo "kernel=$kernel"
	echo "backend=$backend"
	echo "ncpus=$ncpus"
	echo "queue_spec=$queue_spec"
	echo "flows=$flows"
	echo "tcp_bytes_per_flow=$tcp_bytes"
	echo "udp_packets_per_flow=$udp_packets"
	echo "udp_payload=$udp_payload"
	echo "udp_delay_us=$udp_delay_us"
	echo "ready_hold_secs=$ready_hold_secs"
	grep -E 'HOST_G2H_(TCP|UDP)_DONE' "$host_g2h_tcp_log" "$host_g2h_udp_log" || true
	grep -E 'HOST_H2G_SEND_DONE' "$host_h2g_send_log" || true
	grep -E 'VECTOR2_KCSAN_(G2H_GUEST_OK|H2G_GUEST_DONE|H2G_GUEST_OK|QUEUE_TX|QUEUE_RX|QUEUE_DISTRIBUTION_OK|TRAFFIC_OK|REPRO_DONE)' "$run_log" || true
	echo "$tap_state"
	echo "$proc_state"
} >"$summary"

cat "$summary"

if [[ "$tap_state" != "TAP_ABSENT" || "$proc_state" != "UML_PROCESS_ABSENT" ]]; then
	exit 3
fi
