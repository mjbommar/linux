# UML vector driver v2 initial performance baseline

**Status:** partial performance evidence - TCP and initial paced UDP smoke.
**Date:** 2026-05-17.

This note records the first repeatable legacy-vs-vector2 performance
baseline harness for `umlctl`.  It is intentionally small: one TCP
stream per direction and one host.  The harness now supports multiple
transfer sizes and repetitions, which makes it more useful as a
regression tripwire and as a convenient operator workflow, but it does
not close the replacement performance gate.

## Harness

New helper:

```text
tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh
```

The helper:

- generates a temporary Umlfile per driver;
- supports `guest-to-host`, `host-to-guest`, or `both`;
- supports `tcp` or `udp`, with TCP as the default;
- starts a host Python sink for guest-to-host runs;
- starts a guest Python sink for host-to-guest runs;
- boots the UML guest through `umlctl up`;
- sends one or more fixed byte counts across the selected direction;
- records guest-side and host-side MiB/s;
- records guest-side and host-side endpoint process CPU seconds;
- writes a TSV summary, including transfer size, repeat index, protocol,
  endpoint CPU seconds, and per-run logs;
- tears the instance down with `umlctl down --force --rm`.

Defaults:

```text
drivers: vector,vector2
direction: guest-to-host
bytes:   33554432
protocol: tcp
repeat:  1
port:    19091
backend: seccomp
queues:  auto for vector2, forced to 1 for legacy vector
out:     /tmp/um-vector-perf-baseline
```

Example:

```sh
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-baseline \
  tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
  --kernel /home/mjbommar/projects/personal/.build/um-vector-r2-both/linux

UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-h2g \
  tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
  --kernel /home/mjbommar/projects/personal/.build/um-vector-r2-both/linux

UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=both \
UML_VECTOR_PERF_BYTES_LIST=1048576,2097152 \
UML_VECTOR_PERF_REPEAT=3 \
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-repeat \
  tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
  --kernel /home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux
```

## Parser Collision Found

The first vector2 run against a both-drivers kernel exposed an important
side-by-side compatibility bug.  The legacy vector driver registers
`__setup("vec", ...)`, which also matches `vec2.0:...`.  Before the
fix, a both-drivers kernel logged:

```text
uml-vector: Couldn't parse '2.0': Bad device number
Cannot find device "vec2.0"
```

That meant the legacy parser consumed the v2 command-line surface before
the v2 parser could configure the device.

The legacy parser now declines `vec2.` and `vec2=` so vector2 owns those
prefixes, while legacy `vec2:` remains valid as old-driver unit 2.

## Build

Both-drivers runtime kernel:

```sh
make -j"$(nproc)" ARCH=um \
  O=/home/mjbommar/projects/personal/.build/um-vector-r2-both \
  linux
```

Relevant config:

```text
CONFIG_UML_NET_VECTOR=y
CONFIG_UML_NET_VECTOR_V2=y
# CONFIG_UML_NET_VECTOR_V2_INPROC is not set
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
```

## Guest-To-Host Result

Command:

```sh
rm -rf /tmp/um-vector-perf-baseline
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-baseline \
  tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
  --kernel /home/mjbommar/projects/personal/.build/um-vector-r2-both/linux
```

Output:

```text
VECTOR_NET_PERF driver=vector transport=tap queues=1 bytes=33554432 seconds=0.048336 mib_s=662.031
HOST_SINK bytes=33554432 seconds=0.033379 mib_s=958.685 addr=('10.93.0.2', 43978)
VECTOR_NET_PERF driver=vector2 transport=fd queues=4 bytes=33554432 seconds=0.145619 mib_s=219.751
HOST_SINK bytes=33554432 seconds=0.130875 mib_s=244.509 addr=('10.93.0.2', 50258)
summary: /tmp/um-vector-perf-baseline/summary.tsv
```

Summary:

```text
driver   direction      bytes     guest_mib_s   host_mib_s
vector   guest-to-host  33554432  662.031       958.685
vector2  guest-to-host  33554432  219.751       244.509
```

The logs showed:

