# UML vector driver v2 queue-to-CPU policy

**Status:** R8c follow-up - explicit TX queue selection and XPS policy.
**Date:** 2026-05-17.

This note records the first explicit vector2 queue-to-CPU policy.  It
does not close the multiqueue replacement gate by itself; the short
KCSAN smoke in the follow-up note is useful, but longer SMP traffic,
fairness, and performance profiles still need to run.  It does make the
queue selection model small enough to reason about and test.

## Policy

Vector2 now owns an `ndo_select_queue` hook and delegates flow-aware
selection to the kernel's `netdev_pick_tx()` path.  That preserves
core networking behavior such as socket queue cache, packet hash, and
XPS.

On open, vector2 configures XPS for multiqueue devices.  The mapping is
defined over online CPU ordinals:

- when `cpu_count >= queues`, CPU ordinal `c` is eligible for queue
  `c % queues`;
- when `queues > cpu_count`, queue ordinal `q` is eligible on CPU
  ordinal `q % cpu_count`.

The second rule keeps every configured queue associated with an online
CPU even when a test runs more queues than CPUs.  The first rule keeps
every online CPU associated with a queue when CPUs are plentiful.

This policy is deliberately independent of raw CPU ids.  Using online
CPU ordinals keeps the model stable across sparse CPU masks and makes a
future TLA+/state-model representation straightforward.

## Implementation

- `vector2_netdev.c` installs `.ndo_select_queue`.
- `um_vec2_netdev_select_queue()` calls `netdev_pick_tx()` and caps the
  result with `netdev_cap_txqueue()`.
- `um_vec2_netdev_configure_xps()` builds per-queue CPU masks at open
  time and calls `netif_set_xps_queue()`.
- `um_vec2_tx_queue_uses_cpu_ordinal()` is the pure policy helper used
  by both the XPS setup and KUnit.
- `vector2_netdev_test.c` covers both sides of the modulo policy and
  invalid bounds.
- `UML_NET_VECTOR_V2_NETDEV_KUNIT` help now documents that the netdev
  KUnit suite covers configured queue counts and queue-to-CPU policy.

## Validation Evidence

Targeted object build:

```text
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r1-kunit \
  arch/um/drivers/vector2_netdev.o \
  arch/um/drivers/vector2_netdev_test.o -j$(nproc)
```

Full vector2 KUnit:

```text
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r1-kunit -j$(nproc)
/home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt
kunit.py parse /tmp/um-vector-queue-cpu-kunit.log
```

Result:

```text
queue_cpu_kunit_vm_rc=0
um_vector2_netdev (6 subtests)
[PASSED] vector2_netdev_queue_cpu_policy_test
Testing complete. Ran 72 tests: passed: 72
```

Runtime rebuild after the policy change:

```text
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r1-v2only -j$(nproc)
LINK linux
```

Short live fd-multiqueue smoke after the policy change:

```text
umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-fd-multiqueue.toml \
  -W 1 -M 1 --timeout 180 --pass-marker VECTOR2_FD_MULTIQUEUE_OK
[umlctl gate loop] w0 iter1: Pass
==> default PASS=1/1 FAIL=0 TIMEOUT=0 rate=100.0%
TAP_ABSENT
```

Follow-up KCSAN smoke with `queues = "auto"` resolving to four queues:

```text
umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-auto-queues.toml \
  -W 1 -M 1 --timeout 240 --pass-marker VECTOR2_AUTO_QUEUES_OK
[umlctl gate loop] w0 iter1: Pass
==> default PASS=1/1 FAIL=0 TIMEOUT=0 rate=100.0%
TAP_ABSENT
no BUG: KCSAN / data-race signatures in the captured run log
```

## Remaining Work

- Longer KCSAN on TAP and fd multiqueue traffic.
- Longer SMP traffic runs that confirm XPS queue selection under load.
- Fairness and performance profiles against legacy vector and vector2
  in-process TAP.
- kvm-v2 validation after the separate kvm-v2 readiness blocker is
  fixed.
