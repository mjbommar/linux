# umlctl — multi-instance lifecycle CLI

**Status:** v1 spec frozen + implementation scheduled
(2026-04-23). Future versions (v2 top/stat, v3 control
socket) remain parking-lot.
**Companion to:** `08-future-phases/README.md` (this
directory's index), C-10 v2 launcher at
`tools/uml/uml-launcher/`, decisions-log D45 (fork-first
policy — umlctl is in-tree because it's a local operator
tool, not an upstream-bound patch).

Closest analogue: `podman` more than `docker` — rootless,
no daemon, process-tree-based. `umlctl` is the multi-
instance supervisor the v1 launcher deliberately left out
(see C-10 v1 Open-Question Q3: "Cohabitation with classic
`linux ...` invocation — yes; launcher is opt-in").

This memo is **the authoritative spec**; the implementation
tracks it. Bump sections when shipping v2/v3 features.

## Motivation (v1)

Three immediate operational wins:

1. **Eliminates runaway UML processes.** Today, harness +
   selftest runs spawn UML via raw `timeout ./linux …`,
   and the UML child survives `timeout`'s signal because
   the UML kernel has its own signal plumbing. Orphaned
   `linux` processes accumulate in session state until
   someone notices CPU usage is pegged. Happened once
   during the 2026-04-23 session (26 orphans, load 27 on
   an 8-core host). `umlctl start`/`stop` with real pidfile
   + waitpid + zombie-reap turns this into a non-issue.
2. **Registry + listing.** `umlctl ps` answers "what UMLs
   are running" without `pgrep linux` heuristics. Useful
   for humans; required for the follow-on tooling
   (`04-uml-launcher-tui.md` ratatui dashboard,
   `11-uml-api-daemon.md` REST API, `06-uml-mcp.md` MCP
   server — all assume a stable registry).
3. **Consistent semantics across contexts.** One
   `umlctl start` path — works the same for: humans
   at a shell, selftest harnesses (replaces bespoke
   `timeout ./linux` incantations), CI jobs.

## Out of scope for v1

Deferred to v2+ explicitly. Noted so future maintainers
don't expand v1:

- `umlctl top` (ratatui dashboard) → v2.
- `umlctl stat` verbose inspect → v2.
- `umlctl exec` / `umlctl cp` (needs guest-agent) → v2.
- `umlctl attach` (console takeover) → v2.
- `umlctl backend` / `snapshot list` (needs control
  socket) → v3.
- Remote operation, SSH forwarding → never (scope creep;
  different project).
- Live migration → never.

## Storage layout

Follow XDG spec: split persistent state from runtime
(tmpfs-backed) state.

```
$XDG_STATE_HOME/uml/           # default ~/.local/state/uml
├── instances/
│   └── <name>.toml           # manifest (persistent, survives reboot)
├── runs/                     # per-run observability bundles (memo 13 O1.1)
│   └── <run_id>/             # ULID, lexicographic = chronological
│       ├── run.json          # run_id, instance, boot-offset clocks, exit status
│       ├── init.log          # init + userspace stdout
│       ├── kernel.log        # kernel console (O1.2; empty until split lands)
│       ├── events.jsonl      # structured spine events (O1.3)
│       ├── dmesg.jsonl       # /dev/kmsg tail (later phase)
│       └── trace.perfetto    # ftrace capture (later phase)
└── history.jsonl             # append-only audit log

$XDG_RUNTIME_DIR/uml/          # tmpfs; cleared on reboot
├── <name>.pid                 # PID as ASCII (one line)
├── <name>.run_id              # current run_id (side-file; removed on stop)
└── <name>.sock                # reserved for v3 control socket
```

**Why split state vs runtime:**

- Pidfiles on tmpfs → stale PIDs never survive reboot.
  Correctness property, not tidiness.
- Logs on persistent state → post-mortem after a crash
  that took the system down.
- Manifests on persistent state → the registry is the
  whole point.

Overrides via global flags: `--state-dir <path>` and
`--runtime-dir <path>`. Both default to their XDG bases
with `uml` appended.

## TOML manifest schema

One file per instance, immutable after `create`.
Mutations = `rm` + `create`. Prevents "my manifest drifted
under a running instance" class of bug.

```toml
# Example: ~/.local/state/uml/instances/research-1.toml
schema_version = 1

[instance]
name        = "research-1"
created_at  = "2026-04-23T17:42:11-04:00"

[kernel]
path        = "/tmp/uml-research/linux"
sha256      = "abc123..."            # recorded at create time
profile     = "research"             # auto-detected; informational
backend     = "auto"                 # maps to backend= cmdline

[runtime]
mem         = "256M"
ncpus       = 1
cmdline     = "rootfstype=hostfs rw"
root        = "hostfs"
forkserver  = false

[labels]
# free-form k=v; filterable via `ps --filter label=k=v`
```

## Global flags

Apply to every verb:

| Flag | Value | Default | Purpose |
|------|-------|---------|---------|
| `--state-dir <path>` | path | `$XDG_STATE_HOME/uml` | Override registry location |
| `--runtime-dir <path>` | path | `$XDG_RUNTIME_DIR/uml` | Override socket/pidfile location |
| `--log-level <lvl>` | trace\|debug\|info\|warn\|error | info | umlctl's own logging (not guest) |
| `--json` | — | off | Machine-readable (NDJSON for lists) |
| `--no-color` | — | auto | Force plain output |
| `-q, --quiet` | — | off | Suppress non-error output |
| `-V, --version` | — | — | Print version + git-sha + schema |
| `-h, --help` | — | — | Top-level help |

## v1 verb set (five lifecycle + `logs`)

### `umlctl create <name>`

Register a new instance's manifest. Does NOT spawn.

| Flag | Value | Default | Required | Purpose |
|------|-------|---------|----------|---------|
| `--kernel <path>` | path | — | yes | UML `linux` binary path |
| `--profile <name>` | string | autodetect | no | Display only |
| `--mem <size>` | e.g. `256M`, `2G` | `256M` | no | `mem=` cmdline |
| `--backend <kind>` | auto\|ptrace\|seccomp\|kvm\|force=... | auto | no | `backend=` cmdline |
| `--cmdline <raw>` | string | `""` | no | Extra kernel cmdline |
| `--root <path>` | path\|`hostfs` | `hostfs` | no | Root filesystem |
| `--forkserver` | — | off | no | Enable fd-198/199 plumbing |
| `--ncpus <n>` | int ≥1 | 1 | no | SMP CPU count |
| `--label <k=v>` | repeatable | — | no | User labels |
| `--force` | — | off | no | Overwrite existing manifest |

Exit codes: 0 created · 2 bad flag · 4 already exists (no `--force`).

Name validation: `^[a-z0-9][a-z0-9_.-]{0,63}$`. No prefix
matching (podman-style — forces explicit invocation;
easy to add in v2 if missed).

### `umlctl start <name>`

Spawn the UML process from the manifest.

| Flag | Value | Default | Purpose |
|------|-------|---------|---------|
| `--detach, -d` | — | on | Fork + return after kernel is ready |
| `--foreground` | — | off | Don't fork; stream console; Ctrl-C → SIGTERM to UML |
| `--ready-timeout <s>` | int | 30 | Wait-for-ready budget |
| `--no-log` | — | off | Skip log file creation |

Behavior:

- Read manifest. Exit 3 if missing.
- Acquire `flock` on `$RUNTIME/<name>.pid`. Race-free
  start across concurrent invocations.
- If pidfile present AND pid alive → exit 4 (already
  running). If pidfile present AND pid dead → treat as
  stale, remove.
- Fork. Child `execvp`s the kernel with manifest args.
- Parent detects ready (v1: parse stderr for
  `Linux version` + init marker; fall back to
  `--ready-timeout`). v3 replaces this with a control-
  socket handshake.
- `--detach` (default): write pidfile, print pid, exit 0.
- `--foreground`: keep parent as tty relay. Forward
  SIGINT → SIGTERM, SIGQUIT → SIGKILL, SIGTERM
  pass-through, SIGHUP ignored, SIGTSTP → SIGSTOP,
  SIGWINCH pass-through.
- Append start event to history.

Exit codes: 0 started · 2 bad flag · 3 not found · 4
already running · 5 kernel missing / non-executable ·
124 ready-timeout expired.

### `umlctl stop <name>`

Signal + wait + reap. **This is the verb that prevents
runaway UML processes.**

| Flag | Value | Default | Purpose |
|------|-------|---------|---------|
| `--signal, -s <sig>` | TERM\|KILL\|INT\|QUIT | TERM | First signal |
| `--timeout, -t <s>` | int | 10 | Escalation budget |
| `--force, -9` | — | off | SIGKILL directly |
| `--quiet, -q` | — | off | Silent on success |

Behavior:

- Read pidfile. Exit 6 if absent or pid dead.
- Send `--signal`.
- `waitpid(pid, WNOHANG)` poll loop with 100 ms sleep,
  up to `--timeout` seconds.
- If timeout, escalate to SIGKILL, blocking `waitpid`.
- **Reap zombies of any UML-adjacent children** via
  `wait4(-1, WNOHANG)` drain loop. This is the fix for
  the 2026-04-23 orphan-accumulation bug.
- Delete pidfile + any residual `$RUNTIME/<name>.*`.
- Append stop event to history (pid, exit_status,
  signal_sent).

Exit codes: 0 stopped · 3 not found (manifest missing) ·
6 not running · 124 timeout before SIGKILL succeeded
(shouldn't happen; indicates a bug).

### `umlctl rm <name>`

Delete manifest + logs. Destructive; explicit name only
(no glob in v1).

| Flag | Value | Default | Purpose |
|------|-------|---------|---------|
| `--force, -f` | — | off | Stop first if running |
| `--keep-logs` | — | off | Delete manifest, keep logs |

Behavior:

- Exit 3 if manifest missing.
- Exit 5 if running and no `--force`.
- `--force` + running → `stop` internally, then proceed.
- Delete manifest, `$STATE/logs/<name>-*.log`
  (unless `--keep-logs`), any residual `$RUNTIME/<name>.*`.
- Append rm event.

Exit codes: 0 removed · 3 not found · 5 still running.

### `umlctl ps`

List instances. Docker-ps-shaped.

| Flag | Value | Default | Purpose |
|------|-------|---------|---------|
| `--all, -a` | — | off | Include stopped instances |
| `--quiet, -q` | — | off | Names only |
| `--filter, -f <k=v>` | repeatable | — | `state=running`, `profile=research`, `backend=kvm`, `label=foo=bar` |
| `--format <spec>` | go-template or `json` | human-table | Scripting |
| `--no-trunc` | — | off | Don't truncate long fields |
| `--size` | — | off | Include RSS column (one /proc read per row) |

Default output:

```
NAME        PID      BACKEND   PROFILE    MEM   UPTIME   STATE
research-1  4210338  seccomp   research   512M  00:14m   running
fuzz-a      —        —         fuzz        64M  —        stopped
```

`--all` lists only instances whose manifest still exists
(not every entry in history.jsonl — that'd be an audit
log, not a list). History is for post-mortem, not
listing.

Filters are AND-combined, evaluated left-to-right.

`--json` or `--format json` → NDJSON on stdout (one
object per line). Matches `docker --format '{{json .}}'`
and `podman` default.

Exit codes: 0 (always) · 2 bad filter syntax.

### `umlctl logs <name>`

Tail the per-instance log.

| Flag | Value | Default | Purpose |
|------|-------|---------|---------|
| `--follow, -f` | — | off | `tail -f` |
| `--tail <n>` | int | 0 (all) | Last N lines only |
| `--since <ts>` | RFC3339 or `10m` | — | Time filter |
| `--timestamps, -t` | — | off | Prepend host ts to each line |

Exit codes: 0 · 3 not found · 7 log file missing (never
started or `--no-log`).

## Exit-code table

Canonical across all verbs:

| Code | Meaning |
|------|---------|
| 0    | success |
| 1    | generic failure (bug) |
| 2    | CLI misuse |
| 3    | not found |
| 4    | already exists / conflict |
| 5    | still running (rm without --force) |
| 6    | not running (stop of not-running) |
| 7    | log file missing |
| 124  | timeout (matches `timeout(1)`) |

## Name resolution

- Positional `<name>`; no UUIDs, no abbreviations, no
  short-prefix match (podman-style).
- Validation at `create` time: `^[a-z0-9][a-z0-9_.-]{0,63}$`.
- Collision detection via filesystem: `create` fails
  if `$STATE/instances/<name>.toml` exists (unless
  `--force`).
- No rename verb. Mutations = rm + create.

## Concurrency

- Per-instance `flock(LOCK_EX)` on `$RUNTIME/<name>.pid`
  during `start` / `stop`. Prevents two `umlctl start
  foo` from racing.
- `ps` / `logs` are read-only; no locks needed.
- `rm` takes the lock (to block against concurrent
  `start` / `stop`).

## Forward-compat / migration

- `schema_version` in the TOML. v1 writes `schema_version
  = 1`. Future versions' readers check before parsing.
- `v1` manifests are read-only-compatible with v2+; a
  missing optional field = default.
- If we ever need to break the schema, `umlctl migrate`
  reads old manifests, writes new ones. Today no migration
  tool; schema is frozen at 1 for v1.

## Shape of the implementation (v1)

Lives in `tools/uml/uml-launcher/` as a sibling binary.
Shares the crate's Cargo.toml + dependency graph
(`clap`, `figment`, `serde`, `shared_child`, `signal-hook`,
`tracing`, `tracing-subscriber`, `anyhow`).

New modules (all in `tools/uml/uml-launcher/src/`):

- `umlctl_main.rs` — binary entrypoint: `[[bin]] name = "umlctl"`.
- `manifest.rs` — TOML schema, create/read/delete.
- `paths.rs` — XDG resolution, `--state-dir` /
  `--runtime-dir` override plumbing.
- `registry.rs` — list manifests, join against
  `/proc/<pid>/stat`, format for `ps`.
- `supervise.rs` — start (fork + ready-wait), stop
  (signal + waitpid + zombie-reap).
- `history.rs` — append-only `history.jsonl` writer.

Tests: `tools/testing/selftests/um/umlctl-smoke/` does
the full lifecycle: `umlctl create`, `start` against a
research-profile kernel, `ps` verifies it's listed,
`stop` terminates it, post-check asserts zero UML
processes owned by the test user remain. Drives out the
runaway-UML bug.

## Deferred v2+ shape (preview only)

`umlctl top` lands when ratatui is added as a dep; the
parking-lot entry at `04-uml-launcher-tui.md` has the
shape.

`umlctl stat <name>` lands when we add a control socket —
reads `um_backend->name/kind/contract_version` via the
socket and formats them alongside `/proc/<pid>/stat` data.
That's `06-uml-mcp.md`-adjacent; same socket machinery.

`umlctl attach <name>` — console takeover. Needs
launcher-side cooperation (console-fd bookkeeping). v2.

## Cross-references

- `08-future-phases/README.md` — dependency graph for
  the broader tooling cluster.
- `04-uml-launcher-tui.md` — v2 ratatui dashboard
  (depends on umlctl v1 registry).
- `06-uml-mcp.md`, `11-uml-api-daemon.md` — v3+ consumers
  of the v1 registry + future control socket.
- `tools/uml/uml-launcher/` — crate layout umlctl
  extends.
- C-09 `tools/testing/selftests/um/snapshot-smoke/` +
  C-10 v2 — existing invocations that `umlctl` standardizes.