- legacy vector used `vec0`, transport `tap`, queues `1`;
- vector2 used `vec2.0`, transport `fd`, queues `4`;
- vector2 registered successfully in a both-drivers kernel;
- no `uml-vector: Couldn't parse '2.0'` line remained;
- no lingering `vperf-vector0` or `vperf-vector20` TAP device remained.

## Host-To-Guest Result

Command:

```sh
rm -rf /tmp/um-vector-perf-h2g
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-h2g \
  tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
  --kernel /home/mjbommar/projects/personal/.build/um-vector-r2-both/linux
```

Output:

```text
VECTOR_NET_PERF direction=host-to-guest driver=vector transport=tap queues=1 bytes=33554432 seconds=10.021694 mib_s=3.193 addr=('10.93.0.1', 48692)
HOST_SEND bytes=33554432 seconds=10.001529 mib_s=3.200
VECTOR_NET_PERF direction=host-to-guest driver=vector2 transport=fd queues=4 bytes=33554432 seconds=0.070231 mib_s=455.642 addr=('10.93.0.1', 44128)
HOST_SEND bytes=33554432 seconds=0.072685 mib_s=440.253
summary: /tmp/um-vector-perf-h2g/summary.tsv
```

Summary:

```text
driver   direction      bytes     guest_mib_s   host_mib_s
vector   host-to-guest  33554432  3.193         3.200
vector2  host-to-guest  33554432  455.642       440.253
```

The logs showed vector2 registering successfully in the same
both-drivers kernel, no parser collision, and no lingering
`vperf-vec-h2g` or `vperf-v2-h2g` TAP device.

A final script smoke also verified `UML_VECTOR_PERF_DIRECTION=both`
with vector2 and a 1 MiB transfer in both directions.

## Repeated-Size Harness Smoke

The helper was extended with:

- `--bytes-list` / `UML_VECTOR_PERF_BYTES_LIST`;
- `--repeat` / `UML_VECTOR_PERF_REPEAT`;
- per-run output directories keyed by driver, direction, byte count,
  and repeat index;
- a `repeat` column in `summary.tsv`.

The helper later gained explicit TCP/UDP protocol selection and appended
endpoint CPU timing columns. Current summaries include `protocol`,
`guest_cpu_seconds`, `host_cpu_seconds`, and host-to-guest UML process metric
deltas from `umlctl metrics`.

Smoke command:

```sh
rm -rf /tmp/um-vector-perf-repeat-smoke
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=both \
UML_VECTOR_PERF_BYTES_LIST=1048576,2097152 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-repeat-smoke \
  timeout 900s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel /home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux
```

Output:

```text
VECTOR_NET_PERF direction=guest-to-host driver=vector2 transport=fd queues=4 bytes=1048576 seconds=0.016120 mib_s=62.034
HOST_SINK bytes=1048576 seconds=0.004909 mib_s=203.710 addr=('10.93.0.2', 56126)
VECTOR_NET_PERF direction=guest-to-host driver=vector2 transport=fd queues=4 bytes=2097152 seconds=0.022033 mib_s=90.771
HOST_SINK bytes=2097152 seconds=0.010026 mib_s=199.482 addr=('10.93.0.2', 40974)
VECTOR_NET_PERF direction=host-to-guest driver=vector2 transport=fd queues=4 bytes=1048576 seconds=0.003217 mib_s=310.834 addr=('10.93.0.1', 46518)
HOST_SEND bytes=1048576 seconds=0.005527 mib_s=180.922
VECTOR_NET_PERF direction=host-to-guest driver=vector2 transport=fd queues=4 bytes=2097152 seconds=0.004875 mib_s=410.256 addr=('10.93.0.1', 49884)
HOST_SEND bytes=2097152 seconds=0.006060 mib_s=330.042
summary: /tmp/um-vector-perf-repeat-smoke/summary.tsv
TAP_G2H_ABSENT
TAP_H2G_ABSENT
```

Summary shape:

```text
driver   direction      bytes    repeat  guest_mib_s  host_mib_s
vector2  guest-to-host  1048576  1       62.034       203.710
vector2  guest-to-host  2097152  1       90.771       199.482
vector2  host-to-guest  1048576  1       310.834      180.922
vector2  host-to-guest  2097152  1       410.256      330.042
```

