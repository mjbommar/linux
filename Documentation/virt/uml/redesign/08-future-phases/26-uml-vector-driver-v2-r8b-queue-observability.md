# uml-vector-driver-v2 R8b queue observability checkpoint

**Status:** R8 partial - per-queue statistics.
**Date:** 2026-05-17.

R8a made trusted TAP multiqueue structurally real.  R8b adds the
observability needed to prove what each queue is doing.

## Implemented

- `ethtool -S` now returns dynamic per-queue stat names in addition to
  the existing aggregate v2 counters.
- Per-queue names use a stable `queue<N>_<counter>` form:
  - `queue<N>_tx_ring_depth`;
  - `queue<N>_tx_ring_used`;
  - `queue<N>_tx_ring_max_used`;
  - `queue<N>_tx_ring_enqueued`;
  - `queue<N>_tx_ring_completed`;
  - `queue<N>_tx_ring_released`;
  - `queue<N>_rx_batch_depth`;
  - `queue<N>_rx_batch_filled`;
  - `queue<N>_rx_batch_prepared_total`;
  - `queue<N>_rx_batch_received_total`;
  - `queue<N>_rx_batch_consumed_total`;
  - `queue<N>_rx_batch_released_total`.
- Dynamic stat count is based on the maximum of configured queues,
  registered netdev queues, and currently open runtime channels.
- Stopped devices still report zero-valued per-queue stats for the
  configured queue count, so scripts can keep a stable shape across
  stopped/running sampling.

## Validation Evidence

KUnit:

```text
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r1-kunit -j$(nproc)
kunit.py parse /tmp/um-vector-r8b-kunit.log
Testing complete. Ran 66 tests: passed: 66
```

The `um_vector2_ethtool` suite now checks aggregate queue depth and
per-queue `queue0_*` / `queue1_*` stat names and values.

Live seccomp smoke:

```text
[umlctl] sudo: ip tuntap add dev r8b-eth0 mode tap user ... multi_queue
uml-vector2: vec2.0 configured transport=tap mode=inproc requested_queues=2 runtime_queues=2 depth=128
3 packets transmitted, 3 received, 0% packet loss
queue0_tx_ring_depth: 128
queue0_rx_batch_depth: 128
queue1_tx_ring_depth: 128
queue1_rx_batch_depth: 128
QUEUE_STATS_OK
PASS=1/1 FAIL=0 TIMEOUT=0
TAP_ABSENT
```

Parallel traffic distribution smoke:

```text
uml-vector2: vec2.0 configured transport=tap mode=inproc requested_queues=2 runtime_queues=2 depth=128
queue0_tx_ring_enqueued: 1
queue0_tx_ring_completed: 1
queue0_rx_batch_received_total: 2
queue1_tx_ring_enqueued: 40
queue1_tx_ring_completed: 40
queue1_rx_batch_received_total: 43
DISTRIBUTION_DONE
PASS=1/1 FAIL=0 TIMEOUT=0
```

This is intentionally small.  It proves that `umlctl` can request two
trusted TAP queues, the v2 runtime opens two queues, normal guest
traffic reaches both queues, and the per-queue stats expose that fact
to operators.  It is not a performance or fairness result.

## Remaining R8 Work

Per-queue stats and the first parallel traffic smoke make trusted TAP
distribution observable.  R8 still needs:

- fd multiqueue;
- queue-to-CPU mapping policy;
- KCSAN evidence;
- broader traffic shapes and fairness/performance profiles;
- kvm-v2 multiqueue evidence once kvm-v2 baseline readiness is fixed.
