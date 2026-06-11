# UML vector2 1 MiB host-to-guest metric diagnostic

Status: focused bottleneck evidence on `next`; host-to-guest publication cell
remains open.
Date: 2026-06-11.
Tree: `next`.

## Purpose

The remaining fixed-byte TCP gap is the 1 MiB host-to-guest cell.  The helper
now records endpoint CPU time plus UML host-process metric deltas, so this run
uses that instrumentation on the actual open cell rather than another small
schema smoke.

## Command

```sh
rm -rf /tmp/um-vector-perf-h2g-1m-metrics
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-h2g-1m-metrics \
UML_VECTOR_PERF_DRIVERS=vector,vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=4 \
UML_VECTOR_PERF_PORT=19170 \
  timeout 1200s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Result: PASS for all eight rows.

Post-run cleanup checks found no stale `vperf-*` TAP devices and no live
`umlctl` instances from the run.  The generated `summary.tsv` had 22 fields
for the header and every row.

## Throughput

Host-side MiB/s:

| Driver | Values | Median | Best |
| --- | --- | ---: | ---: |
| vector | 1.630, 0.342, 1.602, 0.367 | 0.9845 | 1.630 |
| vector2 | 0.909, 1.085, 0.892, 0.882 | 0.9005 | 1.085 |

Ratios:

| Ratio | Value |
| --- | ---: |
| vector2/vector median | 0.9147 |
| vector2/vector best | 0.6656 |

Guest-side MiB/s:

| Driver | Values | Median | Best |
| --- | --- | ---: | ---: |
| vector | 1.591, 0.282, 1.566, 0.299 | 0.9325 | 1.591 |
| vector2 | 0.876, 1.039, 0.861, 0.852 | 0.8685 | 1.039 |

The legacy vector row is still highly variable.  Vector2 is steadier, but it
does not satisfy the host-to-guest no-regression bar: the median remains below
legacy, and the best observed vector2 host-side rate is only 0.6656 of the
best observed legacy vector rate.

## UML Process Deltas

Median UML host-process deltas during the transfer window:

| Metric | vector | vector2 | vector2/vector |
| --- | ---: | ---: | ---: |
| user CPU seconds | 0.020 | 0.150 | 7.50 |
| system CPU seconds | 0.310 | 0.670 | 2.16 |
| sched run seconds | 0.338087 | 0.817219 | 2.42 |
| sched wait seconds | 0.000319 | 0.001522 | 4.77 |
| sched pcount delta | 2213.0 | 26547.5 | 12.00 |
| voluntary context switches | 2186.5 | 26505.5 | 12.12 |
| involuntary context switches | 11.5 | 29.5 | 2.57 |

Representative summary rows:

```text
vector  host_mib_s=1.630 uml_system_cpu_seconds=0.100000 uml_sched_pcount_delta=2402
vector  host_mib_s=0.342 uml_system_cpu_seconds=0.540000 uml_sched_pcount_delta=2024
vector2 host_mib_s=0.909 uml_system_cpu_seconds=0.670000 uml_sched_pcount_delta=26781
vector2 host_mib_s=1.085 uml_system_cpu_seconds=0.620000 uml_sched_pcount_delta=16795
```

## Interpretation

This run does not close the host-to-guest 1 MiB blocker.  It gives a more
specific next target: vector2 is spending materially more UML process scheduler
activity per transfer, especially voluntary context switches and scheduler
pcount.  The next bottleneck pass should inspect the host-to-guest wakeup,
readiness, and receive scheduling path rather than only RX-slot allocation,
which lazy RX already cleaned up.

The run also does not close the P4.3 syscall-rate gate.  Local privileged
`perf stat -e syscalls:sys_enter_*` remains unavailable under
`perf_event_paranoid=4`.
