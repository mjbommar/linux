# A-02: Refactor existing skas mode into ptrace backend

**Status:** planned
**Effort:** 6 weeks (the largest task in workstream A)
**Dependencies:** A-01
**Blocks:** A-05 (conformance suite), B-* (gates need backend)

## Goal

Move existing skas-mode code in `arch/um/os-Linux/skas/` and
related into `arch/um/backend/ptrace/`, expressing every
externally-visible function via the ops table. After this task,
the kernel calls `um_backend->syscall_dispatch(regs)` instead of
`handle_trap(regs)` directly.

## Approach

Per-op refactor:

1. **Identify call sites** for the op across `arch/um/`.
2. **Move implementation** to `arch/um/backend/ptrace/<op>.c`.
3. **Add op to `um_backend_ptrace` instance.**
4. **Replace call sites** with ops-table dispatch.
5. **Build, run conformance test for the op.** Don't move on
   until passing.

Order matters; do hot path first to surface perf issues early:

1. `syscall_dispatch` (hottest)
2. `page_fault`
3. `context_switch`
4. `ipi_send`
5. `read_clock_ns`, `set_timer`
6. `map_user`, `unmap_user`
7. `host_io_submit`
8. `read_guest_regs`, `write_guest_regs`

## Deliverable

- `arch/um/backend/ptrace/` directory containing the refactored
  skas implementation
- All call sites in `arch/um/kernel/`, `arch/um/drivers/`,
  `arch/um/os-Linux/` updated to ops-table dispatch
- Existing skas tests pass (LTP subset, kselftest)
- Performance: prod-fast-equivalent benchmark within 5% of
  pre-refactor skas baseline (invariant I2)

## Validation

- LTP runtest/syscalls passes with ptrace backend selected
- kselftest/x86, /timers, /membarrier pass
- syz-bisect on a known-good kernel reaches the same result as
  pre-refactor
- Microbenchmark `getpid()` cycles within 5% of baseline

## Open questions

- **Q1**: Some skas code is shared between modes (signal trampoline,
  stub page mgmt). Where does it live? (Plan: `arch/um/backend/
  common/`. Backends include from there.)
- **Q2**: How to handle the skas/skas3 distinction (older host
  kernels)? (Plan: drop skas3-only path; require host with
  skas4+ssas. linux-um list confirms this is OK as of 6.16+.)
- **Q3**: Do we keep the legacy `tt` mode? (Plan: no. It was
  removed years ago. Don't resurrect.)

## Risk: scope explosion

This is the biggest task in workstream A by far. Refactors that
touch ~50 sites across many files tend to grow.

**Mitigation:**
- Per-op subtasks; merge incrementally.
- Each merge keeps the kernel building and testing.
- If an op refactor takes >2 weeks, escalate; the op may need
  redesign in A-01.
