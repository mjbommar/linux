# B-05: Benchmark gate cost — off and on

**Status:** complete (2026-04-18) — kernel-side bench in arch/um/kernel/hooks_bench.c, host scripts in redesign/scripts/uml-gate-bench{,-compare}.sh, baseline checked in; I3 met (0.45–0.68 ns/gate off)
**Effort:** 3 weeks
**Dependencies:** B-02 (gates), A-07 (perf CI infra)
**Blocks:** invariant I3 (off-state cost <2 ns) enforcement

## Goal

Microbenchmarks measuring per-gate cost in both states. Establish
the cost model. Wire into CI so a regression on either state is
caught.

## Approach

1. Microbenchmark per gate:
   - Off state: 1M iterations of the gated path, divided.
   - On state: same, with cheap slow-path handler.
2. Use perf-event counters (cycles, instructions, branch misses)
   for repeatability.
3. Per-backend: ptrace, seccomp, KVM (when available).
4. Per-profile: prod-fast, research, fuzz (sanity).
5. Publish baseline numbers; fail CI on regression.

## Deliverable

- `tools/testing/selftests/uml/perf/gates/` directory
- One benchmark per gate (10 benchmarks)
- Baseline JSON file checked in
- CI integration

## Validation

- Off-state per-gate cost <2 ns (invariant I3)
- On-state cost matches the cost model in
  `01-architecture/three-layers.md` ±20%
- Numbers stable across CI runs (low noise)

## Open questions

- **Q1**: How to amortize over noise? (Plan: 1M iterations per
  measurement, 10 measurements, report median + IQR. ~10 sec
  per benchmark.)
- **Q2**: Does instrumentation overhead (perf-event reading)
  perturb the measurement? (Plan: use `rdtscp` for fine-grain
  timing inside a perf-event-counter shell.)

## Risk

Microbenchmarks measure microbenchmarks, not workloads.

**Mitigation:** also include macrobenchmarks (LTP-perf, fork
storm, network stress) in CI even though these are noisier.
