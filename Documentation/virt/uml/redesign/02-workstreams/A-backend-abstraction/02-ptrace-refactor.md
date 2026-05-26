# A-02: Refactor existing skas mode into ptrace backend

**Status:** **API wired (2026-04-17); some ops stubbed pending follow-up workstreams**

Concretely, all 18 ptrace ops are non-NULL and dispatched at boot,
but the following are **deliberate stubs** rather than full
implementations:

- `probe`, `init`, `shutdown` (`backend/ptrace/lifecycle.c`) — return
  0 / no-op. The real probe code still lives unconditionally in
  `os-Linux/start_up.c::os_early_checks`. Re-homing the probe into
  the ops `probe()` slot waits for the boot-sequence unification with
  the seccomp probe (A-04 work).
- `read_guest_regs`, `write_guest_regs` (`backend/ptrace/debug.c`) —
  return `-EOPNOTSUPP`. No in-tree caller as of A-02; the real impl
  needs a USER helper to issue `PTRACE_GETREGS`/`SETREGS` for an
  arbitrary task and proper turnstile serialization. Unblocked by
  KGDB integration (workstream C-11).

The remaining 13 ops are real implementations, not wrappers around
stubs.
**Effort:** 6 weeks (the largest task in workstream A)
**Dependencies:** A-01
**Blocks:** A-05 (conformance suite), B-* (gates need backend)

## Status detail (first slice)

| Sub | Outcome |
|---|---|
| A-02.0 plan | landed (`notes/02-00-a02-plan.md`) |
| A-02.1 baseline build | clean: 86 MB vmlinux, 0 warn / 0 err, ~2 min cold cache |
| A-02.2 Kconfig stubs | `CONFIG_UM_BACKEND_{PTRACE,PTRACE_ONLY,DYNAMIC,CONTRACT_TEST}` |
| A-02.3 backend/ scaffolding | `arch/um/backend/{ptrace,seccomp,common,contract}/` wired into Kbuild |
| A-02.4 ptrace_ops singleton | `arch/um/backend/ptrace/ptrace_backend.{c,h}` — all 18 fields stubbed; one wired |
| A-02.5 init_backend arbiter | `arch/um/kernel/backend.c` — called from `setup_arch()` before timekeeping_init |
| A-02.6 first cold op | `read_persistent_clock_ns` → `ptrace_read_persistent_clock_ns`; both call sites in `kernel/time.c` migrated; objdump confirms direct call |
| A-02.7 conformance skeleton | `arch/um/backend/contract/test_ops.c` (kunit; gated by CONFIG_UM_BACKEND_CONTRACT_TEST) |
| A-02.8 post-change verify | clean build; +142 B text, +288 B data, 0 B bss; runtime: dmesg shows `um: backend = ptrace (contract v1)`, init=/bin/true exits 0 |
| A-02.9 code-checking tools | checkpatch 7/7 clean; gcc -fanalyzer 4/4 clean; smatch 3/3 clean |
| A-02.10 workstream tracking | this update |

All slices complete:

| Slice | Op(s) | Status |
|---|---|---|
| A-02.HOT-1 | `run_userspace` (the trap loop) | **complete (2026-04-17)** |
| A-02.HOT-2 | `mm_map`, `mm_unmap` | **complete (2026-04-17)** |
| A-02.HOT-3 | `context_switch`, `thread_create`, `thread_start_idle` (jmp_buf wrappers) | **complete (2026-04-17)** |
| A-02.COLD-1 | `read_clock_ns`, `set_timer` | **complete (2026-04-17)** |
| A-02.COLD-2 | `ipi_send`, `init_thread_regs`, `read_guest_regs`, `write_guest_regs` | **complete (2026-04-17)** (debug regs stubbed pending KGDB / C-11) |
| A-02.COLD-3 | `mm_attach`, `mm_detach`, `probe`, `init`, `shutdown` (note: `thread_create`/`thread_start_idle` already done in HOT-3) | **complete (2026-04-17)** (probe/init/shutdown stubbed pending boot-sequence unification with A-03/A-04) |

## HOT-1 deliverables (2026-04-17)

