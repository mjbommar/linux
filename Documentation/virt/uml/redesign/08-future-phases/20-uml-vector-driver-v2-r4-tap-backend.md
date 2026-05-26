# UML vector driver v2 R4 TAP backend

**Status:** IMPLEMENTED R4 checkpoint - trusted TAP open/close.
**Date:** 2026-05-17.

This memo records the fourth runtime buildout checkpoint for vector
networking v2.  R4 adds a trusted in-process TAP host backend and lets
`ip link set vec2.0 up/down` succeed for
`transport=tap,ifname=<tap>`.

R4 still does not move packets.  The netdev remains `NO-CARRIER` after
open and the queue remains stopped until the later datapath phase.

The old `CONFIG_UML_NET_VECTOR` driver remains the production vector
networking path.

## What Landed

R4 adds:

- `arch/um/drivers/vector2_host_tap.c`;
- `arch/um/drivers/vector2_host_tap_test.c`;
- `CONFIG_UML_NET_VECTOR_V2_HOST_TAP_KUNIT`;
- TAP open through the existing UML `os_open_file()` and
  `os_ioctl_generic()` host-call boundary;
- close-on-exec on the owned TAP fd;
- `TUNSETIFF` with `IFF_TAP | IFF_NO_PI | IFF_VNET_HDR`;
- best-effort `TUNSETOFFLOAD` for checksum and TSO flags;
- explicit policy gating before `/dev/net/tun` can be opened:
  `CONFIG_UML_NET_VECTOR_V2_INPROC=y` is required;
- channel lifecycle transitions:
  - `UNINIT -> ALLOCATED -> FD_ATTACHED`;
  - `FD_ATTACHED -> QUIESCING -> CLOSED`;
- TAP fd ownership with a narrow attach helper:
  `um_vec2_tap_attach_fd()` consumes the fd only after success;
- backend-specific close routing in `vector2_netdev.c` so TAP and fd
  cleanup cannot accidentally call the wrong closer;
- `ndo_open()` support for `UM_VEC2_TRANSPORT_TAP`;
- `ndo_stop()` cleanup for the TAP-backed channel;
- an `ethtool -i` version string for the R4 skeleton;
- KUnit coverage for TAP attach/close, busy attach rejection, sandbox
  open denial, and netdev open unwind in sandbox mode.

The trusted TAP path is intentionally narrow:

```text
CONFIG_UML_NET_VECTOR_V2=y
CONFIG_UML_NET_VECTOR_V2_INPROC=y
vec2.0:transport=tap,ifname=soak-tap0
```

Full vnet-header feature reporting, carrier policy, NAPI, and packet
movement remain future phases.

## Sandbox Boundary

`ifname=` is still a trusted host option.  It is rejected by the v2
parser in the default sandbox configuration:

```text
CONFIG_UML_NET_VECTOR_V2=y
# CONFIG_UML_NET_VECTOR_V2_INPROC is not set
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
```

Sandbox rejection result:

```text
uml-vector2: vec2.0 config rejected: key='ifname' msg='trusted host option not permitted' ret=-13
uml-vector2: 1 vec2 command-line spec(s) rejected; no v2 devices configured
R4_SANDBOX_INIT=1
R4_SANDBOX_VEC2_PRESENT=0
```

The sandbox `strace` audit was run with:

```text
strace -f -e trace=open,openat,ioctl -o /tmp/um-vector-r4-sandbox-strace.log \
  /home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  mem=256M noreboot con=fd:0,fd:1 ssl=null \
  root=/dev/root rootfstype=hostfs rootflags=/ rw \
  vec2.0:transport=tap,ifname=soak-tap0,depth=128 \
  init=/tmp/uml-vector-r4-sandbox-init.sh loglevel=7
```

This audit found no matches for:

```text
/dev/net/tun
TUNSETIFF
TUNSETOFFLOAD
```

The future sandbox-safe TAP path must come from a launcher-owned fd
manifest, helper process, or proxy transport with a narrow policy
object.  Sandbox mode must not open host TAP devices from guest command
line authority.

## Manual Runtime Check

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
  init=/tmp/uml-vector-r4-init.sh loglevel=7
```

Relevant result:

```text
uml-vector2: registered netdev vec2.0 for vec2.0
uml-vector2: vec2.0 configured transport=tap mode=auto requested_queues=1 runtime_queues=1 depth=128
uml-vector2: configured 1 experimental v2 runtime device(s)
driver: uml-vector-v2
version: r4-tap-skeleton
R4_UP_RC=0
R4_DOWN_RC=0
```

After `ip link set vec2.0 up`, the device reports:

```text
<NO-CARRIER,BROADCAST,MULTICAST,UP>
```

That is expected in R4 because TAP fd attach succeeds but packet
movement, carrier enablement, NAPI, IRQ registration, and BQL are still
future phases.

No R4 trusted TAP manual boot reported:

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
ok 9 um_vector2_host_tap     pass:4  fail:0 skip:0 total:4
```

Total: 59 passed, 0 failed.

## Build Evidence

All builds below were run on 2026-05-17.

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

V2 trusted TAP/fd:

```text
# CONFIG_UML_NET_VECTOR is not set
CONFIG_UML_NET_VECTOR_V2=y
CONFIG_UML_NET_VECTOR_V2_INPROC=y
CONFIG_UML_NET_VECTOR_V2_CONFIG=y
CONFIG_UML_NET_VECTOR_V2_QUEUE=y
CONFIG_UML_NET_VECTOR_V2_MODEL=y
```

Result: UML build succeeded.

No build reported warnings or errors from `arch/um/drivers/vector2_*`.

## Next Required Phase

R5 should implement the first single-queue TAP packet path.

Required R5 deliverables:

- `ndo_start_xmit()` maps an skb to a v2 TX descriptor;
- TAP TX writes packets with the agreed vnet header contract;
- TAP RX receives packets into v2 RX batches;
- NAPI owns RX delivery and budget accounting;
- carrier and queue wake/stop policy is based on real backend state;
- skb ownership is released exactly once on success, drop, partial
  send, and hard error;
- KUnit covers host TX/RX success, partial send, transient error, and
  fd death using fake host operations;
- manual trusted TAP `ping` and TCP smoke pass before Tier 3 work
  starts.

Multiqueue, sandbox helper/proxy TAP, ethtool feature controls, and
replacement of the old driver remain later phases.
