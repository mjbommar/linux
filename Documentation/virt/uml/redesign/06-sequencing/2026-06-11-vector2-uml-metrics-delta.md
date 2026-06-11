# UML vector2 host-to-guest process metric deltas

Status: host-to-guest UML process metric deltas on `next`; privileged
`perf stat` syscall-rate remains open.
Date: 2026-06-11.
Tree: `next`.

## Purpose

The current vector2 publication blocker is the small host-to-guest fixed-byte
cell.  The existing helper already records throughput, endpoint CPU time, and
guest link/ethtool diagnostics.  It did not record how much CPU and scheduler
work the UML host process consumed during the host-to-guest transfer window.

## What Changed

`tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh` now samples
`umlctl metrics --json` around host-to-guest transfers:

- before sample: after the guest sink reports `GUEST_SINK_READY`;
- after sample: after `VECTOR_NET_PERF_OK` is observed; and
- both JSON files are kept in the per-run output directory.

The helper appends these summary columns:

```text
uml_user_cpu_seconds
uml_system_cpu_seconds
uml_sched_run_seconds
uml_sched_wait_seconds
uml_sched_pcount_delta
uml_voluntary_ctxt_switches_delta
uml_involuntary_ctxt_switches_delta
uml_metrics_before_log
uml_metrics_after_log
```

Guest-to-host runs do not have a clean transfer window in the current helper,
so these columns are `NA` for that direction.

## Validation

Syntax and whitespace:

```sh
bash -n tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh
git diff --check
```

Result: PASS.

Host-to-guest metric smoke:

```sh
rm -rf /tmp/um-vector-perf-metrics-h2g-smoke
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-metrics-h2g-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=65536 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19160 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Result: PASS.  The summary row recorded:

```text
uml_user_cpu_seconds=0.020000
uml_system_cpu_seconds=0.500000
uml_sched_run_seconds=0.525344
uml_sched_wait_seconds=0.000360
uml_sched_pcount_delta=1799
uml_voluntary_ctxt_switches_delta=1785
uml_involuntary_ctxt_switches_delta=13
```

Side-by-side host-to-guest metric smoke:

```sh
rm -rf /tmp/um-vector-perf-metrics-h2g-both-smoke
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-metrics-h2g-both-smoke \
UML_VECTOR_PERF_DRIVERS=vector,vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=65536 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19162 \
  timeout 420s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Result: PASS for both legacy vector and vector2.  The summary shape stayed at
22 fields for the header and both rows, and both rows carried real UML process
metric deltas.

```text
driver  guest_mib_s  host_mib_s  uml_user_cpu_seconds  uml_system_cpu_seconds  uml_sched_run_seconds  uml_sched_pcount_delta
vector  0.144        19.905      0.100000              0.570000                0.673587               5368
vector2 0.099        19.546      0.010000              0.530000                0.544351               1800
```

Guest-to-host shape smoke:

```sh
rm -rf /tmp/um-vector-perf-metrics-g2h-smoke
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-metrics-g2h-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=guest-to-host \
UML_VECTOR_PERF_BYTES_LIST=65536 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19161 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Result: PASS.  The summary shape stayed stable and the UML process metric
columns were `NA`.

Cleanup checks found no stale `vperf-*` TAP devices and no live `umlctl`
instances from the smoke runs.

## Perf Boundary

The host has `perf` installed and exposes syscall tracepoints, but the current
unprivileged environment blocks the required events:

```text
perf_event_paranoid setting is 4
No permissions to read /sys/kernel/tracing//events/syscalls/...
```

These metric deltas therefore do not close the P4.3 syscall-rate gate.  They
are a repeatable, unprivileged diagnostic for the host-to-guest bottleneck
pass.  The final publication gate still needs privileged `perf stat -e
syscalls:sys_*` or an equivalent accepted syscall-rate measurement, plus
steady-state CPU accounting.
