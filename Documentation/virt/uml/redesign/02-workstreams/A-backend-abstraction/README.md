# Workstream A: Backend abstraction

**Effort:** ~6 engineer-months
**Owner role:** Architect
**Critical path:** Yes — gates B, C (defconfig backend selection),
                 D (third backend impl)
**Bookend:** Defines the contract every backend implements

## What this workstream produces

`struct um_backend_ops` — the interface — and two implementations
of it (`ptrace`, `seccomp`) that pass a conformance suite.
Existing UML code is refactored to call through the ops table
instead of hardcoding to skas mode.

Deliverables:

1. `arch/um/include/asm/backend.h` — the ops table definition
2. `arch/um/kernel/backend.c` — selection mechanism
3. `arch/um/backend/ptrace/` — refactored skas mode
4. `arch/um/backend/seccomp/` — wrapper around merged 6.16 work
5. `arch/um/backend/contract/` — conformance test suite
6. Kconfig: `CONFIG_UM_BACKEND_PTRACE`, `CONFIG_UM_BACKEND_SECCOMP`,
   `CONFIG_UM_BACKEND_DYNAMIC` (multi-backend), single-backend
   inlined builds

## Why this is hard

The existing UML code calls into skas-mode-specific functions
directly throughout the kernel. There are ~50 call sites that
need to migrate to ops-table dispatch. Most are mechanical;
some are tangled with assumptions about the skas process model
(stub processes, signal-driven page faults, etc.) that need
careful unwinding.

Performance discipline matters from day 1. Every ops-table
indirect call must be measured; benchmarks in CI prevent
regressions.

## Tasks

| # | Task | Effort | Status |
|---|---|---|---|
| 01 | [Design the ops table](01-ops-table.md) | 2 wk | **design draft complete (2026-04-17)** |
| 02 | [Refactor existing skas mode into ptrace backend](02-ptrace-refactor.md) | 6 wk | **API wired (2026-04-17); 5 ops stubbed pending A-04/C-11** |
| 03 | [Wrap merged 6.16 seccomp work as seccomp backend](03-seccomp-wrap.md) | 4 wk | **API wired across PTRACE_ONLY/SECCOMP_ONLY/DYNAMIC (2026-04-18); same caveats as A-02** |
| 04 | [Kconfig: single vs multi-backend builds](04-kconfig.md) | 2 wk | **complete (2026-04-18); choice block + `backend=` param + matrix coverage** |
| 05 | [Backend conformance test suite](05-contract.md) | 4 wk | **first pass complete (2026-04-18); 20 KUnit tests + 2 harness skeletons; LTP integration deferred** |
| 06 | [Documentation: contract + cost model](06-documentation.md) | 2 wk | **complete (2026-04-18); 3 docs landed; Sphinx builds clean** |
| 07 | [Performance regression CI](07-perf-ci.md) | 2 wk | **complete (2026-04-18); perf scripts + CI template + checked-in baseline** |

Total: ~24 weeks ≈ 6 EM.

## Milestones

- **Month 1**: Ops table designed and circulated for review
- **Month 2**: Ptrace backend passes conformance suite (interface
  freeze)
- **Month 4**: Seccomp backend passes conformance suite
- **Month 5**: Single-backend-inlined builds work; multi-backend
  selection works
- **Month 6**: CI gates in place; performance regression detection
  active

## Open questions for this workstream

- **Q1**: Do we expose backend selection via boot param,
  Kconfig, or both? (Plan: both. Boot param overrides Kconfig
  default in multi-backend builds.)
- **Q2**: What's the abort policy if `force=kvm` and KVM is
  unavailable? (Plan: panic. No silent fallback.)
- **Q3**: How granular is the ops table? (Plan: ~15 ops covering
  trap, mm, schedule, time, IO, debug. Not dozens.)
- **Q4**: Can we add ops without bumping a contract version?
  (Plan: ABI stability is internal, not external. Add freely;
  bump major version on op signature changes.)

See task files for more.

## What success looks like

After this workstream:

- A maintainer can write a new backend by implementing the ops
  table, with no other UML changes required.
- The conformance suite catches backend bugs during CI.
- `prod-fast` (single seccomp backend, inlined) syscall cost is
  measured and within 5% of the pre-refactor `skas` baseline.
- Other workstreams can call `um_backend->op()` (or
  `um_backend_dispatch(op, ...)` for inlinable single-backend) and
  not care which backend is active.

## What failure looks like

- The ops table is too granular (50+ ops, every backend implements
  every one); refactor stalls in mechanical drudgery.
- The ops table is too coarse (5 ops, each with 10-arg signatures
  full of conditionals); backends can't be cleanly separated.
- Indirect-call overhead measurably regresses prod-fast (>5%);
  invariant I2 violated; abandon plan.

Recovery: incremental. Bad ops can be split or merged in the
contract version; performance regressions can usually be fixed by
moving the indirect call out of the hot path or making it a
single-backend inline.
