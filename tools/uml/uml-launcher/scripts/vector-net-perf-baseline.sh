#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Collect a small TCP/UDP baseline for UML vector networking.
#
# This is intentionally a lightweight harness, not a benchmark suite.  It
# keeps the Umlfile shape stable and switches only the network driver so
# vec0 and vector2 can be compared through the same umlctl path.

set -euo pipefail

usage() {
	cat <<'EOF'
usage: vector-net-perf-baseline.sh [--kernel PATH] [--drivers LIST] [--direction DIR] [--protocol tcp|udp] [--bytes N] [--bytes-list LIST] [--repeat N] [--out DIR]

Environment overrides:
  UML_KERNEL                 UML kernel path when --kernel is omitted
  UML_VECTOR_PERF_DRIVERS    comma-separated drivers, default: vector,vector2
  UML_VECTOR_PERF_DIRECTION  guest-to-host, host-to-guest, or both; default: guest-to-host
  UML_VECTOR_PERF_PROTOCOL   tcp or udp, default: tcp
  UML_VECTOR_PERF_BYTES      bytes sent by the active sender per run, default: 33554432
  UML_VECTOR_PERF_BYTES_LIST comma-separated byte counts; overrides UML_VECTOR_PERF_BYTES
  UML_VECTOR_PERF_REPEAT     repetitions per driver/direction/size, default: 1
  UML_VECTOR_PERF_PORT       host/guest sink port, default: 19091
  UML_VECTOR_PERF_BACKEND    umlctl backend, default: seccomp
  UML_VECTOR_PERF_HOST_MODE  vector2 host mode: auto, fd, or inproc; default: auto
  UML_VECTOR_PERF_QUEUES     vector2 queue intent, default: auto
  UML_VECTOR_PERF_TCP_NODELAY set TCP_NODELAY on active TCP sender, default: 0
  UML_VECTOR_PERF_HOST_CHUNK host sender chunk size, default: 65536
  UML_VECTOR_PERF_GUEST_CHUNK guest sender chunk size, default: 65536
  UML_VECTOR_PERF_UDP_PAYLOAD UDP payload size per datagram, default: 1472
  UML_VECTOR_PERF_UDP_PACE_USEC sleep between UDP datagrams, default: 0
  UML_VECTOR_PERF_UDP_RCVBUF requested UDP receive socket buffer, default: 4194304
  UML_VECTOR_PERF_UDP_SNDBUF requested UDP send socket buffer, default: 4194304
  UML_VECTOR_PERF_PERF_STAT enable host-to-guest UML PID perf stat, default: 0
  UML_VECTOR_PERF_PERF_EVENTS comma-separated perf events, default: syscall wildcard + CPU counters
  UML_VECTOR_PERF_PERF_SECONDS perf stat window after guest sink readiness, default: 2
  UML_VECTOR_PERF_OUT        output directory, default: /tmp/um-vector-perf-baseline

Outputs:
  summary.tsv       raw per-run rows
  perf-window.tsv   optional host-to-guest transfer-window perf stat rows
  aggregate.tsv     per-driver median/best throughput and median CPU/scheduler data
  comparison.tsv    vector2/vector ratios when both drivers are present
EOF
}

repo_root="$(git rev-parse --show-toplevel)"
kernel="${UML_KERNEL:-}"
drivers="${UML_VECTOR_PERF_DRIVERS:-vector,vector2}"
direction="${UML_VECTOR_PERF_DIRECTION:-guest-to-host}"
protocol="${UML_VECTOR_PERF_PROTOCOL:-tcp}"
bytes="${UML_VECTOR_PERF_BYTES:-33554432}"
bytes_list="${UML_VECTOR_PERF_BYTES_LIST:-}"
repeat="${UML_VECTOR_PERF_REPEAT:-1}"
port="${UML_VECTOR_PERF_PORT:-19091}"
backend="${UML_VECTOR_PERF_BACKEND:-seccomp}"
host_mode="${UML_VECTOR_PERF_HOST_MODE:-auto}"
queues="${UML_VECTOR_PERF_QUEUES:-auto}"
tcp_nodelay="${UML_VECTOR_PERF_TCP_NODELAY:-0}"
host_chunk="${UML_VECTOR_PERF_HOST_CHUNK:-65536}"
guest_chunk="${UML_VECTOR_PERF_GUEST_CHUNK:-65536}"
udp_payload="${UML_VECTOR_PERF_UDP_PAYLOAD:-1472}"
udp_pace_usec="${UML_VECTOR_PERF_UDP_PACE_USEC:-0}"
udp_rcvbuf="${UML_VECTOR_PERF_UDP_RCVBUF:-4194304}"
udp_sndbuf="${UML_VECTOR_PERF_UDP_SNDBUF:-4194304}"
perf_stat="${UML_VECTOR_PERF_PERF_STAT:-0}"
perf_events="${UML_VECTOR_PERF_PERF_EVENTS:-syscalls:sys_enter_*,task-clock,cpu-clock,context-switches,cpu-migrations,page-faults,cycles,instructions}"
perf_seconds="${UML_VECTOR_PERF_PERF_SECONDS:-2}"
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
	--protocol)
		protocol="$2"
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
case "$protocol" in
tcp|udp)
	;;
