# A-05: Backend conformance test suite

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** A-01, A-02 (need ptrace as ground truth)
**Blocks:** A-03 (seccomp can't claim conformance until tests exist)

## Goal

A test suite that validates every backend implements the ops
table contract correctly. Run in CI on every backend for every
commit. A backend bug surfaces as a test failure, not a kernel
oops three weeks later.

## Approach

The suite has three categories:

### 1. Per-op unit tests

For each op in `um_backend_ops`, test the op in isolation:

- `syscall_dispatch`: known syscall arguments → known return value
- `page_fault`: trigger a page fault at a known address → ops
  receives the address with correct flags
- `context_switch`: switch between two known tasks → CPU state
  preserved
- `read_clock_ns`: monotonic, reasonable values
- ... etc

Lives in `arch/um/backend/contract/test_ops.c`. Run as kunit tests.

### 2. Cross-backend equivalence tests

Run identical workloads on every available backend; assert
identical observable behavior:

- LTP/syscalls subset (read/write/openat/mmap/clone/...) on every
  backend → same return values, same errno, same final state
- Stress tests (many threads, many syscalls, many page faults)
  → no panic, no hang, same end state
- KASAN-positive reproducer → same KASAN report on every backend

Lives in `tools/testing/selftests/uml/cross-backend/`. Driven by
a Python harness that boots each backend and runs the workload.

### 3. Performance regression tests

Measure cost of each op on each backend; alert if any regresses
>5% on prod-fast (invariant I2).

- `getpid()` cycles
- 1k syscalls per second sustained
- page-fault rate under fork-heavy workload
- context-switch rate under thread-heavy workload

Lives in `tools/testing/selftests/uml/perf/`. Driven by `perf
stat` and a Python diff against a baseline.

## Deliverable

- `arch/um/backend/contract/` with kunit tests
- `tools/testing/selftests/uml/cross-backend/` with Python harness
- `tools/testing/selftests/uml/perf/` with perf benchmarks
- CI integration that runs the suite on every backend on every
  commit

## Validation

- Suite runs in <30 min total for all backends
- Tests are deterministic; no flakes
- Suite catches a known bug (regression-introduce a known-bad
  change; verify the suite fails)

## Open questions

- **Q1**: Do cross-backend equivalence tests run on the same host
  or distributed? (Plan: same host, sequentially; total time is
  small enough.)
- **Q2**: How much LTP coverage is realistic? (Plan: ~80% of
  syscalls subset, ~50% of LTP overall. Drivers/networking
  partial.)
- **Q3**: Do we test backend hot-swap (boot one, switch to
  another)? (Plan: no. Not a supported operation; document.)

## Risk

The suite itself becomes the bottleneck — slow CI, flaky tests,
high maintenance cost.

**Mitigation:**
- Tier the suite: fast (smoke) tests run on every commit;
  full suite runs nightly.
- Mark flaky tests immediately; investigate before merging
  any change to them.
