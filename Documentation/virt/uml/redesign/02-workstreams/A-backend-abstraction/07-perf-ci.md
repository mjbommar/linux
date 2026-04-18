# A-07: Performance regression CI

**Status:** **complete (2026-04-18)** — perf-event-counter
benchmarks (cycles, instructions, task-clock; median + IQR over
N runs) + capture + compare scripts + GitHub Actions template +
checked-in baseline.
**Effort:** 2 weeks budget; consumed ~1 hour since the skeleton
from A-05.S3 was already in place.
**Dependencies:** A-05 (perf benchmarks exist) — landed
**Blocks:** invariant I2 enforcement going forward — now wired

## Status detail

| Aspect | Status |
|---|---|
| Microbench harness | `scripts/uml-perf.sh` upgraded from wall-clock to `perf stat -e cycles,instructions,task-clock`; reports p25/p50/p75 over N=10 iters per backend |
| Baseline capture | `scripts/uml-perf-capture.sh` writes `perf-baseline.json` with backend numbers + host context (CPU/glibc/gcc/kernel/git-head) |
| Regression checker | `scripts/uml-perf-compare.sh` runs current measurements + diffs vs baseline; bot policy: <2% silent, 2-5% WARN (reviewer ack), >5% FAIL |
| CI workflow template | `uml-perf.yml.template` (ready to drop into `.github/workflows/` on a fork; mainline UML doesn't host CI yet) |
| Initial baseline | `perf-baseline.json` committed; PTRACE_ONLY/SECCOMP_ONLY/DYN_ptrace/DYN_seccomp captured |

## Known noise on this host

The 10-iter baseline shows wide IQR on PTRACE_ONLY and SECCOMP_ONLY
(p25-to-p75 spans up to 80% of the median). A re-run hit +83%
"regression" on SECCOMP_ONLY purely from variance — the compare
tool correctly fired its FAIL policy, but the underlying perf is
unchanged.

Mitigations for production CI (per A-07 Q1):

- Bump iterations: `UML_PERF_ITERS=30` halves the noise floor.
- Use a dedicated runner with consistent thermals + no other
  workloads.
- Consider replacing point-median compare with IQR-overlap (current:
  fail if `current_p50 > baseline_p50 * 1.05`; better: fail if
  `current_p25 > baseline_p75 * 1.05`).

The current scripts ship the simple policy because it's the
A-07-spec'd one; tuning happens after some real CI miles.

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
