# uml-launcher

Host-side launcher for User-Mode Linux kernels. Replaces the
hand-rolled shell wrappers people write to invoke UML with a
single, declarative CLI tool.

```
uml-launcher run --kernel ./linux --init /bin/sh --mem 512M
```

is equivalent to (and replaces) the traditional:

```
./linux rootfstype=hostfs rootflags=/ init=/bin/sh mem=512M \
        con=null con0=fd:0,fd:1 root=/dev/root rw
```

Workstream C-10 of the UML redesign. See
[`Documentation/virt/uml/redesign/02-workstreams/
C-profiles-and-gaps/10-host-launcher-crosvm.md`](../../../Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/10-host-launcher-crosvm.md)
for the design document.

## Build

```
make -C tools/uml/uml-launcher
```

`cargo build --release` under the hood. The Makefile is a polite
no-op if `cargo` isn't on the host — matches the `tools/bpf/bpftool`
optional-tool pattern.

Install to `/usr/local/bin/uml-launcher`:

```
make -C tools/uml/uml-launcher install
```

## Usage

```
uml-launcher run \
    --kernel /path/to/linux \
    --init /bin/sh \
    --mem 512M
```

Run `uml-launcher run --help` for the full flag surface.

Config file (`~/uml.toml` or any path via `--config`):

```toml
kernel = "/path/to/linux"
init = "/bin/sh"
mem = "512M"
root = "hostfs"
```

Precedence (highest first): CLI flags > `UML_*` env vars > TOML
config > built-in defaults. Merged via
[figment](https://crates.io/crates/figment).

Ready-made example configs live in [`examples/`](examples/) —
one per common UML profile (fuzz, research, sandbox) plus a
daily-driver `dev.toml`. Copy to `~/.config/uml-launcher/` or
alongside your kernel tree, edit the paths, and run
`uml-launcher run --config <name>.toml`.

Structured logging:

```
uml-launcher -vv run ...                 # debug level, plain output
uml-launcher --log-format json run ...   # JSON on stderr (for tooling)
RUST_LOG=uml_launcher=trace uml-launcher run ...  # env overrides
```

## Forkserver interop

The C-09 AFL forkserver protocol wants host fds 198 (ctl) and
199 (status) plumbed into the UML child. Pass an already-open
pair of fds to the launcher:

```
exec 3<>/tmp/ctl  4<>/tmp/status      # fuzzer opens them first
uml-launcher run --forkserver 3,4 --kernel ... --init ...
```

The launcher `dup2`s 3 → 198 and 4 → 199 inside `pre_exec` (post-
fork, pre-`exec`). No shell-side fd juggling required.

## Current scope (v1)

- Single-binary launcher, synchronous child supervision.
- CLI + env + TOML config merge.
- SIGINT/SIGTERM/SIGHUP/SIGQUIT forwarded to the UML child.
- Exit-code passthrough (launcher's `$?` is UML's `$?`).
- `--forkserver` fd plumbing for C-09 AFL protocol.
- hostfs root. Block-device (ubd) root deferred to v2.
- stdio / null console. PTY console deferred.

## Not in v1 (tracked for v2)

- Per-device host processes (console, net, block) over
  vhost-user with `seccompiler` filters — the crosvm/Firecracker
  isolation story.
- AppArmor / SELinux reference profiles.
- Multi-instance daemon + RPC control surface.

See the [design doc](../../../Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/10-host-launcher-crosvm.md) §"v2" for scope.

## Dependencies

Locked in `Cargo.toml`. Pinned for Debian/Fedora packaging:

- `clap` v4 — CLI derive macros.
- `figment` — hierarchical config provider stack.
- `serde`, `toml` (via figment) — config deserialization.
- `shared_child` — Child handle shared across threads.
- `signal-hook` — signal iterator on a dedicated thread.
- `tracing` + `tracing-subscriber` — structured logs.
- `anyhow` — error context at boundaries.
- `nix` + `libc` — `dup2`/`fcntl` in `pre_exec`.

MSRV: Rust 1.74 (`rust-version` in `Cargo.toml`).

## License

GPL-2.0. See `SPDX-License-Identifier` at the top of each source
file.
