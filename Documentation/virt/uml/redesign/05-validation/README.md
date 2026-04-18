# Validation

How we know the architecture works. Five documents:

- [benchmarks.md](benchmarks.md) — what we measure
- [conformance-tests.md](conformance-tests.md) — LTP, kselftest,
  syzkaller corpus reproduction
- [ci-matrix.md](ci-matrix.md) — profile × backend × arch ×
  kernel-version
- [upstream-strategy.md](upstream-strategy.md) — how patches land
  in mainline without a 7-year RFC chain
- [kernel-dev-checklist.md](kernel-dev-checklist.md) — distilled from
  the kernel's own `Documentation/process/` docs; gates at 7 scales
  (project / workstream / series / patch / file / AI-disclosure /
  KVM-subsystem) plus our project-specific architecture-shape gates

## What "validated" means per profile

| Profile | Validation depth |
|---|---|
| prod-fast | Benchmark perf doesn't regress; LTP smoke passes |
| prod-with-hooks | Same as prod-fast + KCOV-flip demo works |
| research | LTP + kselftest pass; syzbot CVE reproduction works |
| fuzz | syzkaller `vm/uml` runs; KCOV records; KASAN catches |
| fuzz-deep | fuzz validation + KCSAN catches synthetic race + replay deterministic |
| sandbox | Pen-test: no host escape; minimal host syscall surface |
| library | Reproduces an LKL-style fuzzing harness; KASAN works |
| embedded | Boots on Linux 3.0 host; no /dev/kvm needed |
| time-travel | Two UMLs in lockstep produce identical output |

## Validation tiers

Match the CI tiers in `04-risks/ci-resource-cost.md`:

- **Tier 1 (smoke)**: every commit; <5 min
- **Tier 2 (integration)**: every merge; <30 min
- **Tier 3 (full matrix)**: nightly; <6 h
- **Tier 4 (pre-release)**: pre-tag; <24 h

## Non-goals for validation

- We do not validate that UML reproduces *every* host kernel
  behavior. Some things (real PCIe enumeration, GPU stacks) are
  out of scope by design.
- We do not validate that profiles compose with arbitrary
  out-of-tree configs. Users running custom Kconfigs are on
  their own.
- We do not promise bit-identical output between profiles
  (except where time-travel mode explicitly says we do).