*)
	echo "protocol must be tcp or udp (got $protocol)" >&2
	exit 2
	;;
esac
case "$host_mode" in
auto|fd|inproc)
	;;
*)
	echo "host mode must be auto, fd, or inproc (got $host_mode)" >&2
	exit 2
	;;
esac
if [[ "$repeat" == '' || "$repeat" == *[!0-9]* || "$repeat" -lt 1 ]]; then
	echo "repeat must be a positive integer (got $repeat)" >&2
	exit 2
fi
if [[ "$tcp_nodelay" != "0" && "$tcp_nodelay" != "1" ]]; then
	echo "TCP_NODELAY must be 0 or 1 (got $tcp_nodelay)" >&2
	exit 2
fi
if [[ "$host_chunk" == '' || "$host_chunk" == *[!0-9]* || "$host_chunk" -lt 1 ]]; then
	echo "host chunk size must be a positive integer (got $host_chunk)" >&2
	exit 2
fi
if [[ "$guest_chunk" == '' || "$guest_chunk" == *[!0-9]* || "$guest_chunk" -lt 1 ]]; then
	echo "guest chunk size must be a positive integer (got $guest_chunk)" >&2
	exit 2
fi
if [[ "$udp_payload" == '' || "$udp_payload" == *[!0-9]* || "$udp_payload" -lt 1 || "$udp_payload" -gt 65507 ]]; then
	echo "UDP payload size must be an integer between 1 and 65507 (got $udp_payload)" >&2
	exit 2
fi
if [[ "$udp_pace_usec" == '' || "$udp_pace_usec" == *[!0-9]* ]]; then
	echo "UDP pace must be a non-negative integer in microseconds (got $udp_pace_usec)" >&2
	exit 2
fi
if [[ "$udp_rcvbuf" == '' || "$udp_rcvbuf" == *[!0-9]* ]]; then
	echo "UDP receive buffer must be a non-negative integer (got $udp_rcvbuf)" >&2
	exit 2
fi
if [[ "$udp_sndbuf" == '' || "$udp_sndbuf" == *[!0-9]* ]]; then
	echo "UDP send buffer must be a non-negative integer (got $udp_sndbuf)" >&2
	exit 2
fi
if [[ "$perf_stat" != "0" && "$perf_stat" != "1" ]]; then
	echo "perf stat flag must be 0 or 1 (got $perf_stat)" >&2
	exit 2
fi
if [[ "$perf_seconds" == '' || "$perf_seconds" == *[!0-9]* || "$perf_seconds" -lt 1 ]]; then
	echo "perf stat seconds must be a positive integer (got $perf_seconds)" >&2
	exit 2
fi
if [[ "$perf_stat" == "1" ]]; then
	if ! command -v perf >/dev/null 2>&1; then
		echo "perf stat requested but perf is not in PATH" >&2
		exit 2
	fi
	if ! sudo -n true >/dev/null 2>&1; then
		echo "perf stat requested but passwordless sudo is unavailable" >&2
		exit 2
	fi
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
perf_summary="$out/perf-window.tsv"
printf 'driver\tdirection\tbytes\trepeat\tguest_seconds\tguest_mib_s\thost_seconds\thost_mib_s\tguest_log\thost_log\tprotocol\tguest_cpu_seconds\thost_cpu_seconds\tuml_user_cpu_seconds\tuml_system_cpu_seconds\tuml_sched_run_seconds\tuml_sched_wait_seconds\tuml_sched_pcount_delta\tuml_voluntary_ctxt_switches_delta\tuml_involuntary_ctxt_switches_delta\tuml_metrics_before_log\tuml_metrics_after_log\n' > "$summary"
printf 'driver\tdirection\tbytes\trepeat\tperf_stat_log\tperf_syscalls_total\tperf_task_clock_ms\tperf_cpu_clock_ms\tperf_context_switches\tperf_cpu_migrations\tperf_page_faults\tperf_cycles\tperf_instructions\tperf_read\tperf_write\tperf_futex\tperf_ioctl\tperf_recvmsg\tperf_sendmsg\tperf_poll\n' > "$perf_summary"

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

	python3 -u - "$port" "$run_bytes" "$protocol" "$udp_payload" "$udp_rcvbuf" >"$host_log" 2>&1 <<'PY' &
