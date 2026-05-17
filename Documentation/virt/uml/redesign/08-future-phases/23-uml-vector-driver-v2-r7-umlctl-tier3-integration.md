# UML vector driver v2 R7 umlctl and Tier 3 integration

**Status:** IMPLEMENTED R7 checkpoint - operator integration and
Tier 3 selection path.
**Date:** 2026-05-17.

This memo records the seventh runtime buildout checkpoint for vector
networking v2.  R7 makes vector v2 easy to select from `umlctl` and
from the Phase J soak daemon without changing the default production
path.  It also hardens teardown after failed starts, which matters for
TAP-backed Tier 3 loops.

The old `CONFIG_UML_NET_VECTOR` driver remains the production vector
networking path.  Vector v2 remains experimental.

## What Landed

R7 adds a user-facing selection layer around the experimental v2
driver:

- `[network] driver = "vector"` remains the default and maps to guest
  device `vec0`;
- `[network] driver = "vector2"` maps to guest device `vec2.0`;
- `umlctl up --network-driver vector2` overrides the Umlfile for quick
  one-off comparison runs;
- `umlctl gate loop --network-driver vector2` applies the same override
  to every generated worker Umlfile;
- `umlctl gate loop --sweep network.driver=vector,vector2` runs a
  side-by-side comparison without editing the input TOML;
- `umlctl up --dry-run` now prints a network plan showing the selected
  driver, guest device, host TAP, transport, host mode, queue count, and
  exact kernel command-line argument;
- the soak daemon has `tier3-django-v2` and `tier3-fastapi-v2` aliases
  that reuse the existing Tier 3 templates with
  `network.driver = "vector2"`;
- soak scoreboard rows record v2 metadata:
  `uml_network_driver`, `uml_netdev_name`, `uml_vmm_backend`,
  `uml_transport`, `uml_queue_count`, and `uml_host_mode`;
- the Tier 3 templates no longer require `nc`; socket readiness is
  checked with Python stdlib.

The resulting operator contract is intentionally simple:

```toml
[network]
mode = "tap"
driver = "vector2"
tap_name = "soak-tap0"
```

`umlctl` compiles that to:

```text
guest device: vec2.0
kernel arg:   vec2.0:transport=tap,mode=inproc,ifname=soak-tap0,depth=128
```

## Teardown Hardening

Tier 3 loops uncovered two lifecycle problems outside the v2 packet
path:

1. `umlctl gate loop` used `stop` plus `rm` after each iteration.
   That removed the manifest but skipped Umlfile host teardown, leaving
   TAP devices behind.
2. `umlctl down --force` called the command wrapper for `stop`.  The
   wrapper exits the process on "not running", so host-side teardown did
   not run after an early start failure or ready-timeout.

R7 changes the loop and down paths:

- `gate loop` now uses `umlctl down -f <worker.toml> --force --rm`
  before an iteration, after early `up` failure, and after saved log
  capture;
- `down --force` calls the supervisor directly, treats missing/not
  running instances as non-fatal, and still runs TAP/iptables teardown;
- `supervise::start()` waits for the child after sending SIGKILL on
  ready-timeout before returning.

The KVM-v2 Tier 3 smoke still fails readiness, but the failure no
longer leaves `soak-tap0` behind.

## User Workflows

One-off dry-run for v2:

```sh
tools/uml/uml-launcher/target/debug/umlctl up \
  -f tier3-django.toml \
  --network-driver vector2 \
  --dry-run
```

Expected network-plan shape:

```text
== network plan ==
  mode=tap driver=vector2 guest_dev=vec2.0 host_tap=soak-tap0 transport=tap host_mode=inproc queues=1
  kernel_arg=vec2.0:transport=tap,mode=inproc,ifname=soak-tap0,depth=128
```

Single-driver loop:

```sh
tools/uml/uml-launcher/target/debug/umlctl gate loop \
  -f tier3-django.toml \
  --network-driver vector2 \
  -W 1 -M 30 --timeout 180 \
  --out /tmp/uml-vector2-tier3-seccomp
```

Side-by-side comparison:

```sh
tools/uml/uml-launcher/target/debug/umlctl gate loop \
  -f tier3-django.toml \
  --sweep network.driver=vector,vector2 \
  -W 1 -M 30 --timeout 180 \
  --out /tmp/uml-vector-tier3-compare
```

Soak daemon aliases:

```sh
UML_KERNEL=/path/to/linux \
tools/testing/selftests/um/soak/run-soak-daemon.sh \
  --workloads tier3-django-v2,tier3-fastapi-v2 \
  --workers 1 \
  --iters-per-rotation 1 \
  --budget-sec 7200 \
  --out /tmp/uml-vector2-tier3-soak
```

## Validation Evidence

Rust formatting and unit/integration tests:

```text
cargo fmt --check
cargo test

src/main.rs:                 68 passed
src/bin/umlctl/main.rs:      64 passed
tests/apparmor_profile.rs:    3 passed
tests/frontend_handshake.rs:  1 passed
tests/selinux_module.rs:      2 passed
```

Shell and whitespace checks:

```text
bash -n tools/testing/selftests/um/soak/run-soak-daemon.sh
git diff --check
```

KUnit was run directly and parsed through `kunit.py` after `ply` was
installed:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt

python3 tools/testing/kunit/kunit.py parse /tmp/um-vector-r7-kunit.log
```

Parsed result:

```text
Testing complete. Ran 64 tests: passed: 64
```

Dry-run override evidence:

```text
mode=tap driver=vector2 guest_dev=vec2.0 host_tap=soak-tap0 transport=tap host_mode=inproc queues=1
kernel_arg=vec2.0:transport=tap,mode=inproc,ifname=soak-tap0,depth=128
backend=force=seccomp
```

Live seccomp sweep smoke:

```text
umlctl gate loop \
  -f /tmp/um-vector-r7-dry/_tier3-django-v2-seccomp-w0.toml \
  --sweep network.driver=vector2 \
  -W 1 -M 1 --timeout 120 \
  --out /tmp/um-vector-r7-sweep-smoke

network.driver=vector2 PASS=1/1 FAIL=0 TIMEOUT=0
```

The saved run log showed:

```text
Kernel command line: ... vec2.0:transport=tap,mode=inproc,ifname=soak-tap0,depth=128 ...
uml-vector2: registered netdev vec2.0 for vec2.0
SERVER_READY
GUEST_CURL ok=100 fail=0
TIER3_OK
REPRO_DONE rc=0
```

Two-iteration seccomp cleanup smoke:

```text
PASS=2/2 FAIL=0 TIMEOUT=0
TAP_ABSENT
```

KVM-v2 cleanup smoke:

```text
PASS=0/1 FAIL=1 TIMEOUT=0
umlctl: instance 'soak-tier3-django-kvm-v2-w0-w0' did not become ready within 60s
TAP_ABSENT
```

The KVM-v2 failure remains a readiness issue to debug.  R7 only claims
that the v2 selection and teardown paths are now operator-safe enough
to keep iterating.

## Remaining Replacement Blockers

R7 does not claim:

- 30/30 Tier 3 Django success on both seccomp and kvm-v2;
- FastAPI/uvicorn live validation;
- 2h or 24h soak success;
- host-to-guest TCP battery outside the in-guest HTTP curl loop;
- fd datapath packet movement;
- sandbox-helper networking;
- multiqueue;
- performance parity with the legacy vector driver.

Those remain replacement gates.  R7 makes them easier to run and safer
to repeat.
