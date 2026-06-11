# syzkaller `vm/uml` backend shim

This directory contains a reference implementation of the syzkaller
`vmimpl.Pool` and `vmimpl.Instance` backend for User-Mode Linux
fork-server pools.

The heavy lifting lives in `umlctl` under `tools/uml/uml-launcher`.
The Go shim shells out to `umlctl`, decodes JSON envelopes, and maps
the result into syzkaller's VM interface.

## Integration

1. In a syzkaller checkout, create the package directory:

   ```sh
   mkdir -p vm/uml
   cp /path/to/linux/tools/uml/syzkaller-vm-shim/uml.go vm/uml/uml.go
   ```

2. Add the blank import to `vm/vm.go`:

   ```go
   import (
       _ "github.com/google/syzkaller/vm/uml"
   )
   ```

3. Build `umlctl` from `tools/uml/uml-launcher` and put it on `PATH`,
   or set an absolute `umlctl` path in the syzkaller manager config.

4. Configure the syzkaller manager `vm` block:

   ```json
   {
       "vm": {
           "pool": "syz-pool",
           "umlctl": "/usr/local/bin/umlctl",
           "runtime_dir": "/run/user/1000/uml-syz",
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

## Responsibilities

| Method | Behavior |
|--------|----------|
| `ctor` | Decode `Env.Config`; optionally start `umlctl pool serve`. |
| `Create` | Run `umlctl pool take --json` and decode the instance envelope. |
| `Run` | Run `umlctl exec --json` and merge stdout, stderr, and console frames. |
| `Forward` | Run `umlctl port-forward --json`, with a local gateway fallback. |
| `Copy` | Translate hostfs paths; no data copy is needed. |
| `Close` | Run `umlctl pool destroy --name <pool>`. |

If `runtime_dir` is set, the shim passes it to every `umlctl` invocation as
the global `--runtime-dir` option. If `auto_serve` is set and `mem_mb` is
non-zero, the shim starts the daemon with the matching `--mem <mem_mb>M`.

## Wire Contracts

The shim depends on these `umlctl` JSON shapes:

| Command | Contract |
|---------|----------|
| `pool take --json` | one `SpawnResult` object with `pid`, `instance`, `mac`, `tap`, `ipv4_cidr`, and `ipv4_gateway` |
| `exec --json -- argv...` | NDJSON stream with `start`, `stdout`, `stderr`, `console`, and `exit` frames |
| `port-forward --json` | one object with a `guest_address` field shaped as `host:port` |
| `pool status --json` | daemon status reply |
| `pool destroy` | idempotent shutdown and reap |

Schema bumps should be reflected in the `schema_version` field carried
by the relevant frame.

The shim uses request-specific lazy `pool take` calls. It supplies instance,
MAC, TAP, IPv4, and gateway values so syzkaller instances have deterministic
network identity. It does not consume `pool take --ready`, because ready members
carry daemon-assigned identity and are not rebound to caller-supplied TAP/IP
values.

`exec/1` is the current stable execution contract. The shim sends argv through
`umlctl exec --json`, and `umlctl` sends structured argv/env/cwd/timeout fields
to the pool daemon. The daemon may lower that request through the current UML
mconsole `exec` command internally. In this implementation `--timeout` requires
the guest image to provide `timeout(1)` at `/usr/bin/timeout` or `/bin/timeout`.
A stricter daemon-to-kernel argv transport is future `exec/2` work and must use
a schema bump.
