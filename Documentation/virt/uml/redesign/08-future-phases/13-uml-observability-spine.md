# uml-observability-spine — the unifying telemetry architecture for UML tooling

**Status:** PROPOSED — future phase. Parking lot (2026-04-23).
Phase O1.1 (run_id + bundle dir + boot-offset clock) and
O1.3 (schema registry + events.jsonl) landed 2026-04-23
as the umlctl follow-on; remaining O1 sub-lifts + O2-O6
remain parking-lot. Not in the A/B/C/D plan. This memo is
an **architectural spine**, not a shippable tool — it
specifies the shared schema + transport + bundle format
that the seven already-proposed observability tools in this
directory depend on but do not individually own.

**Companions / consumers (existing memos that this spine
feeds):**

- `04-uml-launcher-tui.md` — ratatui dashboard
- `05-umlctl.md` — v1 lifecycle CLI (reserved v3 control socket)
- `06-uml-mcp.md` — MCP server for AI-agent consumers
- `09-uml-diff.md` — kernel-pair semantic diff
- `10-uml-record-replay.md` — deterministic capture + replay
- `11-uml-api-daemon.md` — REST API daemon
- `12-uml-perfetto-trace.md` — ftrace → Perfetto converter

Without the spine, each of the seven implements its own
half-telemetry and they never correlate. With the spine,
they are verbs on a common substrate: one run-id, one
clock, one schema, one bundle.

## Motivation

The 2026-04-22 tooling batch + the umlctl v1 work shipped
2026-04-23 left an architectural gap visible only once all
the pieces were on the table:

- Every consumer tool reinvents "what does one run of UML
  look like as data" — record/replay has `.umlrec`, Perfetto
  has `.perfetto-trace`, umlctl v1 has `history.jsonl`,
  dashboards have whatever they scrape live. None of these
  share a run-id, a clock, or a schema.
- The C workstream ships rich kernel-side instrumentation
  (KASAN, KFENCE, KCSAN, KMSAN, UBSAN, ftrace, kprobes,
  BPF) — but every one of those signals surfaces today as
  printk lines that downstream tools regex. Sanitizer
  splats are first-class events; treating them as log text
  is leaving capability on the floor.
- Host-visible state (`/proc/<pid>/*`, cgroup v2, host-
  side eBPF observing the UML pid from outside) is barely
  captured anywhere. `umlctl ps --size` reads one field of
  one file; that's it.
- Guest-internal state needs a transport to reach the host.
  The C-10 v2 launcher has a vhost-user backend stack;
  nothing proposes a vhost-user-trace ring that streams
  guest tracefs output to the host without cooperating
  with qemu-style devices we don't need.
- Selftests today grep dmesg. That's not an assertion
  surface; it's a convention. "No KCSAN race fired during
  this test" should be `umlctl assert --no-kcsan`, not
  `grep -q 'BUG: KCSAN' dmesg && exit 1`.

The spine solves all five by treating a run of UML as a
structured **bundle** with declared schemas for events,
metrics, traces, and logs, tied together by a common
**run-id + boot-offset-ns** clock.

## First principles

1. **Three surfaces, one timeline.** Host-visible, guest-
   kernel, and guest-userspace all time-align on a
   boot-offset-ns clock. "What was the kernel doing when
   userspace blocked" becomes a single query against one
   index, not a three-way correlation exercise across three
   file formats.
2. **Structured, not scraped.** Every signal has a stable
   schema: `{instance, run_id, host_ts_ns, guest_ts_ns,
   schema, level, fields}`. OpenTelemetry OTLP / OpenMetrics
   on the wire so off-the-shelf backends (Grafana, Tempo,
   Loki, Jaeger, Prometheus) consume it without adapters.
3. **Pull for host-observable, push for guest-internal.**
   `/proc` + cgroup scrape on interval. Guest ring-buffers
   stream from a guest-agent with backpressure (drop-with-
   gap-marker rather than stall the guest).
4. **Zero-config baseline, opt-in for expensive signals.**
   Out of the box: host /proc deltas, cgroup counters,
   kernel console, structured event log. Heavy signals
   (function_graph, full-syscall tracepoints, BPF aggregators)
   turn on per-instance per-window via explicit CLI.
5. **Ride UML's existing determinism substrate.** The
   research profile already enables
   `CONFIG_UML_TIME_TRAVEL_SUPPORT=y`; guest_ts_ns is
   already monotonic + reproducible. Don't reinvent.
