#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Collect a small TCP baseline for UML vector networking.
#
# This is intentionally a lightweight harness, not a benchmark suite.  It
# keeps the Umlfile shape stable and switches only the network driver so
# legacy vector and vector2 can be compared through the same umlctl path.

set -euo pipefail

usage() {
	cat <<'EOF'
usage: vector-net-perf-baseline.sh [--kernel PATH] [--drivers LIST] [--direction DIR] [--bytes N] [--bytes-list LIST] [--repeat N] [--out DIR]

Environment overrides:
  UML_KERNEL                 UML kernel path when --kernel is omitted
  UML_VECTOR_PERF_DRIVERS    comma-separated drivers, default: vector,vector2
  UML_VECTOR_PERF_DIRECTION  guest-to-host, host-to-guest, or both; default: guest-to-host
  UML_VECTOR_PERF_BYTES      bytes sent by the guest per run, default: 33554432
  UML_VECTOR_PERF_BYTES_LIST comma-separated byte counts; overrides UML_VECTOR_PERF_BYTES
  UML_VECTOR_PERF_REPEAT     repetitions per driver/direction/size, default: 1
  UML_VECTOR_PERF_PORT       host TCP sink port, default: 19091
  UML_VECTOR_PERF_BACKEND    umlctl backend, default: seccomp
  UML_VECTOR_PERF_QUEUES     vector2 queue intent, default: auto
  UML_VECTOR_PERF_OUT        output directory, default: /tmp/um-vector-perf-baseline
EOF
}

repo_root="$(git rev-parse --show-toplevel)"
kernel="${UML_KERNEL:-}"
drivers="${UML_VECTOR_PERF_DRIVERS:-vector,vector2}"
direction="${UML_VECTOR_PERF_DIRECTION:-guest-to-host}"
bytes="${UML_VECTOR_PERF_BYTES:-33554432}"
bytes_list="${UML_VECTOR_PERF_BYTES_LIST:-}"
repeat="${UML_VECTOR_PERF_REPEAT:-1}"
port="${UML_VECTOR_PERF_PORT:-19091}"
backend="${UML_VECTOR_PERF_BACKEND:-seccomp}"
queues="${UML_VECTOR_PERF_QUEUES:-auto}"
out="${UML_VECTOR_PERF_OUT:-/tmp/um-vector-perf-baseline}"

while [[ $# -gt 0 ]]; do
	case "$1" in
	--kernel)
		kernel="$2"
		shift 2
		;;
	--drivers)
		drivers="$2"
		shift 2
		;;
	--direction)
		direction="$2"
		shift 2
		;;
	--bytes)
		bytes="$2"
		shift 2
		;;
	--bytes-list)
		bytes_list="$2"
		shift 2
		;;
	--repeat)
		repeat="$2"
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
if ! command -v python3 >/dev/null 2>&1; then
	echo "python3 is required on host and guest hostfs" >&2
	exit 2
fi
case "$direction" in
guest-to-host|host-to-guest|both)
	;;
*)
	echo "direction must be guest-to-host, host-to-guest, or both (got $direction)" >&2
	exit 2
	;;
esac
if [[ "$repeat" == '' || "$repeat" == *[!0-9]* || "$repeat" -lt 1 ]]; then
	echo "repeat must be a positive integer (got $repeat)" >&2
	exit 2
fi

if [[ -z "$bytes_list" ]]; then
	bytes_list="$bytes"
fi
IFS=',' read -r -a byte_counts <<<"$bytes_list"
for b in "${byte_counts[@]}"; do
	if [[ "$b" == '' || "$b" == *[!0-9]* || "$b" -lt 1 ]]; then
		echo "byte counts must be positive integers (got $b)" >&2
		exit 2
	fi
done

mkdir -p "$out"
summary="$out/summary.tsv"
printf 'driver\tdirection\tbytes\trepeat\tguest_seconds\tguest_mib_s\thost_seconds\thost_mib_s\tguest_log\thost_log\n' > "$summary"

