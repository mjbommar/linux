# A-05: Backend conformance test suite

**Status:** **first pass complete (2026-04-18)** — 20-test KUnit
suite covering all 18 ops + sanity, runs at boot in PTRACE_ONLY /
SECCOMP_ONLY / DYNAMIC. Cross-backend equivalence + perf-baseline
skeletons in `Documentation/virt/uml/redesign/scripts/`. LTP
integration + per-op functional tests for the few ops that need a
real guest are deferred to follow-up workstreams (notably C-09
syzkaller harness for syscall coverage and A-07 for cycle-accurate
perf gates).
**Effort:** 4 weeks; this slice consumed ~3 (kunit suite + 2
skeleton scripts); the LTP/Python harness in tools/testing/selftests/
is the remaining ~1 wk equivalent.
**Dependencies:** A-01, A-02 (need ptrace as ground truth) — both
landed.
**Blocks:** A-03 (seccomp now tested), C profiles' validation gates.

## Status detail

| Aspect | Status |
|---|---|
| Per-op KUnit tests | `arch/um/backend/contract/test_ops.c`: 20 tests covering all 18 ops (wired-up checks + functional checks for read_clock_ns, set_timer DISABLE, init_thread_regs, read/write_guest_regs stubs, read_persistent_clock_ns) plus 2 sanity tests (contract_version, all_ops_populated) |
| Single-backend assertion | each test uses ASSERT_OP_PTR_EQ helper that verifies the symbol matches the dispatch macro expansion in PTRACE_ONLY/SECCOMP_ONLY builds |
| Suite gating | `CONFIG_UM_BACKEND_CONTRACT_TEST` (tristate, default n); enabling with `CONFIG_KUNIT=y` makes the suite auto-run at boot via `kunit_test_suite()` |
| Cross-backend equivalence skeleton | `scripts/uml-cross-backend.sh`: builds PTRACE_ONLY + SECCOMP_ONLY, boots identical init, normalizes + diffs dmesg. Pass criterion is documented as "diff only on backend-identification line"; full LTP harness is follow-up. |
| Perf regression skeleton | `scripts/uml-perf.sh`: 5-iter wall-time per backend (PTRACE_ONLY, SECCOMP_ONLY, DYN/{ptrace,seccomp}). Establishes baseline; A-07 wires CI gates against it. |
| Matrix integration | `uml-boot-matrix.sh` accepts `UML_MATRIX_KUNIT=1` env to enable the contract suite during matrix runs |

## Goal

A test suite that validates every backend implements the ops
table contract correctly. Run in CI on every backend for every
commit. A backend bug surfaces as a test failure, not a kernel
oops three weeks later.

## Approach

The suite has three categories:

### 1. Per-op unit tests

For each op in `um_backend_ops`, test the op in isolation:

- `run_userspace`: known syscall arguments → known return value; a
  triggered page fault surfaces in `regs->faultinfo` with the correct
  address and write/exec flags (fault delivery is folded into
  `run_userspace` rather than a separate `page_fault` op)
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