This smoke verifies the harness mechanics and vector2 cleanup across
multiple transfer sizes.  It is not a replacement decision because it
does not compare legacy vector across those sizes and uses only one
repeat per size.

## Repeated Legacy-vs-Vector2 Sweep

The next run used the same helper for both drivers, both directions,
three transfer sizes, and two repeats per cell.

Command:

```sh
rm -rf /tmp/um-vector-perf-both-repeat
UML_VECTOR_PERF_DRIVERS=vector,vector2 \
UML_VECTOR_PERF_DIRECTION=both \
UML_VECTOR_PERF_BYTES_LIST=1048576,8388608,33554432 \
UML_VECTOR_PERF_REPEAT=2 \
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-both-repeat \
  timeout 1800s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel /home/mjbommar/projects/personal/.build/um-vector-r2-both/linux
```

Result:

```text
summary: /tmp/um-vector-perf-both-repeat/summary.tsv
```

Average guest-side throughput:

```text
driver   direction      bytes     repeats  avg_guest_mib_s
vector   guest-to-host  1048576   2        70.626
vector2  guest-to-host  1048576   2        45.532
vector   guest-to-host  8388608   2        357.916
vector2  guest-to-host  8388608   2        159.387
vector   guest-to-host  33554432  2        624.725
vector2  guest-to-host  33554432  2        224.369
vector   host-to-guest  1048576   2        0.498
vector2  host-to-guest  1048576   2        230.654
vector   host-to-guest  8388608   2        2.246
vector2  host-to-guest  8388608   2        357.231
vector   host-to-guest  33554432  2        2.373
vector2  host-to-guest  33554432  2        452.553
```

Cleanup checks after the run:

```text
TAP_ABSENT:vperf-vec-g2h
TAP_ABSENT:vperf-vec-h2g
TAP_ABSENT:vperf-v2-g2h
TAP_ABSENT:vperf-v2-h2g
UML_PROCESS_ABSENT
```

Interpretation:

- at the May baseline, the guest-to-host vector2 fd multiqueue regression
  persisted across all measured sizes;
- at the May baseline, the host-to-guest vector2 fd multiqueue path remained
  much faster than legacy vector TAP across all measured sizes;
- legacy host-to-guest results were especially weak and variable on
  this host;
- this is still a lightweight TCP smoke, not an accepted performance
  replacement decision.

## 2026-06-11 Current `next` TCP Refresh

After the TX/RX NAPI scheduling fix, the same lightweight fixed-byte harness
was rerun on current `next` with the in-tree `./linux` binary:

```sh
rm -rf /tmp/um-vector-perf-current-bidi
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-current-bidi \
UML_VECTOR_PERF_DIRECTION=both \
UML_VECTOR_PERF_BYTES_LIST=1048576,8388608,33554432 \
UML_VECTOR_PERF_REPEAT=2 \
UML_VECTOR_PERF_PORT=19093 \
  timeout 1800s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Summary:

```text
summary: /tmp/um-vector-perf-current-bidi/summary.tsv
```

Best observed host-side throughput:

```text
direction      bytes     vector_mib_s  vector2_mib_s  ratio
guest-to-host  1048576   754.867       729.377        0.966
guest-to-host  8388608   981.650       988.662        1.007
guest-to-host  33554432  1100.600      1018.140       0.925
host-to-guest  1048576   1.887         0.912          0.483
host-to-guest  8388608   1.200         2.403          2.002
host-to-guest  33554432  4.219         3.802          0.901
```

The small host-to-guest cell was then rerun with four repeats:

```sh
rm -rf /tmp/um-vector-perf-h2g-1m-rerun
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-h2g-1m-rerun \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=4 \
UML_VECTOR_PERF_PORT=19094 \
  timeout 900s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Focused rerun best observed host-side throughput:

```text
direction      bytes    vector_mib_s  vector2_mib_s  ratio
host-to-guest  1048576  1.648         0.954          0.579
```

