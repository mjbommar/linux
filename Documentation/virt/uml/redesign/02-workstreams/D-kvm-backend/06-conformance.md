# D-06: Conformance + benchmarks

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** D-02..D-05 (complete impl)
**Blocks:** workstream D bookend; ship

## Goal

KVM backend passes the conformance suite from A-05. Bookend
benchmark: `getpid()` <100 ns on bare metal.

## Approach

1. Run A-05 conformance suite against KVM backend.
2. Fix every failure; KVM backend must behave identically to
   ptrace and seccomp for kernel correctness (invariant I4).
3. Run perf benchmarks from B-05 + A-07.
4. Compare to ptrace and seccomp baselines.
5. Document cost model.

## Deliverable

- KVM backend passes 100% of conformance
- Bookend benchmark: `getpid()` <100 ns measured with `rdtscp`
- Perf comparison report:
  `02-workstreams/D-kvm-backend/perf-report.md`

## Validation

- LTP runtest/syscalls passes on KVM backend identically to
  other backends
- KASAN-positive reproducer reproduces on KVM backend (invariant
  I5)
- syzkaller (after C-08) can target KVM backend

## Open questions

- **Q1**: Does KASAN actually work under KVM backend, or does
  EPT mapping of the shadow region break? (Plan: validate; if
  broken, document as a backend limitation. KASAN profile uses
  seccomp.)
- **Q2**: Does time-travel work under KVM backend? (Plan:
  probably yes; the time-travel coordinator is host-side. But
  KVM's clock interactions need verification.)

## Risk

Bookend may not be achievable. ~100 ns target was extrapolated
from gVisor's measured numbers; UML's setup may have higher
cost.

**Mitigation:**
- Acceptable fallback: <300 ns. Still a 5× win over seccomp.
- If even 300 ns isn't reached, KVM backend ships as
  "experimental, prod-fast still uses seccomp".
