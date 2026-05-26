# B-05: Benchmark gate cost — off and on

**Status:** first-pass complete (2026-04-18) — kernel-side bench in
`arch/um/kernel/hooks_bench.c`, host scripts in `redesign/scripts/
uml-gate-bench{,-compare}.sh`, baseline checked in at
`notes/bench-baseline.json`. Off-state cost comfortably under
I3's 2 ns per-gate ceiling under the current methodology. A
precise per-gate methodology (cycle counters + CPU pinning) is
deferred to follow-up **B-05-precise** (see below); the first
pass is already enough to support CI regression gating.

**Effort:** 3 weeks planned; ~1 day delivered for the first pass
**Dependencies:** B-02 (gates), A-07 (perf CI infra)
**Blocks:** invariant I3 enforcement in CI

## Goal

Microbenchmarks measuring per-hook-helper cost in both states.
Establish a baseline. Wire into CI so a regression on either
state is caught.

## What the first pass delivered

1. **Kernel-side bench** (`arch/um/kernel/hooks_bench.c`) runs a
   tight-loop measurement inside the guest kernel. 10 batches of
   100k iterations per call site; median ns-per-call reported
   with one decimal preserved (units of ns × 1000).
2. **Wall-time clock**: `ktime_get_ns()`. Not cycle counters.
3. **Four call sites measured**: `syscall_entry`, `syscall_exit`,
   `context_switch`, `clock_read`. `page_fault` and `irq_entry`
   are not in the tight-loop harness because their natural call
   shape (fault delivery, IRQ dispatch) is not suitable for a
   synthetic loop; their cost is covered by end-to-end benchmarks
   from workstream A-07.
4. **Host trigger** (`uml-gate-bench.sh`): boots UML, mounts
   debugfs in an init-script guest, reads
   `/sys/kernel/debug/um/bench`, parses into JSON.
5. **Regression guard** (`uml-gate-bench-compare.sh`): diffs a
   fresh run against `notes/bench-baseline.json`; fails if any
   per-site delta exceeds 15% (configurable via
   `UML_BENCH_CEILING_PCT`).
6. **Single-sample baseline** checked in. CI should run ≥3 and
   pick median before comparing.

## What the first pass does NOT deliver

These items were on the original B-05 plan but require invasive
methodology work and are split into **B-05-precise** as a named
follow-up:

- Per-gate cost (first pass measures per-call-site cost, which
  is N gates at once; backing out individual gate cost requires
  turning gates on one at a time — achievable but noisy at the
  ktime precision used today).
- Cycle-level measurement via `rdtscp`.
- CPU pinning (`taskset` + `sched_setaffinity` on the kernel-side
  bench thread) to remove scheduler noise.
- Branch-miss counters via perf-event counters attached to the
  benchmark thread.
- Per-backend + per-profile matrix sweep (ptrace/seccomp/KVM ×
  prod-fast/research/fuzz). The current bench runs on whichever
  DYNAMIC build has `CONFIG_DEBUG_FS=y`; expanding to the full
  matrix is mechanical.

## Validation (first pass)

- Every checked-in baseline number is below invariant I3's 2 ns
  per-gate ceiling (derived per-gate figures in
  `bench-baseline.json`'s `derived_per_gate_ns_x1000` section).
- `uml-gate-bench-compare.sh` returns 0 on a rerun against the
  checked-in baseline at the default 15% tolerance.
- Numbers are broadly stable across quick reruns but not stable
  enough to defend a specific external "X.X ns per gate" claim;
  the defended claim is "<2 ns per gate under the current
  methodology."

## Open questions / B-05-precise scope

- **Q1 — CPU pinning.** The kernel-side bench runs on whatever CPU
  the scheduler put the reader task on; cross-CPU migration adds
  noise. B-05-precise: pin the bench thread.
- **Q2 — rdtscp vs ktime.** `ktime_get_ns` adds ~10 ns per read; at
  our 100k iterations per batch this amortizes well but fine-grain
  per-iter precision would want `rdtscp` inside the loop. B-05-precise.
- **Q3 — per-gate isolation.** Today the loop measures "all gates
  for site X in state Y"; backing out per-gate cost requires a
  toggle-sweep (only gate 1 on, only gate 2 on, ...). B-05-precise.
- **Q4 — perf-counter shell.** Reading HW counters from inside the
  UML guest means the guest's perf subsystem must be present and
  functional — deferred until workstream C lands first-class perf
  integration.

## Risk

Microbenchmarks measure microbenchmarks, not workloads. Also: a
single-sample baseline can be cherry-picked by a regression that
fits inside the noise envelope.

**Mitigation:**
- The comparison script's 15% ceiling is loose enough to accept
  normal run-to-run variance and tight enough to catch real
  regressions.
- CI runs multiple samples; manual runs should too.
- Macrobenchmarks (LTP-perf, fork storm, end-to-end syscall loop)
  live in workstream A-07's `uml-perf.sh` path and catch
  regressions that the microbenchmark would miss.