Vector2 was also rerun as a single-queue fd configuration to isolate whether
the 1 MiB gap was caused only by multiqueue overhead:

```sh
rm -rf /tmp/um-vector-perf-h2g-1m-q1
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-h2g-1m-q1 \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=4 \
UML_VECTOR_PERF_QUEUES=1 \
UML_VECTOR_PERF_PORT=19095 \
  timeout 900s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Best observed host-side throughput:

```text
direction      bytes    vector_mib_s  vector2_q1_mib_s  ratio
host-to-guest  1048576  1.648         1.093             0.663
```

The harness now prints `VECTOR_NET_DIAG_BEGIN/END` blocks around each transfer,
including `ip -d link`, `ip -s link`, route state, `ethtool -k`, and
`ethtool -S` output when those tools are available.  The diagnostic smoke was
validated in both directions:

```sh
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-diag-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_QUEUES=1 \
UML_VECTOR_PERF_PORT=19096 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"

UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-diag-g2h-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=guest-to-host \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_QUEUES=1 \
UML_VECTOR_PERF_PORT=19097 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

The host-to-guest diagnostic smoke recorded `vnet_hdr_enabled: 1` and, after
the 1 MiB transfer, `napi_polls: 1257`, `rx_irqs: 500`,
`rx_batch_prepared_total: 31936`, and `rx_batch_received_total: 909` with no
RX or TX error counters.

The next RX-side cleanup changed fd and TAP receive batching to prepare one
RX slot per nonblocking read attempt instead of preparing the full NAPI budget
up front.  This removes the diagnostic over-preparation without closing the
small-transfer throughput gap.  A focused before/after comparison used
host-to-guest 1 MiB transfers:

```sh
rm -rf /tmp/um-vector-perf-h2g-1m-lazyrx
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-h2g-1m-lazyrx \
UML_VECTOR_PERF_DRIVERS=vector,vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=3 \
UML_VECTOR_PERF_PORT=19099 \
  timeout 900s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Focused lazy-RX best observed host-side throughput:

```text
driver   host_mib_s best
vector   1.601
vector2  0.951
ratio    0.594
```

The useful result is in the counters, not the throughput.  Before lazy RX, a
representative vector2 1 MiB host-to-guest run recorded
`rx_batch_prepared_total: 32448`, `rx_batch_received_total: 906`, and
`rx_batch_released_total: 31542`.  After lazy RX, comparable runs recorded
about `rx_batch_prepared_total: 1408..1416`,
`rx_batch_received_total: 903..907`, and `rx_batch_released_total: 505..509`,
with no RX/TX error counters.  The normal guest-to-host TCP gate still passes
after the change: legacy vector median 39215.2 Mbps, vector2 median
38960.1 Mbps, ratio 0.993 against the 0.85 bar.

## RX Checksum Feature Alignment

The next feature-alignment slice fixed a mismatch in vector2's netdev feature
reporting.  The TAP and inherited-fd receive paths already consume
`struct virtio_net_hdr` through `virtio_net_hdr_to_skb()`, but vector2 only
advertised TX checksum offload when `csum=1`.  The driver now advertises fixed
RX checksum support for that configuration, matching legacy vector's
vnet-header TAP behavior.

Validation:

```sh
make ARCH=um -j$(nproc)

./linux mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt

UML_KERNEL=$PWD/linux \
  tools/testing/selftests/um/vector2-fd-handoff-smoke/run-vector2-fd-handoff-smoke.sh

UML_KERNEL=$PWD/linux \
  tools/testing/selftests/um/vector2-fd-multiqueue-smoke/run-vector2-fd-multiqueue-smoke.sh

UML_KERNEL=$PWD/linux \
  tools/testing/selftests/um/vector2-inproc-tap-smoke/run-vector2-inproc-tap-smoke.sh
```

Results:

- build PASS;
- `um_vector2_*` KUnit: 89 pass, 0 fail, 2 trusted-TAP skips;
- fd handoff, fd multiqueue, and trusted in-process TAP smokes PASS.

The focused 1 MiB host-to-guest diagnostic used:

```sh
rm -rf /tmp/um-vector-perf-h2g-rxcsum
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-h2g-rxcsum \
UML_VECTOR_PERF_DRIVERS=vector,vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=2 \
UML_VECTOR_PERF_PORT=19120 \
  timeout 900s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

