# uml-vector-driver-v2 R8a TAP multiqueue checkpoint

**Status:** R8 partial - trusted TAP multiqueue shape.
**Date:** 2026-05-17.

R8 is the SMP/multiqueue phase.  This checkpoint does not close R8,
but it removes the main single-queue hard-code from the trusted TAP
runtime path and gives `umlctl` a first-class way to request queue
counts.

## Implemented

- v2 netdev registration uses parsed `queues=N` instead of forcing
  `alloc_etherdev_mqs(..., 1, 1)`.
- `um_vec2_dev` records the registered netdev queue count separately
  from currently open runtime channels.
- trusted TAP open allocates one channel, queue pair, TAP fd, NAPI
  instance, read IRQ, and write IRQ per configured queue.
- TAP fds use `IFF_MULTI_QUEUE` when `queues > 1`.
- TX maps `skb_get_queue_mapping(skb)` onto the corresponding v2
  channel.
- TX backpressure stops and wakes the affected netdev subqueue.
- datapath start/stop iterates all channels and unwinds partial starts.
- ethtool queue counters aggregate across all open channels.
- `umlctl` supports:
  - `[network] queues = N`;
  - later follow-up: `[network] queues = "auto"` resolves from
    `[runtime].ncpus`;
  - `umlctl up --network-queues N`;
  - later follow-up: `umlctl up --network-queues auto`;
  - `umlctl gate loop --network-queues N`;
  - `umlctl gate loop --sweep network.queues=1,2` and later
    `network.queues=1,auto`;
  - host TAP setup with `multi_queue` for vector2 queues above one;
  - host TAP teardown with matching `multi_queue` deletion.

Later fd-handoff work changes the default vector2 `host_mode=auto`
plan, including multiqueue, to `transport=fd`.  Force
`--network-host-mode inproc` only when a comparison must keep trusted
in-process host behavior constant.

## User Surface

Umlfile:

```toml
[network]
mode = "tap"
driver = "vector2"
queues = 2
```

or:

```toml
[runtime]
ncpus = 2

[network]
mode = "tap"
driver = "vector2"
queues = "auto"
```

One-off CLI override:

```sh
umlctl up -f workload.toml --network-driver vector2 --network-queues 2 --dry-run
umlctl gate loop -f workload.toml --network-driver vector2 --network-queues 2 -W 1 -M 1
```

Dry-run output shows:

```text
mode=tap driver=vector2 guest_dev=vec2.0 host_tap=soak-tap0 transport=fd host_mode=fd queues=2 queue_spec=2
kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=2
```

For a forced trusted in-process comparison, dry-run output shows:

```text
mode=tap driver=vector2 guest_dev=vec2.0 host_tap=soak-tap0 transport=tap host_mode=inproc queues=2 queue_spec=2
kernel_arg=vec2.0:transport=tap,mode=inproc,ifname=soak-tap0,depth=128,queues=2
sudo sh -c "ip tuntap add dev soak-tap0 mode tap user ... multi_queue"
sudo sh -c "ip tuntap del dev soak-tap0 mode tap multi_queue"
```

The guest init metadata block reports `UMLCTL_NETWORK_QUEUES=2`.

## Validation Evidence

Build and unit coverage:

```text
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r1-kunit -j$(nproc)
kunit.py parse /tmp/um-vector-r8a-kunit.log
Testing complete. Ran 66 tests: passed: 66

cargo fmt --check
cargo test
```

New KUnit coverage includes:

- configured v2 netdev queue count;
- TAP channel index initialization for attached fd tests;
- ethtool queue-stat aggregation across multiple channels.

Manual direct driver smoke:

```text
Kernel command line: ... vec2.0:transport=tap,mode=inproc,ifname=r8a-mq0,depth=64,queues=2 ...
uml-vector2: vec2.0 configured transport=tap mode=inproc requested_queues=2 runtime_queues=2 depth=64
3 packets transmitted, 3 received, 0% packet loss
vec2.0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP
MQ_OK
PASS=1/1 FAIL=0 TIMEOUT=0
```

`umlctl` multiqueue smoke:

```text
[umlctl] sudo: ip tuntap add dev soak-tap0 mode tap user ... multi_queue
[umlctl] network: driver=vector2 guest_dev=vec2.0 tap=soak-tap0 transport=tap host_mode=inproc queues=2
Kernel command line: ... vec2.0:transport=tap,mode=inproc,ifname=soak-tap0,depth=128,queues=2 ...
uml-vector2: vec2.0 configured transport=tap mode=inproc requested_queues=2 runtime_queues=2 depth=128
SERVER_READY
TIER3_OK
REPRO_DONE rc=0
PASS=1/1 FAIL=0 TIMEOUT=0
TAP_ABSENT
```

## Remaining R8 Work

R8 is still open.  This checkpoint does not yet provide:

- fd multiqueue;
- queue-to-CPU mapping policy;
- KCSAN evidence;
- queue-distribution evidence under parallel traffic;
- kvm-v2 multiqueue evidence, because kvm-v2 currently fails
  no-network readiness before vector2 can be meaningfully validated.

R8b adds the per-queue ethtool stat names needed for distribution
instrumentation and records a small parallel traffic smoke where both
queue0 and queue1 counters move under `umlctl --network-queues 2`.
