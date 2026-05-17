# UML vector driver v2 umlctl auto queues

**Status:** R8 follow-up - operator convenience for SMP queue sizing.
**Date:** 2026-05-17.

This note records the `umlctl` side of making vector v2 easy to use for
SMP experiments.  The kernel driver still receives an explicit numeric
`queues=N` command-line field.  The convenience lives in `umlctl`: users
can request automatic queue sizing once, and `umlctl` resolves it before
launch.

## User Contract

For vector2 TAP, the recommended SMP shape is:

```toml
[runtime]
ncpus = 4

[network]
mode = "tap"
driver = "vector2"
host_mode = "auto"
queues = "auto"
```

`queues = "auto"` resolves to `[runtime].ncpus`, subject to the vector2
queue limit of 1024.  The generated driver argument is numeric:

```text
vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4
```

Single-vCPU auto queueing is also valid.  It records the user intent as
`queue_spec=auto`, but omits `queues=1` from the driver argument to keep
the command line equivalent to the default single-queue form:

```text
vec2.0:transport=fd,mode=fd,fd=200,depth=128
```

The CLI override accepts the same value:

```sh
umlctl up -f workload.toml --network-driver vector2 --network-queues auto --dry-run
umlctl gate loop -f workload.toml --network-driver vector2 --network-queues auto -W 1 -M 10
umlctl gate loop -f workload.toml --sweep network.queues=1,auto -W 1 -M 10
```

`auto` is deliberately vector2-only.  Legacy `vector` keeps the fixed
single-queue contract.

## Metadata And Labels

The generated network plan now distinguishes the user's queue request
from the resolved runtime count:

```text
queues=4 queue_spec=auto
```

Manifest labels include both fields:

```text
umlctl.network.queues=4
umlctl.network.queue_spec=auto
```

Guest init metadata mirrors the same split:

```sh
UMLCTL_NETWORK_QUEUE_SPEC=auto
UMLCTL_NETWORK_QUEUES=4
```

This keeps workloads simple.  Most scripts should use
`UMLCTL_NETWORK_QUEUES` because it is the resolved count the driver is
actually using.  Audit tools can use `UMLCTL_NETWORK_QUEUE_SPEC` or the
manifest label when they need to know whether the operator requested a
fixed count or automatic sizing.

## Implementation Notes

`tools/uml/uml-launcher/src/bin/umlctl/deploy.rs` now models
`network.queues` as a typed queue spec instead of a bare integer:

- fixed integer values serialize back as TOML integers;
- `"auto"` serializes back as a TOML string;
- validation rejects zero, values above 1024, and `auto` outside
  vector2 TAP mode;
- compilation resolves `auto` from `[runtime].ncpus` before rendering
  host TAP setup, inherited fd counts, init metadata, and the vector2
  kernel command line.

`tools/uml/uml-launcher/src/bin/umlctl/gate_loop.rs` accepts `auto` in
both `--network-queues` and `--sweep network.queues=...`, so soak
matrices can compare fixed and per-vCPU queue shapes without creating
separate Umlfiles.

## Example

`tools/uml/uml-launcher/examples/vector2-auto-queues.toml` is the
focused smoke for this path.  It sets `ncpus = 4`, `queues = "auto"`,
and uses launcher-owned fd handoff.

The dry-run plan shows the resolved contract:

```text
mode=tap driver=vector2 guest_dev=vec2.0 host_tap=v2autoq0 transport=fd host_mode=fd queues=4 queue_spec=auto
kernel_arg=vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4
inherited_fds=tap:v2autoq0 -> fds=200..203
```

The generated init metadata includes:

```text
UMLCTL_NETWORK_QUEUE_SPEC='auto'
UMLCTL_NETWORK_QUEUES='4'
UMLCTL_NETWORK_FD_COUNT='4'
```

## Validation Evidence

This follow-up was checked with:

- `cargo test --manifest-path tools/uml/uml-launcher/Cargo.toml`;
- `cargo fmt --manifest-path tools/uml/uml-launcher/Cargo.toml --check`;
- `git diff --check`;
- `um_vector2_*` KUnit parse against the current KUnit build:
  72/72 passed;
- dry-run of `tools/uml/uml-launcher/examples/vector2-auto-queues.toml`,
  confirming `queues=4 queue_spec=auto`,
  `fd=200,queues=4`, and inherited fds `200..203`;
- dry-run of `tools/uml/uml-launcher/examples/fastapi.toml` with
  `--network-driver vector2 --network-queues auto`, confirming
  single-vCPU auto resolves to queue count 1 while preserving
  `UMLCTL_NETWORK_QUEUE_SPEC=auto`.
- one live KCSAN-instrumented seccomp gate iteration using
  `vector2-auto-queues.toml`: `PASS=1/1`, no lingering `v2autoq0`,
  `requested_queues=4 runtime_queues=4`, and no KCSAN data-race
  signatures in `/tmp/um-vector-auto-kcsan/loop`.

## Replacement Boundary

Automatic queue sizing is an `umlctl` usability feature.  It does not
close the vector v2 replacement gates by itself.  The remaining KCSAN,
long SMP, performance, kvm-v2, and compatibility gates still apply.
