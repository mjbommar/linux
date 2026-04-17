# A-01: Design the ops table

**Status:** planned
**Effort:** 2 weeks
**Dependencies:** none (foundation task)
**Blocks:** every other task in workstreams A, B, C, D

## Goal

Define `struct um_backend_ops` — the contract every backend must
implement. This is the single most important design decision in
the entire plan. Get it wrong and every other task pays.

## Approach

1. Inventory every place existing UML code makes a backend-relevant
   call. Group by category (trap, mm, schedule, time, IO, debug).
2. For each category, define ops at the right granularity:
   - Too coarse → backends need internal switches; abstraction leaks
   - Too fine → cost of indirect dispatch dominates; refactor stalls
3. Sketch ptrace and seccomp implementations against the draft.
   If either implementation requires "and a side channel for X",
   the ops table is wrong.
4. Sketch a KVM implementation against the draft. If KVM needs ops
   the others don't (it does — VMENTRY/VMEXIT lifecycle), those
   are KVM-specific extensions, not part of the core contract.
5. Circulate to maintainers (Berg, Ivanov, Bie, Weinberger) for
   feedback before any implementation.

## Deliverable

- `arch/um/include/asm/backend.h` with the ops table
- `arch/um/Documentation/backend-contract.rst` describing semantics
- A 2-page design memo for LKML circulation

## Validation

- Three implementation sketches (ptrace, seccomp, KVM) fit the
  contract without per-backend extensions to the core ops.
- All ~50 existing backend-relevant call sites in `arch/um/` map
  cleanly to one or more ops.
- Maintainers ack the design (or push back; iterate).

## Open questions

- **Q1**: Are ops synchronous or can they return a "completion to
  poll later" handle? (Plan: synchronous. Async via I/O subsystem
  if needed; not in core ops.)
- **Q2**: Do ops carry a `void *backend_private` or is per-backend
  state global? (Plan: global per-backend; backends are singletons.)
- **Q3**: How do we handle ops that one backend wants to inline
  (seccomp's `syscall_dispatch`, hot path) vs ops where the
  indirect call is fine (rare debug ops)? (Plan: hot ops get the
  single-backend-inline treatment in `04-kconfig.md`; cold ops
  always indirect.)
- **Q4**: Versioning: do we expose a contract version to the
  kernel for runtime introspection? (Plan: yes, a single `u32`
  in the ops table. Bumped on signature changes.)

## Risk: this design lives forever

Once shipped, the ops table is hard to change because every
backend depends on it. A future-incompatible change forces every
backend to migrate in lockstep.

**Mitigation:**
- Long review cycle (4 weeks before freeze).
- Versioned contract; new ops can be added (new backends fill
  them, old backends report `not implemented`).
- Decisions logged in `04-risks/decisions-log.md`.
