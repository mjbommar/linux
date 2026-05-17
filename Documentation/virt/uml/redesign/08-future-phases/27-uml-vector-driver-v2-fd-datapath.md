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
- fd open now preflights the inherited fd with `os_stat_fd()` and
  rejects unsupported file types before allocating channel state.
- fd multiqueue core now treats `fd=N,queues=Q` as a contiguous
  inherited-fd range `N..N+Q-1`, validates the whole range before
  duplicating any fd, and unwinds already-opened channels on failure.
- `umlctl` fd handoff now opens one TAP fd per configured queue and
  maps the contiguous inherited range starting at fd 200 into the UML
  process.
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

After fd preflight diagnostics:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt
kunit.py parse /tmp/um-vector-fd-diagnostics-kunit.log
Testing complete. Ran 69 tests: passed: 69
```

After fd multiqueue core:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt
kunit.py parse /tmp/um-vector-fd-multiqueue-kunit.log
Testing complete. Ran 71 tests: passed: 71
```

The new fd diagnostic KUnit cases verify both closed/missing fd and
wrong-type fd behavior:

```text
uml-vector2-fd: vec2.1 inherited fd 2147483647 is not usable: fstat failed ret=-9
uml-vector2-fd: vec2.5 inherited fd 11 has unsupported type directory mode=040775; expected char, fifo, or socket
```

The new fd tests cover:

- raw-frame TX from the v2 TX ring into a supplied UNIX datagram fd;
- raw-frame RX from a supplied UNIX datagram fd into the v2 RX batch;
- fd-backed `netdev_open()` now reaching carrier-on through the common
  datapath startup path.
- bad inherited fd diagnostics and closed-state unwind.
- two-queue contiguous-fd open/close;
- full-range fd validation before duplication, so a missing later fd
  cannot be masked by `dup()` reusing that fd number.

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

Launcher-owned TAP fd smoke:

```text
umlctl up ... --wait-for FD_HANDOFF_OK
[umlctl] network: driver=vector2 guest_dev=vec2.0 tap=v2fd0 transport=fd host_mode=fd queues=1
[umlctl] network-fd: open tap=v2fd0 and inherit as fd=200
Kernel command line: ... vec2.0:transport=fd,mode=fd,fd=200,depth=128 ...
uml-vector2: registered netdev vec2.0 for vec2.0
uml-vector2: vec2.0 configured transport=fd mode=fd requested_queues=1 runtime_queues=1 depth=128
3 packets transmitted, 3 received, 0% packet loss
queue0_rx_batch_received_total: 24
FD_HANDOFF_OK
Device "v2fd0" does not exist.
```

Repeated `umlctl gate loop` fd-handoff smoke:

```text
umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-fd-handoff.toml \
  -W 1 -M 3 --timeout 120 --pass-marker VECTOR2_FD_HANDOFF_OK
==> default PASS=3/3 FAIL=0 TIMEOUT=0 rate=100.0%
```

Each saved run log showed `vec2.0:transport=fd,mode=fd,fd=200`,
`uml-vector2: registered netdev vec2.0`, successful 3/3 ping, and
queue0 TX/RX counters.  A post-loop TAP check returned:

```text
fd_diag_gate_tap_after_rc=1
Device "v2fd0" does not exist.
```

The repeated loop ran with the `umlctl` TAP cleanup audit enabled.
For TAP-backed Umlfiles, each iteration now fails if the declared TAP
still exists under `/sys/class/net` after `down --force --rm`.

Launcher-owned TAP fd multiqueue smoke:

```text
umlctl up -f tools/uml/uml-launcher/examples/vector2-fd-multiqueue.toml \
  --wait-for VECTOR2_FD_MULTIQUEUE_OK
[umlctl] network: driver=vector2 guest_dev=vec2.0 tap=v2fdmq0 transport=fd host_mode=fd queues=4
[umlctl] network-fd: open tap=v2fdmq0 and inherit fds=200..203
Kernel command line: ... vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4 ...
uml-vector2: vec2.0 configured transport=fd mode=fd requested_queues=4 runtime_queues=4 depth=128
numtxqueues 4
3 packets transmitted, 3 received, 0% packet loss
VECTOR2_FD_MULTIQUEUE_OK
```

Repeated fd multiqueue gate-loop smoke:

```text
umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-fd-multiqueue.toml \
  -W 1 -M 3 --timeout 180 --pass-marker VECTOR2_FD_MULTIQUEUE_OK
==> default PASS=3/3 FAIL=0 TIMEOUT=0 rate=100.0%
TAP_ABSENT
```

No cleanup failure logs were produced by that loop.

## Remaining Work

This checkpoint does not close the fd replacement gates.  Remaining fd
work includes:

- KCSAN and longer SMP traffic validation for launcher-owned fd
  multiqueue;
- fd batching with `sendmmsg()` / `recvmmsg()` or an accepted simpler
  replacement;
- scatter-gather TX without forced linearization;
- fd performance and fairness profiles;
- kvm-v2 fd validation after the baseline kvm-v2 readiness issue is
  fixed.