It now records vector2 `rx-checksumming: on [fixed]`, `tx-checksumming: on`,
GSO/TSO/GRO enabled, and `vnet_hdr_enabled: 1`.  The small-transfer
throughput gap remains open:

```text
driver   repeat  host_mib_s
vector   1       1.574
vector   2       0.342
vector2  1       0.952
vector2  2       0.893
```

The normal guest-to-host TCP gate remains green:

```text
vector  median Mbps: 39854.4
vector2 median Mbps: 39441.7
ratio: 0.990
```

Interpretation:

- current guest-to-host fixed-byte TCP now clears the 0.85 bar across all
  three measured sizes;
- host-to-guest clears the larger 8 MiB and 32 MiB cells in this harness;
- host-to-guest 1 MiB remains a reproducible small-transfer regression in the
  fixed-byte harness; single-queue vector2 and lazy RX both improve the cell
  but do not close it;
  and
- this refresh improves the TCP story, but it does not close P4.3 because
  broader UDP, syscall-rate, full CPU-utilisation, and the host-to-guest
  small-transfer issue remain open.

## UDP Harness Extension: 2026-06-11

The helper now accepts `UML_VECTOR_PERF_PROTOCOL=tcp|udp` or
`--protocol tcp|udp`. TCP remains the default. UDP mode sends the requested
fixed byte count as datagrams and treats missing bytes as a failed run instead
of reporting a partial throughput result.

Additional knobs:

- `UML_VECTOR_PERF_UDP_PAYLOAD`, default `1472`;
- `UML_VECTOR_PERF_UDP_PACE_USEC`, default `0`.

The summary file now includes a `protocol` column.

Validation:

```sh
bash -n tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh
tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh --help

rm -rf /tmp/um-vector-perf-tcp-smoke
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-tcp-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=guest-to-host \
UML_VECTOR_PERF_BYTES_LIST=65536 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19140 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"

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

Results:

- TCP compatibility smoke PASS with `protocol=tcp` in the guest/host lines and
  summary row.
- Vector2 UDP bidirectional smoke PASS.

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

```text
driver   direction       host_mib_s
vector   guest-to-host   7.471
vector   host-to-guest   8.760
vector2  guest-to-host   7.319
vector2  host-to-guest   8.660
```

An unpaced 1 MiB UDP matrix attempt completed the legacy vector guest-to-host
row, but legacy vector host-to-guest did not reach the exact-byte completion
marker after the host sent the full payload. Treat that as useful negative
evidence and keep unpaced/larger UDP coverage open for the final publication
matrix.

## CPU Timing Extension: 2026-06-11

The helper now records endpoint process CPU time for each fixed-byte run. Host
`HOST_SINK`/`HOST_SEND` lines and guest `VECTOR_NET_PERF` lines include
`cpu_seconds=...`, and `summary.tsv` appends `guest_cpu_seconds` and
`host_cpu_seconds` after the existing columns.

Validation:

```sh
bash -n tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh
git diff --check

rm -rf /tmp/um-vector-perf-cpu-tcp-smoke
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-cpu-tcp-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=guest-to-host \
UML_VECTOR_PERF_BYTES_LIST=65536 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19150 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"

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

Result: syntax checks PASS, vector2 TCP 64 KiB guest-to-host smoke PASS, and
paced vector2 UDP 64 KiB bidirectional smoke PASS. The smoke summaries include
the appended CPU columns.

This is a lightweight diagnostic, not the final CPU-utilisation gate. It does
not measure total host CPU, UML kernel CPU, softirq time, or syscall count, and
short or paced transfers can round small guest process times to `0.000000`.

## Host-To-Guest UML Process Metrics: 2026-06-11

The helper now samples `umlctl metrics --json` before and after host-to-guest
fixed-byte transfers.  The summary appends UML process user/system CPU seconds,
scheduler run/wait seconds, scheduler pcount, voluntary/involuntary context
switch deltas, and the before/after metrics JSON paths.

