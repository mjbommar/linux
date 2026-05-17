# uml-vector-driver-v2 umlctl usability follow-up

**Status:** R7 follow-up - operator convenience and diagnostics.
**Date:** 2026-05-17.

This note records the user-facing contract between `umlctl` and the
experimental vector v2 driver.  The goal is not to make v2 the default
driver yet.  The goal is to make v2 easy to select, easy to compare
with legacy vector, and easy to debug when a backend or workload fails
before readiness.

## User Contract

The stable selection surface is:

```toml
[network]
mode = "tap"
driver = "vector2"
queues = 1       # or "auto" to match [runtime].ncpus
# host_mode defaults to "auto"
```

For vector2 TAP, `host_mode = "auto"` selects the launcher-owned fd
path.  Single queue produces:

```text
vec2.0:transport=fd,mode=fd,fd=200,depth=128
```

Multiqueue produces a contiguous inherited fd range:

```text
vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4
```

For SMP experiments, `queues = "auto"` is the convenient shape.  It
resolves to `[runtime].ncpus` in `umlctl`, then emits the same numeric
driver command line as a fixed queue count.  The CLI and gate-loop
override accept the same value:

```sh
umlctl up -f tier3-django.toml --network-driver vector2 --network-queues auto --dry-run
umlctl gate loop -f tier3-django.toml --network-driver vector2 --network-queues auto -W 1 -M 10
umlctl gate loop -f tier3-django.toml --network-driver vector2 --sweep network.queues=1,auto -W 1 -M 10
```

`umlctl` opens the host TAP after setup and inherits one fd per queue
into the UML process.  The sandboxed guest receives only delegated fd
numbers.

For one-off comparison runs, the CLI override is preferred:

```sh
umlctl up -f tier3-django.toml --network-driver vector2 --dry-run
umlctl gate loop -f tier3-django.toml --sweep network.driver=vector,vector2 -W 1 -M 30
```

For a focused fd-handoff smoke without a larger workload, use:

```sh
UML_KERNEL=/path/to/uml/linux \
  umlctl up -f tools/uml/uml-launcher/examples/vector2-fd-handoff.toml \
    --wait-for VECTOR2_FD_HANDOFF_OK
UML_KERNEL=/path/to/uml/linux \
  umlctl up -f tools/uml/uml-launcher/examples/vector2-fd-multiqueue.toml \
    --wait-for VECTOR2_FD_MULTIQUEUE_OK
```

For multiqueue TAP experiments:

```sh
umlctl up -f tier3-django.toml --network-driver vector2 --network-queues 2 --dry-run
umlctl gate loop -f tier3-django.toml --network-driver vector2 --network-queues 2 -W 1 -M 1
umlctl gate loop -f tier3-django.toml --network-driver vector2 --sweep network.queues=1,2 -W 1 -M 10
```

For sandbox audits, use the built-in strace path instead of hand-running
strace and regexes:

```sh
umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-auto-queues.toml \
  --network-driver vector2 --network-queues auto \
  --pass-marker VECTOR2_AUTO_QUEUES_OK \
  --audit-vector-sandbox \
  -W 1 -M 1 --timeout 180
```

`--audit-vector-sandbox` implies `--strace`.  Each iteration preserves
`strace-N.log` and `strace-audit-N.log` next to the copied `run-N.log`.
The audit fails the iteration if the vector host trace contains an
actual `/dev/net/tun` open, `TUNSETIFF`, `AF_PACKET`, `bpf()`, or UML
network-helper exec.  It intentionally ignores ELF string-buffer hits
and guest `AF_NETLINK` activity.

Current vector2 multiqueue uses the launcher-owned fd path by default.
Force the trusted in-process path explicitly only when comparing host
backend behavior:

```sh
umlctl up -f tier3-django.toml --network-driver vector2 --network-host-mode inproc --dry-run
```

The override and sweep write into the generated Umlfile's `[network]`
configuration rather than into the guest environment.  That keeps the
driver choice in one place and avoids hidden shell-variable behavior.

## Guest Metadata

Generated init scripts now export a small reserved metadata block before
init phases run:

```sh
UMLCTL_NETWORK_MODE
UMLCTL_NETWORK_DRIVER
UMLCTL_NETDEV
UMLCTL_TAP_NAME
UMLCTL_HOST_IP
UMLCTL_GUEST_IP
UMLCTL_GATEWAY
UMLCTL_NETWORK_TRANSPORT
UMLCTL_NETWORK_HOST_MODE
UMLCTL_NETWORK_QUEUE_SPEC
UMLCTL_NETWORK_QUEUES
UMLCTL_NETWORK_FD
UMLCTL_NETWORK_FD_COUNT
```