import socket
import sys
import time

port = int(sys.argv[1])
expected = int(sys.argv[2])
protocol = sys.argv[3]
udp_payload = int(sys.argv[4])
udp_rcvbuf = int(sys.argv[5])

if protocol == "tcp":
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", port))
        sock.listen(1)
        print(f"HOST_SINK_READY protocol=tcp port={port}", flush=True)
        conn, addr = sock.accept()
        with conn:
            start = time.monotonic()
            cpu_start = time.process_time()
            received = 0
            while True:
                data = conn.recv(1024 * 1024)
                if not data:
                    break
                received += len(data)
else:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if udp_rcvbuf:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, udp_rcvbuf)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", port))
        sock.settimeout(20)
        print(f"HOST_SINK_READY protocol=udp port={port}", flush=True)
        received = 0
        addr = None
        start = None
        cpu_start = None
        while received < expected:
            try:
                data, addr = sock.recvfrom(max(udp_payload, 1) + 64)
            except TimeoutError:
                break
            if start is None:
                start = time.monotonic()
                cpu_start = time.process_time()
            received += len(data)
        if start is None:
            start = time.monotonic()
            cpu_start = time.process_time()

elapsed = max(time.monotonic() - start, 1e-9)
cpu_elapsed = max(time.process_time() - cpu_start, 0.0)
mib_s = received / 1048576.0 / elapsed
print(
    f"HOST_SINK protocol={protocol} bytes={received} seconds={elapsed:.6f} "
    f"mib_s={mib_s:.3f} cpu_seconds={cpu_elapsed:.6f} addr={addr}",
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
	echo "host $protocol sink did not become ready" >&2
	cat "$host_log" >&2 || true
	return 1
}

host_send() {
	local guest_ip="$1"
	local host_log="$2"
	local run_bytes="$3"

	python3 -u - "$guest_ip" "$port" "$run_bytes" "$protocol" "$tcp_nodelay" "$host_chunk" "$udp_payload" "$udp_pace_usec" "$udp_sndbuf" >"$host_log" 2>&1 <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
total = int(sys.argv[3])
protocol = sys.argv[4]
nodelay = int(sys.argv[5])
tcp_chunk = int(sys.argv[6])
udp_payload = int(sys.argv[7])
udp_pace = int(sys.argv[8]) / 1_000_000.0
udp_sndbuf = int(sys.argv[9])
sent = 0

start = time.monotonic()
cpu_start = time.process_time()
if protocol == "tcp":
    chunk = bytes(tcp_chunk)
    with socket.create_connection((host, port), timeout=20) as sock:
        if nodelay:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        while sent < total:
            n = min(len(chunk), total - sent)
            sock.sendall(chunk[:n])
            sent += n
        sock.shutdown(socket.SHUT_WR)
else:
    chunk = bytes(udp_payload)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if udp_sndbuf:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, udp_sndbuf)
        sock.connect((host, port))
        while sent < total:
            n = min(len(chunk), total - sent)
            sock.send(chunk[:n])
            sent += n
            if udp_pace:
                time.sleep(udp_pace)
elapsed = max(time.monotonic() - start, 1e-9)
cpu_elapsed = max(time.process_time() - cpu_start, 0.0)
mib_s = sent / 1048576.0 / elapsed
print(
    f"HOST_SEND protocol={protocol} bytes={sent} seconds={elapsed:.6f} "
    f"mib_s={mib_s:.3f} cpu_seconds={cpu_elapsed:.6f}",
    flush=True,
)
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
		if grep -Eq 'Kernel panic|panic - not syncing|Attempted to kill init' "$log"; then
			echo "guest exited before $marker in $name" >&2
			return 1
		fi
		sleep 1
	done

	echo "timed out waiting for $marker in $name" >&2
	return 1
}

collect_metrics() {
	local name="$1"
	local out_json="$2"

	cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
		--bin umlctl -- metrics "$name" --json >"$out_json" 2>"$out_json.stderr"
}

