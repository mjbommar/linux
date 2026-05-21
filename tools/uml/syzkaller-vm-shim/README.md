# syzkaller `vm/uml` backend shim

Reference implementation of the syzkaller `vmimpl.Pool` /
`vmimpl.Instance` backend for User-Mode Linux fork-server pools
(Memo 09 Phase 4, spec memo 11).

## Status

LANDED reference implementation.  Vendored here in the Linux tree
under `tools/uml/syzkaller-vm-shim/` so the umlctl wire shape and
the Go shim can evolve together; the file is intended to be
copied verbatim (one rename) into upstream syzkaller at
`vm/uml/uml.go` when the upstream PR lands (spec memo 11 §7).

## What this file is

A glue layer: the entire heavy lifting — kernel boot in template-
pause + fork mode, identity re-plumbing per-take, TAP/IPv4/iptables
setup, mconsole bookkeeping — lives in the in-tree Rust binary
`umlctl` (`tools/uml/uml-launcher`).  This Go file's only jobs
are:

  * `ctor` — optionally start `umlctl pool serve` in background,
    decode `Env.Config` into our `Config` struct.
  * `Create` — shell to `umlctl pool take --json`, decode the
    `SpawnResult` envelope into an `instance`.
  * `Run` — shell to `umlctl exec --json`, NDJSON-decode the
    `start`/`stdout`/`stderr`/`console`/`exit` frame stream into
    the `[]byte` + `error` channel pair vmimpl expects.
  * `Forward` — shell to `umlctl port-forward --json` for the
    TAP-direct gateway address; falls back to deriving it locally
    from the instance's recorded gateway IP if the daemon is
    unreachable.
  * `Copy` — pure host-fs path translation (UML mounts the host
    root at `/host` by default), no actual copy.
  * `Close` — shell to `umlctl pool destroy --name <pool>`.

Total: ~280 lines of Go (per spec memo 11 §5 the budget was ~150,
exceeded slightly because the production version wires
`ctx`-cancellation, stop-channel, and error envelope decoding that
the §5 pseudocode elided).

## How to drop this into syzkaller

1.  In your syzkaller checkout, create the package directory:

    ```sh
    mkdir -p vm/uml
    cp /path/to/linux/tools/uml/syzkaller-vm-shim/uml.go vm/uml/uml.go
    ```

2.  Add the blank import to `vm/vm.go`:

    ```go
    import (
        // …existing imports…
        _ "github.com/google/syzkaller/vm/uml"
    )
    ```

3.  Make sure `umlctl` (built from `tools/uml/uml-launcher` in this
    tree) is on `PATH`, or set `umlctl: "/abs/path/to/umlctl"` in
    your syzkaller config's `vm` block.

4.  Configure your syzkaller manager's `vm` block with:

    ```jsonc
    {
        "vm": {
            "pool": "syz-pool",
            "umlctl": "/usr/local/bin/umlctl",
            "auto_serve": true,
            "kernel": "/path/to/uml-fork-vmlinux",
            "count": 4,
            "mem_mb": 256,
            "tap_prefix": "tap-syz-",
            "subnet": "10.7.0.0/24",
            "hostfs_map": "/host"
        }
    }
    ```

5.  Build syzkaller normally; the `vm/uml` package compiles into
    `syz-manager` via the blank import.

## Why a fork rather than upstream

The Memo 09 plan is to target upstream acceptance, with a fork as
plan B (spec memo 11 §7).  This vendored copy supports both:
- if upstream accepts, the file becomes the upstream source;
- if upstream rejects, we maintain a fork and the operator copies
  the file from this directory.

## What this file does NOT do

  * Boot the master kernel — `umlctl pool serve` does that.
  * Allocate TAP devices or iptables rules — operator does that
    once per host, or `umlctl up` does it per-deployment.
  * Drive the in-guest exec primitive — `umlctl exec` proxies to
    `uml_mconsole(1)`'s `exec` verb in the MVP; a pty pair plus
    SCM_RIGHTS is on the roadmap (spec memo 11 §6 q3).

## Wire contracts this shim depends on

  * `umlctl pool take --name <pool> --json`: stdout = one
    `SpawnResult` JSON object with at least `pid`, `instance`,
    `mac`, `tap`, `ipv4_cidr`, `ipv4_gateway` fields.
  * `umlctl exec --name <pool> --pid <PID> --json -- argv...`:
    stdout = NDJSON stream of `start` / `stdout` / `stderr` /
    `console` / `exit` frames per spec memo 11 §3.4.  Exit-frame
    is always present.
  * `umlctl port-forward --name <pool> --pid <PID> --host-port
    <P> --json`: stdout = one JSON object with `guest_address`
    field shaped as `"host:port"`.
  * `umlctl pool status --name <pool> --json`: stdout = the
    daemon's status reply.
  * `umlctl pool destroy --name <pool> <PID>`: idempotent SIGKILL
    + zombie reap.

Schema bumps to any of the above use the `schema_version` field
in the relevant header frame (spec memo 11 §3.6).
