# Sequencing

When and in what order. Three views of the same plan:

- [24-month-plan.md](24-month-plan.md) — calendar view; quarter-
  by-quarter
- [milestones.md](milestones.md) — verifiable checkpoints
- [critical-path.md](critical-path.md) — what blocks what
- [parallelism-map.md](parallelism-map.md) — what can run
  concurrently
- [post-q1-push.md](post-q1-push.md) — 2026-04-23: the ten
  remaining big-unknown / engineering-lift items, grouped into
  seven phases with explicit dependency + risk-retiring
  sequencing. Drives execution after the Q1 milestone cluster
  landed.
- [PLAN-2026-05-14.md](PLAN-2026-05-14.md) — 2026-05-14: previous
  sprint plan (Phase J Track A scaffolding round + Track B
  time-machine ports + Track D upstream queue prep).
- [post-2026-05-14-next-sprint.md](post-2026-05-14-next-sprint.md)
  — earlier next-sprint plan; subsumed by the 05-19 plan below
  after Round 14 closure + memo 52 landings.
- [post-2026-05-19-next-sprint/](post-2026-05-19-next-sprint/) —
  **current sprint plan** (8 per-system memos under one
  README). Sequencing: vector2 default flip → UBD io_uring →
  hostfs io_uring + `openat2` → time-travel record/replay
  wiring → common epoll completion → vCPU thread pinning →
  console batching → virtio-rng modernisation.

## The shape

24 engineer-months total, scaled up or down by team size:

- **2 EM/month team** (5 people): plan completes in ~12 months
- **1 EM/month team** (single dedicated engineer + part-time
  helpers): ~24 months
- **0.5 EM/month team** (one part-time engineer): ~48 months;
  consider scope reduction

The architecture is robust to slower execution. The risk is
external: if it takes 4 years, LKL + Rust VMM + crosvm + eBPF
together may eat the niche before we finish.

## Compression vs sequence

Some workstreams can run concurrently after Workstream A's
interface freeze. The 24-EM number assumes ~50% parallelism;
strict sequential would be ~36 EM, perfect parallelism ~16 EM.

Critical path through the plan is:
A-01 → A-02 → A-04 → C-01 → all C-tasks → ship.

That's roughly 15 weeks of A + 3 weeks of C-01 + 8 weeks of
parallel C-tasks = ~26 weeks ≈ 6 months wall-clock for a
team of 4. Workstreams B and D add to engineer-months but
not to wall-clock if parallelized.
