# UML vector driver v2 lifecycle stress gate

**Status:** partial runtime lifecycle evidence - harness and smoke.
**Date:** 2026-05-17.

This note records a reusable `umlctl gate loop` workload for live
vector2 `ip link up/down` lifecycle testing.  It is the runtime
counterpart to the fd backend KUnit failure-stress tests.

The full replacement gate remains open until the same harness passes a
10,000-cycle live run with leak checks.

## Harness

New Umlfile:

```text
tools/uml/uml-launcher/examples/vector2-lifecycle-stress.toml
```

The workload uses the normal vector2 fd-handoff path:

```text
driver:      vector2
transport:   fd
host mode:   fd
queues:      1
guest dev:   vec2.0
host TAP:    v2life0
inherited fd: 200
```

The guest phase:

- reads `open_attempts` and `closes` from `ethtool -S`;
- repeats `ip link set "$UMLCTL_NETDEV" down` and
  `ip link set "$UMLCTL_NETDEV" up`;
- checks that open and close counter deltas are at least the requested
  cycle count;
- pings the gateway after the lifecycle loop;
- prints `VECTOR2_LIFECYCLE_STRESS_OK` only after the counter and ping
  checks pass.

The default cycle count is 10,000:

```text
UML_VECTOR2_LIFECYCLE_CYCLES=10000
```

Shorter runs can be requested through `umlctl gate loop --sweep`, which
injects the cycle count into the generated worker Umlfile:

```sh
UML_KERNEL=/path/to/uml/linux \
  umlctl gate loop \
    -f tools/uml/uml-launcher/examples/vector2-lifecycle-stress.toml \
    -W 1 -M 1 --timeout 180 \
    --sweep UML_VECTOR2_LIFECYCLE_CYCLES=25 \
    --pass-marker VECTOR2_LIFECYCLE_STRESS_OK
```

## Dry-Run Validation

Command:

```sh
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  tools/uml/uml-launcher/target/debug/umlctl up \
    -f tools/uml/uml-launcher/examples/vector2-lifecycle-stress.toml \
    --dry-run
```

The rendered network plan included:

```text
mode=tap driver=vector2 guest_dev=vec2.0 host_tap=v2life0 transport=fd host_mode=fd queues=1 queue_spec=1
kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128
inherited_fds=tap:v2life0 -> fd=200
```

The generated init script exported:

```text
UML_VECTOR2_LIFECYCLE_CYCLES=10000
UMLCTL_NETDEV=vec2.0
UMLCTL_NETWORK_FD=200
UMLCTL_NETWORK_FD_COUNT=1
```

## Smoke Evidence

Command:

```sh
rm -rf /tmp/um-vector-lifecycle-smoke
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  timeout 300s tools/uml/uml-launcher/target/debug/umlctl gate loop \
    -f tools/uml/uml-launcher/examples/vector2-lifecycle-stress.toml \
    -W 1 -M 1 --timeout 180 \
    --sweep UML_VECTOR2_LIFECYCLE_CYCLES=25 \
    --pass-marker VECTOR2_LIFECYCLE_STRESS_OK \
    --fail-marker 'VECTOR2_LIFECYCLE_.*(FAIL|BAD|MISSING|MISMATCH|NO_ETHTOOL)|VERIFY_FAIL|kernel BUG|Kernel panic' \
    --out /tmp/um-vector-lifecycle-smoke
```

Result:

```text
==> UML_VECTOR2_LIFECYCLE_CYCLES=25 PASS=1/1 FAIL=0 TIMEOUT=0
```

The saved run log included:

```text
VECTOR2_LIFECYCLE_PROGRESS cycles=25
VECTOR2_LIFECYCLE_COUNTERS cycles=25 open_delta=25 close_delta=25 before_open=1 after_open=26 before_close=0 after_close=25
open_attempts: 26
closes: 25
VECTOR2_LIFECYCLE_STRESS_OK
```

Host cleanup checks after the run:

```text
TAP_ABSENT
UML_PROCESS_ABSENT
```

## What This Closes

This checkpoint closes the missing harness gap for live vector2
lifecycle repetition.  Developers now have a checked `umlctl` workload
that exercises the real netdev open/stop path and verifies runtime
ethtool counters.

## What Remains Open

This checkpoint does not close the replacement lifecycle gate.  The
remaining work is:

- run the default 10,000-cycle workload to completion;
- preserve the run log and cleanup evidence;
- decide whether to add a fault-injection knob for live failed opens,
  since the current runtime path can repeat open/stop but cannot
  intentionally make selected opens fail without changing the driver
  configuration.
