# UML vector2 UDP performance baseline

Status: initial UDP harness and paced fixed-byte evidence on `next`; broader
publication gates remain open.
Date: 2026-06-11.
Tree: `next`.

## Purpose

The vector2 publication plan requires UDP evidence in addition to the TCP
guest-to-host gate.  The existing `vector-net-perf-baseline.sh` helper only
measured TCP, so UDP remained a missing P4.3 data point.

## What Changed

- Added `UML_VECTOR_PERF_PROTOCOL=tcp|udp` and `--protocol tcp|udp`.
- Kept TCP as the default protocol.
- Added UDP fixed-byte sender/sink paths for both directions.
- Added `UML_VECTOR_PERF_UDP_PAYLOAD`, defaulting to 1472 bytes.
- Added `UML_VECTOR_PERF_UDP_PACE_USEC`, defaulting to 0, so operators can run
  raw unpaced UDP or a paced no-drop fixed-byte diagnostic.
- Added a `protocol` column to `summary.tsv`.

## Validation

Syntax and help:

```sh
bash -n tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh
tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh --help
```

TCP default compatibility smoke:

```sh
rm -rf /tmp/um-vector-perf-tcp-smoke
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-tcp-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=guest-to-host \
UML_VECTOR_PERF_BYTES_LIST=65536 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19140 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Result: PASS.  The run produced `VECTOR_NET_PERF protocol=tcp` and a
`summary.tsv` row with `protocol=tcp`.

UDP vector2 bidirectional smoke:

```sh
rm -rf /tmp/um-vector-perf-udp-smoke
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-udp-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=both \
UML_VECTOR_PERF_PROTOCOL=udp \
UML_VECTOR_PERF_BYTES_LIST=65536 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19141 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Result: PASS for guest-to-host and host-to-guest.

Paced 1 MiB UDP matrix:

```sh
rm -rf /tmp/um-vector-perf-udp-1m-paced
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-udp-1m-paced \
UML_VECTOR_PERF_DRIVERS=vector,vector2 \
UML_VECTOR_PERF_DIRECTION=both \
UML_VECTOR_PERF_PROTOCOL=udp \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19144 \
UML_VECTOR_PERF_UDP_PACE_USEC=100 \
  timeout 900s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Host-side MiB/s:

| Driver | Direction | Host MiB/s |
| ------ | --------- | ---------- |
| vector | guest-to-host | 7.471 |
| vector | host-to-guest | 8.760 |
| vector2 | guest-to-host | 7.319 |
| vector2 | host-to-guest | 8.660 |

An unpaced 1 MiB UDP matrix attempt completed the legacy vector
guest-to-host row, but legacy vector host-to-guest did not reach the exact-byte
completion marker after the host sent the full payload.  That is useful
negative evidence, but not a passing UDP publication gate.

## Disposition

This closes the missing UDP harness capability and records a first paced 1 MiB
legacy-vs-vector2 UDP comparison.  It does not close vector2 publication
readiness: unpaced/larger UDP behavior, syscall-rate data, CPU-utilisation
data, multiqueue fairness, the natural seccomp long run, full KVM-v2 Tier 3
networking, and the 1 MiB TCP host-to-guest follow-up remain open.