metrics_delta() {
	local before_json="$1"
	local after_json="$2"

	if [[ ! -s "$before_json" || ! -s "$after_json" ]]; then
		printf 'NA\tNA\tNA\tNA\tNA\tNA\tNA'
		return
	fi

	python3 - "$before_json" "$after_json" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as f:
        before = json.load(f)
    with open(sys.argv[2], encoding="utf-8") as f:
        after = json.load(f)
except Exception:
    print("NA\tNA\tNA\tNA\tNA\tNA\tNA", end="")
    sys.exit(0)

bproc = before.get("proc", {})
aproc = after.get("proc", {})
clock_hz = aproc.get("clock_hz") or bproc.get("clock_hz") or 100

def delta(key):
    return max(int(aproc.get(key, 0)) - int(bproc.get(key, 0)), 0)

user_cpu = delta("utime_ticks") / float(clock_hz)
system_cpu = delta("stime_ticks") / float(clock_hz)
sched_run = delta("sched_run_ns") / 1_000_000_000.0
sched_wait = delta("sched_wait_ns") / 1_000_000_000.0

print(
    f"{user_cpu:.6f}\t"
    f"{system_cpu:.6f}\t"
    f"{sched_run:.6f}\t"
    f"{sched_wait:.6f}\t"
    f"{delta('sched_pcount')}\t"
    f"{delta('nr_voluntary_ctxt_switches')}\t"
    f"{delta('nr_involuntary_ctxt_switches')}",
    end="",
)
PY
}

metrics_pid() {
	local metrics_json="$1"

	python3 - "$metrics_json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
print(int(data["proc"]["pid"]))
PY
}

TRANSFER_PERF_PID=""

start_transfer_perf() {
	local uml_pid="$1"
	local perf_log="$2"

	if [[ "$perf_stat" != "1" ]]; then
		return 0
	fi

	sudo -n perf stat -x, -p "$uml_pid" -e "$perf_events" -o "$perf_log" -- sleep "$perf_seconds" &
	TRANSFER_PERF_PID=$!
	# Give perf a moment to attach before the active sender starts.
	sleep 0.2
}

stop_transfer_perf() {
	if [[ -z "$TRANSFER_PERF_PID" ]]; then
		return 0
	fi

	wait "$TRANSFER_PERF_PID" >/dev/null 2>&1 || true
	TRANSFER_PERF_PID=""
}

perf_fields() {
	local perf_log="$1"

	if [[ ! -s "$perf_log" ]]; then
		printf 'NA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA'
		return
	fi

	python3 - "$perf_log" <<'PY'
import csv
import sys

path = sys.argv[1]
fields = {
    "task-clock": "NA",
    "cpu-clock": "NA",
    "context-switches": "NA",
    "cpu-migrations": "NA",
    "page-faults": "NA",
    "cycles": "NA",
    "instructions": "NA",
}
selected = {
    "syscalls:sys_enter_read": "NA",
    "syscalls:sys_enter_write": "NA",
    "syscalls:sys_enter_futex": "NA",
    "syscalls:sys_enter_ioctl": "NA",
    "syscalls:sys_enter_recvmsg": "NA",
    "syscalls:sys_enter_sendmsg": "NA",
    "syscalls:sys_enter_poll": "NA",
}
total_syscalls = 0.0

with open(path, encoding="utf-8", newline="") as f:
    for row in csv.reader(f):
        if len(row) < 3 or not row[0] or row[0].startswith("#"):
            continue
        value = row[0].strip()
        event = row[2].strip()
        try:
            numeric = float(value)
        except ValueError:
            continue
        if event.startswith("syscalls:sys_enter_"):
            total_syscalls += numeric
        if event in fields:
            fields[event] = value
        if event in selected:
            selected[event] = str(int(numeric))

out = [
    str(int(total_syscalls)),
    fields["task-clock"],
    fields["cpu-clock"],
    fields["context-switches"],
    fields["cpu-migrations"],
    fields["page-faults"],
    fields["cycles"],
    fields["instructions"],
    selected["syscalls:sys_enter_read"],
    selected["syscalls:sys_enter_write"],
    selected["syscalls:sys_enter_futex"],
    selected["syscalls:sys_enter_ioctl"],
    selected["syscalls:sys_enter_recvmsg"],
    selected["syscalls:sys_enter_sendmsg"],
    selected["syscalls:sys_enter_poll"],
]
print("\t".join(out), end="")
PY
}

