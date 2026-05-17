# UML vector driver v2 R3 fd backend

**Status:** IMPLEMENTED R3 checkpoint - trusted direct-fd open/close.
**Date:** 2026-05-17.

This memo records the third runtime buildout checkpoint for vector
networking v2.  R3 added a trusted direct-fd host backend and lets
`ip link set vec2.0 up/down` succeed for `transport=fd,fd=<n>`.

The original R3 checkpoint did not move packets.  The later fd datapath
follow-up in `27-uml-vector-driver-v2-fd-datapath.md` adds
single-queue raw Ethernet TX/RX over an inherited fd.  A later sandbox
follow-up allows inherited `fd=` in sandbox builds because it consumes
already-delegated launcher authority instead of creating host network
resources inside UML.  Launcher-owned fd manifests, fd multiqueue, and
fd performance profiles remain future work.

The old `CONFIG_UML_NET_VECTOR` driver remains the production vector
networking path.

## What Landed

R3 adds:

- `arch/um/drivers/vector2_host_fd.c`;
- `arch/um/drivers/vector2_host_fd_test.c`;
- `CONFIG_UML_NET_VECTOR_V2_HOST_FD_KUNIT`;
- direct-fd duplication through `os_dup_file()`;
- explicit close ownership for the duplicated fd;
- channel lifecycle transitions:
  - `UNINIT -> ALLOCATED -> FD_ATTACHED`;
  - `FD_ATTACHED -> QUIESCING -> CLOSED`;
- `ndo_open()` support for `UM_VEC2_TRANSPORT_FD`;
- `ndo_stop()` cleanup for the fd-backed channel;
- an `ethtool -i` version string for the R3 skeleton;
- KUnit coverage for fd open/close, bad-fd failure, and netdev
  open/stop integration.

The trusted direct-fd path is intentionally narrow:

```text
CONFIG_UML_NET_VECTOR_V2=y
CONFIG_UML_NET_VECTOR_V2_INPROC=y
vec2.0:transport=fd,fd=0
```

The driver duplicates the supplied fd.  It owns and closes only the
duplicate, not the original fd named on the command line.

## Sandbox Boundary

Direct numeric `fd=` started as a trusted host option in R3.  After the
fd datapath follow-up it is allowed in the default sandbox
configuration because the UML process can only use fds that the
launcher already deliberately left open:

```text
CONFIG_UML_NET_VECTOR_V2=y
# CONFIG_UML_NET_VECTOR_V2_INPROC is not set
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
```

Sandbox TAP rejection still fails closed:

```text
uml-vector2: vec2.0 config rejected: key='ifname' msg='trusted host option not permitted' ret=-13
uml-vector2: 1 vec2 command-line spec(s) rejected; no v2 devices configured
R3_SANDBOX_INIT=1
```

The future launcher-owned fd manifest is still required for usability,
fd naming, leak resistance, and auditability.  The kernel-side
sandbox boundary is narrower: inherited fds are permitted; host TAP,
raw socket, helper, BPF, VDE, and similar host-resource creation
options remain rejected unless trusted in-process mode is enabled.

## Manual Runtime Check

Trusted direct-fd kernel config:

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
  vec2.0:transport=fd,fd=0,depth=128 \
  init=/tmp/uml-vector-r3-init.sh loglevel=7
```

Relevant result:

```text
uml-vector2: registered netdev vec2.0 for vec2.0
uml-vector2: vec2.0 configured transport=fd mode=auto requested_queues=1 runtime_queues=1 depth=128
uml-vector2: configured 1 experimental v2 runtime device(s)
R3_SHOW_RC=0
driver: uml-vector-v2
version: r3-fd-skeleton
R3_ETHTOOL_RC=0
R3_UP_RC=0
R3_SHOW_AFTER_UP_RC=0
R3_DOWN_RC=0
R3_SHOW_AFTER_DOWN_RC=0
```

In the original R3 open/close checkpoint, after
`ip link set vec2.0 up`, the device reported:

```text
<NO-CARRIER,BROADCAST,MULTICAST,UP>
```

That was expected in R3 because fd attach succeeded but packet
movement, carrier policy, NAPI, IRQ registration, and BQL were still
future phases.

No R3 manual boot reported:

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
```

Total for the original R3 checkpoint: 55 passed, 0 failed.

The fd datapath follow-up extends this suite to 68 total vector2 tests,
including fd TX and RX packet movement over UNIX datagram fds.

## Build Matrix

All builds below were run on 2026-05-17.

Old driver only:

```text
CONFIG_UML_NET_VECTOR=y
# CONFIG_UML_NET_VECTOR_V2 is not set
```

Result: UML build succeeded.

V2 sandbox:

```text
# CONFIG_UML_NET_VECTOR is not set
CONFIG_UML_NET_VECTOR_V2=y
# CONFIG_UML_NET_VECTOR_V2_INPROC is not set
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
CONFIG_UML_NET_VECTOR_V2_CONFIG=y
CONFIG_UML_NET_VECTOR_V2_QUEUE=y
CONFIG_UML_NET_VECTOR_V2_MODEL=y
```

Result: UML build succeeded.

V2 trusted direct-fd:

```text
# CONFIG_UML_NET_VECTOR is not set
CONFIG_UML_NET_VECTOR_V2=y
CONFIG_UML_NET_VECTOR_V2_INPROC=y
CONFIG_UML_NET_VECTOR_V2_CONFIG=y
CONFIG_UML_NET_VECTOR_V2_QUEUE=y
CONFIG_UML_NET_VECTOR_V2_MODEL=y
```

Result: UML build succeeded.

Both drivers:

```text
CONFIG_UML_NET_VECTOR=y
CONFIG_UML_NET_VECTOR_V2=y
# CONFIG_UML_NET_VECTOR_V2_INPROC is not set
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
CONFIG_UML_NET_VECTOR_V2_CONFIG=y
CONFIG_UML_NET_VECTOR_V2_QUEUE=y
CONFIG_UML_NET_VECTOR_V2_MODEL=y
```

Result: UML build succeeded.

Neither driver:

```text
# CONFIG_UML_NET_VECTOR is not set
# CONFIG_UML_NET_VECTOR_V2 is not set
```

Result: UML build succeeded.

No build reported warnings or errors from `arch/um/drivers/vector2_*`.

## Next Required Phase

R4 should implement trusted TAP open/close without packet movement.

Required R4 deliverables:

- `vector2_host_tap.c`;
- TAP open/attach gated by `CONFIG_UML_NET_VECTOR_V2_INPROC`;
- TAP fd ownership and close unwind through the same channel lifecycle
  model used by fd;
- explicit rejection in sandbox builds before `/dev/net/tun` is opened;
- `ip link set vec2.0 up/down` working for trusted TAP;
- KUnit failure injection for TAP open, attach, and close unwind where
  practical;
- manual `strace` or equivalent evidence that sandbox mode does not
  open `/dev/net/tun`.

Packet movement remains R5.
