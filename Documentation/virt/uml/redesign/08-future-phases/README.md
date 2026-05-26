# Future Phases / Parking Lot

This directory captures **useful but not yet committed** work beyond
the current A/B/C/D roadmap.

Purpose:

- Keep end-user-facing ideas from getting lost while workstreams A-D
  stay focused.
- Distinguish clearly between:
  - **planned now** — already covered by A-D,
  - **partially planned** — gestured at, but not fully scoped,
  - **missing** — not meaningfully on the roadmap yet.
- Give future planning passes a place to start from when the current
  critical path is stable.

Rules:

- Nothing in this directory is a commitment.
- Notes here should be framed from the point of view of an end user,
  operator, or contributor, not only from the point of view of kernel
  internals.
- If an item graduates into a real workstream task, move it out of the
  parking lot and into the main plan.

### Boundary with workstream C (and the rest of the A–D plan) — D24

A parking lot that quietly re-plans the next workstream is a failure
mode: ideas here accumulate, then collide with C's scope once C
starts, and the team re-debates decisions that were already made.

Policy: **items in this directory begin strictly where workstream C
ends.** That means:

- Any item that fits C's existing task list (any `C-NN.md` spec)
  is a C task, not a future phase. If a note here describes work
  that overlaps C, the scope either folds into C or the note is
  narrowed to the slice C explicitly defers.