6. **Diff is the headline query.** UML's value proposition
   is regression-hunting. "This run vs the last clean one,
   alert on outliers" is the primary UX; tailing is the
   secondary UX. Tools that cannot be differenced are tools
   you eventually stop looking at.

## Architecture

### The clock

Every host-side and guest-side emitter tags events with
two timestamps:

- `host_ts_ns` — `clock_gettime(CLOCK_BOOTTIME)` on the
  host, monotonic since host boot.
- `guest_ts_ns` — `ktime_get_boottime_ns()` inside the
  guest, monotonic since guest boot.

umlctl records `{host_ts_ns_at_exec, guest_ts_ns_epoch=0}`
at the moment `execve` returns in the UML child. Events
from both sides normalize against those zeros. On x86 a
TDSC-aligned fast path brings correlation under 1 µs.

Under time-travel mode, guest_ts_ns is deterministic +
replayable, which means two replays of the same recorded
run produce byte-identical event streams on the guest-side
axis. Host-side events (scrapes, host eBPF) remain
wall-clock-variable.

### The bundle

One run of UML → one directory, one archive:

```text
$STATE/runs/<run_id>/
├── manifest.toml         # the umlctl manifest that spawned this run
├── run.json              # run_id, clocks_at_exec, host info, exit_status
├── events.jsonl          # structured events (one JSON per line, schema'd)
├── metrics.openmetrics   # scrape history (text exposition format)
├── kernel.log            # kernel console (separated from init stdout)
├── init.log              # init + userspace stdout
├── dmesg.jsonl           # /dev/kmsg tail with facility/level/seq preserved
├── trace.perfetto        # ftrace + tracepoints in Perfetto format (optional)
├── bpf/                  # BPF program outputs (optional)
│   └── <name>.jsonl
├── proc/                 # periodic /proc snapshots (optional)
│   ├── 0000.tar.zst
│   └── 0001.tar.zst
└── inputs.umlrec         # record/replay inputs (if recorded)
```

`run_id` format: ULID (lexicographic + time-sorted). One
umlctl instance may have many runs over time; the
`instance` field ties them together.

A bundle is self-describing: `run.json` declares which
optional components are present. Missing components are
fine (depending on profile / opt-ins).

### The transports

**Host → host (scrape):**

- Per-instance `/metrics` endpoint on
  `$RUNTIME/<name>.metrics.sock` — OpenMetrics text
  exposition, Prometheus-compatible. Scraped by
  `uml-observe` (or any Prometheus).
- `/proc/<pid>/{stat,status,io,schedstat,smaps_rollup,
  wchan}` parsed on scrape interval, exposed on the
  same /metrics endpoint.
- cgroup v2 `cpu.stat`, `memory.{current,peak,events}`,
  `io.stat`, `pids.current` for the UML pid's slice.
- **Host-side eBPF** attached to the UML pid from
  outside the guest: off-CPU flame graph, scheduler
  wakeup-latency histogram, per-syscall latency. For
  KVM backend, `kvm:*` tracepoints give exit-reason
  histograms at zero guest cost. For seccomp backend,
  `seccomp:*` hits. For ptrace backend, `signal:*` +
  `syscalls:*`. This is where per-backend observability
  becomes automatic — no guest cooperation required.

**Guest → host (stream):**

- **vhost-user-trace** — a new vhost-user backend class
  alongside v2's block/net/console backends. The guest
  kernel writes to a virtio-tracebuf device; the backend
  mmaps the shared ring and forwards to
  `$STATE/runs/<run_id>/events.jsonl` +
  `trace.perfetto`. Zero-copy on the hot path; drop-with-
  gap-marker under backpressure.
- **Fallback transport** when vhost-user-trace isn't
  compiled: a UNIX socket on a hostfs mount, owned by a
  tiny guest-agent (`umld` — single static binary, no
  deps) that reads `/dev/kmsg` + tracefs + proc and
  writes to the socket. Slower, higher CPU, but works
  today.

**Control plane:**

- `$RUNTIME/<name>.sock` — the control socket reserved
  in umlctl v3. Commands: `trace-start`, `trace-stop`,
  `event-emit`, `proc-snapshot`, `metrics-pull`. Guest-
  agent speaks the same protocol; host requests travel
  guest-ward through vsock or the hostfs fallback.

### The schema

Events follow the Elastic Common Schema (ECS) shape,
trimmed:

