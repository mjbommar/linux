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

v2 (this section — per decisions-log D52):

**Shape.** One multi-call binary, not per-device binaries. The
same `uml-launcher` gains a `backend <class>` subcommand; each
class (console, net, block, rng, vsock) dispatches to its own
module. This matches crosvm's `crosvm device <kind>` precedent
and keeps distro packaging to one binary + one manpage + one
AppArmor profile family. Q2 (single vs per-device binaries)
from the v1 Open Questions is closed: single.

**Crate stack (v2 additions).**

| Concern | Crate | Note |
|---|---|---|
| vhost-user protocol | `vhost` | frontend/backend bindings |
| backend event loop | `vhost-user-backend` | `VhostUserBackend` trait + run loop |
| guest memory | `vm-memory` | `GuestMemoryMmap` + region management |
| virtio queue | `virtio-queue` | split-queue descriptor parsing (mandatory); packed optional |
| uapi bindings | `virtio-bindings` | auto-generated from `<linux/virtio_*.h>` |
| eventfd / ioctl | `vmm-sys-util` | rust-vmm utility crate |
| seccomp | `seccompiler` | Firecracker-authored; now standalone rust-vmm |

Per-class helper crates (`virtio-net`, `virtio-blk`, etc.) in
rust-vmm are available but uneven; v2 consumes them where they
cleanly fit and writes the device logic locally where they
don't. Don't depend on one of those crates without verifying
last-publish recency on docs.rs at the time of landing.

**Bisectable commit plan.**

4. **v2 commit 1:** subcommand scaffolding. `uml-launcher
   backend <class>` clap subcommand dispatches to module-level
   handlers. No actual vhost logic yet — each class's handler
   prints a `not-yet-implemented` line and returns 0. Units
   tested. Adds `backend` to cli.rs; no new runtime deps.
   **Tests:** `cargo test` green on argv parsing + dispatch.
   **Build-time:** no new crates; MSRV unchanged.

5. **v2 commit 2:** first real backend — `console`. Simplest
   vhost-user surface: one RX queue + one TX queue, byte-
   oriented, maps naturally to stdin/stdout or a pty. Depends
   on `vhost`, `vhost-user-backend`, `vm-memory`,
   `virtio-queue`, `virtio-bindings`, `vmm-sys-util`. Adds
   about 400-600 LOC of Rust. **Tests:** a selftest that
   spawns `uml-launcher backend console --socket ...`, connects
   a UML guest with `virtio_uml.device=<socket>:3` (virtio
   console id), and exchanges bytes over the console.

6. **v2 commit 3:** seccomp wrapper. Per-class filter loaded
   from a const JSON blob compiled into the binary. Applied
   *after* socket bind + fd plumbing, *before* the event-loop
   enter — matches crosvm/Firecracker discipline. Filter for
   `console` allows `read/write/ppoll/epoll_*/rt_sigreturn/
   exit(_group)` and little else; any attempted syscall
   outside the allowlist → `SIGSYS`. **Tests:** deny-list
   unit test (invoke a banned syscall in a spawn-and-check
   harness; expect SIGSYS).

7. **v2 commit 4:** `net` backend. Taps rather than
   user-networking in v1 to keep the scope tight; v2+ can add
   slirp. Depends on the same rust-vmm stack plus tap ioctls
   via `vmm-sys-util`. Adds ~800 LOC. Seccomp filter specific
   to net (adds TUN/TAP ioctls, `recvmsg/sendmsg`,
   `getsockname`). **Tests:** selftest spawns both backends,
   boots UML with virtio-console + virtio-net, asserts
   packet round-trip.

8. **v2 commit 5:** `block` backend. File-backed image via
   `O_DIRECT` + `preadv/pwritev`. ~500 LOC + class-specific
   seccomp. **Tests:** selftest boots UML from a virtio-blk
   image, reads/writes a canary file, compares to expected.

9. **v2 commit 6:** AppArmor reference profile.
   `tools/uml/uml-launcher/apparmor/uml-launcher` generic
   profile + per-backend sub-profiles (`uml-launcher//backend_
   console`, `//backend_net`, `//backend_block`). Covers the
   `change_profile` transitions the backends execute post-
   seccomp. **Tests:** a selftest that boots under an
   apparmor-enforcing profile, asserts the profile is active
   in `/proc/<pid>/attr/current`. Optional behind the
   `apparmor_parser` tool being available on the host.

10. **v2 commit 7:** SELinux reference policy module.
    `tools/uml/uml-launcher/selinux/uml_launcher.te` with a
    `uml_launcher_t` domain + per-backend types. Parallel to
    AppArmor in intent; either LSM is enough, both are
    redundant-safe. **Tests:** out-of-tree `make -f
    /usr/share/selinux/devel/Makefile` builds the module
    cleanly; runtime enforcement is a manual/optional
    verification.

11. **v2 commit 8:** orchestration. `uml-launcher run` gains
    `--virtio <class>:<class-args>` options (e.g. `--virtio
    console --virtio net:tap0 --virtio block:./rootfs.img`).
    Launcher spawns the backend processes before UML, creates
    the unix-domain sockets, passes them as file descriptors +
    paths, supervises their lifetime. UML's cmdline gets the
    corresponding `virtio_uml.device=<socket>:<id>` entries
    appended automatically. **Tests:** end-to-end `uml-launcher
    run --kernel ./linux --init /bin/true --virtio console` —
    UML boots under decomposed backends and exits cleanly.

12. **v2 commit 9:** docs + status flip.
    `Documentation/virt/uml/launcher.rst` gains a "Per-device
    decomposition" section. `03-profiles/sandbox.md` flips to
    reference the decomposed mode as the default for `sandbox`.
    `10-host-launcher-crosvm.md` Status line: `landed v2`.

Each commit builds on the previous but is independently
bisectable: `cargo build` + `cargo test` clean at every step.
Every commit that ships runtime code passes the research-
profile Q1 bar (checkpatch is not applicable to tools/; `cargo
clippy -- -D warnings` is the analogue and runs in CI).

**v2 out-of-scope (v3+).**

- systemd unit templates (belongs with distro-packaging PRs
  rather than in-tree).
- Multi-instance supervisor / daemon mode.
- JSON-RPC / gRPC control surface.
- slirp user-networking backend (tap-only in v2 `net`).
- vfio-user device passthrough.
- gpu, snd, wl, pmem backends (niche; add on demand).

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
