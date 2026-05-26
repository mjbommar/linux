# UML vector driver v2 R1 runtime skeleton

**Status:** IMPLEMENTED R1 checkpoint - not a swap-ready driver.
**Date:** 2026-05-17.

This memo records the first runtime buildout checkpoint for vector
networking v2.  It makes the v2 work visible as an experimental UML
runtime feature, but it intentionally stops before Linux netdev
registration and packet movement.

The old `CONFIG_UML_NET_VECTOR` driver remains the production vector
networking path.

## What Landed

R1 adds these runtime skeleton pieces:

- `CONFIG_UML_NET_VECTOR_V2`, default `n`, visible in UML networking
  Kconfig.
- `CONFIG_UML_NET_VECTOR_V2_INPROC`, default `n`, for trusted
  in-process host networking authority.
- `CONFIG_UML_NET_VECTOR_V2_SANDBOX`, derived from
  `!CONFIG_UML_NET_VECTOR_V2_INPROC`, for the default sandbox parsing
  policy.
- `arch/um/drivers/vector2_internal.h`, defining private runtime
  ownership objects:
  - `struct um_vec2_dev`
  - `struct um_vec2_channel`
  - `struct um_vec2_queue_pair`
  - collected command-line specs.
- `arch/um/drivers/vector2_cmdline.c`, collecting v2-only boot
  arguments without touching the legacy `vecN:` parser.
- `arch/um/drivers/vector2_core.c`, validating collected specs at
  late init and creating internal v2 runtime objects.
- `arch/um/drivers/vector2_cmdline_test.c`, covering the command-line
  splitter under KUnit.

The accepted v2 development syntaxes are:

```text
vec2.<unit>:<key>=<value>,<key>=<value>
vec2=<unit>,<key>=<value>,<key>=<value>
```

Examples:

```text
vec2.0:transport=proxy,depth=128,queues=2
vec2=1,transport=proxy
```

The v2 command-line parser rejects empty units, empty specs,
non-decimal units, missing delimiters, and duplicate units.  Runtime
configuration still goes through `um_vec2_config_parse()`, so sandbox
policy rejects trusted host options such as `ifname` unless
`CONFIG_UML_NET_VECTOR_V2_INPROC=y`.

## What Did Not Land

R1 deliberately does not provide any of the swap-in pieces:

- no v2 `struct net_device_ops`;
- no `alloc_etherdev_mqs()`;
- no `register_netdevice()` or `register_netdev()`;
- no `ndo_open`, `ndo_stop`, or `ndo_start_xmit`;
- no NAPI poll function;
- no real host fd ownership;
- no TAP open path;
- no ethtool operations;
- no packet movement;
- no legacy `vecN:` compatibility mapping.

This is the runtime skeleton needed before R2 can register an
inspectable netdev.

## Sandbox Policy

R1 keeps the secure default:

```text
CONFIG_UML_NET_VECTOR_V2=y
# CONFIG_UML_NET_VECTOR_V2_INPROC is not set
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
```

In that mode, v2 accepts sandbox-safe parser inputs such as
`transport=proxy`, but rejects trusted host authority.  This preserves
the UML v2 security boundary while the driver is still being built.

The first trusted host mode is only enabled by:

```text
CONFIG_UML_NET_VECTOR_V2_INPROC=y
```

Future phases must keep that distinction intact:

- sandbox mode consumes launcher-owned fds or an explicit safe proxy;
- in-process mode may open TAP/raw/GRE/L2TPv3 resources directly;
- guest boot arguments must not be able to trigger helper execution,
  TAP creation, raw sockets, or BPF loading in sandbox builds.

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

## Manual Runtime Checks

The v2-only kernel was built in:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-v2only
```

No v2 arguments:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  mem=256M noreboot con=fd:0,fd:1 ssl=null \
  root=/dev/root rootfstype=hostfs rootflags=/ rw \
  init=/tmp/uml-vector-r1-init.sh loglevel=7
```

Result: boot reached init and halted.  No `uml-vector2` devices were
configured.

Sandbox rejection:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  mem=256M noreboot con=fd:0,fd:1 ssl=null \
  root=/dev/root rootfstype=hostfs rootflags=/ rw \
  vec2.0:transport=tap,ifname=tap0 \
  init=/tmp/uml-vector-r1-init.sh loglevel=7
```

Result:

```text
uml-vector2: vec2.0 config rejected: key='ifname' msg='trusted host option not permitted' ret=-13
uml-vector2: 1 vec2 command-line spec(s) rejected; no v2 devices configured
```

The kernel continued to init and halted cleanly.

Accepted sandbox-safe specs:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  mem=256M noreboot con=fd:0,fd:1 ssl=null \
  root=/dev/root rootfstype=hostfs rootflags=/ rw \
  vec2.0:transport=proxy,depth=128,queues=2 \
  vec2=1,transport=proxy \
  init=/tmp/uml-vector-r1-init.sh loglevel=7
```

Result:

```text
uml-vector2: vec2.0 configured transport=proxy mode=auto queues=2 depth=128
uml-vector2: vec2.1 configured transport=proxy mode=auto queues=1 depth=64
uml-vector2: configured 2 experimental v2 runtime device(s)
```

The kernel reached init and halted cleanly.

None of the manual boots reported:

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
```

Total: 48 passed, 0 failed.

## Next Required Phase

R2 must turn this skeleton into an inspectable Linux netdev without
moving packets yet.

Required R2 deliverables:

- `vector2_netdev.c`;
- `static const struct net_device_ops um_vec2_netdev_ops`;
- `alloc_etherdev_mqs()` with `queues=1` forced initially;
- v2 netdev name assignment that is stable and does not conflict with
  the legacy `vecN` path;
- registration from parsed v2 command-line specs;
- unregister cleanup;
- minimal read-only ethtool driver info;
- `ndo_open` and `ndo_stop` wired through the v2 lifecycle model;
- clean failure when no backend is implemented yet.

R2 exit gate:

```text
ip link show <v2dev>
ip link set <v2dev> up     # fails cleanly while backend is missing
ip link set <v2dev> down   # remains safe and repeatable
```

The driver remains non-replacement until later phases provide real fd
and TAP backends, RX/TX datapath, NAPI, ethtool stats, multiqueue,
sandbox enforcement evidence, compatibility, performance, and soak
results.
