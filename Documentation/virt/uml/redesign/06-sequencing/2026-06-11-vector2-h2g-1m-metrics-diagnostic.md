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

The run also does not close the P4.3 syscall-rate gate.  The helper now has
bounded transfer-window `perf stat` support for host-to-guest runs, but this
diagnostic used only the unprivileged `umlctl metrics` deltas.

## Parameter And Topology Sweep

The next bounded pass used the same 1 MiB host-to-guest cell with three repeats
per driver.  Each run enabled transfer-window perf collection:

```sh
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_PROTOCOL=tcp \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=3 \
UML_VECTOR_PERF_PERF_STAT=1 \
UML_VECTOR_PERF_PERF_SECONDS=1 \
  tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

The cells varied only the named knob:

| Cell | Extra setting | Host median ratio | Host best ratio | Scheduler pcount ratio | Transfer-window syscall ratio |
| --- | --- | ---: | ---: | ---: | ---: |
| default fd/multiqueue | none | 0.650602 | 0.655297 | 1.193753 | 0.179778 |
| 16 KiB host chunk | `UML_VECTOR_PERF_HOST_CHUNK=16384` | 1.788009 | 0.530945 | 0.234691 | 1.005365 |
| TCP_NODELAY | `UML_VECTOR_PERF_TCP_NODELAY=1` | 0.560706 | 0.524691 | 1.685262 | 0.092742 |
| single-queue fd | `UML_VECTOR_PERF_QUEUES=1` | 0.810510 | 0.906732 | 1.214615 | 0.362521 |
| in-process TAP | `UML_VECTOR_PERF_HOST_MODE=inproc` | 0.639896 | 0.661140 | 1.141699 | 0.173316 |

Result: PASS for all 30 raw transfer rows, with no stale `vperf-*` TAP devices
or live `umlctl` instances after the runs.

The 16 KiB chunk cell is not a closure despite the median ratio above 1.0:
legacy vector had two slow outliers, and the best-run vector2/vector ratio was
only 0.530945.  `TCP_NODELAY` and in-process TAP also do not close the cell.
The strongest hint is single-queue fd mode: it improves the host-side median
ratio to 0.810510 and the best ratio to 0.906732, but it still misses the
median no-regression bar.  That points the next implementation pass at vector2
queue selection, wakeup, and receive scheduling policy rather than at fd
handoff alone.