Validation:

```sh
bash -n tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh
git diff --check

rm -rf /tmp/um-vector-perf-metrics-h2g-smoke
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-metrics-h2g-smoke \
UML_VECTOR_PERF_DRIVERS=vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=65536 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19160 \
  timeout 300s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"

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

Result: syntax checks PASS, vector2 TCP 64 KiB host-to-guest smoke PASS with
real UML process metric deltas, side-by-side legacy vector/vector2 TCP 64 KiB
host-to-guest smoke PASS with 22-field summary rows for both drivers, and
vector2 TCP 64 KiB guest-to-host shape smoke PASS with `NA` metric fields.

The host-to-guest smoke recorded:

```text
uml_user_cpu_seconds=0.020000
uml_system_cpu_seconds=0.500000
uml_sched_run_seconds=0.525344
uml_sched_wait_seconds=0.000360
uml_sched_pcount_delta=1799
uml_voluntary_ctxt_switches_delta=1785
uml_involuntary_ctxt_switches_delta=13
```

This is useful for the 1 MiB host-to-guest bottleneck pass, but it does not
replace the P4.3 syscall-rate gate.  On this host, unprivileged `perf stat -e
syscalls:sys_enter_*` is blocked by `perf_event_paranoid=4`.

## 1 MiB Host-To-Guest Metric Diagnostic: 2026-06-11

The open 1 MiB host-to-guest cell was rerun with the new UML process metric
columns:

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

Result: PASS for all eight rows.  The generated `summary.tsv` had 22 fields
for the header and every row, and cleanup checks found no stale `vperf-*` TAP
devices or live `umlctl` instances.

Host-side MiB/s:

| Driver | Values | Median | Best |
| --- | --- | ---: | ---: |
| vector | 1.630, 0.342, 1.602, 0.367 | 0.9845 | 1.630 |
| vector2 | 0.909, 1.085, 0.892, 0.882 | 0.9005 | 1.085 |

Vector2/legacy ratios:

| Ratio | Value |
| --- | ---: |
| median host-side MiB/s | 0.9147 |
| best host-side MiB/s | 0.6656 |

Median UML host-process deltas:

| Metric | vector | vector2 | vector2/vector |
| --- | ---: | ---: | ---: |
| system CPU seconds | 0.310 | 0.670 | 2.16 |
| sched run seconds | 0.338087 | 0.817219 | 2.42 |
| sched pcount delta | 2213.0 | 26547.5 | 12.00 |
| voluntary context switches | 2186.5 | 26505.5 | 12.12 |

This does not close the host-to-guest no-regression bar.  It does narrow the
next bottleneck pass: vector2 is steadier than legacy vector in this short run,
but it spends much more scheduler/wakeup activity per transfer.

## Interpretation

This is a baseline, not an acceptance result.  On this short
guest-to-host run, vector2 fd multiqueue was originally slower than legacy
vector TAP.  The 2026-06-11 TX/RX NAPI scheduling fix removed that broad
guest-to-host TCP regression in the current fixed-byte refresh.  Host-to-guest
is now mixed: vector2 is strong at larger sizes in this harness, but the 1 MiB
cell remains below legacy.

The immediate value is that future vector2 changes now have a simple
side-by-side `umlctl` command to catch large regressions and to track
whether fd multiqueue batching, NAPI scheduling, and receive-side work improve
throughput.

## Remaining Gate

The full replacement performance gate still needs:

- UDP packet rate;
- larger repeat counts and more host/kernel samples for the TCP size
  sweep;
- host-to-guest small-transfer follow-up;
- use the fixed-byte harness diagnostics to compare RX IRQ, NAPI, and batch
  counters plus UML process metric deltas between legacy and vector2;
- inspect host-to-guest wakeup/readiness/receive scheduling behavior, because
  the 1 MiB metric diagnostic shows about 12x higher vector2 scheduler pcount
  and voluntary context-switch deltas;
- single-queue vector2 fd and TAP comparisons;
- syscall and batching profiles;
- full CPU-utilisation and CPU cycles per packet if practical;
- KCSAN/fairness runs under concurrent traffic.
