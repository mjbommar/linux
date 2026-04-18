# A-01: Design the ops table

**Status:** design draft complete (2026-04-17)
**Owner:** claude-code session
**Effort:** 2 weeks (design pass; LKML review cycle separate)
**Dependencies:** none (foundation task)
**Blocks:** every other task in workstreams A, B, C, D

## Status detail

| Deliverable | Location | State |
|---|---|---|
| Backend ops header | `arch/um/include/asm/backend.h` | landed (compiles standalone; dispatch macro tested across all 4 build variants) |
| Contract spec | `Documentation/virt/uml/backend-contract.rst` | landed |
| LKML design memo (draft) | `notes/10-lkml-memo.md` | drafted; pending project-owner review before sending |
| Skas survey | `notes/00-skas-survey.md` | landed |
| Call-site inventory | `notes/01-call-site-inventory.md` | landed (87 sites, 23 files) |
| Op categorization | `notes/02-categories.md` | landed (18 ops, 5 hot) |
| Per-backend sketches | `notes/04-ptrace-sketch.md`, `05-seccomp-sketch.md`, `06-kvm-sketch.md` | landed; all three fit the contract without per-backend extensions |
| Inventory ↔ ops coverage | `notes/07-coverage.md` | landed; all sites accounted for |
| Open-question resolutions | `notes/08-decisions.md` | landed; logged as D8 in `04-risks/decisions-log.md` |

Next steps: send LKML memo (after project-owner sign-off), then begin
A-02 (ptrace refactor, 6 wk) targeting an interface freeze in month 2
of the workstream calendar.

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