queue_toml() {
	local driver="$1"

	if [[ "$driver" != "vector2" ]]; then
		printf 'queues = 1\n'
		return
	fi

	case "$queues" in
	auto)
		printf 'queues = "auto"\n'
		;;
	''|*[!0-9]*)
		echo "queues must be a positive integer or auto (got $queues)" >&2
		exit 2
		;;
	*)
		printf 'queues = %s\n' "$queues"
		;;
	esac
}

start_sink() {
	local host_log="$1"
	local run_bytes="$2"

	python3 -u - "$port" "$run_bytes" >"$host_log" 2>&1 <<'PY' &
import socket
import sys
import time

port = int(sys.argv[1])
expected = int(sys.argv[2])

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.listen(1)
    print(f"HOST_SINK_READY port={port}", flush=True)
    conn, addr = sock.accept()
    with conn:
        start = time.monotonic()
        received = 0
        while True:
            data = conn.recv(1024 * 1024)
            if not data:
                break
            received += len(data)
        elapsed = max(time.monotonic() - start, 1e-9)
        mib_s = received / 1048576.0 / elapsed
        print(
            f"HOST_SINK bytes={received} seconds={elapsed:.6f} mib_s={mib_s:.3f} addr={addr}",
            flush=True,
        )
        if received != expected:
            sys.exit(3)
PY
	SINK_PID=$!
}

wait_sink_ready() {
	local host_log="$1"
	local i

	for i in $(seq 1 50); do
		if grep -q 'HOST_SINK_READY' "$host_log"; then
			return 0
		fi
		sleep 0.1
	done
	echo "host TCP sink did not become ready" >&2
	cat "$host_log" >&2 || true
	return 1
}

host_send() {
	local guest_ip="$1"
	local host_log="$2"
	local run_bytes="$3"

	python3 -u - "$guest_ip" "$port" "$run_bytes" >"$host_log" 2>&1 <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
total = int(sys.argv[3])
chunk = bytes(65536)
sent = 0

start = time.monotonic()
with socket.create_connection((host, port), timeout=20) as sock:
    while sent < total:
        n = min(len(chunk), total - sent)
        sock.sendall(chunk[:n])
        sent += n
    sock.shutdown(socket.SHUT_WR)
elapsed = max(time.monotonic() - start, 1e-9)
mib_s = sent / 1048576.0 / elapsed
print(f"HOST_SEND bytes={sent} seconds={elapsed:.6f} mib_s={mib_s:.3f}", flush=True)
PY
}

wait_guest_marker() {
	local name="$1"
	local marker="$2"
	local log="$3"
	local deadline=$((SECONDS + 180))

	while (( SECONDS < deadline )); do
		cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
			--bin umlctl -- logs "$name" --require-current --tail 0 \
			>"$log" 2>&1 || true
		if grep -q "$marker" "$log"; then
			return 0
		fi
		sleep 1
	done

	echo "timed out waiting for $marker in $name" >&2
	return 1
}

phase_toml() {
	case "$1" in
	guest-to-host)
		cat <<'EOF'
[[init.phases]]
name = "guest-to-host"
cmd = """
python3 - "$UMLCTL_GATEWAY" "$UML_VECTOR_PERF_PORT" "$UML_VECTOR_PERF_BYTES" <<'PY'
import os
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
total = int(sys.argv[3])
chunk = bytes(65536)
sent = 0
start = time.monotonic()
with socket.create_connection((host, port), timeout=20) as sock:
    while sent < total:
        n = min(len(chunk), total - sent)
        sock.sendall(chunk[:n])
        sent += n
    sock.shutdown(socket.SHUT_WR)
elapsed = max(time.monotonic() - start, 1e-9)
mib_s = sent / 1048576.0 / elapsed
print(
    "VECTOR_NET_PERF "
    "direction=guest-to-host "
    f"driver={os.environ.get('UMLCTL_NETWORK_DRIVER', '')} "
    f"transport={os.environ.get('UMLCTL_NETWORK_TRANSPORT', '')} "
    f"queues={os.environ.get('UMLCTL_NETWORK_QUEUES', '')} "
    f"bytes={sent} seconds={elapsed:.6f} mib_s={mib_s:.3f}"
)
print("VECTOR_NET_PERF_OK")
PY
"""
expect = "VECTOR_NET_PERF_OK"
timeout_secs = 120
EOF
		;;
	host-to-guest)
		cat <<'EOF'
