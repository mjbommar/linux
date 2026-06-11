# UML vector2 fixed-byte CPU timing

Status: helper-level CPU timing on `next`; full syscall-rate and
CPU-utilisation publication gates remain open.
Date: 2026-06-11.
Tree: `next`.

## Purpose

The vector2 publication plan still needs CPU and syscall evidence in addition
to throughput. The fixed-byte helper already records wall-clock seconds and
MiB/s for guest and host endpoints, but it did not expose even a lightweight
per-process CPU signal for focused runs.

## What Changed

`tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh` now records
Python process CPU time for the active sender or sink on both sides:

- guest `VECTOR_NET_PERF` lines include `cpu_seconds=...`;
- host `HOST_SINK` and `HOST_SEND` lines include `cpu_seconds=...`; and
- `summary.tsv` appends `guest_cpu_seconds` and `host_cpu_seconds`.

The existing defaults are unchanged. TCP remains the default protocol, UDP
remains opt-in through `UML_VECTOR_PERF_PROTOCOL=udp`, and existing throughput
columns stay in the same order.

## Measurement Boundary

This is deliberately not the final CPU-utilisation gate. It records CPU time
for the Python endpoint process doing the fixed-byte send or receive. It does
not measure total host CPU, UML kernel CPU, softirq time, or syscall counts,
and short or paced runs can round small guest process times to `0.000000`.

The remaining P4.3 gate still needs full-system CPU-utilisation and
syscall-rate evidence, such as `perf stat -e syscalls:sys_*` paired with
steady-state CPU accounting.

## Validation

Syntax and whitespace:

```sh
bash -n tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh
git diff --check
```

Result: PASS.

TCP default compatibility smoke:

```sh
rm -rf /tmp/um-vector-perf-cpu-tcp-smoke
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-cpu-tcp-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=guest-to-host \
UML_VECTOR_PERF_BYTES_LIST=65536 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19150 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Result: PASS.

```text
VECTOR_NET_PERF protocol=tcp direction=guest-to-host driver=vector2 transport=fd queues=4 bytes=65536 seconds=0.012901 mib_s=4.844 cpu_seconds=0.020000
HOST_SINK protocol=tcp bytes=65536 seconds=0.001073 mib_s=58.265 cpu_seconds=0.000191 addr=('10.93.0.2', 42814)
```

The TCP summary includes the appended CPU columns:

```text
driver direction bytes repeat guest_seconds guest_mib_s host_seconds host_mib_s guest_log host_log protocol guest_cpu_seconds host_cpu_seconds
vector2 guest-to-host 65536 1 0.012901 4.844 0.001073 58.265 ... tcp 0.020000 0.000191
```

UDP bidirectional smoke:

```sh
rm -rf /tmp/um-vector-perf-cpu-udp-smoke
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-cpu-udp-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=both \
UML_VECTOR_PERF_PROTOCOL=udp \
UML_VECTOR_PERF_BYTES_LIST=65536 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19151 \
UML_VECTOR_PERF_UDP_PACE_USEC=100 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Result: PASS for guest-to-host and host-to-guest.

```text
VECTOR_NET_PERF protocol=udp direction=guest-to-host driver=vector2 transport=fd queues=4 bytes=65536 seconds=0.008822 mib_s=7.085 cpu_seconds=0.000000
HOST_SINK protocol=udp bytes=65536 seconds=0.008275 mib_s=7.553 cpu_seconds=0.000458 addr=('10.93.0.2', 50340)
VECTOR_NET_PERF protocol=udp direction=host-to-guest driver=vector2 transport=fd queues=4 bytes=65536 seconds=0.006810 mib_s=9.178 cpu_seconds=0.000000 addr=('10.93.0.1', 60921)
HOST_SEND protocol=udp bytes=65536 seconds=0.007194 mib_s=8.688 cpu_seconds=0.000476
```

Cleanup checks found no stale `vperf-*` TAP devices and no live `umlctl`
instances from the smoke runs.

## Disposition

This closes the missing fixed-byte helper support for endpoint CPU timing.
It does not close the P4.3 CPU-utilisation or syscall-rate gate. Future
publication runs can use the appended columns as a lightweight diagnostic
alongside the stronger `perf stat` and steady-state CPU accounting that remain
required.
