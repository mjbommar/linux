# UML vector driver v2 live failed-open injection

**Status:** runtime failed-open proof for vector2 fd handoff.
**Date:** 2026-05-17.

This note records the live failed-open injection checkpoint for vector2.
It complements the fd failure-stress KUnit coverage by driving the real
`ndo_open()` path from userspace with `ip link set up`, then validating
that the injected failure leaves the device closed, registered, and
accounted in ethtool stats.

The injection knob is intentionally explicit and opt-in:

```text
fail_open_after=N
```

When set, vector2 fails the Nth and later netdev open attempts.  The
default zero value is a no-op.  `umlctl` exposes the knob as
`[network].fail_open_after` for vector2-only test Umlfiles and renders it
into the `vec2.*` command-line argument.

## Implementation

Kernel side:

- `struct um_vec2_config` now carries `fail_open_after`;
- the vector2 config parser accepts `fail_open_after=<uint>`;
- `um_vec2_netdev_open()` increments `open_attempts` with an
  increment-and-return helper, then fails before entering `OPENING` when
  the configured threshold is reached;
- failures still flow through the existing `open_failures` accounting;
- KUnit covers parser acceptance and the closed-state injected failure.

`umlctl` side:

- `[network].fail_open_after = N` is accepted only with
  `network.driver = "vector2"` and `N >= 1`;
- generated vector2 fd/inproc kernel args append
  `,fail_open_after=N`;
- manifest labels and guest metadata export
  `umlctl.network.fail_open_after=N` and
  `UMLCTL_NETWORK_FAIL_OPEN_AFTER=N`;
- `tools/uml/uml-launcher/examples/vector2-failed-open.toml` is the
  operator-facing live gate.

## KUnit

Command:

```sh
make -j$(nproc) ARCH=um \
  O=/home/mjbommar/projects/personal/.build/um-vector-r1-kunit \
  arch/um/drivers/vector2_config_test.o \
  arch/um/drivers/vector2_host_fd_test.o \
  arch/um/drivers/vector2_netdev.o

make -j$(nproc) ARCH=um \
  O=/home/mjbommar/projects/personal/.build/um-vector-r1-kunit \
  linux

timeout 180s /home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt \
  >/tmp/um-vector2-failed-open-kunit.log 2>&1
```

Result:

```text
um_vector2_* KUnit: 76/76 passed
um_vector2_host_fd: 12/12 passed
vector2_fd_injected_open_failure_stays_closed_test: ok
```

The captured KUnit log was scanned for:

```text
not ok
FAILED
panic
BUG
WARNING
KCSAN
data-race
```

The scan returned no matches.

## Dry Run

Command:

```sh
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  tools/uml/uml-launcher/target/debug/umlctl up \
    -f tools/uml/uml-launcher/examples/vector2-failed-open.toml \
    --dry-run >/tmp/um-vector2-failed-open-dry-run.log
```

Relevant dry-run output:

```text
export UMLCTL_NETWORK_FAIL_OPEN_AFTER='2'
kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128,fail_open_after=2
vec2.0:transport=fd,mode=fd,fd=200,depth=128,fail_open_after=2
```

## Live Gate

Command:

```sh
rm -rf /tmp/um-vector2-failed-open
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  timeout 300s tools/uml/uml-launcher/target/debug/umlctl gate loop \
    -f tools/uml/uml-launcher/examples/vector2-failed-open.toml \
    -W 1 -M 1 --timeout 180 \
    --pass-marker VECTOR2_FAILED_OPEN_OK \
    --fail-marker 'VECTOR2_FAILED_OPEN_.*(FAIL|UNEXPECTED|MISSING|BAD)|VERIFY_FAIL|kernel BUG|Kernel panic|BUG:|WARNING:|KCSAN:|data-race' \
    --out /tmp/um-vector2-failed-open
```

Gate result:

```text
==> default PASS=1/1 FAIL=0 TIMEOUT=0 rate=100.0% (Wilson 95% CI [20.7%, 100.0%]) elapsed=5s
```

The workload used `fail_open_after = 2`, so the generated init script's
initial `ip link set vec2.0 up` succeeded.  The live phase then pinged
the host gateway, took the link down, and attempted a second
`ip link set vec2.0 up`.  That second open failed with `-EIO` through
the real netdev path:

```text
vec2.0: vector v2 injected open failure at attempt 2 threshold 2
RTNETLINK answers: Input/output error
VECTOR2_FAILED_OPEN_COUNTERS open_delta=1 fail_delta=1 close_delta=1 before_state=4 after_state=2 before_open=1 after_open=2 before_fail=0 after_fail=1 before_close=0 after_close=1
VECTOR2_FAILED_OPEN_OK
```

The post-failure ethtool state was:

```text
lifecycle_state: 2
open_attempts: 2
open_failures: 1
closes: 1
```

State `4` is `RUNNING`; state `2` is `REGISTERED`.  The post-failure
link was down, with no attached running datapath.

The copied run log was scanned for warning, BUG, panic, KCSAN,
data-race, failed-phase, and failed-open error markers.  The narrowed
runtime-log scan returned no matches.

Post-run cleanup:

```text
TAP_ABSENT
UML_PROCESS_ABSENT
```

The host checks found no lingering `v2failopen0` TAP and no running
`umlctl` instance.

## What This Closes

This closes the audit item that asked for live failed-open injection if
replacement approval required runtime proof beyond KUnit.  Vector2 now
has both unit-level and live netdev-entry failure evidence for the fd
handoff open path.

## What Remains Open

This is a failure-path proof, not a replacement approval by itself.  The
broader replacement gate still needs:

- the separate kvm-v2 baseline readiness blocker fixed;
- vector2 Tier 3 Django/FastAPI reruns on kvm-v2 after that fix;
- longer workload soaks;
- longer and more varied SMP/KCSAN fairness profiles;
- performance disposition for the measured guest-to-host regression;
- CI/preflight wiring for sandbox audit and workload gates;
- the legacy `vecN:` transition decision.
