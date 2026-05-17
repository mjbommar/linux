# UML vector driver v2 umlctl fd handoff

**Status:** R7/R5 follow-up - launcher-owned fd handoff.
**Date:** 2026-05-17.

This note records the first `umlctl` workflow where vector v2 can use
a host TAP device without the UML guest opening `/dev/net/tun` or
parsing a host TAP name.  The launcher owns the host authority, opens
the TAP fd, and inherits it into the UML process at a stable fd number.

This does not make vector v2 replacement-ready.  It closes the
operator-usability gap where fd mode worked only if a human manually
arranged an fd number.

## User Contract

For a normal single-queue vector2 TAP deployment:

```toml
[network]
mode = "tap"
driver = "vector2"
queues = 1
# host_mode defaults to "auto"
```

`host_mode = "auto"` now maps this to:

```text
vec2.0:transport=fd,mode=fd,fd=200,depth=128
```

`umlctl up` still creates and configures the host TAP with the usual
sudo setup steps.  After that, `umlctl start` opens the TAP as the
invoking user and passes the fd to UML as fd 200.  The generated
manifest records the fd handoff with `umlctl.network.*` labels so a
later `umlctl start <name>` can repeat the same handoff.

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

For current vector2 multiqueue TAP:

```sh
umlctl up -f workload.toml --network-driver vector2 --network-queues 2
```

`host_mode = "auto"` selects `transport=tap,mode=inproc` for this case
because fd multiqueue is not implemented yet.  An explicit
`--network-host-mode fd --network-queues 2` is rejected.

## Manifest Labels

`umlctl up` appends internal labels to the manifest:

```text
umlctl.network.mode=tap
umlctl.network.driver=vector2
umlctl.network.guest_dev=vec2.0
umlctl.network.tap_name=<tap>
umlctl.network.transport=fd
umlctl.network.host_mode=fd
umlctl.network.queues=1
umlctl.network.fd=200
```

The labels are deliberately explicit.  They make `umlctl start`
deterministic and give future audit tooling a simple source of truth
without reparsing free-form kernel command-line text.

## Dry-Run Shape

Single-queue vector2:

```text
== network plan ==
  mode=tap driver=vector2 guest_dev=vec2.0 host_tap=uml-tap0 transport=fd host_mode=fd queues=1
  kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128
  inherited_fd=tap:uml-tap0 -> fd:200
```

Current multiqueue vector2:

```text
== network plan ==
  mode=tap driver=vector2 guest_dev=vec2.0 host_tap=uml-tap0 transport=tap host_mode=inproc queues=2
  kernel_arg=vec2.0:transport=tap,mode=inproc,ifname=uml-tap0,depth=128,queues=2
```

## Security Boundary

The fd path is the preferred sandbox shape:

- host resource creation stays in `umlctl`;
- the UML guest receives only a delegated fd number;
- sandbox vector2 builds can accept `transport=fd,fd=<n>` while still
  rejecting `ifname=`, helper scripts, BPF paths, raw endpoints, VDE,
  and similar host-resource options;
- the launcher uses `dup2()` in the child pre-exec path and clears
  `FD_CLOEXEC` only on the target inherited fd.

The in-process TAP path remains useful for comparison and current
multiqueue work, but it is not the desired sandbox endpoint.

## Validation Evidence

Rust unit and integration tests:

```text
cargo test --manifest-path tools/uml/uml-launcher/Cargo.toml
```

Result:

```text
uml-launcher unit tests: 68 passed
umlctl unit tests: 72 passed
apparmor_profile: 3 passed
frontend_handshake: 1 passed
selinux_module: 2 passed
```

Manual dry-run checks:

```sh
cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
  --bin umlctl -- up -f tools/uml/uml-launcher/examples/fastapi.toml \
  --network-driver vector2 --dry-run
```

confirmed:

```text
transport=fd host_mode=fd queues=1
kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128
inherited_fd=tap:uml-tap0 -> fd:200
```

and:

```sh
cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
  --bin umlctl -- up -f tools/uml/uml-launcher/examples/fastapi.toml \
  --network-driver vector2 --network-queues 2 --dry-run
```

confirmed:

```text
transport=tap host_mode=inproc queues=2
kernel_arg=vec2.0:transport=tap,mode=inproc,ifname=uml-tap0,depth=128,queues=2
```

The explicit unsupported combination:

```sh
--network-driver vector2 --network-queues 2 --network-host-mode fd
```

is rejected with:

```text
network.host_mode = 'fd' currently requires network.queues = 1
```

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
  mode=tap driver=vector2 guest_dev=vec2.0 host_tap=v2fd0 transport=fd host_mode=fd queues=1
  kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128
  inherited_fd=tap:v2fd0 -> fd:200
```

The live run reached the guest marker:

```text
[umlctl] network: driver=vector2 guest_dev=vec2.0 tap=v2fd0 transport=fd host_mode=fd queues=1
[umlctl] network-fd: open tap=v2fd0 and inherit as fd=200
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

## Remaining Work

- fd multiqueue;
- fd-specific teardown/audit assertions in gate output;
- kvm-v2 validation after the separate kvm-v2 readiness blocker is
  fixed;
- performance comparison between legacy vector, vector2 fd, and
  vector2 in-process TAP.
