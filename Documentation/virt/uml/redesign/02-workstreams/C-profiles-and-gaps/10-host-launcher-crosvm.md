# C-10: Crosvm-style host launcher

**Status:** landed v1 (2026-04-20); post-landing fix
`672edefe415a` (2026-04-21) corrected `--root` / `--console`
precedence so TOML/env overlays actually take effect when the
CLI flag is absent (see D46). `tools/uml/uml-launcher/` Rust
binary + Makefile + user doc + `launcher-smoke` regression
selftest all shipped. `cargo build --release` clean, `cargo
test` 13/13 pass (2 new precedence-regression tests), end-to-end
spawn + signal-forward + exit-code passthrough validated (UML
boot with `init=/bin/true` → launcher returns 134 per 128+SIGABRT
convention, selftest asserts). v2 (per-device vhost-user +
seccomp) remains tracked in the Roadmap section below; this
series delivers v1 only.
**Effort:** 6 weeks (budget). v1 scope below was **~1 session of
disciplined work** given the locked crate stack and small v1
feature set; v2 (vhost-user device decomposition + seccomp)
is the remainder of the budget and lands separately.
**Dependencies:** A (backend ops), B (static-key gates) — both
                  landed. No kernel changes needed for v1.
**Blocks:** sandbox profile having strong host-side isolation;
            distro-friendly packaging of UML; external-orchestrator
            (libvirt / kata / k8s) consumers of UML.

## Goal

Ship `tools/uml/uml-launcher/` — a Rust host-side binary that
replaces the hand-rolled shell wrappers everyone writes to invoke
UML. Beautiful, easy to use, powerful.

Concretely: a user runs

```
uml-launcher run --kernel ./linux --init /bin/sh --mem 512M
```

instead of

```
./linux rootfstype=hostfs rootflags=/ init=/bin/sh mem=512M \
        con=null con0=fd:0,fd:1 root=/dev/root rw ...
```

v1 target: clean replacement for the shell-wrapper pattern. v2
target (separate work): per-device process decomposition over
vhost-user + per-device seccomp filters, matching crosvm /
Firecracker isolation posture for UML's sandbox profile.

## v1 scope (this series)

- CLI tool with clap-derive subcommands (`run` today; `snapshot`,
  `attach`, `list`, `stop` reserved for v2+).
- Declarative config: CLI flags > env vars > TOML file > defaults,
  merged via `figment`.
- UML-process spawn via `std::process::Command`, argv built from
  config, fd plumbing via `CommandExt::pre_exec` + `nix::dup2`.
- Supervision: `shared_child` so main thread and signal thread
  both hold the handle; `signal-hook` dedicated thread forwards
  SIGINT / SIGTERM to the child and drains SIGCHLD.
- Forkserver fd support: `--forkserver` flag plumbs host fds 198
  (ctl) and 199 (status) into the child — this is what lets
  external harnesses (AFL++, snapshot-smoke-driver.py) invoke
  UML without reaching for dup2 themselves. **This is the C-09 v1
  ceiling interop story**; the M8 fuzz milestone needs it.
- Console wiring: `--console stdio|null|pty`.
- hostfs share: `--hostfs host_path:guest_mount` (repeatable).
- Structured logging via `tracing` + `tracing-subscriber`;
  `--log-format plain|json`, `-v/-vv/-vvv`.