write_aggregate_summaries() {
	local aggregate="$out/aggregate.tsv"
	local comparison="$out/comparison.tsv"

	python3 - "$summary" "$aggregate" "$comparison" <<'PY'
import csv
import statistics
import sys
from collections import defaultdict

summary_path, aggregate_path, comparison_path = sys.argv[1:]

with open(summary_path, encoding="utf-8") as f:
    rows = list(csv.DictReader(f, delimiter="\t"))

def number(row, key):
    value = row.get(key, "")
    if not value or value == "NA":
        return None
    try:
        return float(value)
    except ValueError:
        return None

def values(rows, key):
    return [value for row in rows if (value := number(row, key)) is not None]

def fmt(value):
    if value is None:
        return "NA"
    return f"{value:.6f}"

def median(rows, key):
    vals = values(rows, key)
    if not vals:
        return None
    return statistics.median(vals)

def best(rows, key):
    vals = values(rows, key)
    if not vals:
        return None
    return max(vals)

def ratio(candidate, baseline):
    if candidate is None or baseline is None or baseline == 0:
        return None
    return candidate / baseline

grouped = defaultdict(list)
for row in rows:
    grouped[(row["protocol"], row["direction"], row["bytes"], row["driver"])].append(row)

aggregate_fields = [
    "protocol",
    "direction",
    "bytes",
    "driver",
    "repeats",
    "guest_mib_s_median",
    "guest_mib_s_best",
    "host_mib_s_median",
    "host_mib_s_best",
    "guest_cpu_seconds_median",
    "host_cpu_seconds_median",
    "uml_user_cpu_seconds_median",
    "uml_system_cpu_seconds_median",
    "uml_sched_run_seconds_median",
    "uml_sched_wait_seconds_median",
    "uml_sched_pcount_delta_median",
    "uml_voluntary_ctxt_switches_delta_median",
    "uml_involuntary_ctxt_switches_delta_median",
]

with open(aggregate_path, "w", encoding="utf-8", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=aggregate_fields, delimiter="\t")
    writer.writeheader()
    for protocol, direction, byte_count, driver in sorted(grouped):
        group = grouped[(protocol, direction, byte_count, driver)]
        writer.writerow({
            "protocol": protocol,
            "direction": direction,
            "bytes": byte_count,
            "driver": driver,
            "repeats": len(group),
            "guest_mib_s_median": fmt(median(group, "guest_mib_s")),
            "guest_mib_s_best": fmt(best(group, "guest_mib_s")),
            "host_mib_s_median": fmt(median(group, "host_mib_s")),
            "host_mib_s_best": fmt(best(group, "host_mib_s")),
            "guest_cpu_seconds_median": fmt(median(group, "guest_cpu_seconds")),
            "host_cpu_seconds_median": fmt(median(group, "host_cpu_seconds")),
            "uml_user_cpu_seconds_median": fmt(median(group, "uml_user_cpu_seconds")),
            "uml_system_cpu_seconds_median": fmt(median(group, "uml_system_cpu_seconds")),
            "uml_sched_run_seconds_median": fmt(median(group, "uml_sched_run_seconds")),
            "uml_sched_wait_seconds_median": fmt(median(group, "uml_sched_wait_seconds")),
            "uml_sched_pcount_delta_median": fmt(median(group, "uml_sched_pcount_delta")),
            "uml_voluntary_ctxt_switches_delta_median": fmt(
                median(group, "uml_voluntary_ctxt_switches_delta")
            ),
            "uml_involuntary_ctxt_switches_delta_median": fmt(
                median(group, "uml_involuntary_ctxt_switches_delta")
            ),
        })

comparison_fields = [
    "protocol",
    "direction",
    "bytes",
    "baseline_driver",
    "candidate_driver",
    "baseline_repeats",
    "candidate_repeats",
    "guest_mib_s_median_ratio",
    "guest_mib_s_best_ratio",
    "host_mib_s_median_ratio",
    "host_mib_s_best_ratio",
    "uml_system_cpu_seconds_median_ratio",
    "uml_sched_run_seconds_median_ratio",
    "uml_sched_pcount_delta_median_ratio",
    "uml_voluntary_ctxt_switches_delta_median_ratio",
]

by_shape = defaultdict(dict)
for (protocol, direction, byte_count, driver), group in grouped.items():
    by_shape[(protocol, direction, byte_count)][driver] = group

with open(comparison_path, "w", encoding="utf-8", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=comparison_fields, delimiter="\t")
    writer.writeheader()
    for protocol, direction, byte_count in sorted(by_shape):
        drivers = by_shape[(protocol, direction, byte_count)]
        if "vector" not in drivers or "vector2" not in drivers:
            continue
        base = drivers["vector"]
        candidate = drivers["vector2"]
        writer.writerow({
            "protocol": protocol,
            "direction": direction,
            "bytes": byte_count,
            "baseline_driver": "vector",
            "candidate_driver": "vector2",
            "baseline_repeats": len(base),
            "candidate_repeats": len(candidate),
            "guest_mib_s_median_ratio": fmt(
                ratio(median(candidate, "guest_mib_s"), median(base, "guest_mib_s"))
            ),
            "guest_mib_s_best_ratio": fmt(
                ratio(best(candidate, "guest_mib_s"), best(base, "guest_mib_s"))
            ),
            "host_mib_s_median_ratio": fmt(
                ratio(median(candidate, "host_mib_s"), median(base, "host_mib_s"))
            ),
            "host_mib_s_best_ratio": fmt(
                ratio(best(candidate, "host_mib_s"), best(base, "host_mib_s"))
            ),
            "uml_system_cpu_seconds_median_ratio": fmt(
                ratio(
                    median(candidate, "uml_system_cpu_seconds"),
                    median(base, "uml_system_cpu_seconds"),
                )
            ),
            "uml_sched_run_seconds_median_ratio": fmt(
                ratio(
                    median(candidate, "uml_sched_run_seconds"),
                    median(base, "uml_sched_run_seconds"),
                )
            ),
            "uml_sched_pcount_delta_median_ratio": fmt(
                ratio(
                    median(candidate, "uml_sched_pcount_delta"),
                    median(base, "uml_sched_pcount_delta"),
                )
            ),
            "uml_voluntary_ctxt_switches_delta_median_ratio": fmt(
                ratio(
                    median(candidate, "uml_voluntary_ctxt_switches_delta"),
                    median(base, "uml_voluntary_ctxt_switches_delta"),
                )
            ),
        })
PY
}

