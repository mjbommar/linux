# Vector2 Buffered Unpaced UDP Refresh

Date: 2026-06-11
Branch: `next`

## Purpose

This note records the next UDP publication-matrix slice for the vector2
fixed-byte performance helper.  The prior UDP evidence was deliberately paced
at 1 MiB because unpaced host-to-guest UDP lost datagrams before exact-byte
completion.  This slice adds explicit UDP socket buffer controls and records
what changes with the larger host and guest socket buffers.

## Harness Change

`tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh` now accepts:

- `UML_VECTOR_PERF_UDP_RCVBUF`, default `4194304`;
- `UML_VECTOR_PERF_UDP_SNDBUF`, default `4194304`.

The host and guest UDP sinks request `SO_RCVBUF`; the host and guest UDP
senders request `SO_SNDBUF`.  The generated Umlfile propagates both values
into the guest environment.  The wait loop also fails fast if the guest log
contains a kernel panic or init-exit panic before the expected success marker,
instead of waiting for the full marker timeout.

## Validation

Static validation:

```sh
bash -n tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh
tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh --help
git diff --check
```

Buffered unpaced 1 MiB UDP matrix:

```sh
rm -rf /tmp/um-vector-perf-udp-1m-unpaced-buf
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-udp-1m-unpaced-buf \
UML_VECTOR_PERF_DRIVERS=vector,vector2 \
UML_VECTOR_PERF_DIRECTION=both \
UML_VECTOR_PERF_PROTOCOL=udp \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19170 \
  timeout 900s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Result: PASS for both drivers and both directions.

```text
driver   direction       guest_mib_s  host_mib_s
vector   guest-to-host   101.948      105.918
vector   host-to-guest    40.088      466.051
vector2  guest-to-host    98.605      102.022
vector2  host-to-guest    45.803      459.233
```

`comparison.tsv` reported vector2/vector host-side median ratios of
`0.963217` for guest-to-host and `0.985371` for host-to-guest.  The
host-to-guest UML process scheduler pcount and voluntary context-switch median
ratios were `1.020321` and `1.019545`, respectively.

Buffered unpaced 8 MiB guest-to-host evidence:

```text
driver   direction       guest_mib_s  host_mib_s
vector   guest-to-host   100.344      100.736
vector2  guest-to-host    97.317       97.788
```

8 MiB host-to-guest exact-byte completion remains open even with the larger
socket buffers.  The host sender completed the full payload, but the guest
received fewer bytes before exiting:

```text
driver   host_sent_bytes  guest_received_bytes
vector   8388608          5358080
vector2  8388608          5674560
vector2  8388608          5674560  # fail-fast validation rerun
```

The fail-fast validation run returned in about 25 seconds with:

```text
guest exited before VECTOR_NET_PERF_OK in vperf-vector2-host-to-guest-udp-b8388608-r1
```

## Current Reading

The 1 MiB unpaced UDP gap is no longer open.  Buffered unpaced 1 MiB UDP is
now positive side-by-side evidence, and 8 MiB guest-to-host UDP is also
positive for both drivers.  Larger host-to-guest UDP is still not publication
ready because both legacy vector and vector2 lose datagrams before exact-byte
completion at 8 MiB.  The next UDP work should focus on whether the
host-to-guest condition needs pacing, rate limiting, socket-buffer
documentation, or a different acceptance shape for UDP's lossy semantics.