[[init.phases]]
name = "host-to-guest"
cmd = """
python3 - "$UML_VECTOR_PERF_PORT" "$UML_VECTOR_PERF_BYTES" <<'PY'
import os
import socket
import sys
import time

port = int(sys.argv[1])
expected = int(sys.argv[2])
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.listen(1)
    print("GUEST_SINK_READY", flush=True)
    conn, addr = sock.accept()
    with conn:
        start = time.monotonic()
        received = 0
        while True:
            data = conn.recv(1024 * 1024)
            if not data:
                break
            received += len(data)
        elapsed = max(time.monotonic() - start, 1e-9)
        mib_s = received / 1048576.0 / elapsed
        print(
            "VECTOR_NET_PERF "
            "direction=host-to-guest "
            f"driver={os.environ.get('UMLCTL_NETWORK_DRIVER', '')} "
            f"transport={os.environ.get('UMLCTL_NETWORK_TRANSPORT', '')} "
            f"queues={os.environ.get('UMLCTL_NETWORK_QUEUES', '')} "
            f"bytes={received} seconds={elapsed:.6f} mib_s={mib_s:.3f} "
            f"addr={addr}"
        )
        if received != expected:
            sys.exit(3)
print("VECTOR_NET_PERF_OK")
PY
"""
expect = "VECTOR_NET_PERF_OK"
timeout_secs = 120
EOF
		;;
	esac
}

