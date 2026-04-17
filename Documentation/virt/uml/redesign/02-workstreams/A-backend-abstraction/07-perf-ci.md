# A-07: Performance regression CI

**Status:** planned
**Effort:** 2 weeks
**Dependencies:** A-05 (perf benchmarks exist)
**Blocks:** invariant I2 enforcement going forward

## Goal

Automated detection of performance regressions on prod-fast
profile. Every commit measured; >5% regression blocks merge;
2-5% regression requires explicit ack.

## Approach

1. **Baseline**: capture prod-fast benchmark numbers from the
   pre-refactor `skas` mode at workstream start. Bookend.
2. **Per-commit**: CI runs benchmarks on prod-fast build,
   compares to baseline.
3. **Reporting**: results posted as commit status; visible in
   PR review.
4. **Bot policy**:
   - Regression <2%: no action.
   - Regression 2-5%: bot comments; reviewer must ack.
   - Regression >5%: bot blocks merge.
5. **Baseline updates**: explicit, batched, with
   sign-off.

## Deliverable

- CI workflow file in `.github/workflows/uml-perf.yml` (or
  equivalent for whatever CI hosts UML)
- Perf-bot comment template
- Documented procedure for bumping the baseline

## Validation

- A test commit with a known +10% regression is blocked
- A test commit with a known -10% improvement passes and is
  flagged for baseline bump
- Bot doesn't false-positive on noise (>10 runs averaged)

## Open questions

- **Q1**: How do we account for hardware variance across CI
  runners? (Plan: run on dedicated runners; use perf-event
  counters not wall time; report distribution not point.)
- **Q2**: How granular are the benchmarks? (Plan: 5-10 microbenchmarks
  + LTP-perf. Total runtime <10 min in CI.)

## Risk

CI noise causes false positives → engineers ignore the bot →
real regressions slip.

**Mitigation:**
- Use perf-event counters (cycles, instructions, cache misses),
  not wall-clock time
- Run each benchmark N times, report median + IQR
- Threshold compares to upper-bound, not point estimate