- `anyhow::Result<()>` main, `.context(...)` at boundaries.
- Exit status passthrough (the launcher's exit code is the UML
  child's exit code).

## v1 non-goals (explicitly out)

- vhost-user per-device processes (v2).
- seccomp filters (v2; designed alongside per-device processes).
- AppArmor / SELinux profiles (v2).
- Multi-instance supervision / daemon mode (v3).
- RPC control surface (v3; JSON-RPC or gRPC).
- Distro packaging (`.deb`/`.rpm`) beyond a reasonable Cargo.toml
  with shallow deps.

## Crate stack (v1)

Locked after web research for "beautiful, easy to use, powerful":

| Concern | Crate | Why |
|---|---|---|
| CLI parsing | **clap v4** (derive) | De-facto standard; derive + subcommands + env-var fallbacks + shell completions. |
| Config merge | **figment** | Provider model matches "CLI > env > file > default" precedence natively. |
| Child process | **std::process::Command + shared_child** | `shared_child` lets signal thread and main both hold the handle. |
| Signals | **signal-hook** | Dedicated-thread `Signals` iterator; widest ecosystem support. |
| Logging | **tracing + tracing-subscriber** | `EnvFilter`, `fmt().json()`, spans for launch/supervise/reap phases. |
| Errors | **anyhow** | Binary-only; `.context(...)` at boundaries; backtrace via RUST_BACKTRACE=1. |
| FD passing | **nix::unistd::dup2** inside `CommandExt::pre_exec` | Canonical post-fork/pre-exec pattern. |

**No async runtime in v1** — spawn one child, supervise, reap
is ~100 LOC sync. tokio comes in v2 with vhost-user (rust-vmm's
`vhost` + `vhost-user-backend` crates use tokio internally).

MSRV pinned at **Rust 1.74** (`rust-version = "1.74"` in
Cargo.toml). Avoids 2024-edition-only features; keeps Debian
stable buildable.

## File layout

```
tools/uml/
└── uml-launcher/
    ├── Cargo.toml            # pinned deps, MSRV, workspace-less
    ├── Makefile              # kbuild integration (no-op if cargo absent)
    ├── README.md             # one-page "what + why"
    └── src/
        ├── main.rs           # entry, tracing init, subcommand dispatch
        ├── cli.rs            # clap derive structs
        ├── config.rs         # figment loader + merge
        ├── launcher.rs       # build Command, pre_exec, spawn
        └── signal.rs         # signal-hook thread + forwarding
```

Intentionally flat. Single binary. No workspace for v1 — add one
if v2 grows per-device helper binaries (`uml-launcher-net`,
`uml-launcher-block`, ...).

## Makefile pattern

`tools/uml/uml-launcher/Makefile` detects cargo and no-ops if
absent. Matches the "optional tool" convention used elsewhere
under `tools/` (e.g., `tools/bpf/bpftool` detects libbfd).
Invocation:

```
make -C tools/uml/uml-launcher            # cargo build --release
make -C tools/uml/uml-launcher check      # cargo clippy --all-targets
make -C tools/uml/uml-launcher test       # cargo test
make -C tools/uml/uml-launcher install    # cp target/release/uml-launcher $(PREFIX)/bin/
```

`tools/uml/Makefile` iterates subdirs if more land later.

## Validation

AGENT-PROMPT §3 Q1 bar per commit:

- `cargo build --release` clean on stable Rust 1.74+.
- `cargo clippy --all-targets -- -D warnings` clean.
- `cargo test` passes (unit tests for argv construction + config
  merge).
- `scripts/checkpatch.pl` clean (checkpatch has rough Rust
  support; at minimum SPDX headers + commit format).

C-10-specific validation:

- **End-to-end spawn**: `uml-launcher run --kernel
  /tmp/uml-c09-build/linux --init /bin/true --mem 128M` exits 0.
- **Signal forwarding**: launch UML in the background, `kill
  -TERM <launcher-pid>`, verify the UML child receives SIGTERM
  and exits cleanly, launcher returns its exit code.
- **Forkserver interop**: `uml-launcher run --forkserver ...`
  wraps a UML binary that speaks the C-09 AFL protocol; the
  existing `tools/testing/selftests/um/snapshot-smoke/
  snapshot-smoke-driver.py` should work end-to-end when replaced
  with `uml-launcher run --forkserver`.
- **Config-merge semantics**: unit-tested via a table-driven
  test of the figment provider stack.

## Selftest

`tools/testing/selftests/um/launcher-smoke/` pattern:

- `run-launcher-smoke.sh` — host driver; runs `uml-launcher run
  --kernel $UML_BINARY --init /bin/true` and expects exit 0.
- `launcher-smoke.sh` — guest init (reuse userspace-smoke's
  shape; or just `/bin/true` since we're testing the launcher,
  not the kernel).
- Registered in `tools/testing/selftests/um/Makefile` TARGETS.

## Commit plan (bisectable)

1. **commit 1:** skeleton. `tools/uml/uml-launcher/` with
   `Cargo.toml`, `Makefile`, `README.md`, and empty `src/main.rs`
   that just prints `uml-launcher v0.1.0`. `cargo build` clean.

2. **commit 2:** `run` subcommand end-to-end. clap CLI +
   figment config + launcher.rs argv construction + signal.rs
   supervisor. End-to-end: spawns UML, forwards signals, returns
   exit code. Units-tested argv builder + config merge. Tested
   manually by launching `/tmp/uml-c09-build/linux
   init=/bin/true`.

3. **commit 3:** user doc + selftest + landed status.
   `Documentation/virt/uml/launcher.rst`, toctree entry,
   `tools/testing/selftests/um/launcher-smoke/`, flip this
   doc's Status to `landed v1 (YYYY-MM-DD)`.

v2 (separate future series):

4. **commit 4+:** vhost-user helpers via rust-vmm `vhost` +
   `vhost-user-backend`. One helper per device class
   (console, net, block). Per-device seccomp via `seccompiler`.
   Per-device namespaces.

5. **commit N:** AppArmor reference profile, SELinux policy,
   systemd unit template. Multi-instance supervisor.

## Open questions

- **Q1:** Is upstream OK with Rust in `tools/uml/`? Kernel-Rust
  at the top-level `rust/` is accepted as of Linux 7.0
  (2026). Userspace Rust under `tools/` is less trodden but not
  unprecedented. **Plan:** ship v1 behind `make` that no-ops if
  cargo is absent; engage distro packagers (Debian `debcargo`,
  Fedora `rust2rpm`) early.
- **Q2:** Single multi-call binary (busybox style) or per-device
  binaries? **v1 defers** — v1 is one binary (`uml-launcher`).
  v2 decides after the vhost helper processes are real.
- **Q3:** Cohabitation with classic `linux ...` invocation?
  **Yes.** Launcher is opt-in. Kernel drops nothing.
- **Q4:** MSRV policy evolution. **Conservative** — pin 1.74,
  bump deliberately in a separate commit when needed. Don't
  chase bleeding-edge features.

## Risk

- **v1 risk: low.** One binary, well-understood crates, no kernel
  changes. Worst case: `make` no-ops and UML runs via the classic
  shell-wrapper path as today.
- **v2 risk: medium.** vhost-user per-device + seccomp is
  architecturally familiar (crosvm, Firecracker) but distro
  packaging + LSM profile work is wide. Documented in the
  original doc's "Risk" section; unchanged here.
- **Upstream review risk: low-to-medium.** First Rust binary
  under `tools/uml/`. Maintainers may want to scrutinize crate
  vendoring, MSRV, and whether it duplicates existing VM
  management tools (libvirt has no UML driver today; Rust-in-kbuild
  precedent lives at the top-level `rust/` directory).

## Cross-references

- `00-vision.md` — "sandbox profile" story the launcher enables.
- `06-sequencing/critical-path.md` — C-10 is parallelizable,
  off-critical-path for M8 fuzz milestone.
- `03-profiles/sandbox.md` — consumer profile.
- `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md`
  — C-09's fd 198/199 protocol that C-10 v1 supports via
  `--forkserver`.
- rust-vmm project — `vhost`, `vhost-user-backend`, `seccompiler`
  crates for v2.
- crosvm / Firecracker — prior art for per-device decomposition.