run_driver() {
	local driver="$1"
	local perf_dir="$2"
	local run_bytes="$3"
	local repeat_idx="$4"
	local safe_driver="${driver//[^A-Za-z0-9_.-]/_}"
	local safe_dir="${perf_dir//[^A-Za-z0-9_.-]/_}"
	local run_dir="$out/${safe_driver}-${safe_dir}-b${run_bytes}-r${repeat_idx}"
	local umlf="$run_dir/Umlfile.toml"
	local umlctl_log="$run_dir/umlctl-up.log"
	local guest_log="$run_dir/guest.log"
	local host_log="$run_dir/host-${safe_dir}.log"
	local name="vperf-$safe_driver-$safe_dir-b${run_bytes}-r${repeat_idx}"
	local driver_tag="vec"
	local dir_tag="g2h"
	local tap
	local sink_pid

	if [[ "$driver" == "vector2" ]]; then
		driver_tag="v2"
	fi
	if [[ "$perf_dir" == "host-to-guest" ]]; then
		dir_tag="h2g"
	fi
	tap="vperf-$driver_tag-$dir_tag"

	rm -rf "$run_dir"
	mkdir -p "$run_dir"

	cat >"$umlf" <<EOF
schema_version = 1

[instance]
name = "$name"
labels = { service = "vector-net-perf", driver = "$driver" }

[kernel]
path = "$kernel"
backend = "$backend"
append = []

[runtime]
mem = "768M"
ncpus = 4

[network]
mode = "tap"
driver = "$driver"
host_mode = "auto"
$(queue_toml "$driver")
tap_name = "$tap"
guest_ip = "10.93.0.2/24"
host_ip = "10.93.0.1/24"
gateway = "10.93.0.1"
nameservers = []
masquerade_via = "auto"
ports = []

[env]
PATH = "/usr/bin:/bin:/sbin:/usr/sbin"
UML_VECTOR_PERF_BYTES = "$run_bytes"
UML_VECTOR_PERF_REPEAT = "$repeat_idx"
UML_VECTOR_PERF_PORT = "$port"

[[init.phases]]
name = "network-metadata"
cmd = "env | grep '^UMLCTL_NETWORK_' | sort; echo UMLCTL_NETDEV=\$UMLCTL_NETDEV"

$(phase_toml "$perf_dir")

[debug]
strace = false
gdb = false
gdb_port = 5678
log_dir = ""
keep_running_on_failure = false
EOF

	if [[ "$perf_dir" == "guest-to-host" ]]; then
		start_sink "$host_log" "$run_bytes"
		sink_pid="$SINK_PID"
		trap 'kill "$sink_pid" 2>/dev/null || true' RETURN
		wait_sink_ready "$host_log"

		if ! timeout 240s cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
			--bin umlctl -- up -f "$umlf" --wait-for VECTOR_NET_PERF_OK \
			--wait-timeout 180 >"$umlctl_log" 2>&1; then
			cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
				--bin umlctl -- down -f "$umlf" --force --rm >/dev/null 2>&1 || true
			echo "umlctl up failed for driver=$driver direction=$perf_dir; see $umlctl_log" >&2
			return 1
		fi

		cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
			--bin umlctl -- logs "$name" --tail 0 >"$guest_log" 2>&1 || true

		if ! wait "$sink_pid"; then
			cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
				--bin umlctl -- down -f "$umlf" --force --rm >/dev/null 2>&1 || true
			echo "host TCP sink failed for driver=$driver; see $host_log" >&2
			return 1
		fi
		trap - RETURN
	else
		if ! timeout 240s cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
			--bin umlctl -- up -f "$umlf" --wait-for GUEST_SINK_READY \
			--wait-timeout 180 >"$umlctl_log" 2>&1; then
			cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
				--bin umlctl -- down -f "$umlf" --force --rm >/dev/null 2>&1 || true
			echo "umlctl up failed for driver=$driver direction=$perf_dir; see $umlctl_log" >&2
			return 1
		fi

		if ! host_send "10.93.0.2" "$host_log" "$run_bytes"; then
			cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
				--bin umlctl -- down -f "$umlf" --force --rm >/dev/null 2>&1 || true
			echo "host TCP send failed for driver=$driver; see $host_log" >&2
			return 1
		fi
		if ! wait_guest_marker "$name" VECTOR_NET_PERF_OK "$guest_log"; then
			cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
				--bin umlctl -- down -f "$umlf" --force --rm >/dev/null 2>&1 || true
			return 1
		fi
	fi

	cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
		--bin umlctl -- down -f "$umlf" --force --rm >/dev/null 2>&1 || true

	local guest_line host_line guest_seconds guest_mib_s host_seconds host_mib_s
	guest_line="$(grep 'VECTOR_NET_PERF ' "$guest_log" | tail -1)"
	host_line="$(grep -E 'HOST_(SINK|SEND) ' "$host_log" | tail -1)"
	guest_seconds="$(sed -n 's/.* seconds=\([0-9.]*\).*/\1/p' <<<"$guest_line")"
	guest_mib_s="$(sed -n 's/.* mib_s=\([0-9.]*\).*/\1/p' <<<"$guest_line")"
	host_seconds="$(sed -n 's/.* seconds=\([0-9.]*\).*/\1/p' <<<"$host_line")"
	host_mib_s="$(sed -n 's/.* mib_s=\([0-9.]*\).*/\1/p' <<<"$host_line")"

	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$driver" "$perf_dir" "$run_bytes" "$repeat_idx" "$guest_seconds" "$guest_mib_s" \
		"$host_seconds" "$host_mib_s" "$guest_log" "$host_log" >> "$summary"
	echo "$guest_line"
	echo "$host_line"
}

IFS=',' read -r -a driver_list <<<"$drivers"
if [[ "$direction" == "both" ]]; then
	direction_list=(guest-to-host host-to-guest)
else
	direction_list=("$direction")
fi
for driver in "${driver_list[@]}"; do
	for perf_dir in "${direction_list[@]}"; do
		for run_bytes in "${byte_counts[@]}"; do
			for repeat_idx in $(seq 1 "$repeat"); do
				run_driver "$driver" "$perf_dir" "$run_bytes" "$repeat_idx"
			done
		done
	done
done

echo "summary: $summary"
