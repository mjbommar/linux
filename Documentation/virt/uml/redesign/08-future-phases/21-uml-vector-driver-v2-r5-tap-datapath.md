# UML vector driver v2 R5 TAP datapath

**Status:** IMPLEMENTED R5 checkpoint - single-queue trusted TAP packet
movement.
**Date:** 2026-05-17.

This memo records the fifth runtime buildout checkpoint for vector
networking v2.  R5 turns the trusted TAP backend from open/close
scaffolding into the first working single-queue packet path.

R5 is still not replacement-ready.  It is a deliberately narrow TAP
datapath checkpoint: one queue, trusted in-process TAP only, no
multiqueue, no sandbox helper/proxy TAP, and no performance tuning.

The old `CONFIG_UML_NET_VECTOR` driver remains the production vector
networking path.

## What Landed

R5 adds:

- v2 TX/RX queue allocation for trusted TAP channels;
- TAP fd nonblocking mode after `TUNSETIFF`;
- channel-owned NAPI state;
- channel-owned read IRQ through `um_request_irq(UM_IRQ_ALLOC, ...)`;
- channel lifecycle transitions through:
  - `FD_ATTACHED -> IRQ_ATTACHED`;
  - `IRQ_ATTACHED -> NAPI_ENABLED`;
  - `NAPI_ENABLED -> ACTIVE`;
- `ndo_start_xmit()` enqueue into the v2 TX ring;
- TAP TX through `um_vec2_host_ops.tx_batch`;
- TAP RX through `um_vec2_host_ops.rx_batch`;
- vnet header formation with `virtio_net_hdr_from_skb()`;
- vnet header receive normalization with `virtio_net_hdr_to_skb()`;
- NAPI delivery through `napi_gro_receive()`;
- queue cleanup that releases pending skbs during close/unwind;
- carrier-on and queue start only for the TAP datapath;
- KUnit coverage for TAP TX write and RX read using pipe-backed fds.

The R5 implementation intentionally uses a simple linearized TX path.
That is correct enough for the first safety checkpoint, but it is not
the final performance design.  Later phases should replace it with
scatter-gather and batched syscall paths once the single-queue behavior
is stable.

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
  init=/tmp/uml-vector-r5-init.sh loglevel=7
```

Guest-side smoke:

```text
ip addr add 192.168.42.2/30 dev vec2.0
ip link set vec2.0 up
ping -c 3 -W 1 192.168.42.1
ip link set vec2.0 down
```

Relevant result:

```text
driver: uml-vector-v2
version: r5-tap-datapath
R5_UP_RC=0
vec2.0: <BROADCAST,MULTICAST,UP,LOWER_UP>
3 packets transmitted, 3 received, 0% packet loss
R5_PING_RC=0
R5_DOWN_RC=0
```

The guest interface counters showed packet movement:

```text
RX: bytes=472 packets=6 errors=0 dropped=2
TX: bytes=336 packets=4 errors=0 dropped=0
```

No R5 trusted TAP manual boot reported:

```text
Kernel panic
BUG:
Oops
Kernel mode fault
general protection fault
NULL pointer
```

## KUnit Coverage

KUnit was run with:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt
```

Result:

```text
ok 1 um_vector2_config       pass:12 fail:0 skip:0 total:12
ok 2 um_vector2_queue        pass:8  fail:0 skip:0 total:8
ok 3 um_vector2_transport    pass:8  fail:0 skip:0 total:8
ok 4 um_vector2_fake_host    pass:8  fail:0 skip:0 total:8
ok 5 um_vector2_model        pass:7  fail:0 skip:0 total:7
ok 6 um_vector2_cmdline      pass:5  fail:0 skip:0 total:5
ok 7 um_vector2_netdev       pass:4  fail:0 skip:0 total:4
ok 8 um_vector2_host_fd      pass:3  fail:0 skip:0 total:3
ok 9 um_vector2_host_tap     pass:6  fail:0 skip:0 total:6
```

Total: 61 passed, 0 failed.

The new R5 TAP tests cover:

- TX ring descriptor completion after a vnet-header write;
- RX batch fill and consume after a vnet-header read;
- fd ownership through attach/close;
- sandbox open denial and netdev unwind.

## Known Limits

R5 deliberately did not claim:

- sandbox-safe TAP datapath;
- multiqueue;
- batching with `sendmmsg()` or `recvmmsg()`;
- scatter-gather TX without linearization;
- write-ready IRQ backpressure handling for persistent `-EAGAIN`;
- ethtool ring/stat/coalescing controls;
- KCSAN/KASAN/KFENCE coverage;
- Tier 3 Django/FastAPI soak readiness.

These are replacement blockers, not optional cleanup.

The later fd datapath follow-up in
`27-uml-vector-driver-v2-fd-datapath.md` adds trusted single-queue
direct-fd packet movement.  Launcher-owned fd manifests,
sandbox-safe fd authority, fd multiqueue, batching, and fd performance
work remain replacement blockers.

## Next Required Phase

R6 should harden the single-queue datapath into an inspectable driver
surface.

Required R6 deliverables:

- ethtool stats for lifecycle state, queue depth, TX/RX packets, drops,
  transient errors, and fd death;
- ethtool ring parameters for the single queue;
- explicit feature reporting for vnet header, checksum, GSO, GRO, and
  current limitations;
- targeted KUnit for stopped/running ethtool behavior;
- repeated open/up/ping/down loops;
- write-side `-EAGAIN` handling that cannot leave a full TX ring stuck;
- a short TAP throughput and syscall-count baseline against the legacy
  driver.

R7 should then run the broader Tier 3 and soak matrix before any
multiqueue work starts.
