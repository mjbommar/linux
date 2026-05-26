# CI matrix

Profile × backend × host arch × host kernel × test tier.

## The full matrix (informational)

```
profiles:   prod-fast prod-with-hooks research fuzz fuzz-deep sandbox library embedded time-travel
backends:   ptrace seccomp kvm
host arch:  x86_64 (always); aarch64 (when available)
host kernel: latest mainline, latest stable, latest LTS, oldest supported (3.0 for embedded)
tier:       1-smoke 2-integration 3-full 4-prerelease
```

Cardinality:
- Profiles × Backends = 9 × 3 = 27, but many profile-backend combos are
  N/A (e.g., library profile has no backend). Effective: ~20.
- × 2 host archs = ~40
- × 4 host kernels = ~160
- × 4 tiers = ~640 distinct configurations

We don't test all 640 in tier 1 (that would be hours). The matrix
below shows what runs at each tier.

## Tier 1: smoke (every commit, <5 min)

```
prod-fast       seccomp x86_64 latest    smoke
research        seccomp x86_64 latest    smoke
fuzz            seccomp x86_64 latest    smoke
sandbox         seccomp x86_64 latest    smoke
```

Just enough to catch breakage. ~5 min total.

## Tier 2: integration (every merge to main, <30 min)

Add per Tier 1:
- All profiles, seccomp backend, x86_64, latest host
- prod-fast on KVM (if KVM available in CI runner)
- LTP runtest/syscalls subset on prod-fast and research
- KCOV-flip demo on prod-with-hooks

## Tier 3: full matrix (nightly, <6 h)

Add per Tier 2:
- All profiles × all backends × x86_64 × latest host
- LTP full suite on research and fuzz
- syzkaller corpus replay (subset)
- All sanitizer KUnit tests

## Tier 4: pre-release (pre-tag, <24 h)

Add per Tier 3:
- Multiple host kernel versions (latest, stable, LTS, oldest)
- ARM64 host (when available)
- Long-running fuzz (overnight)
- Full conformance suite from A-05

## Per-PR matrix

Default for a PR: Tier 1 + (paths-changed-aware Tier 2 subset).

Paths-changed mapping:

| Path touched | Add to PR matrix |
|---|---|
| `arch/um/backend/ptrace/` | Tier 2: ptrace backend in all profiles |
| `arch/um/backend/seccomp/` | Tier 2: seccomp in all profiles |
| `arch/um/backend/kvm/` | Tier 2: KVM if available |
| `arch/um/include/asm/um-hooks.h` | Tier 2: KCOV-flip demo |
| `arch/um/configs/profiles/<X>.config` | Tier 2: profile X full validation |
| Anywhere else in `arch/um/` | Tier 1 only |

## Hardware fleet

Tier 1-3 run on cloud x86_64 runners (8-vCPU Ubuntu 24.04 hosts).

ARM64 runners: AWS Graviton or Apple M-series for tier 4 only.

Old-kernel runners: containers with old userlands; for embedded
profile validation in tier 4.

Bare-metal runner: one dedicated machine for KVM backend
benchmarks (the only place we measure real <100 ns numbers).

## Failure modes and triage

| Symptom | First-suspect |
|---|---|
| One profile fails everywhere | Profile defconfig / Kconfig regression |
| One backend fails everywhere | Backend impl bug |
| Specific test flakes on one host | Host-kernel-version interaction |
| Perf regression on one profile | Something added cost on its hot path |
| Cross-backend equivalence test diff | I4 violation; serious |

## Cost (estimated)

- Tier 1: ~10 commits/day × $0.10 = $1/day
- Tier 2: ~5 merges/day × $1 = $5/day
- Tier 3: 1 nightly × $20 = $20/day
- Tier 4: ~1 release/2 weeks × $200 = $14/day amortized

Total: ~$40/day = ~$15k/year. Aligns with R4 estimate.
