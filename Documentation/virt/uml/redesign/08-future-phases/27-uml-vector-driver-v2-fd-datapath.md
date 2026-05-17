# UML vector driver v2 fd datapath checkpoint

**Status:** R5 follow-up - trusted single-queue direct-fd packet path.
**Date:** 2026-05-17.

This note records the first packet-moving direct-fd backend for vector
networking v2.  It does not make fd mode replacement-ready.  It proves
that a supplied fd can use the same v2 queue ownership, NAPI, IRQ, and
ethtool surfaces as the trusted TAP path.

## Implemented

- `vector2_runtime.c` now owns shared runtime queue-pair allocation and
  cleanup helpers.
- `struct um_vec2_channel` now records `rx_fd` and `tx_fd`, so netdev
  datapath startup does not need TAP-specific fd accessors.
- `vector2_host_fd.c` allocates a v2 queue pair for the fd channel.
- The duplicated fd is forced nonblocking before datapath use.
- fd TX writes raw Ethernet frames from the v2 TX ring.
- fd RX reads raw Ethernet frames into the v2 RX batch.
- fd channels use the same NAPI/read-IRQ/write-IRQ startup path as TAP.
- `um_vector2_host_fd` KUnit now covers fd TX and RX packet movement.
- sandbox builds accept inherited `fd=` specs while still rejecting
  host resource creation options such as TAP `ifname=`.

The direct-fd wire format is deliberately simple: one raw Ethernet
frame per fd read/write.  There is no virtio-net header in fd mode.
The TAP backend still uses vnet headers because TAP advertises and
expects them.

## Validation Evidence

Targeted object build:

```text
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r3-fd \
  arch/um/drivers/vector2_runtime.o \
  arch/um/drivers/vector2_host_fd.o \
  arch/um/drivers/vector2_host_tap.o \
  arch/um/drivers/vector2_netdev.o \
  arch/um/drivers/vector2_host_fd_test.o \
  arch/um/drivers/vector2_host_tap_test.o -j$(nproc)
```

Full runtime and KUnit UML builds completed:

```text
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r3-fd -j$(nproc)
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r1-kunit -j$(nproc)
```

KUnit:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt
kunit.py parse /tmp/um-vector-fd-datapath-kunit.log
Testing complete. Ran 68 tests: passed: 68
```

The new fd tests cover:

- raw-frame TX from the v2 TX ring into a supplied UNIX datagram fd;
- raw-frame RX from a supplied UNIX datagram fd into the v2 RX batch;
- fd-backed `netdev_open()` now reaching carrier-on through the common
  datapath startup path.

Manual no-root fd datapath smoke:

```text
vec2.0:transport=fd,fd=4,depth=128
FD_DATAPATH_INIT
vec2.0: <BROADCAST,MULTICAST,UP,LOWER_UP>
3 packets transmitted, 3 received, 0% packet loss
tx_ring_enqueued: 4
rx_batch_received_total: 4
queue0_tx_ring_enqueued: 4
queue0_rx_batch_received_total: 4
FD_DATAPATH_OK
fd_datapath_proc_rc=0
fd_datapath_arp_replies=1
fd_datapath_icmp_replies=3
fd_datapath_marker=True
```

The smoke used an inherited UNIX datagram socket fd and a host-side
ARP/ICMP responder.  That proves fd packet movement without requiring
root, TAP, helper execution, or raw sockets.

Sandbox policy check:

```text
CONFIG_UML_NET_VECTOR_V2=y
# CONFIG_UML_NET_VECTOR_V2_INPROC is not set
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
```

With that config, inherited `transport=fd,fd=<n>` is accepted because
the fd was already delegated by the launcher.  TAP `ifname=`, helper,
BPF, raw socket, VDE, and similar host-resource creation options still
require `CONFIG_UML_NET_VECTOR_V2_INPROC=y`.

Sandbox fd datapath smoke:

```text
vec2.0:transport=fd,fd=4,depth=128
uml-vector2: vec2.0 configured transport=fd mode=auto requested_queues=1 runtime_queues=1 depth=128
FD_SANDBOX_INIT
vec2.0: <BROADCAST,MULTICAST,UP,LOWER_UP>
3 packets transmitted, 3 received, 0% packet loss
queue0_tx_ring_enqueued: 4
queue0_rx_batch_received_total: 4
FD_SANDBOX_OK
fd_sandbox_proc_rc=0
fd_sandbox_arp_replies=1
fd_sandbox_icmp_replies=3
fd_sandbox_marker=True
fd_sandbox_rejected=False
```

## Remaining Work

This checkpoint does not close the fd replacement gates.  Remaining fd
work includes:

- live `umlctl up` smoke coverage for launcher-owned TAP fd handoff;
- stronger sandbox fd diagnostics when the referenced fd is missing or
  has the wrong type;
- fd multiqueue;
- fd batching with `sendmmsg()` / `recvmmsg()` or an accepted simpler
  replacement;
- scatter-gather TX without forced linearization;
- fd performance and fairness profiles;
- kvm-v2 fd validation after the baseline kvm-v2 readiness issue is
  fixed.