```json
{
  "@timestamp": "2026-04-23T21:06:04.077892990Z",
  "host_ts_ns": 1234567890123,
  "guest_ts_ns": 41187000,
  "run_id": "01HW5TSP9C7JMX3QZZZZZZZZZ",
  "instance": "research-1",
  "schema": "uml.sanitizer.kasan.v1",
  "event.category": "sanitizer",
  "event.severity": "error",
  "event.action": "use-after-free",
  "kasan": {
    "access_size": 8,
    "access_type": "read",
    "allocation_pc": "0xffffffff81234567",
    "free_pc": "0xffffffff81234abc",
    "stack": ["..."]
  }
}
```

Schemas are versioned (`.v1`, `.v2`) and frozen once
shipped. Adding fields is additive; removing/renaming is
a version bump. `umlctl schema` lists every declared
schema with its field doc strings.

Canonical schemas (initial set — not exhaustive):

| Schema | Source | When it fires |
|---|---|---|
| `uml.lifecycle.v1` | umlctl | create/start/stop/rm |
| `uml.boot.initcall.v1` | guest kernel | `initcall_debug=y` |
| `uml.sanitizer.kasan.v1` | guest kernel | KASAN report |
| `uml.sanitizer.kfence.v1` | guest kernel | KFENCE report |
| `uml.sanitizer.kcsan.v1` | guest kernel | KCSAN race report |
| `uml.sanitizer.kmsan.v1` | guest kernel | KMSAN origin |
| `uml.sanitizer.ubsan.v1` | guest kernel | UBSAN warning |
| `uml.oom.v1` | guest kernel | OOM kill |
| `uml.rcu_stall.v1` | guest kernel | RCU stall detector |
| `uml.lockdep.v1` | guest kernel | lockdep splat |
| `uml.watchdog_stall.v1` | guest kernel | softlockup/hardlockup |
| `uml.panic.v1` | guest kernel | panic() |
| `uml.test.hook.v1` | guest userspace | `umlctl event emit …` |
| `uml.host.sched_lat.v1` | host eBPF | scheduler latency bucket |
| `uml.host.kvm_exit.v1` | host eBPF | KVM exit reason counted |

Sanitizer events in particular: these are today printk
lines. Promoting them to schema'd events unlocks
`umlctl assert --no-kasan` + selftest integration without
regex fragility.

## Query / UX surface

Everything in this list is driven by the spine; most are
already parking-lot memos, marked inline with which memo
owns the UX.

```text
umlctl top                          # [04-tui] live dashboard, sparklines per instance
umlctl stat <name>                  # [spine] one-shot deep snapshot: proc + metrics + last 100 events
umlctl dmesg <name> [-f]            # [spine] kernel-only console readback (separated from init)
umlctl events <name> [-f] \
    --filter event.category=sanitizer    # [spine] structured event log tail
umlctl trace <name> --events sched:* \
    --duration 10s --out t.perfetto # [12-perfetto] ftrace → perfetto capture
umlctl metrics <name>               # [11-api-daemon] OpenMetrics scrape
umlctl assert <name> --no-rcu-stall \
    --no-kasan --no-kcsan --no-panic  # [spine] CI assertion — exit 1 on any match
umlctl diff <runA> <runB>           # [09-diff] telemetry delta
umlctl replay <runA>                # [10-record-replay] re-run deterministically
umlctl export <run_id> --otlp <url> # [spine] push bundle to OTLP collector
umlctl export <run_id> --bundle <path>.umlbundle  # [spine] zstd archive the bundle dir
umlctl import <path>.umlbundle      # [spine] reattach a bundle for local query
```

The headline flows that the spine enables but no individual
memo delivers:

1. **CI regression hunt.**
   ```text
   $ umlctl start research-1 --ready-timeout 30
   $ run-workload.sh
   $ umlctl stop research-1
   $ umlctl assert <run_id> --no-kasan --no-kcsan --no-rcu-stall --no-panic \
                              --max sched.wakeup_lat_p99=500us \
                              --max mem.peak=300M
   ```
   Fails CI on any regression. Compositional: every assertion
   maps to a schema + predicate.

2. **Fleet diff.**
   ```text
   $ umlctl diff --baseline 'last-20-clean-runs-of(research-1)' <new_run_id>
   + new event schema:uml.sanitizer.kcsan.v1 (1 occurrence)
   ~ metric sched.wakeup_lat_p99: 220us → 480us (+118%, outside 3σ of baseline)
   ```
   Anomaly detection over a trailing window — the thing no
   single consumer memo builds.

3. **Post-mortem share.**
   ```text
   $ umlctl export <run_id> --bundle bug-42.umlbundle
   $ # upload bug-42.umlbundle to bugtracker
   # Recipient:
   $ umlctl import bug-42.umlbundle
   $ umlctl events bug-42 --filter event.category=sanitizer
   $ umlctl replay bug-42        # re-runs deterministically on their host
   ```
   The bundle is the reproducer. Self-contained, shareable,
   replayable.

