# uml-launcher TUI — ratatui dashboard for live UML instances

**Status:** PROPOSED — future phase. Not in the A/B/C/D plan.
This note captures the shape only.

Policy check (per the 08-future-phases boundary rule): this
idea does not overlap C-10 v1 or v2. v1 supervises a single
UML child; v2 decomposes device backends into per-device
processes. Neither surfaces a multi-instance dashboard. TUI is
a layer above both.

## Ideal end-user flow

Today, operators who run several UML instances reach for `ps`,
`htop`, and ad-hoc shell loops to see what's alive, what each
instance is doing, and where host RSS is going. That's a gap
we can close cleanly once the v2 backend-decomposition work
lands — every UML guest then owns a tree of host processes
(the kernel plus its vhost-user backends), and that tree is
the natural thing to visualize.

Target flow:

```text
$ uml-launcher tui
┌─────────────────────────────────────────────────── UML instances (3) ───┐
│ NAME        PROFILE   PID     MEM    RSS    BACKENDS  LSM        STATE │
│ research-1  research  412033  512M   380M   3 (▣▣▣)   apparmor   run   │
│ fuzz-a      fuzz      412190  256M   210M   2 (▣▣□)   -          idle  │
│ sandbox-1   sandbox   412244  128M   95M    3 (▣▣▣)   apparmor   run   │
└──────────────────────────────────────────────────────────────────────────┘
  ENTER expand · s snapshot · k kill · l logs · q quit
```

Expand a row with ENTER:

```text
▽ research-1  (PID 412033, profile research)
    backend console  pid 412034  seccomp:on  apparmor:backend_console  fds:3
    backend net      pid 412035  seccomp:on  apparmor:backend_net      fds:4
    backend block    pid 412036  seccomp:on  apparmor:backend_block    fds:5
    guest pagefault rate  412 / s
    guest syscall rate    1.4k / s
```

## What this builds on

Everything the TUI needs is already present (or designed) by
the time v2 lands:

- **C-10 v1** — `uml-launcher run` has supervision handles
  (`shared_child`) and child-pid visibility.
- **C-10 v2 commit 8 (orchestration)** — spawns per-device
  backends and records their sockets + PIDs. That map is the
  TUI's primary data source.
- **B-03 debugfs controls** (`/sys/kernel/debug/um/`) — live
  backend state and hooks are readable from the guest; the
  host-side TUI can scrape `um_*` stats per instance.
- **C-09 forkserver fds** — fuzz instances already expose
  `state_version` etc.; the TUI can fold those into per-row
  state.

## Prior art

- **htop** — canonical per-process TUI. Widely understood
  keybinding vocabulary (F9 kill, F5 tree, / search). Borrow
  those reflexes; don't invent new ones.
- **docker/podman `stats`** — concurrent-stream-of-metrics
  pattern. Its one-row-per-container model maps naturally to
  one-row-per-UML-instance.
- **k9s** — kubernetes TUI. Demonstrates that a read-write
  TUI with scoped kill / restart / exec actions is operator-
  friendly once the namespacing is clear.
- **ratatui** (formerly tui-rs) — current maintained Rust TUI
  crate. Widget set (`Table`, `Sparkline`, `Gauge`) covers the
  needs above without custom drawing.

## Build shape

- Single binary — `uml-launcher tui` as a new subcommand next
  to `run` and `backend`. No new top-level crate; lives in
  `tools/uml/uml-launcher/src/tui/`.
- Data model: polls the launcher's instance registry (a
  sibling umlctl or the instance-state dir, see 05) every
  500ms. No in-process DB; TUI is a read-mostly viewer with
  a few imperative keys (`k`, `s`, `l`).
- Metrics source: host `/proc/<pid>/{status,stat,io}` for
  RSS / faults / IO; UML-side `/sys/kernel/debug/um/` exposed
  via hostfs where the profile permits it; backend-side
  seccomp status via `prctl(PR_GET_SECCOMP)` results the
  backend reports on its control socket.
- Rendering: `ratatui` + `crossterm` backend (portable,
  headless-friendly for CI screenshots).

## Non-goals

- **Remote rendering** (SSH-friendly terminal streaming,
  agent/daemon split). If you want "watch UMLs on machine B
  from machine A", use `11-uml-api-daemon.md` + a web client.
  TUI stays local-process.
- **Historical metrics** (time-series DB, playback). TUI is
  live-only; history is a Perfetto job (see 12).
- **Interactive kernel poking** (kprobes via keyboard). That
  belongs in `08-uml-probe-web.md` where the UX tolerates
  richer input.

## Open questions

- **Q1: permissions.** The TUI wants to read `/proc/<pid>/*`
  for every UML on the host. Runs as the user that launched
  the instances by default; a sysadmin variant (`tui --all`)
  would need CAP_SYS_PTRACE or read-all-procs. Matches the
  `htop` UX precedent.
- **Q2: state store schema.** TUI and `umlctl` (05) share
  the same per-instance state; they need one canonical schema
  so the TUI doesn't drift. Resolved by letting umlctl own
  the schema and the TUI be a pure consumer (read from
  `~/.local/state/uml/<instance>/`).
- **Q3: what to show for fuzz profile.** AFL fd 198/199
  traffic rate? Iteration rate? Answer: iteration rate as a
  sparkline in the expanded row; fuzz profile is the one
  where "how fast is this running" matters most.

## Effort estimate

2-3 focused days once v2 + umlctl (05) land. Pure tools/
work, no kernel changes. Dependencies: ratatui, crossterm,
procfs (crate). Depends on umlctl's state schema existing.

## Dependencies

- C-10 v2 (backend decomposition) fully landed.
- 05-umlctl.md (multi-instance state store) designed.

## Cross-references

- `02-workstreams/C-profiles-and-gaps/10-host-launcher-crosvm.md`
  — v2 orchestration is the data source.
- `05-umlctl.md` — shares the state schema.
- `08-uml-probe-web.md` — the richer-input "do things in the
  guest" analogue; TUI is view-mostly, web is do-things.
