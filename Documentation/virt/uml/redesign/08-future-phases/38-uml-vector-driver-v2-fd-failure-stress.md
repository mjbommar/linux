# UML vector driver v2 fd failure stress

**Status:** partial failure-injection evidence - KUnit only.
**Date:** 2026-05-17.

This note records a focused fd-backend KUnit checkpoint for vector2.
It strengthens the open/close and failure-unwind coverage around the
launcher-owned fd transport, but it does not close the full replacement
lifecycle gate.  The full gate still requires live `ip link up/down`
repetition with failure injection and leak checks.

## New Coverage

`arch/um/drivers/vector2_host_fd_test.c` now includes three additional
fd backend cases:

- `vector2_fd_netdev_open_stop_repeats_test`
  repeatedly opens and stops a vector2 netdev over an inherited pipe fd.
  Each iteration verifies the modeled lifecycle state, channel
  attachment, carrier state, close unwind, and open/close counters.
- `vector2_fd_netdev_bad_fd_unwinds_closed_test`
  opens through the real `ndo_open()` path with an invalid fd and
  expects `-EBADF`.  It verifies that the device returns to
  `REGISTERED`, leaves no channels, keeps carrier off, records one open
  failure, and allows a safe stop while already closed.
- `vector2_fd_missing_config_repeats_closed_test`
  calls the fd backend directly with no configured inherited fd for
  10,000 iterations.  Every iteration must return `-EINVAL` and leave
  the backend without attached channels.

The repeat counts are intentionally bounded for KUnit:

```text
successful netdev open/stop repeats: 1000
missing-config fd-open failures:     10000
```

The successful repeat uses the netdev lifecycle path because it checks
the integration with carrier state and stats.  The 10,000-failure case
uses the direct fd backend entry point to avoid producing thousands of
expected netdev error lines while still exercising the closed-state
invariant.

## Build

Object build:

```sh
make -j"$(nproc)" ARCH=um \
  O=/home/mjbommar/projects/personal/.build/um-vector-r1-kunit \
  arch/um/drivers/vector2_host_fd_test.o
```

Result:

```text
passed
```

## KUnit

Command:

```sh
make -j"$(nproc)" ARCH=um \
  O=/home/mjbommar/projects/personal/.build/um-vector-r1-kunit \
  linux

/home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt \
  > /tmp/um-vector2-fd-stress-kunit.log 2>&1
```

Result:

```text
# um_vector2_config: pass:12 fail:0 skip:0 total:12
# um_vector2_queue: pass:8 fail:0 skip:0 total:8
# um_vector2_transport: pass:8 fail:0 skip:0 total:8
# um_vector2_fake_host: pass:8 fail:0 skip:0 total:8
# um_vector2_model: pass:7 fail:0 skip:0 total:7
# um_vector2_cmdline: pass:5 fail:0 skip:0 total:5
# um_vector2_netdev: pass:6 fail:0 skip:0 total:6
# um_vector2_ethtool: pass:4 fail:0 skip:0 total:4
# um_vector2_host_fd: pass:11 fail:0 skip:0 total:11
# um_vector2_host_tap: pass:6 fail:0 skip:0 total:6
```

Total vector2 result:

```text
75/75 passed
```

The fd host suite included:

```text
ok 7 vector2_fd_netdev_open_stop_repeats_test
ok 8 vector2_fd_netdev_bad_fd_unwinds_closed_test
ok 9 vector2_fd_missing_config_repeats_closed_test
```

A scan of the captured KUnit log found no `not ok`, `FAILED`, `panic`,
`BUG`, `WARNING`, `KCSAN`, `data-race`, or lockdep signatures.

## What This Closes

This checkpoint improves the unit-level evidence for:

- fd backend open/close idempotence across repeated netdev start/stop;
- bad-fd unwind through the real netdev open path;
- missing fd config failure staying closed across many attempts;
- open-attempt, open-failure, and close counter behavior.

It reduces the risk of leaks and stale state in the fd attach path.
It also makes future fd lifecycle changes easier to review because the
expected closed-state invariants are now executable tests.

## What Remains Open

This checkpoint does not prove replacement readiness.  The remaining
fd lifecycle gate still needs:

- live failed-open injection, not only KUnit failure calls;
- a runtime way to make selected opens fail without changing the driver
  configuration;
- KCSAN or lockdep coverage while multiqueue traffic is active;
- integration into the same `umlctl gate loop` evidence path used by
  the sandbox and workload smokes.

The successful live 10,000-cycle `ip link down/up` repetition was
covered later by `39-uml-vector-driver-v2-lifecycle-stress-gate.md`.