## What exists vs what this spine adds

Already in the roadmap (nothing new needed):

- C workstream: every sanitizer + tracing + kprobe source.
  The raw signals exist; this spine promotes them to
  events.
- Research profile: `CONFIG_UML_TIME_TRAVEL_SUPPORT=y`,
  determinism for `guest_ts_ns` + replay.
- C-10 v2 launcher: vhost-user device plumbing —
  `vhost-user-trace` is one more backend class in the
  same stack.
- C-08 `/sys/kernel/um/state_version`: the stable guest
  surface; this spine's guest-agent speaks it.
- umlctl v1 control socket reservation
  (`$RUNTIME/<name>.sock`): the spine's control plane.

New / missing / this spine's contributions:

1. **`run_id` + boot-offset-ns clock normalization.** Not
   in any memo; half-implied by record/replay.
2. **The bundle directory format + `.umlbundle` archive.**
   Unifies what today is scattered across `.umlrec`,
   `.perfetto-trace`, `history.jsonl`, ad-hoc log files.
3. **Declared event schemas (`uml.*.v1`).** The ECS-
   shaped JSON + the registry. Turns sanitizer/lockdep/
   RCU splats into first-class events.
4. **Host-side eBPF attached to the UML pid.** Per-
   backend observability (KVM exit reasons, seccomp
   filter hits, ptrace stops) for free, no guest
   cooperation.
5. **Split kernel console from init stdout.** Cheap
   (~30 LOC in `supervise.rs`): open a second log file,
   pass `console=fd:N` to the kernel. Unblocks
   `umlctl dmesg` vs `umlctl logs --init`.
6. **vhost-user-trace backend.** New device class in
   C-10 v2's existing stack.
7. **Guest-agent `umld`.** Static binary for the
   non-vhost fallback path. Reads `/dev/kmsg` + tracefs
   + proc, writes to a UNIX socket.
8. **`umlctl assert`.** First-class CI assertion verb
   over events + metrics. Replaces dmesg-grep.
9. **`umlctl diff --baseline`.** Anomaly detection over
   a trailing window. Paired with 09-diff's binary-
   diff, gives both axes.
10. **OTLP / OpenMetrics emission.** Off-the-shelf-backend
    compatibility (Grafana/Loki/Tempo/Prometheus) without
    per-backend adapters.
11. **Schema registry + `umlctl schema` verb.** Self-
    describing — new consumers learn the surface without
    reading code.

Items 1-3 are the *spine itself*. Items 4-11 are what the
spine unblocks — each can ship as its own workstream once
the spine is frozen.

## Security posture

- Guest cannot forge host-side events: host `host_ts_ns`
  + host eBPF signals are emitted on the host side,
  never trusted from the guest.
- Host cannot inject guest-side events: the guest-agent
  is one-way (guest → host). Host control requests go
  through the control socket and are explicitly
  acknowledged.
- Bundle export is opt-in: `umlctl export` never auto-
  uploads; operators must name the destination.
- OTLP endpoints honor mTLS when configured; no default
  anonymous egress.
- Guest-agent runs as non-root inside the guest where
  possible (some tracefs operations require CAP_SYS_ADMIN
  — that's a guest-side policy decision, not a spine
  requirement).

## Performance posture

- Baseline capture (proc scrape + cgroup + kernel console
  + events): target <1% host CPU overhead, negligible
  guest overhead.
- Heavy signals (function_graph, full-syscall
  tracepoints, per-packet net tracepoints): target
  <10% guest-wall-clock overhead at steady state; opt-in
  only, per-window.
- Backpressure: guest-agent drops oldest events (ring
  overflow → gap-marker event) rather than stalling the
  guest. Gap markers are themselves schema'd events so
  you know when you're missing data.
- Bundle size: zstd-compressed, ~10 MB for a 60-second
  baseline run, ~100 MB with function_graph on.
- Sampling knobs: every heavy emitter has an
  `every_n`/`rate_hz`/`max_events` cap so runaway cases
  self-throttle.

## Phasing

This spine is a multi-quarter body of work. A plausible
landing sequence (each phase is independently useful):

**Phase O1 — format freeze + cheap wins (1-2 weeks):**

Split into independently-landable sub-lifts; bracketed
status trails each line (**[LANDED]** / **[pending]**).

