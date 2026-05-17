# UML vector driver v2 initial performance baseline

**Status:** partial performance evidence - guest-to-host TCP only.
**Date:** 2026-05-17.

This note records the first repeatable legacy-vs-vector2 performance
baseline harness for `umlctl`.  It is intentionally small: one
guest-to-host TCP stream, one transfer size, and one host.  It is useful
as a regression tripwire and as a convenient operator workflow, but it
does not close the replacement performance gate.

## Harness

New helper:

```text
tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh
```

The helper:

- generates a temporary Umlfile per driver;
- starts a host Python TCP sink on `0.0.0.0:${UML_VECTOR_PERF_PORT}`;
- boots the UML guest through `umlctl up`;
- has the guest send a fixed byte count to `$UMLCTL_GATEWAY`;
- records guest-side and host-side MiB/s;
- writes a TSV summary and per-run logs;
- tears the instance down with `umlctl down --force --rm`.

Defaults:

```text
drivers: vector,vector2
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

## Result

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
driver   bytes     guest_mib_s   host_mib_s
vector   33554432  662.031       958.685
vector2  33554432  219.751       244.509
```

The logs showed:

- legacy vector used `vec0`, transport `tap`, queues `1`;
- vector2 used `vec2.0`, transport `fd`, queues `4`;
- vector2 registered successfully in a both-drivers kernel;
- no `uml-vector: Couldn't parse '2.0'` line remained;
- no lingering `vperf-vector0` or `vperf-vector20` TAP device remained.

## Interpretation

This is a baseline, not an acceptance result.  On this short
guest-to-host run, vector2 fd multiqueue is slower than legacy vector
TAP.  That is expected to remain a replacement blocker until the cause
is understood or the regression is explicitly accepted with the legacy
driver retained for a transition period.

The immediate value is that future vector2 changes now have a simple
side-by-side `umlctl` command to catch large regressions and to track
whether fd multiqueue batching work improves throughput.

## Remaining Gate

The full replacement performance gate still needs:

- host-to-guest TCP;
- UDP packet rate;
- larger and repeated transfer sizes;
- single-queue vector2 fd and TAP comparisons;
- syscall and batching profiles;
- CPU cycles per packet if practical;
- KCSAN/fairness runs under concurrent traffic.
