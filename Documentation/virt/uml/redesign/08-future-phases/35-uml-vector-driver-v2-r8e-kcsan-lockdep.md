# UML vector driver v2 R8e KCSAN lockdep follow-up

**Status:** R8 partial - queue lockdep fix plus repeated KCSAN smoke.
**Date:** 2026-05-17.

This note records a locking bug found while extending the vector2
auto-queue KCSAN smoke from one iteration to ten iterations.

The original one-iteration smoke passed the workload marker and had no
KCSAN data-race report, but a broader log scan showed a lockdep splat
from `ethtool -S vec2.0`:

```text
WARNING: inconsistent lock state
inconsistent {IN-SOFTIRQ-W} -> {SOFTIRQ-ON-W} usage.
ethtool/... takes:
(&queue->tx_lock), at: um_vec2_get_ethtool_stats
```

The warning was valid.  NAPI polls take the vector2 queue locks from
softirq context, while ethtool stats sampled the same queue counters in
process context with plain `spin_lock()`.  That leaves local softirqs
enabled while a process-context reader holds a lock that a softirq can
also take.

## Fix

The queue lock discipline is now explicit:

- `um_vec2_get_ethtool_stats()` uses `spin_lock_bh()` /
  `spin_unlock_bh()` while sampling TX and RX queue counters.
- `um_vec2_netdev_start_xmit()` uses `spin_lock_bh()` /
  `spin_unlock_bh()` while enqueueing TX descriptors from the netdev
  transmit path.
- `um_vec2_netdev_poll()` remains a NAPI softirq owner and keeps its
  existing queue locking.

This keeps bottom halves disabled in process-context paths that share
queue locks with NAPI.

## Build And KUnit

Targeted KCSAN object build:

```sh
make -j"$(nproc)" ARCH=um \
  O=/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan \
  arch/um/drivers/vector2_ethtool.o \
  arch/um/drivers/vector2_netdev.o
```

KCSAN runtime kernel rebuild:

```sh
make -j"$(nproc)" ARCH=um \
  O=/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan \
  linux
```

KUnit kernel rebuild and vector2 KUnit parse:

```sh
make -j"$(nproc)" ARCH=um \
  O=/home/mjbommar/projects/personal/.build/um-vector-r1-kunit \
  linux

timeout 300s /home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt \
  > /tmp/um-vector2-lockdep-kunit.log 2>&1

python3 tools/testing/kunit/kunit.py parse \
  /tmp/um-vector2-lockdep-kunit.log
```

Result:

```text
Testing complete. Ran 72 tests: passed: 72
```

## Repeated KCSAN Gate

Command:

```sh
rm -rf /tmp/um-vector-auto-kcsan-lockfix
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux \
  timeout 2400s cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
    --bin umlctl -- gate loop \
    -f tools/uml/uml-launcher/examples/vector2-auto-queues.toml \
    -W 1 -M 10 --timeout 240 \
    --pass-marker VECTOR2_AUTO_QUEUES_OK \
    --out /tmp/um-vector-auto-kcsan-lockfix/loop
test ! -e /sys/class/net/v2autoq0 && echo TAP_ABSENT
```

Result:

```text
PASS=10/10 FAIL=0 TIMEOUT=0
TAP_ABSENT
```

The captured logs showed the expected vector2 fd multiqueue contract on
every run:

```text
vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4
uml-vector2: vec2.0 configured transport=fd mode=fd requested_queues=4 runtime_queues=4 depth=128
UMLCTL_NETWORK_QUEUE_SPEC=auto
UMLCTL_NETWORK_QUEUES=4
numtxqueues 4 numrxqueues 4
VECTOR2_AUTO_QUEUES_OK
```

The KCSAN kernel for this checkpoint has `CONFIG_NR_CPUS=2`; the
`queues = "auto"` Umlfile still resolves to four vector2 queues because
the user-facing queue intent comes from `[runtime].ncpus = 4`.  This is
a useful coverage point for the queue-to-CPU policy where queue count
exceeds online CPU count.

Post-run scans found ten captured run logs and no warning, panic,
lockdep, KCSAN, or data-race splat:

```sh
find /tmp/um-vector-auto-kcsan-lockfix/loop -path '*run-*.log' \
  -type f | wc -l

rg -n 'WARNING:|BUG: KCSAN|BUG:|data-race|KCSAN:|Kernel panic|inconsistent lock state' \
  /tmp/um-vector-auto-kcsan-lockfix/loop -S
```

Results:

```text
10
no matches
```

The normal KCSAN boot banner remains present and is not a race report:

```text
kcsan: non-strict mode configured - use CONFIG_KCSAN_STRICT=y to see all data races
```

## Remaining Gate

This closes the specific lockdep bug found by the repeated KCSAN smoke
and gives vector2 stronger fd multiqueue evidence than the original
one-run R8d note.  It still does not close the full R8 concurrency and
performance gate.  Remaining work:

- heavier concurrent TCP/UDP traffic under KCSAN;
- longer SMP run time than this short ten-iteration smoke;
- queue fairness and throughput profiles;
- kvm-v2 readiness and vector2 reruns after the kvm-v2 baseline is
  fixed.
