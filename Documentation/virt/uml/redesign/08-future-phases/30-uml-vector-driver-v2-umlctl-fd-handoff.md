# UML vector driver v2 umlctl fd handoff

**Status:** R7/R5 follow-up - launcher-owned fd handoff, including fd
multiqueue launch.
**Date:** 2026-05-17.

This note records the first `umlctl` workflow where vector v2 can use
a host TAP device without the UML guest opening `/dev/net/tun` or
parsing a host TAP name.  The launcher owns the host authority, opens
the TAP fd or fd range, and inherits it into the UML process at stable
fd numbers.

This does not make vector v2 replacement-ready.  It closes the
operator-usability gap where fd mode worked only if a human manually
arranged an fd number.

## User Contract

For a normal vector2 TAP deployment:

```toml
[network]
mode = "tap"
driver = "vector2"
queues = 4
# host_mode defaults to "auto"
```

For a normal vector2 SMP deployment, the preferred convenience form is:

```toml
[runtime]
ncpus = 4

[network]
mode = "tap"
driver = "vector2"
queues = "auto"
# host_mode defaults to "auto"
```

`umlctl` resolves `auto` to the requested vCPU count before launch.  The
vector2 driver still receives an explicit numeric `queues=N`.

`host_mode = "auto"` now maps this to:

```text
vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4
```

`umlctl up` still creates and configures the host TAP with the usual
sudo setup steps.  For `queues = N`, the TAP is created with
`multi_queue`, `umlctl start` opens one TAP fd per queue as the
invoking user, and the child pre-exec path maps those fds into the UML
process as the contiguous range `200..200+N-1`.  The generated manifest
records the fd handoff with `umlctl.network.*` labels so a later
`umlctl start <name>` can repeat the same handoff.

For single-queue use, the same contract produces the shorter command
line without `queues=`:

```text
vec2.0:transport=fd,mode=fd,fd=200,depth=128
```

For compatibility or performance comparison against the trusted
in-process TAP path:

```toml
[network]
mode = "tap"
driver = "vector2"
host_mode = "inproc"
```

or:

```sh
umlctl up -f workload.toml --network-driver vector2 --network-host-mode inproc
```

For vector2 fd multiqueue:

```sh
umlctl up -f workload.toml --network-driver vector2 --network-queues 2
```

`host_mode = "auto"` selects `transport=fd,mode=fd` for this case.
An explicit `--network-host-mode fd --network-queues 2` is accepted.

## Manifest Labels

`umlctl up` appends internal labels to the manifest:

```text
umlctl.network.mode=tap
umlctl.network.driver=vector2
umlctl.network.guest_dev=vec2.0
umlctl.network.tap_name=<tap>
umlctl.network.transport=fd
umlctl.network.host_mode=fd
umlctl.network.queues=<N>
umlctl.network.queue_spec=<N-or-auto>
umlctl.network.fd=200
umlctl.network.fd_count=<N>
```

The labels are deliberately explicit.  They make `umlctl start`
deterministic and give future audit tooling a simple source of truth
without reparsing free-form kernel command-line text.

## Dry-Run Shape

Single-queue vector2:

```text
== network plan ==
  mode=tap driver=vector2 guest_dev=vec2.0 host_tap=uml-tap0 transport=fd host_mode=fd queues=1 queue_spec=1
  kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128
  inherited_fds=tap:uml-tap0 -> fd=200
```

Multiqueue vector2:

```text
== network plan ==
  mode=tap driver=vector2 guest_dev=vec2.0 host_tap=uml-tap0 transport=fd host_mode=fd queues=4 queue_spec=4
  kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4
  inherited_fds=tap:uml-tap0 -> fds=200..203
```

Automatic vector2 queue sizing:

```text
== network plan ==
  mode=tap driver=vector2 guest_dev=vec2.0 host_tap=v2autoq0 transport=fd host_mode=fd queues=4 queue_spec=auto
  kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4
  inherited_fds=tap:v2autoq0 -> fds=200..203
```

Trusted in-process comparison remains available:

