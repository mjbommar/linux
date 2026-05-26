# R3: prod-fast performance regressions

## The risk

Adding the architecture (backend ops, static keys, section split)
regresses prod-fast performance. Invariant I2 says this can't
happen, but invariants don't enforce themselves.

If prod-fast regresses by 10-20%, the entire architecture loses
credibility. Every "but it lets you flip on tracing!" gets
"but I can't afford it".

## Why this is plausible

- Indirect call through ops table is ~5-10 cycles.
- Static-key gate is ~1 ns when off but in tight loops these
  add up.
- Section split adds page-table entries and TLB pressure.
- Patchable section may live in a less-cached page.

10% regression is achievable through carelessness.

## Mitigation: layered defenses

### Layer 1: invariant I2

Codified in `01-architecture/invariants.md`. Every patch must
not regress prod-fast >5%; 2-5% requires explicit ack.

### Layer 2: A-07 perf CI

Workstream A delivers automated regression detection on every
commit. Bot blocks merges with >5% regression.

### Layer 3: B-05 microbenchmarks

Workstream B's gate cost benchmarks track per-gate overhead.
Tightens the per-gate budget.

### Layer 4: single-backend inline builds

`CONFIG_UM_BACKEND_SECCOMP_ONLY=y` etc. eliminates the indirect
call. prod-fast that ships with one backend pays zero
abstraction cost vs the pre-refactor `skas`-mode code.

### Layer 5: profile-specific exemption

If prod-with-hooks costs 5% more than prod-fast (because the
slow-path code is linked in even though gates are off), that's
documented. prod-fast users who care about that 5% pick
prod-fast, not prod-with-hooks.

## What we accept

- Prod-fast on multi-backend dynamic dispatch may be 1-2%
  slower than single-backend inline. Document; offer the
  inline build for the absolute speed crowd.
- Prod-with-hooks may be ~5% slower than prod-fast. Document;
  this is the cost of runtime flexibility.

## What we don't accept

- A regression on prod-fast single-backend inline relative to
  pre-refactor `skas` baseline. This is the speed of light
  promise; we don't break it.

## Triggers for review

- Any month where the bot reports a regression that's
  acked-but-explained, review whether the explanation was real.
- Quarterly: re-baseline against the prior quarter; ensure
  cumulative drift hasn't snuck up.

## Bail-out scenario

If after 12 months we find that the architecture costs 10%+ on
prod-fast despite mitigations, declare a special workstream:
either find a way to claw it back or admit the architecture
trades 10% for the flexibility and decide if that's worth it.

If it isn't, we'd refactor toward a more compile-time-heavy
model where profiles diverge in code (not just config). Not
the end of the world; just less elegant.