- O1.1 — run_id + bundle directory format + boot-offset-ns
  clock in umlctl. Teach umlctl start to mint a ULID,
  create `$STATE/runs/<run_id>/`, record
  `host_ts_ns_at_exec` (CLOCK_BOOTTIME), finalize
  `run.json` on stop. **[LANDED 2026-04-23]**
- O1.2 — Split kernel console from init stdout in umlctl
  supervise.rs. `umlctl dmesg` verb. **[pending]**
- O1.3 — Schema registry + event-emission library.
  Initial schemas: `uml.lifecycle.v1` (emitted),
  `uml.panic.v1` + `uml.oom.v1` (declared, producers land
  with O1.2's dmesg parser). umlctl emits its own
  lifecycle into `events.jsonl`. Lives inline in
  `tools/uml/uml-launcher/src/bin/umlctl/{events,schema}.rs`
  for now; extraction to a standalone
  `tools/uml/uml-observe/` crate waits for a second
  consumer. **[LANDED 2026-04-23]**
- O1.4 — `umlctl export --bundle` → `.umlbundle.tar.zst`.
  **[pending]**
- O1.5 — `umlctl events <name> --filter ...` verb.
  **[pending]**
- O1.6 — `umlctl assert <name> --no-kasan ...` CI-style
  predicates over events. Replaces dmesg-grep in
  selftests. **[pending]**

**Phase O2 — host-side observability (2-4 weeks):**

- `/proc/<pid>/*` + cgroup scraper.
- Host-side eBPF skeleton (off-CPU, wakeup-lat, KVM
  exits for KVM backend).
- OpenMetrics endpoint per instance.
- `umlctl metrics` verb.

**Phase O3 — sanitizer promotion (2-3 weeks):**

- dmesg parser converts KASAN/KFENCE/KCSAN/KMSAN/UBSAN
  printk lines to schema'd events (transitional path
  while kernel-side native emission catches up).
- Upstream-direction: add a tracepoint to each sanitizer's
  "report" path so native event emission is a future
  kernel change, not a permanent parser.
- `umlctl assert` verb.

**Phase O4 — guest-agent + transport (4-8 weeks):**

- `umld` static binary, hostfs-socket fallback transport.
- vhost-user-trace backend class.
- Live `/dev/kmsg` readback + tracefs stream.
- `umlctl events --follow` end-to-end.

**Phase O5 — diff + baseline (2-4 weeks):**

- `umlctl diff --baseline` with trailing-window anomaly
  detection.
- Integration with `09-uml-diff` for binary-pair mode.
- `umlctl replay` integration with `10-uml-record-replay`.

**Phase O6 — ecosystem integration (ongoing):**

- OTLP exporter.
- Perfetto importer for tracefs (merges `12-uml-
  perfetto-trace`'s scope into the spine).
- MCP server exposes the schema registry +
  `umlctl events` over JSON-RPC (merges `06-uml-mcp`'s
  scope into the spine).

Phases O1-O3 are tractable inside the current A/B/C/D
plan's cadence; O4+ benefits from the C-10 v2 launcher
stack being fully landed and a small window of "no
in-flight launcher refactor" to add the trace backend
class cleanly.

## Cross-references

- Parent plan: `08-future-phases/README.md` (parking-lot
  index).
- Existing consumers: memos 04, 05, 06, 09, 10, 11, 12
  (this directory).
- Host-side instrumentation source: workstream C's
  profile defconfigs (`02-workstreams/C-profiles-and-
  gaps/`).
- Control-socket reservation: `05-umlctl.md` §"Deferred
  v2+ shape".
- Time-travel determinism: research profile defconfig
  + `10-uml-record-replay.md`.
- vhost-user backend stack: `tools/uml/uml-launcher/`
  C-10 v2 commit series + decisions-log D52.

## Out of scope for this memo

- No commitment to a specific OTLP collector, Grafana
  dashboard schema, or log backend. The spine emits
  standards; consumers pick backends.
- No commitment to a Rust vs Go vs C guest-agent
  implementation. Phase O4 picks.
- No live-migration, remote-operation, or SSH-forwarding
  scope. Those remain "different project" per `05-
  umlctl.md`.
- No cloud / SaaS component. Local-first throughout.

## Status flip criteria

This memo leaves "PROPOSED" only when:

1. A workstream owner commits to Phase O1 scope (format
   freeze + cheap wins) inside a concrete quarter.
2. Decisions-log entry records the commit with owner +
   target date.
3. `uml-observe` crate exists under `tools/uml/uml-
   observe/` with the schema registry skeleton.

Until then: this is a reference for anyone writing one
of the seven consumer memos, so their independent work
converges rather than diverges.
