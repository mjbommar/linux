# UML vector driver v2 R6 ethtool hardening

**Status:** IMPLEMENTED R6 checkpoint - ethtool visibility and TAP
write-wakeup hardening.
**Date:** 2026-05-17.

This memo records the sixth runtime buildout checkpoint for vector
networking v2.  R6 makes the single-queue trusted TAP path easier to
inspect and harder to wedge, but it still does not make vector v2 a
replacement for the legacy `CONFIG_UML_NET_VECTOR` driver.

The old `CONFIG_UML_NET_VECTOR` driver remains the production vector
networking path.

## What Landed

R6 adds:

- explicit v2 runtime counters stored in `struct um_vec2_stats`;
- ethtool stats strings and values for lifecycle, configuration,
  queue counters, IRQ counters, NAPI polls, drops, transient TX
  errors, fatal errors, and backend-death observations;
- stopped-safe ethtool sampling that checks runtime queue pointers
  before dereference;
- queue-counter sampling under the TX/RX queue locks;
- ethtool ring parameter reporting;
- stopped-only ring depth changes through `ethtool -G`;
- `-EBUSY` rejection for ring depth changes while the device is
  opening or running;
- ethtool coalesce reporting for the parsed `coalesce_usecs` policy
  input;
- TAP write IRQ registration so a full TX ring that hit write-side
  `-EAGAIN` can be woken by host fd writability;
- KUnit coverage for stopped stats, ring policy, and exported drop
  counters.

R6 intentionally keeps coalescing as reported policy rather than a
timer-driven batching implementation.  Real coalescing and performance
tuning remain later work.

## Trusted Runtime Check

Trusted TAP kernel config:

```text
# CONFIG_UML_NET_VECTOR is not set
CONFIG_UML_NET_VECTOR_V2=y
CONFIG_UML_NET_VECTOR_V2_INPROC=y
CONFIG_UML_NET_VECTOR_V2_CONFIG=y
CONFIG_UML_NET_VECTOR_V2_QUEUE=y
CONFIG_UML_NET_VECTOR_V2_MODEL=y
```

Manual boot:

```text
/home/mjbommar/projects/personal/.build/um-vector-r3-fd/linux \
  mem=256M noreboot con=fd:0,fd:1 ssl=null \
  root=/dev/root rootfstype=hostfs rootflags=/ rw \
  vec2.0:transport=tap,ifname=soak-tap0,depth=128 \
  init=/tmp/uml-vector-r6-init.sh loglevel=7
```

Stopped-state ethtool behavior:

```text
driver: uml-vector-v2
version: r6-ethtool-hardening
R6_DRVINFO_RC=0
R6_RING_GET_STOPPED_RC=0
R6_RING_SET_STOPPED_RC=0
R6_RING_GET_RESIZED_RC=0
R6_COAL_GET_STOPPED_RC=0
```

The ring depth changed from 128 to 256 while stopped:

```text
Current hardware settings:
RX: 128
TX: 128

Current hardware settings:
RX: 256
TX: 256
```

Running-state ring changes failed closed:

```text
R6_LOOP_1_RING_SET_RUNNING_RC=81
R6_LOOP_2_RING_SET_RUNNING_RC=81
R6_LOOP_3_RING_SET_RUNNING_RC=81
```

The runtime loop brought the interface up, pinged the host, and brought
the interface down three times:

```text
R6_LOOP_1_UP_RC=0
1 packets transmitted, 1 received, 0% packet loss
R6_LOOP_1_PING_RC=0
R6_LOOP_1_DOWN_RC=0

R6_LOOP_2_UP_RC=0
1 packets transmitted, 1 received, 0% packet loss
R6_LOOP_2_PING_RC=0
R6_LOOP_2_DOWN_RC=0

R6_LOOP_3_UP_RC=0
1 packets transmitted, 1 received, 0% packet loss
R6_LOOP_3_PING_RC=0
R6_LOOP_3_DOWN_RC=0
```

The ethtool stats surface showed the new counters moving:

```text
configured_depth: 256
napi_polls: 18
rx_irqs: 9
tx_irqs: 9
tx_ring_depth: 256
rx_batch_depth: 256
tx_busy: 0
tx_dropped: 0
tx_transient_errors: 0
tx_fatal_errors: 0
rx_alloc_errors: 0
rx_proto_drops: 0
rx_fatal_errors: 0
backend_dead: 0
```

No R6 trusted TAP manual boot reported:

```text
Kernel panic
BUG:
Oops
general protection fault
NULL pointer
```

## Sandbox Check

The sandbox build still rejects direct trusted TAP authority:

```text
uml-vector2: vec2.0 config rejected: key='ifname' msg='trusted host option not permitted' ret=-13
uml-vector2: 1 vec2 command-line spec(s) rejected; no v2 devices configured
R6_SANDBOX_VEC2_PRESENT=1
Device "vec2.0" does not exist.
```

The `R6_SANDBOX_VEC2_PRESENT=1` line is the `ip link show dev vec2.0`
exit status, so `1` means the device was absent.

`strace` over the sandbox boot found no:

```text
/dev/net/tun
TUNSETIFF
TUNSETOFFLOAD
```

## KUnit Coverage

KUnit was run with:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt
```

Result:

```text
ok 1  um_vector2_config       pass:12 fail:0 skip:0 total:12
ok 2  um_vector2_queue        pass:8  fail:0 skip:0 total:8
ok 3  um_vector2_transport    pass:8  fail:0 skip:0 total:8
ok 4  um_vector2_fake_host    pass:8  fail:0 skip:0 total:8
ok 5  um_vector2_model        pass:7  fail:0 skip:0 total:7
ok 6  um_vector2_cmdline      pass:5  fail:0 skip:0 total:5
ok 7  um_vector2_netdev       pass:4  fail:0 skip:0 total:4
ok 8  um_vector2_ethtool      pass:3  fail:0 skip:0 total:3
ok 9  um_vector2_host_fd      pass:3  fail:0 skip:0 total:3
ok 10 um_vector2_host_tap     pass:6  fail:0 skip:0 total:6
```

Total: 64 passed, 0 failed.

The new R6 ethtool tests cover:

- stopped-state stat collection without runtime queues;
- ring max/current reporting;
- stopped-only ring depth changes;
- running-state ring change rejection;
- exported TX xmit/drop counters.

## Build Coverage

The following UML build matrices completed without warnings or errors:

```text
um-vector-r1-kunit
um-vector-r1-v2only
um-vector-r3-fd
um-vector-r0
um-vector-r2-both
um-vector-r2-neither
```

## Known Limits

R6 deliberately does not claim:

- fd datapath support;
- sandbox-safe TAP datapath;
- host-to-guest TCP validation;
- Django/FastAPI Tier 3 readiness;
- real timer-driven coalescing;
- scatter-gather TX without linearization;
- throughput parity with the legacy vector driver;
- multiqueue;
- KCSAN/KASAN/KFENCE coverage.

These are replacement blockers, not optional cleanup.

## Next Required Phase

R7 should move from single-ping smoke to workload eligibility:

- add a v2 selection path in the soak/operator templates;
- run Django Tier 3 with v2 TAP on seccomp and kvm-v2;
- run FastAPI or uvicorn variants where dependencies are available;
- collect crash-scraper metadata for v2 netdev name, backend,
  transport, queue count, host mode, and ethtool stats;
- add at least one host-to-guest TCP check before declaring v2 eligible
  for longer soaks.
