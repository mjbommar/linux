# R4: CI resource cost

## The risk

9 profiles × 3 backends × N tests × M kernel versions × K hosts
= a lot of CI hours. A single commit could take hours to fully
validate.

Engineers stop running CI; bugs land that should have been caught.

## Mitigation: tiered CI

### Tier 1: smoke (every commit, ~5 min)

- Build all 9 profiles
- Boot test on each (`init=/bin/true`)
- Microbenchmarks on prod-fast (perf regression detection)

### Tier 2: integration (every merge to main, ~30 min)

- LTP runtest/syscalls on prod-fast and research
- KASAN-positive reproducer on research
- syzkaller smoke (10 min) on fuzz
- Cross-backend equivalence (ptrace vs seccomp) on a few key
  tests

### Tier 3: full matrix (nightly, ~6 hours)

- Every profile × every available backend
- Full LTP
- syzkaller corpus replay
- Performance benchmarks (longer-running)
- KMSAN (if/when present) on fuzz-deep

### Tier 4: pre-release (before tag, ~24 hours)

- Multiple host kernel versions
- Old/weird hosts (embedded profile validation)
- ARM64 host (when present)
- Long-running fuzz (overnight)
- Conformance suite from A-05 across everything

## Hardware cost estimate

Rough numbers, x86_64 cloud:

- Tier 1: 1 4-vCPU runner, ~5 min/commit = $0.10/commit
- Tier 2: 2 8-vCPU runners, ~30 min/merge = $1/merge
- Tier 3: 4 16-vCPU runners, ~6 h/night = $20/night = $600/mo
- Tier 4: 8 16-vCPU runners, ~24 h/release = $200/release

Annual: ~$10k-15k for CI infra. Affordable for this kind of
project.

ARM64 host (when present) adds Apple silicon or AWS Graviton
runners; small additional cost.

## Mitigation: incremental matrix

Don't try to validate every combination always. Use:

- **Selective testing**: a patch that touches only `arch/um/
  net/` skips block-device tests in tier 1
- **Failure-aware skip**: if KMSAN isn't ported yet, skip its
  tier 3 jobs
- **Crowd-sourced validation**: maintainers + downstream users
  contribute "I tested profile X on host Y" reports

## Triggers for review

- Tier 1 exceeds 10 minutes — too slow for per-commit
- Tier 3 takes more than 12 hours — won't finish overnight
- Cost exceeds $20k/year — talk to project sponsor

## What we accept

- Some combinations untested in CI (sandbox profile + ARM64 +
  Linux 4.x — probably never). Document; user beware.
- Some tests flaky (best effort; quarantine and fix).
- Engineers occasionally re-run a CI job because of noise.
