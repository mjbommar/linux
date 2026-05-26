# UML vector driver v2 R2 netdev skeleton

**Status:** IMPLEMENTED R2 checkpoint - inspectable netdev only.
**Date:** 2026-05-17.

This memo records the second runtime buildout checkpoint for vector
networking v2.  R2 registers an inspectable Linux netdev for each
accepted `vec2` command-line spec.  It still does not move packets and
does not open host networking resources.

The old `CONFIG_UML_NET_VECTOR` driver remains the production vector
networking path.

## What Landed

R2 adds these runtime pieces:

- `arch/um/drivers/vector2_netdev.c`;
- `arch/um/drivers/vector2_ethtool.c`;
- `arch/um/drivers/vector2_netdev_test.c`;
- `static const struct net_device_ops um_vec2_netdev_ops`;
- `alloc_etherdev_mqs(..., 1, 1)` for the first forced single-queue
  skeleton;
- stable v2 netdev names using `vec2.<unit>`;
- `register_netdevice()` from validated v2 command-line specs;
- unregister cleanup through `um_vec2_netdev_unregister()`;
- read-only `ethtool -i` driver information;
- `ndo_open()` wired through the v2 device lifecycle and unwinding
  cleanly to `REGISTERED` with `-EOPNOTSUPP`;
- `ndo_stop()` safe while stopped or during future open unwind paths;
- `ndo_start_xmit()` safety-drop behavior for impossible pre-datapath
  calls;
- KUnit coverage for name/MAC setup, open unwind, stop handling, and
  xmit drop behavior.

The R2 runtime flow is:

```text
vec2 boot spec
  -> vector2_cmdline.c collects the spec
  -> vector2_core.c validates typed config
  -> um_vec2_dev transitions NEW -> CONFIGURED
  -> vector2_netdev.c allocates and initializes netdev vec2.<unit>
  -> register_netdevice()
  -> um_vec2_dev transitions CONFIGURED -> REGISTERED
```

`ip link set vec2.<unit> up` intentionally fails today:

```text
vec2.0: vector v2 host backend is not implemented yet
RTNETLINK answers: Operation not supported
```

The failure path is part of R2's contract.  It proves the netdev is
reachable through the Linux networking stack without pretending that fd
or TAP backends exist.

## What Did Not Land

R2 deliberately does not provide:

- successful `ndo_open()` to `RUNNING`;
- host fd ownership;
- TAP open/attach;
- RX/TX packet movement;
- NAPI;
- IRQ registration;
- BQL accounting;
- ethtool stats, ring parameters, or coalescing;
- multiqueue runtime allocation beyond forced `1 x 1` netdev queues;
- legacy `vecN:` compatibility mapping;
- Tier 3 workload support.

The next phase must implement the fd host backend first, before any
trusted in-process TAP path.

## Manual Runtime Check

The v2-only kernel was built in:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-v2only
```

Manual boot:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  mem=256M noreboot con=fd:0,fd:1 ssl=null \
  root=/dev/root rootfstype=hostfs rootflags=/ rw \
  vec2.0:transport=proxy,depth=128,queues=2 \
  init=/tmp/uml-vector-r2-init.sh loglevel=7
```

Relevant result:

```text
uml-vector2: registered netdev vec2.0 for vec2.0
uml-vector2: vec2.0 configured transport=proxy mode=auto requested_queues=2 runtime_queues=1 depth=128
uml-vector2: configured 1 experimental v2 runtime device(s)
R2_SHOW_RC=0
driver: uml-vector-v2
version: r2-netdev-skeleton
bus-info: uml
R2_ETHTOOL_RC=0
RTNETLINK answers: Operation not supported
R2_UP_RC=2
R2_SHOW_AFTER_UP_RC=0
R2_DOWN_RC=0
```

No manual boot reported:

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
```

Total: 52 passed, 0 failed.

## Build Matrix

All builds below were run on 2026-05-17.

Old driver only:

```text
CONFIG_UML_NET_VECTOR=y
# CONFIG_UML_NET_VECTOR_V2 is not set
```

Result: UML build succeeded.

V2 only:

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

R3 must provide the fd host backend before trusted TAP.

Required R3 deliverables:

- `vector2_host_fd.c`;
- explicit fd ownership policy;
- command-line and launcher-manifest distinction for fd authority;
- one forced queue pair backed by real fds;
- open path that can reach `RUNNING` for fd mode;
- stop path that closes or releases fds exactly once;
- KUnit failure injection for fd attach, fd death, and close unwind;
- manual socketpair or launcher-owned-fd smoke without root.

R3 exit gate:

```text
ip link set vec2.0 up
ip link set vec2.0 down
```

Both operations must work with fd-backed v2 networking and no packet
movement requirement yet.  Packet movement remains R5.