- When scope is ambiguous, it moves *to* C first. Future phases
  inherit only what C has explicitly written off ("C-NN.md out of
  scope: …").
- "Phase E wants X" does not override "C plans X" — C wins by
  default; phase E takes over only if C's owner says so in writing.
- Adding a new item here requires a one-line check against C's
  task list; if it matches, reclassify the note as a C proposal
  and move the idea to `02-workstreams/C-profiles-and-gaps/`
  instead.

Logged as D24 in `04-risks/decisions-log.md`.

## Notes

### Existing entries (pre-2026-04-22)

- [01-end-user-ideal-world.md](01-end-user-ideal-world.md) — external
  expectations, ideal end-user UX, and the biggest gaps between that UX
  and the current roadmap.
- [02-snapshot-to-disk.md](02-snapshot-to-disk.md) — the v2 design for
  snapshot-to-disk / resume / review beyond C-09 v1's in-memory fork-
  server. Chosen format is ELF64 core dump + UML `PT_NOTE` types; opens
  when D35's revisit triggers fire.
- [03-uml-fuzz-rust-companion.md](03-uml-fuzz-rust-companion.md) — a
  Rust, in-tree, syzlang-compatible companion fuzzer under
  `tools/fuzz/uml-fuzz/`. Positions itself around two capabilities
  syzkaller-as-is doesn't exploit: direct C-09 forkserver integration
  (speed wedge) and UML time-travel / record-replay integration
  (research-capability wedge). Opens only after C-08 lands upstream
  and produces measured data on its limits. See D48.

### Tooling / UX cluster (2026-04-22 batch)

Nine end-user tooling ideas captured this session. Common
theme: they all sit on top of the landed + in-flight
redesign infrastructure (forkserver, decomposed backends,
tracing, time-travel) and expose it through a modern
developer-ergonomics layer. None are commitments; all are
parking-lot designs for future prioritization.

- [04-uml-launcher-tui.md](04-uml-launcher-tui.md) — ratatui
  dashboard: htop-style multi-instance view with per-backend
  PIDs, seccomp / LSM status, metrics. Sits on top of C-10 v2
  orchestration + umlctl state. ~2-3 focused days.
- [05-umlctl.md](05-umlctl.md) — multi-instance lifecycle
  CLI (create / start / ps / logs / stop / snapshot / rm)
  with TOML manifests under `~/.local/state/uml/`. The
  missing piece between C-10 v1's single-shot `run` and
  real operator workflows. ~1-2 weeks.
- [06-uml-mcp.md](06-uml-mcp.md) — Model Context Protocol
  server exposing UML lifecycle + introspection to LLM
  agents. Concrete realization of the vision doc's "LLMs
  iteratively experiment" line; the forkserver's <50ms
  iteration is the speed wedge. ~2-3 weeks.
- [07-uml-bisect.md](07-uml-bisect.md) — unattended
  `git bisect run` wrapped around the existing boot-matrix
  + kprobes-stress + Q1 scripts. UML's fast-boot turns
  bisections from hour-scale to minutes. ~2-3 days shell,
  +1 week to promote to Rust.
- [08-uml-probe-web.md](08-uml-probe-web.md) — axum-based
  web server exposing a guest's tracefs + debugfs over
  HTTP/SSE with a browser UI. Share live traces with a
  colleague via a signed URL. ~1-2 weeks.
- [09-uml-diff.md](09-uml-diff.md) — semantic diff between
  two UML boots. Generalizes `scripts/uml-cross-backend.sh`
  from PTRACE_ONLY-vs-SECCOMP_ONLY to arbitrary (kernel,
  args, workload) pairs. Primary consumer: 07 as a bisection
  criterion. ~1-2 weeks.
- [10-uml-record-replay.md](10-uml-record-replay.md) —
  deterministic record + replay on top of
  `CONFIG_UML_TIME_TRAVEL_SUPPORT`. Shareable bug repros,
  time-travel debugging. ~3-4 weeks.
- [11-uml-api-daemon.md](11-uml-api-daemon.md) — systemd
  user-service REST API exposing umlctl's verbs for
  external orchestrators (k8s, nomad, nix). Rootless + mTLS
  posture. ~3-4 weeks.
- [12-uml-perfetto-trace.md](12-uml-perfetto-trace.md) —
  thin converter from tracefs (`function_graph`, kprobes)
  to Perfetto JSON trace format. Suddenly every researcher
  has a browser-based timeline / flame-graph view of a UML
  boot. Lowest-cost, highest-leverage of the nine
  (~1-2 days).

### Observability architecture (2026-04-23)

- [13-uml-observability-spine.md](13-uml-observability-spine.md) —
  the unifying telemetry architecture the seven tools in
  the 2026-04-22 batch implicitly depend on but none
  individually own. Declares `run_id` + boot-offset-ns
  clock, schema registry (`uml.*.v1` ECS-shaped events),
  bundle directory format (`.umlbundle`), vhost-user-trace
  transport + hostfs-socket fallback, OTLP/OpenMetrics
  emission. Without the spine, every consumer memo
  reinvents half of it and they never correlate; with it,
  memos 04/05/06/09/10/11/12 become verbs on a common
  substrate. Multi-quarter; phases O1-O6. Not a
  commitment.

### Networking architecture (2026-05-17)

- [14-uml-vector-driver-v2.md](14-uml-vector-driver-v2.md) —
  complete rewrite plan for UML vector networking: typed config,
  explicit lifecycle and queue state machines, host/transport ops
  split, SMP/multiqueue design, formal-model hooks, and a clear
  security split between trusted in-process vector networking and
  UML v2 sandbox helper networking. Not a commitment.
- [15-uml-vector-driver-v2-buildout.md](15-uml-vector-driver-v2-buildout.md) —
  concrete buildout plan for turning the vector v2 foundations into a
  real netdev driver: runtime Kconfig, command-line registration,
  `net_device_ops`, fd/TAP host backends, TX/RX datapath, multiqueue,
  sandbox policy, replacement gates, and validation matrix. Not a
  commitment.
- [16-uml-vector-legacy-tap-crash-r0.md](16-uml-vector-legacy-tap-crash-r0.md) —
  R0 note for the legacy TAP NULL dereference found during Tier 3
  Django smoke testing: reproducer, root cause, queue-optional fix,
  and validation commands. Not a commitment.
- [17-uml-vector-driver-v2-r1-runtime-skeleton.md](17-uml-vector-driver-v2-r1-runtime-skeleton.md) —
  R1 implementation note for the experimental vector v2 runtime
  skeleton: Kconfig, v2-only command-line collection, late-init typed
  config validation, sandbox policy, build matrix, manual boots, and
  KUnit evidence. Not a commitment.
- [18-uml-vector-driver-v2-r2-netdev-skeleton.md](18-uml-vector-driver-v2-r2-netdev-skeleton.md) —
  R2 implementation note for the inspectable vector v2 netdev
  skeleton: `net_device_ops`, forced single-queue registration,
  read-only ethtool driver info, clean `-EOPNOTSUPP` open unwind,
  manual runtime evidence, build matrix, and KUnit coverage. Not a
  commitment.
- [19-uml-vector-driver-v2-r3-fd-backend.md](19-uml-vector-driver-v2-r3-fd-backend.md) —
  R3 implementation note for the trusted direct-fd backend: fd
  duplication ownership, channel lifecycle attach/close, fd-mode
  `ip link up/down`, sandbox rejection of raw `fd=`, build matrix,
  manual runtime evidence, and KUnit coverage. Not a commitment.
- [20-uml-vector-driver-v2-r4-tap-backend.md](20-uml-vector-driver-v2-r4-tap-backend.md) —
  R4 implementation note for the trusted TAP backend: `/dev/net/tun`
  open through the v2 host boundary, TAP fd ownership, channel lifecycle
  attach/close, TAP-mode `ip link up/down`, sandbox rejection of
  direct `ifname=`, `strace` sandbox evidence, build evidence, and
  KUnit coverage. Not a commitment.
- [21-uml-vector-driver-v2-r5-tap-datapath.md](21-uml-vector-driver-v2-r5-tap-datapath.md) —
  R5 implementation note for the first trusted TAP packet path: v2
  queue ownership, NAPI/read-IRQ channel ownership, TAP TX/RX host ops,
  vnet-header normalization, guest-to-host ping evidence, known
  replacement blockers, and KUnit coverage. Not a commitment.
- [22-uml-vector-driver-v2-r6-ethtool-hardening.md](22-uml-vector-driver-v2-r6-ethtool-hardening.md) —
  R6 implementation note for ethtool hardening: stopped-safe stats,
  ring policy, coalesce policy reporting, TAP write-IRQ wakeups,
  repeated trusted TAP up/ping/down evidence, sandbox rejection
  evidence, and KUnit coverage. Not a commitment.
- [23-uml-vector-driver-v2-r7-umlctl-tier3-integration.md](23-uml-vector-driver-v2-r7-umlctl-tier3-integration.md) —
  R7 implementation note for `umlctl` and Tier 3 integration:
  `network.driver = "vector2"`, `--network-driver`,
  `--sweep network.driver=...`, dry-run network plans, soak v2
  aliases, v2 metadata, teardown hardening, seccomp smoke and 30/30
  repetition evidence, KUnit evidence, and the remaining kvm-v2
  readiness gap. Not a commitment.
- [24-uml-vector-driver-v2-umlctl-usability.md](24-uml-vector-driver-v2-umlctl-usability.md) —
  R7 follow-up note for the operator-facing `umlctl`/vector v2
  contract: guest `UMLCTL_NETWORK_*` metadata, driver-neutral workload
  phases, ready-timeout `run_id`/`init_log` correlation, gate-loop
  failed-start log preservation, and practical comparison commands.
  Not a commitment.
- [25-uml-vector-driver-v2-r8a-tap-multiqueue.md](25-uml-vector-driver-v2-r8a-tap-multiqueue.md) —
  R8 partial implementation note for trusted TAP multiqueue: parsed
  `queues=N` reaches netdev registration, TAP opens one channel/fd/NAPI
  pair per queue, TX maps by skb queue, `umlctl` exposes
  `[network] queues` / `--network-queues`, and seccomp multiqueue smoke
  evidence is recorded. Not a commitment.
- [26-uml-vector-driver-v2-r8b-queue-observability.md](26-uml-vector-driver-v2-r8b-queue-observability.md) —
  R8 partial implementation note for per-queue observability:
  dynamic `ethtool -S` queue stat names, stopped/running stable stat
  shape, KUnit coverage, live seccomp `queues=2` ethtool evidence, and
  a small parallel traffic smoke where both TAP queues move.
  Not a commitment.
- [27-uml-vector-driver-v2-fd-datapath.md](27-uml-vector-driver-v2-fd-datapath.md) —
  R5 follow-up implementation note for direct-fd packet movement:
  shared runtime queue helpers, raw Ethernet TX/RX over inherited fds,
  fd NAPI/IRQ startup, KUnit coverage, no-root manual ARP/ICMP ping,
  and launcher-owned fd multiqueue smoke evidence. Not a commitment.
- [28-uml-vector-driver-v2-r7-seccomp-30of30.md](28-uml-vector-driver-v2-r7-seccomp-30of30.md) —
  R7 partial validation note for vector2 TAP under seccomp:
  Tier 3 Django stdlib-shim `PASS=30/30`, teardown evidence, and the
  remaining kvm-v2 readiness limitation. Not a commitment.
- [29-uml-vector-driver-v2-completion-audit.md](29-uml-vector-driver-v2-completion-audit.md) —
  Current completion audit mapping the buildout definition of done to
  concrete artifacts, validation evidence, and remaining replacement
  blockers. Not complete.
- [30-uml-vector-driver-v2-umlctl-fd-handoff.md](30-uml-vector-driver-v2-umlctl-fd-handoff.md) —
  R7/R5 follow-up note for launcher-owned vector2 fd handoff:
  `network.host_mode`, default fd selection, fd range manifest labels,
  dry-run shape, sandbox boundary, live single-queue and multiqueue
  `umlctl up` smoke evidence, and remaining SMP/perf gaps. Not a
  commitment.
- [31-uml-vector-driver-v2-r8c-queue-cpu-policy.md](31-uml-vector-driver-v2-r8c-queue-cpu-policy.md) —
  R8 follow-up note for explicit vector2 queue-to-CPU policy:
  `ndo_select_queue`, XPS setup, modulo CPU/queue mapping rules, KUnit
  coverage, and remaining KCSAN/perf gaps. Not a commitment.
- [32-uml-vector-driver-v2-umlctl-auto-queues.md](32-uml-vector-driver-v2-umlctl-auto-queues.md) —
  R8 follow-up note for `umlctl` automatic vector2 queue sizing:
  `queues = "auto"` / `--network-queues auto`, resolution from
  `[runtime].ncpus`, resolved queue metadata, manifest labels, gate-loop
  sweep support, dry-run evidence, and remaining replacement gates. Not
  a commitment.
- [33-uml-vector-driver-v2-r8d-kcsan-auto-queue-smoke.md](33-uml-vector-driver-v2-r8d-kcsan-auto-queue-smoke.md) —
  R8 partial validation note for a short KCSAN-instrumented seccomp
  gate run of vector2 fd multiqueue through `queues = "auto"`:
  `PASS=1/1`, no TAP leak, resolved queue metadata, and no KCSAN
  data-race signatures. Not a commitment.
- [34-uml-vector-driver-v2-fastapi-uvicorn-smoke.md](34-uml-vector-driver-v2-fastapi-uvicorn-smoke.md) —
  R7/R8 validation note for a real FastAPI + uvicorn smoke under
  vector2 fd handoff on seccomp: gateway ping, `SERVER_READY`,
  `FASTAPI_HTTP ok=51 fail=0`, `VECTOR2_FASTAPI_OK`,
  `REPRO_DONE rc=0`, a short `PASS=10/10` repetition, and no TAP
  leak. Not a commitment.
- [35-uml-vector-driver-v2-r8e-kcsan-lockdep.md](35-uml-vector-driver-v2-r8e-kcsan-lockdep.md) —
  R8 follow-up for a lockdep warning found during repeated KCSAN
  validation: process-context queue users now disable bottom halves,
  vector2 KUnit passes 72/72, and the auto-queue fd multiqueue KCSAN
  gate passes `PASS=10/10` with no warning, panic, KCSAN, data-race,
  or TAP leak. Not a commitment.
- [36-uml-vector-driver-v2-perf-baseline.md](36-uml-vector-driver-v2-perf-baseline.md) —
  Initial legacy-vs-vector2 `umlctl` performance baseline: adds a
  guest-to-host TCP helper script, fixes the legacy `vec` parser so it
  leaves `vec2.` / `vec2=` for vector2 in both-drivers kernels, records
  short 32 MiB guest-to-host and host-to-guest comparisons, adds
  byte-list/repeat support, records a repeated 1 MiB/8 MiB/32 MiB
  legacy-vs-vector2 bidirectional sweep, and keeps the full performance
  gate open. Not a commitment.
- [37-uml-vector-driver-v2-sandbox-strace-audit.md](37-uml-vector-driver-v2-sandbox-strace-audit.md) —
  Focused vector2 fd-handoff sandbox audit: wires `umlctl up --strace`
  into supervised starts, records the UML tracee PID for clean
  teardown, captures a vector2 auto-queue fd boot, adds
  `umlctl gate loop --audit-vector-sandbox`, and documents a narrowed
  syscall scan with no actual host TAP open, `TUNSETIFF`, `AF_PACKET`,
  BPF, or UML helper exec from the vector host path. Not a commitment.
- [38-uml-vector-driver-v2-fd-failure-stress.md](38-uml-vector-driver-v2-fd-failure-stress.md) —
  Focused fd backend KUnit stress: repeats vector2 netdev open/stop
  1000 times, checks bad-fd `ndo_open()` unwind, repeats missing-fd
  backend failure 10,000 times, records `um_vector2_*` KUnit 75/75, and
  defers runtime lifecycle repetition to the follow-up live gate. Not a
  commitment.
- [39-uml-vector-driver-v2-lifecycle-stress-gate.md](39-uml-vector-driver-v2-lifecycle-stress-gate.md) —
  Runtime lifecycle stress harness for vector2 fd handoff: adds a
  reusable `umlctl gate loop` Umlfile that repeats live
  `ip link down/up`, verifies ethtool open/close deltas, records a
  25-cycle smoke plus a 10,000-cycle pass with clean TAP/process
  teardown, with failed-open injection closed later by checkpoint 43.
  Not a commitment.
- [40-uml-vector-driver-v2-kcsan-fastapi.md](40-uml-vector-driver-v2-kcsan-fastapi.md) —
  KCSAN workload evidence for vector2 fd handoff: runs the real
  FastAPI + uvicorn smoke under the KCSAN UML kernel, records
  `FASTAPI_HTTP ok=51 fail=0`, `VECTOR2_FASTAPI_OK`, clean TAP/process
  teardown, and no warning/BUG/KCSAN/data-race signatures. Not a
  commitment.
- [41-uml-vector-driver-v2-kcsan-concurrent-traffic.md](41-uml-vector-driver-v2-kcsan-concurrent-traffic.md) —
  KCSAN concurrent traffic evidence for vector2 fd multiqueue: adds a
  reusable host/guest TCP+UDP harness, records exact bidirectional
  TCP byte counts and UDP packet counts, verifies all four TX and RX
  queues moved across the initial pass plus three repeats, adds fixed
  two-queue/six-flow and paced eight-flow larger-volume profiles, and
  captures clean teardown with no warning/BUG/KCSAN/data-race
  signatures. Not a commitment.
- [42-uml-vector-driver-v2-fastapi-30.md](42-uml-vector-driver-v2-fastapi-30.md) —
  Longer FastAPI + uvicorn vector2 fd-handoff repetition:
  `PASS=30/30 FAIL=0 TIMEOUT=0`, all 30 runs reached `SERVER_READY`,
  `FASTAPI_HTTP ok=51 fail=0`, `VECTOR2_FASTAPI_OK`, clean teardown,
  and no warning/BUG/panic/failure signatures. Not a commitment.
- [43-uml-vector-driver-v2-failed-open-injection.md](43-uml-vector-driver-v2-failed-open-injection.md) —
  Live failed-open injection proof for vector2 fd handoff: adds the
  explicit `fail_open_after` config and `umlctl` key, verifies
  `um_vector2_*` KUnit 76/76, and records a live gate where the second
  `ip link set up` fails through `ndo_open()` with expected
  open/failure/close counters, registered state, clean TAP/process
  teardown, and no warning/BUG/KCSAN/data-race signatures. Not a
  commitment.
- [44-uml-vector-driver-v2-kvmv2-readiness.md](44-uml-vector-driver-v2-kvmv2-readiness.md) —
  KVM-v2 readiness checkpoint for vector2: rebuilds a proper
  `CONFIG_UM_BACKEND_KVM_V2=y` vector2 runtime, records no-network
  seccomp and KVM-v2 readiness passes, vector2 fd handoff on KVM-v2,
  and Django stdlib-shim smoke. One 30-run Django gate failed at
  `PASS=29/30 FAIL=1` with a guest `python3` abort, while a diagnostic
  rerun passed `PASS=30/30 FAIL=0`; a longer sample then reached only
  `PASS=57/60 FAIL=2 TIMEOUT=1` from guest Python failures before
  `SERVER_READY` plus one startup timeout. A trace-enabled rerun reached
  `PASS=59/60 FAIL=1 TIMEOUT=0` and captured
  `KVMV2T_DUMP_BEGIN reason=debugfs entries=5140` for the Python abort.
  Not a commitment.
- [45-uml-vector-driver-v2-seccomp-soak-status.md](45-uml-vector-driver-v2-seccomp-soak-status.md) —
  End-of-day vector2 seccomp Tier 3 soak status: a requested-stop run
  reached 6142/7200 seconds, 970/970 passes, Django-v2 490/490,
  FastAPI-v2 480/480, all rows using vector2 `vec2.0` TAP/inproc with
  one queue, all per-run logs reaching `SERVER_READY`,
  `GUEST_CURL ok=100 fail=0`, and `TIER3_OK`, no hidden fatal/BUG/KCSAN
  signatures, and clean TAP/process teardown. Strong long-run evidence,
  but not the final accepted 2-hour gate because the run stopped at
  85.3% of the budget. Not a commitment.

Dependency graph for this batch (→ = "needs"):

```
                    05 (umlctl)
                   /           \
                  /             \
                 v               v
             04 (tui)        11 (api)
                                |
                                v
                          (external orchestrators)

     05 ─┬─> 06 (mcp) ─┬─> 07 (bisect) <─── 09 (diff)
         │             └─> 12 (perfetto)
         │
         └─> 10 (record-replay) ─> 12 (perfetto)

     08 (probe-web) — independent; shares HMAC-token auth pattern with 11
```

Natural sequencing, if and when the parking lot opens:
12 first (cheapest, no deps), then 05, then everything that
depends on 05. 06 and 10 are the longest-tail items.

Memo 13 (observability spine) sits orthogonal to the above
graph: its Phase O1 (format freeze + cheap wins) slots in
alongside 05, and its later phases unblock the correlated-
query stories that 04/09/10/11 all want but none
individually deliver. If 13's O1 lands before 04/06/09/10/
11, those memos pick up a run_id + schema registry for
free instead of rolling their own.