phase_toml() {
	case "$1" in
	guest-to-host)
		cat <<'EOF'
[[init.phases]]
name = "guest-to-host"
cmd = """
DEV="${UMLCTL_NETDEV:-}"
echo "VECTOR_NET_DIAG_BEGIN before direction=guest-to-host dev=$DEV"
ip -d link show "$DEV" || true
ip -s link show "$DEV" || true
ip route show || true
if command -v ethtool >/dev/null 2>&1; then
    ethtool -k "$DEV" || true
    ethtool -S "$DEV" || true
else
    echo "VECTOR_NET_DIAG ethtool=missing"
fi
echo "VECTOR_NET_DIAG_END before direction=guest-to-host dev=$DEV"
python3 - "$UMLCTL_GATEWAY" "$UML_VECTOR_PERF_PORT" "$UML_VECTOR_PERF_BYTES" <<'PY'
import os
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
total = int(sys.argv[3])
protocol = os.environ.get("UML_VECTOR_PERF_PROTOCOL", "tcp")
nodelay = int(os.environ.get("UML_VECTOR_PERF_TCP_NODELAY", "0"))
tcp_chunk = int(os.environ.get("UML_VECTOR_PERF_GUEST_CHUNK", "65536"))
udp_payload = int(os.environ.get("UML_VECTOR_PERF_UDP_PAYLOAD", "1472"))
udp_pace = int(os.environ.get("UML_VECTOR_PERF_UDP_PACE_USEC", "0")) / 1_000_000.0
udp_sndbuf = int(os.environ.get("UML_VECTOR_PERF_UDP_SNDBUF", "0"))
sent = 0
start = time.monotonic()
cpu_start = time.process_time()
if protocol == "tcp":
    chunk = bytes(tcp_chunk)
    with socket.create_connection((host, port), timeout=20) as sock:
        if nodelay:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        while sent < total:
            n = min(len(chunk), total - sent)
            sock.sendall(chunk[:n])
            sent += n
        sock.shutdown(socket.SHUT_WR)
elif protocol == "udp":
    chunk = bytes(udp_payload)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if udp_sndbuf:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, udp_sndbuf)
        sock.connect((host, port))
        while sent < total:
            n = min(len(chunk), total - sent)
            sent += sock.send(chunk[:n])
            if udp_pace:
                time.sleep(udp_pace)
else:
    print(f"unsupported protocol={protocol}", file=sys.stderr)
    sys.exit(2)
elapsed = max(time.monotonic() - start, 1e-9)
cpu_elapsed = max(time.process_time() - cpu_start, 0.0)
mib_s = sent / 1048576.0 / elapsed
print(
    "VECTOR_NET_PERF "
    f"protocol={protocol} "
    "direction=guest-to-host "
    f"driver={os.environ.get('UMLCTL_NETWORK_DRIVER', '')} "
    f"transport={os.environ.get('UMLCTL_NETWORK_TRANSPORT', '')} "
    f"queues={os.environ.get('UMLCTL_NETWORK_QUEUES', '')} "
    f"bytes={sent} seconds={elapsed:.6f} mib_s={mib_s:.3f} "
    f"cpu_seconds={cpu_elapsed:.6f}"
)
PY
rc=$?
echo "VECTOR_NET_DIAG_BEGIN after direction=guest-to-host dev=$DEV"
ip -s link show "$DEV" || true
if command -v ethtool >/dev/null 2>&1; then
    ethtool -S "$DEV" || true
else
    echo "VECTOR_NET_DIAG ethtool=missing"
fi
echo "VECTOR_NET_DIAG_END after direction=guest-to-host dev=$DEV"
if [ "$rc" -ne 0 ]; then
    exit "$rc"
fi
echo "VECTOR_NET_PERF_OK"
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
DEV="${UMLCTL_NETDEV:-}"
echo "VECTOR_NET_DIAG_BEGIN before direction=host-to-guest dev=$DEV"
ip -d link show "$DEV" || true
ip -s link show "$DEV" || true
ip route show || true
if command -v ethtool >/dev/null 2>&1; then
    ethtool -k "$DEV" || true
    ethtool -S "$DEV" || true
else
    echo "VECTOR_NET_DIAG ethtool=missing"
fi
echo "VECTOR_NET_DIAG_END before direction=host-to-guest dev=$DEV"
python3 - "$UML_VECTOR_PERF_PORT" "$UML_VECTOR_PERF_BYTES" <<'PY'
import os
import socket
import sys
import time

port = int(sys.argv[1])
expected = int(sys.argv[2])
protocol = os.environ.get("UML_VECTOR_PERF_PROTOCOL", "tcp")
udp_payload = int(os.environ.get("UML_VECTOR_PERF_UDP_PAYLOAD", "1472"))
udp_rcvbuf = int(os.environ.get("UML_VECTOR_PERF_UDP_RCVBUF", "0"))

if protocol == "tcp":
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", port))
        sock.listen(1)
        print("GUEST_SINK_READY", flush=True)
        conn, addr = sock.accept()
        with conn:
            start = time.monotonic()
            cpu_start = time.process_time()
            received = 0
            while True:
                data = conn.recv(1024 * 1024)
                if not data:
                    break
                received += len(data)
elif protocol == "udp":
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if udp_rcvbuf:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, udp_rcvbuf)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", port))
        sock.settimeout(20)
        print("GUEST_SINK_READY", flush=True)
        received = 0
        addr = None
        start = None
        cpu_start = None
        while received < expected:
            try:
                data, addr = sock.recvfrom(max(udp_payload, 1) + 64)
            except TimeoutError:
                break
            if start is None:
                start = time.monotonic()
                cpu_start = time.process_time()
            received += len(data)
        if start is None:
            start = time.monotonic()
            cpu_start = time.process_time()
else:
    print(f"unsupported protocol={protocol}", file=sys.stderr)
    sys.exit(2)

elapsed = max(time.monotonic() - start, 1e-9)
cpu_elapsed = max(time.process_time() - cpu_start, 0.0)
mib_s = received / 1048576.0 / elapsed
print(
    "VECTOR_NET_PERF "
    f"protocol={protocol} "
    "direction=host-to-guest "
    f"driver={os.environ.get('UMLCTL_NETWORK_DRIVER', '')} "
    f"transport={os.environ.get('UMLCTL_NETWORK_TRANSPORT', '')} "
    f"queues={os.environ.get('UMLCTL_NETWORK_QUEUES', '')} "
    f"bytes={received} seconds={elapsed:.6f} mib_s={mib_s:.3f} "
    f"cpu_seconds={cpu_elapsed:.6f} addr={addr}"
)
if received != expected:
    sys.exit(3)
PY
rc=$?
echo "VECTOR_NET_DIAG_BEGIN after direction=host-to-guest dev=$DEV"
ip -s link show "$DEV" || true
if command -v ethtool >/dev/null 2>&1; then
    ethtool -S "$DEV" || true
else
    echo "VECTOR_NET_DIAG ethtool=missing"
fi
echo "VECTOR_NET_DIAG_END after direction=host-to-guest dev=$DEV"
if [ "$rc" -ne 0 ]; then
    exit "$rc"
fi
echo "VECTOR_NET_PERF_OK"
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
	local safe_protocol="${protocol//[^A-Za-z0-9_.-]/_}"
	local run_dir="$out/${safe_driver}-${safe_dir}-${safe_protocol}-b${run_bytes}-r${repeat_idx}"
	local umlf="$run_dir/Umlfile.toml"
	local umlctl_log="$run_dir/umlctl-up.log"
	local guest_log="$run_dir/guest.log"
	local host_log="$run_dir/host-${safe_dir}-${safe_protocol}.log"
	local metrics_before="$run_dir/uml-metrics-before.json"
	local metrics_after="$run_dir/uml-metrics-after.json"
	local perf_log="$run_dir/perf-stat.csv"
	local name="vperf-$safe_driver-$safe_dir-$safe_protocol-b${run_bytes}-r${repeat_idx}"
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
labels = { service = "vector-net-perf", driver = "$driver", protocol = "$protocol" }

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
host_mode = "$host_mode"
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
UML_VECTOR_PERF_PROTOCOL = "$protocol"
UML_VECTOR_PERF_TCP_NODELAY = "$tcp_nodelay"
UML_VECTOR_PERF_GUEST_CHUNK = "$guest_chunk"
UML_VECTOR_PERF_UDP_PAYLOAD = "$udp_payload"
UML_VECTOR_PERF_UDP_PACE_USEC = "$udp_pace_usec"
UML_VECTOR_PERF_UDP_RCVBUF = "$udp_rcvbuf"
UML_VECTOR_PERF_UDP_SNDBUF = "$udp_sndbuf"

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
			echo "host $protocol sink failed for driver=$driver; see $host_log" >&2
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

		collect_metrics "$name" "$metrics_before" || true
		if [[ "$perf_stat" == "1" ]]; then
			start_transfer_perf "$(metrics_pid "$metrics_before")" "$perf_log"
		fi

		if ! host_send "10.93.0.2" "$host_log" "$run_bytes"; then
			stop_transfer_perf
			cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
				--bin umlctl -- down -f "$umlf" --force --rm >/dev/null 2>&1 || true
			echo "host $protocol send failed for driver=$driver; see $host_log" >&2
			return 1
		fi
		if ! wait_guest_marker "$name" VECTOR_NET_PERF_OK "$guest_log"; then
			stop_transfer_perf
			cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
				--bin umlctl -- down -f "$umlf" --force --rm >/dev/null 2>&1 || true
			return 1
		fi
		stop_transfer_perf
		if [[ "$perf_stat" == "1" && ! -s "$perf_log" ]]; then
			cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
				--bin umlctl -- down -f "$umlf" --force --rm >/dev/null 2>&1 || true
			echo "perf stat requested but no counters were written for driver=$driver; see $perf_log" >&2
			return 1
		fi
		collect_metrics "$name" "$metrics_after" || true
	fi

	cargo run --manifest-path "$repo_root/tools/uml/uml-launcher/Cargo.toml" \
		--bin umlctl -- down -f "$umlf" --force --rm >/dev/null 2>&1 || true

	local guest_line host_line guest_seconds guest_mib_s host_seconds host_mib_s guest_cpu_seconds host_cpu_seconds metrics_fields metrics_before_log metrics_after_log perf_log_field perf_stat_fields
	guest_line="$(grep 'VECTOR_NET_PERF ' "$guest_log" | tail -1)"
	host_line="$(grep -E 'HOST_(SINK|SEND) ' "$host_log" | tail -1)"
	guest_seconds="$(sed -n 's/.* seconds=\([0-9.]*\).*/\1/p' <<<"$guest_line")"
	guest_mib_s="$(sed -n 's/.* mib_s=\([0-9.]*\).*/\1/p' <<<"$guest_line")"
	host_seconds="$(sed -n 's/.* seconds=\([0-9.]*\).*/\1/p' <<<"$host_line")"
	host_mib_s="$(sed -n 's/.* mib_s=\([0-9.]*\).*/\1/p' <<<"$host_line")"
	guest_cpu_seconds="$(sed -n 's/.* cpu_seconds=\([0-9.]*\).*/\1/p' <<<"$guest_line")"
	host_cpu_seconds="$(sed -n 's/.* cpu_seconds=\([0-9.]*\).*/\1/p' <<<"$host_line")"
	metrics_fields="$(metrics_delta "$metrics_before" "$metrics_after")"
	metrics_before_log=""
	metrics_after_log=""
	if [[ -s "$metrics_before" && -s "$metrics_after" ]]; then
		metrics_before_log="$metrics_before"
		metrics_after_log="$metrics_after"
	fi
	perf_log_field=""
	if [[ -s "$perf_log" ]]; then
		perf_log_field="$perf_log"
	fi
	perf_stat_fields="$(perf_fields "$perf_log")"

	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$driver" "$perf_dir" "$run_bytes" "$repeat_idx" "$guest_seconds" "$guest_mib_s" \
		"$host_seconds" "$host_mib_s" "$guest_log" "$host_log" "$protocol" \
		"$guest_cpu_seconds" "$host_cpu_seconds" $metrics_fields \
		"$metrics_before_log" "$metrics_after_log" >> "$summary"
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$driver" "$perf_dir" "$run_bytes" "$repeat_idx" "$perf_log_field" \
		"$perf_stat_fields" >> "$perf_summary"
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
write_aggregate_summaries
echo "aggregate: $out/aggregate.tsv"
echo "comparison: $out/comparison.tsv"
