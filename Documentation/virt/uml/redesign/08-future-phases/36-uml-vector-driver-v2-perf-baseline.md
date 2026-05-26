# UML vector driver v2 initial performance baseline

**Status:** partial performance evidence - bidirectional TCP smoke.
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
- starts a host Python TCP sink for guest-to-host runs;
- starts a guest Python TCP sink for host-to-guest runs;
- boots the UML guest through `umlctl up`;
- sends one or more fixed byte counts across the selected direction;
- records guest-side and host-side MiB/s;
- writes a TSV summary, including transfer size and repeat index, plus
  per-run logs;
- tears the instance down with `umlctl down --force --rm`.

Defaults:

```text
drivers: vector,vector2
direction: guest-to-host
bytes:   33554432
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

- the guest-to-host vector2 fd multiqueue regression persisted across
  all measured sizes;
- the host-to-guest vector2 fd multiqueue path remained much faster
  than legacy vector TAP across all measured sizes;
- legacy host-to-guest results were especially weak and variable on
  this host;
- this is still a lightweight TCP smoke, not an accepted performance
  replacement decision.

## Interpretation

This is a baseline, not an acceptance result.  On this short
guest-to-host run, vector2 fd multiqueue is slower than legacy vector
TAP.  On the host-to-guest run, vector2 fd multiqueue is much faster
than legacy vector TAP on this host.  The repeated-size sweep confirms
that both findings are reproducible enough to treat as measured
blockers, not one-off noise.

The immediate value is that future vector2 changes now have a simple
side-by-side `umlctl` command to catch large regressions and to track
whether fd multiqueue batching work improves throughput.

## Remaining Gate

The full replacement performance gate still needs:

- UDP packet rate;
- larger repeat counts and more host/kernel samples for the TCP size
  sweep;
- single-queue vector2 fd and TAP comparisons;
- syscall and batching profiles;
- CPU cycles per packet if practical;
- KCSAN/fairness runs under concurrent traffic.
