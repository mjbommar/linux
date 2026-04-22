# umlctl — multi-instance lifecycle CLI

**Status:** PROPOSED — future phase. Not in the A/B/C/D plan.

Closest analogue: `docker` / `podman`, but for UML. Not a
container runtime — UML isn't a container. `umlctl` is the
multi-instance supervisor the v1 launcher deliberately left
out (see C-10 v1 Open-Question Q3: "Cohabitation with classic
`linux ...` invocation — yes; launcher is opt-in").

## Ideal end-user flow

```text
$ umlctl create research-1 --profile research --kernel ./linux \
                            --rootfs debian-bookworm.img --mem 512M
Created instance research-1

$ umlctl start research-1
Started research-1 (pid 412033, console at ~/.local/state/uml/research-1/console.sock)

$ umlctl ps
NAME         PROFILE   STATUS   UPTIME   MEM    RSS    BACKENDS
research-1   research  running  42m      512M   380M   3 (c,n,b)
fuzz-a       fuzz      exited   -        256M   -      -

$ umlctl logs research-1 -f
[    0.000000] Linux version 7.0.0-uml ...
...

$ umlctl attach research-1        # drop into the console
$ umlctl exec research-1 -- uname -a  # run a cmd via hostfs init wrapper
$ umlctl snapshot research-1 --label before-upgrade
$ umlctl stop research-1
$ umlctl rm research-1
```

One directory per named instance, versioned by a simple TOML
manifest:

```text
~/.local/state/uml/research-1/
├── manifest.toml          # create-time spec (kernel, profile, rootfs, mem, ...)
├── console.sock           # vhost-user console backend
├── net.sock               # vhost-user net backend (if declared)
├── block.sock             # vhost-user block backend (if declared)
├── kernel.log             # dmesg capture for `logs -f`
├── uml.pid                # kernel PID (while running)
├── backends.pids          # backend PIDs (one per line)
├── metrics.prom            # prometheus-format scrape endpoint file
└── snapshots/
    └── before-upgrade/    # labeled snapshot, see 02-snapshot-to-disk.md
```

## What this builds on

- **C-10 v1** — `uml-launcher run` is the spawn primitive.
  umlctl calls it internally.
- **C-10 v2** — per-device backends. umlctl spawns them as
  the run-time's children; their PIDs land in
  `backends.pids`.
- **C-09 v1 forkserver** — `fuzz` profile instances inherit
  the forkserver fds from the manifest.
- **08-future-phases/02-snapshot-to-disk.md** — the v2
  snapshot format umlctl's `snapshot` / `restore` commands
  will speak.

## Prior art

- **podman** — rootless-first, no daemon by default. umlctl
  should match that posture. `~/.local/state/uml/` is the
  XDG state directory; no root, no system-level service,
  opt-in daemon for multi-instance background supervision.
- **docker** — clear CLI verb vocabulary (`ps`, `logs`,
  `exec`, `stop`, `rm`). Borrow directly; users already know
  it.
- **systemd-nspawn** / **machinectl** — nspawn's
  instance-is-a-directory model + machinectl's one-verb-per-
  lifecycle-step is closer to UML's "guest as a
  long-running process" shape than docker's ephemeral
  container model.
- **Firecracker CLI (`firecracker`)** — monolithic one-
  instance-per-process model with an API socket. Simpler than
  docker, more capable than a shell wrapper. Reference for
  the minimum-viable CLI surface.

## Build shape

- Rust binary under `tools/uml/umlctl/` (parallel to
  `tools/uml/uml-launcher/`). Shares the `uml-launcher`
  Cargo workspace if dependencies cleanly match; otherwise
  its own crate with a narrow dep on the launcher's config
  types.
- State-store module wraps `~/.local/state/uml/<name>/`.
  Pure filesystem; no SQLite, no daemon. `umlctl ps` walks
  the directory, reads manifest + pidfile, cross-checks
  against `kill -0`.
- Process supervision is spawn-and-forget: `umlctl start`
  execs `uml-launcher run` via `systemd-run --user
  --no-block` (default) or as a plain background child
  (`--no-systemd`). Both write `uml.pid`. `umlctl stop`
  reads the pid, `kill -TERM`, wait, `kill -KILL` on
  timeout.
- Log capture: `uml-launcher` stdout/stderr tee'd to
  `kernel.log` (append + rotate at 64 MiB; configurable).
- Metrics: per-instance prometheus-format scrape file
  written every 5s by a sidecar written in `uml-launcher`
  when `--metrics` is enabled. Picked up by the TUI (04)
  or Prometheus via `file_sd_configs`.

## Verbs

| Verb | Action |
|---|---|
| `create <name>` | Write `manifest.toml`, allocate state dir. Doesn't start. |
| `start <name>` | Spawn `uml-launcher run` from the manifest. Write `uml.pid`. |
| `ps` | List instances + status. |
| `stop <name>` | SIGTERM, wait, SIGKILL timeout. |
| `kill <name>` | SIGKILL immediately. Equivalent to force-stop. |
| `rm <name>` | Delete state dir. Refuses if running. `--force` overrides. |
| `logs <name> [-f]` | `tail` (optionally `-f`) the kernel.log. |
| `attach <name>` | Connect to console socket. `q` or `Ctrl-]` to detach without killing. |
| `exec <name> -- <cmd>` | Run cmd in guest via hostfs-init wrapper. |
| `snapshot <name> [--label L]` | Create `snapshots/L/`. |
| `restore <name> --label L` | Boot with the labeled snapshot. |
| `metrics <name>` | cat the prometheus scrape file. |

## Non-goals

- **Orchestration across hosts** (k8s/nomad). That's 11's
  territory — umlctl is a single-host CLI.
- **Network isolation policy** (bridges, NAT, routing). tap
  device name goes in the manifest; plumbing is outside
  umlctl.
- **Rich image management** (registry, layering). Manifest
  references existing rootfs images on disk. `umlctl
  pull` / `umlctl build` explicitly out.
- **Daemon with REST API.** Separate from umlctl — see 11.

## Open questions

- **Q1: systemd integration.** Should `umlctl start` default
  to `systemd-run --user` (units show up in `systemctl
  --user`) or a plain child? Default to systemd-user when
  available, fall back to plain child otherwise. TUI (04)
  then shows both cohorts.
- **Q2: rootfs format.** hostfs? ubd image? Manifest says
  which, launcher flags flow through. v2 block backend (C-10
  v2) is the path for ubd images.
- **Q3: multi-user.** One user's instances are invisible to
  another by design (everything in `~/.local/state/uml/`).
  Shared-host multi-user deployment lands in 11 via a
  system-service API daemon.

## Effort estimate

1-2 weeks. Mostly plumbing (manifest schema, state dir
helpers, subcommand wiring). The heavy lifting (spawn, vhost
backends) is already in uml-launcher.

## Dependencies

- C-10 v2 (backend decomposition) landed; umlctl's `create`
  manifest schema references it.
- 04-uml-launcher-tui.md (shares state schema).

## Cross-references

- `02-workstreams/C-profiles-and-gaps/10-host-launcher-crosvm.md`
  — C-10 ruled `umlctl`-class multi-instance UX out of v1
  scope; this is where that ends up.
- `04-uml-launcher-tui.md` — the TUI over this CLI's state.
- `11-uml-api-daemon.md` — the host-service REST API that
  wraps umlctl's operations for external orchestrators.
- `02-snapshot-to-disk.md` — the snapshot/restore format.
