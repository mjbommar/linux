# UML vector driver v2 initial performance baseline

**Status:** partial performance evidence - bidirectional TCP smoke.
**Date:** 2026-05-17.

This note records the first repeatable legacy-vs-vector2 performance
baseline harness for `umlctl`.  It is intentionally small: one TCP
stream per direction, one transfer size, and one host.  It is useful as
a regression tripwire and as a convenient operator workflow, but it does
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
- sends a fixed byte count across the selected direction;
- records guest-side and host-side MiB/s;
- writes a TSV summary and per-run logs;
- tears the instance down with `umlctl down --force --rm`.

Defaults:

```text
drivers: vector,vector2
direction: guest-to-host
bytes:   33554432
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

## Interpretation

This is a baseline, not an acceptance result.  On this short
guest-to-host run, vector2 fd multiqueue is slower than legacy vector
TAP.  On the host-to-guest run, vector2 fd multiqueue is much faster
than legacy vector TAP on this host.  Both findings need repeated runs
and profiling before they can support a replacement decision.

The immediate value is that future vector2 changes now have a simple
side-by-side `umlctl` command to catch large regressions and to track
whether fd multiqueue batching work improves throughput.

## Remaining Gate

The full replacement performance gate still needs:

- UDP packet rate;
- larger and repeated transfer sizes;
- single-queue vector2 fd and TAP comparisons;
- syscall and batching profiles;
- CPU cycles per packet if practical;
- KCSAN/fairness runs under concurrent traffic.
