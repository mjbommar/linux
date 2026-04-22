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