| Sub | Outcome |
|---|---|
| HOT-1.0 plan | `notes/02-00-a02-plan.md` (Plan section), `notes/timings.md` |
| HOT-1.1 trap_user.c | `arch/um/backend/ptrace/trap_user.c` — `ptrace_run_userspace()` extracted; `get_skas_faultinfo`+`handle_trap` now static helpers in this file |
| HOT-1.2 userspace router | `arch/um/os-Linux/skas/process.c::userspace()` is now a 3-line loop dispatching to `seccomp_userspace_iter` (legacy inline) or `um_backend_dispatch(run_userspace, regs)` |
| HOT-1.3 wiring | `ptrace_ops.run_userspace = ptrace_run_userspace`; prototype in `<backend.h>` (new shared header) |
| HOT-1.4 build/boot | clean defconfig build; init=/bin/true boot succeeds, every boot syscall exercises the migrated path |
| HOT-1.5 conformance | `arch/um/backend/contract/test_ops.c` extended with `backend_run_userspace_wired_test` |
| HOT-1.6 tools | checkpatch clean (only pre-existing UM_KERN_ERR style warnings); gcc -fanalyzer + smatch clean |
| HOT-1.7 tracking | this update + decision D11 in `04-risks/decisions-log.md` |

Notable structural change: created `arch/um/include/shared/backend.h` for
the dispatch surface (extern `um_backend`, prototypes, dispatch macro)
so that USER TUs can use `um_backend_dispatch()` too. The kernel-only
struct definitions stay in `<asm/backend.h>` which now
`#include <backend.h>` for the shared bits.

## HOT-2 deliverables (2026-04-17)

| Sub | Outcome |
|---|---|
| HOT-2.0 plan | wrapper pattern: shared impl in `os-Linux/skas/mem.c::um_stub_mm_{map,unmap}` (renamed from bare `map`/`unmap` per A-01.1 finding); per-backend thin wrappers register in ops |
| HOT-2.1 rename | `map` → `um_stub_mm_map`, `unmap` → `um_stub_mm_unmap` in `os.h` + `os-Linux/skas/mem.c` |
| HOT-2.2 ptrace mm wrappers | `arch/um/backend/ptrace/mm.c` (1 new TU, 32 lines); each wrapper compiles to ~12 B |
| HOT-2.3 wiring | `ptrace_ops.mm_map = ptrace_mm_map; .mm_unmap = ptrace_mm_unmap;`. Prototypes added to `<backend.h>` |
| HOT-2.4 tlb.c | local `vm_ops.{mmap,unmap}` now sourced from `um_backend->mm_map / mm_unmap` for user-mm; `kern_map`/`kern_unmap` stay for `init_mm` |
| HOT-2.5 mmu.c | `init_new_context` initial `unmap(new_id, 0, STUB_START)` → `um_backend_dispatch(mm_unmap, …)` |
| HOT-2.6 build/boot | clean defconfig; vmlinux text +80 B; init=/bin/true boot OK |
| HOT-2.7 conformance | `backend_mm_map_wired_test` added |
| HOT-2.8 tools | checkpatch on the diff: 0 errors, 0 warnings; gcc -fanalyzer + smatch clean on new code (file-mode warnings on os.h/tlb.c/mmu.c/mem.c are pre-existing) |
| HOT-2.9 tracking | this update; no new decisions log entry — wrapper pattern follows D11 |

## HOT-3 deliverables (2026-04-17)

| Sub | Outcome |
|---|---|
| HOT-3.0 plan | 3 ops (context_switch + thread_create + thread_start_idle) share the jmp_buf machinery; per-backend wrappers in arch/um/backend/ptrace/thread.c; shared switch_threads/new_thread/start_idle_thread impls stay in os-Linux/skas/process.c (move to backend/common/ deferred to A-02.COLD-3) |
| HOT-3.1 thread.c | `arch/um/backend/ptrace/thread.c` (47 lines, 3 thin wrappers) |
| HOT-3.2 wiring | ptrace_ops scheduling fields populated; prototypes in `<backend.h>` |
| HOT-3.3 callers | kernel/process.c::__switch_to + copy_thread; kernel/skas/process.c::start_uml — all switched to `um_backend_dispatch(...)` |
| HOT-3.4 build/boot | clean defconfig; vmlinux text +123 B; init=/bin/true boot OK; every kthread switch exercises ptrace_context_switch |
| HOT-3.5 conformance | `backend_thread_ops_wired_test` added (4th test in suite) |
| HOT-3.6 tools | checkpatch on HOT-3 diff: 0 errors, 0 warnings; gcc -fanalyzer + smatch clean on new code |
| HOT-3.7 tracking | this update |

Each HOT migration carries a perf-CI gate per A-07: cycle-level
microbenchmark vs the baseline captured in `notes/timings.md`.

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