For legacy TAP this reports `UMLCTL_NETWORK_DRIVER=vector` and
`UMLCTL_NETDEV=vec0`.  For vector v2 TAP it reports
`UMLCTL_NETWORK_DRIVER=vector2` and `UMLCTL_NETDEV=vec2.0`.  For
`network.mode = "none"` it reports `UMLCTL_NETWORK_DRIVER=none`, an
empty netdev, and zero queues.

Workload phases should use these variables when they need to inspect or
wait on the selected interface:

```toml
[[init.phases]]
name = "link-state"
cmd = "ip -d link show \"$UMLCTL_NETDEV\" || true"
expect = ""
timeout_secs = 5
```

This lets a single workload TOML run on legacy vector and v2 without
hard-coding `vec0` or `vec2.0`.

## Dry-Run Network Plan

`umlctl up --dry-run` remains the first-line inspection command.  It
prints:

- selected driver;
- guest netdev name;
- host TAP name;
- transport;
- host mode;
- resolved queue count and original queue spec;
- exact UML kernel command-line argument;
- inherited fd or fd-range mapping when vector2 fd handoff is selected.

That output is the quickest way to verify that a run is actually using
v2 before spending time on a soak.

## Ready-Timeout Diagnostics

Ready-timeout failures now carry run correlation data:

```text
umlctl: instance 'NAME' did not become ready within 180s pid=PID run_id=RUN init_log=PATH
```

`umlctl gate loop` preserves this data in each worker iteration.  If
`umlctl up` fails before printing the normal `started ... run_id=...`
line, the loop writes `run-N.log` with:

- the complete `umlctl up` stdout/stderr;
- the failed run bundle's `init.log`, when the timeout surfaced one.

This matters for vector v2 because kvm-v2 readiness failures can happen
before the guest reaches normal printk output.  The failed bundle path
is the only reliable way to distinguish "driver did not register" from
"backend never reached Linux boot".

## Current Practical Guidance

Use this sequence when checking a vector v2 workload:

```sh
umlctl up -f workload.toml --network-driver vector2 --dry-run
umlctl up -f workload.toml --network-driver vector2 --network-queues auto --dry-run
umlctl gate loop -f workload.toml --network-driver vector2 -W 1 -M 1 --timeout 120
umlctl gate loop -f workload.toml --sweep network.driver=vector,vector2 -W 1 -M 30 --timeout 180
```

If the one-shot gate fails, inspect the worker directory first:

```sh
sed -n '1,220p' /tmp/umlctl-loop-*/p0_default/w0/up-1.log
sed -n '1,260p' /tmp/umlctl-loop-*/p0_default/w0/run-1.log
sed -n '1,120p' /tmp/umlctl-loop-*/p0_default/w0/strace-audit-1.log
```

Only move to a longer soak after the dry-run plan and the one-shot
gate show the expected driver and netdev.

## Validation Evidence

This follow-up was checked with:

- `cargo fmt --check`;
- `cargo test` in `tools/uml/uml-launcher`;
- `umlctl up --dry-run --network-driver vector2`, confirming
  `UMLCTL_NETWORK_DRIVER=vector2`, `UMLCTL_NETDEV=vec2.0`, the v2
  network plan, and the
  `vec2.0:transport=fd,mode=fd,fd=200,depth=128` kernel argument;
- `tools/uml/uml-launcher/examples/vector2-fd-handoff.toml` dry-run
  coverage for the dedicated fd-handoff smoke path;
- `tools/uml/uml-launcher/examples/vector2-auto-queues.toml` dry-run
  coverage for `queues = "auto"`, including resolved `queues=4`,
  `queue_spec=auto`, and inherited fds `200..203`;
- a synthetic sleeping-kernel ready-timeout, confirming `umlctl up`
  prints `pid`, `run_id`, and `init_log`;
- a synthetic `umlctl gate loop` ready-timeout, confirming `run-1.log`
  captures both the up output and failed bundle `init.log`;
- a live seccomp vector2 Tier 3 Django one-shot:
  `network.driver=vector2 PASS=1/1 FAIL=0 TIMEOUT=0`, with
  `uml-vector2: registered netdev vec2.0`, `SERVER_READY`, `TIER3_OK`,
  `REPRO_DONE rc=0`, and no lingering `soak-tap0`.
- a live vector2 auto-queue sandbox audit gate:
  `PASS=1/1 FAIL=0 TIMEOUT=0`, with `strace-1.log`,
  `strace-audit-1.log`, no lingering `v2autoq0`, and no traced vector
  host TAP open, `TUNSETIFF`, `AF_PACKET`, `bpf()`, or UML helper exec.

## Replacement Boundary

These usability changes do not close the vector v2 replacement gates.
They make the gates easier to run and easier to debug.  The production
path remains legacy `vecN:` until the buildout plan's seccomp, kvm-v2,
fd datapath, multiqueue, sandbox, and performance gates pass.