```text
== network plan ==
  mode=tap driver=vector2 guest_dev=vec2.0 host_tap=uml-tap0 transport=tap host_mode=inproc queues=4 queue_spec=4
  kernel_arg=vec2.0:transport=tap,mode=inproc,ifname=uml-tap0,depth=128,queues=4
```

## Security Boundary

The fd path is the preferred sandbox shape:

- host resource creation stays in `umlctl`;
- the UML guest receives only a delegated fd number;
- sandbox vector2 builds can accept `transport=fd,fd=<n>` while still
  rejecting `ifname=`, helper scripts, BPF paths, raw endpoints, VDE,
  and similar host-resource options;
- the launcher uses `dup2()` in the child pre-exec path and clears
  `FD_CLOEXEC` only on the target inherited fd;
- before pre-exec, the launcher moves any source TAP fd that happens
  to overlap the target inherited range above that range, so `dup2()`
  cannot clobber a later source fd during multiqueue handoff.

The in-process TAP path remains useful for trusted comparison and
debugging, but it is not the desired sandbox endpoint.

## Validation Evidence

Rust unit and integration tests:

```text
cargo test --manifest-path tools/uml/uml-launcher/Cargo.toml
```

Result:

```text
uml-launcher unit tests: 68 passed
umlctl unit tests: 77 passed
apparmor_profile: 3 passed
frontend_handshake: 1 passed
selinux_module: 2 passed
```

Manual dry-run checks:

```sh
cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
  --bin umlctl -- up -f tools/uml/uml-launcher/examples/fastapi.toml \
  --network-driver vector2 --network-queues 4 --dry-run
```

confirmed:

```text
transport=fd host_mode=fd queues=4 queue_spec=4
kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4
inherited_fds=tap:uml-tap0 -> fds=200..203
```

and:

```sh
cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
  --bin umlctl -- up -f tools/uml/uml-launcher/examples/fastapi.toml \
  --network-driver vector2 --network-queues 4 --network-host-mode inproc --dry-run
```

confirmed trusted in-process comparison remains explicit:

```text
transport=tap host_mode=inproc queues=4 queue_spec=4
kernel_arg=vec2.0:transport=tap,mode=inproc,ifname=uml-tap0,depth=128,queues=4
```

The explicit fd multiqueue combination now resolves to
`transport=fd,mode=fd,fd=200,queues=N`; it is no longer rejected.
The automatic queue sizing path resolves `queues = "auto"` from
`[runtime].ncpus` before rendering the same fd multiqueue shape.

Live `umlctl up` fd-handoff smoke:

```text
kernel: /home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux
config: CONFIG_UML_NET_VECTOR_V2=y
config: # CONFIG_UML_NET_VECTOR_V2_INPROC is not set
config: CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
tap: v2fd0
```

The dry-run network plan for that Umlfile was:

```text
== network plan ==
  mode=tap driver=vector2 guest_dev=vec2.0 host_tap=v2fd0 transport=fd host_mode=fd queues=1 queue_spec=1
  kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128
  inherited_fds=tap:v2fd0 -> fd=200
```

The live run reached the guest marker:

```text
[umlctl] network: driver=vector2 guest_dev=vec2.0 tap=v2fd0 transport=fd host_mode=fd queues=1 queue_spec=1
[umlctl] network-fd: open tap=v2fd0 and inherit fd=200
started v2fdhandoff pid=3485995 run_id=01KRTWVEZH98GMK0YSCCB43T3H
wait-for matched /FD_HANDOFF_OK/
```

Guest evidence from the preserved `init.log`:

```text
Kernel command line: ... vec2.0:transport=fd,mode=fd,fd=200,depth=128 ...
uml-vector2: registered netdev vec2.0 for vec2.0
uml-vector2: vec2.0 configured transport=fd mode=fd requested_queues=1 runtime_queues=1 depth=128
vec2.0: <BROADCAST,MULTICAST,UP,LOWER_UP> ... numtxqueues 1
3 packets transmitted, 3 received, 0% packet loss
queue0_rx_batch_prepared_total: 2816
queue0_rx_batch_received_total: 24
queue0_rx_batch_consumed_total: 24
queue0_rx_batch_released_total: 2792
FD_HANDOFF_OK
```

Teardown evidence:

```text
fd_handoff_down2_rc=0
tap_after2_rc=1
Device "v2fd0" does not exist.
```

Repeated gate-loop smoke after fd preflight diagnostics:

```text
umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-fd-handoff.toml \
  -W 1 -M 3 --timeout 120 --pass-marker VECTOR2_FD_HANDOFF_OK
[umlctl gate loop] w0 iter1: Pass
[umlctl gate loop] w0 iter2: Pass
[umlctl gate loop] w0 iter3: Pass
==> default PASS=3/3 FAIL=0 TIMEOUT=0 rate=100.0%
```

The post-loop TAP check confirmed teardown:

```text
fd_diag_gate_tap_after_rc=1
Device "v2fd0" does not exist.
```

`umlctl gate loop` now also audits cleanup for TAP-backed Umlfiles:
after each `down --force --rm`, the loop checks `/sys/class/net` for
the generated TAP name.  A leaked TAP marks that iteration as FAIL and
writes `cleanup-<phase>-<iter>.log` into the worker output directory.
The focused fd-handoff loop above produced no cleanup failure logs.

Live `umlctl up` fd-multiqueue smoke:

```text
kernel: /home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux
version: 7.0.0-g15eb64ff158a-dirty
tap: v2fdmq0
```

The dry-run network plan for that Umlfile was:

```text
== network plan ==
  mode=tap driver=vector2 guest_dev=vec2.0 host_tap=v2fdmq0 transport=fd host_mode=fd queues=4 queue_spec=4
  kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4
  inherited_fds=tap:v2fdmq0 -> fds=200..203
```

The live run reached the guest marker:

```text
[umlctl] network: driver=vector2 guest_dev=vec2.0 tap=v2fdmq0 transport=fd host_mode=fd queues=4 queue_spec=4
[umlctl] network-fd: open tap=v2fdmq0 and inherit fds=200..203
started vector2-fd-multiqueue pid=3563521 run_id=01KRTYJRQBXHFTPRFHPMT702WH
[umlctl] wait-for matched /VECTOR2_FD_MULTIQUEUE_OK/
```

Guest evidence from the preserved `init.log`:

```text
Kernel command line: ... vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4 ...
uml-vector2: vec2.0 configured transport=fd mode=fd requested_queues=4 runtime_queues=4 depth=128
UMLCTL_NETWORK_FD=200
UMLCTL_NETWORK_FD_COUNT=4
UMLCTL_NETWORK_QUEUE_SPEC=4
UMLCTL_NETWORK_QUEUES=4
UMLCTL_NETWORK_TRANSPORT=fd
numtxqueues 4
3 packets transmitted, 3 received, 0% packet loss
VECTOR2_FD_MULTIQUEUE_OK
```

The ethtool snapshot showed per-queue counters for queues 0 through 3,
with RX movement on multiple queues and TX movement on selected queues
as the host stack hashed the short ping flow.

Repeated gate-loop fd-multiqueue smoke:

```text
umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-fd-multiqueue.toml \
  -W 1 -M 3 --timeout 180 --pass-marker VECTOR2_FD_MULTIQUEUE_OK
[umlctl gate loop] w0 iter1: Pass
[umlctl gate loop] w0 iter2: Pass
[umlctl gate loop] w0 iter3: Pass
==> default PASS=3/3 FAIL=0 TIMEOUT=0 rate=100.0%
TAP_ABSENT
```

No `cleanup-*.log` files were produced by that loop.

KUnit after this launcher integration:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-kunit/linux \
  mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt
kunit.py parse /tmp/um-vector-fd-mq-umlctl-kunit.log
Testing complete. Ran 71 tests: passed: 71
```

## Remaining Work

- kvm-v2 validation after the separate kvm-v2 readiness blocker is
  fixed;
- KCSAN and longer SMP traffic runs on fd multiqueue;
- queue-to-CPU policy validation under longer SMP traffic and fairness
  profiling;
- performance comparison between legacy vector, vector2 fd, and
  vector2 in-process TAP.
