# A-01.5 — Seccomp backend sketch

Date: 2026-04-17
Owner: claude-code session
Inputs: A-01.0 (survey), A-01.1 (inventory), A-01.2 (categories),
A-01.3 (`asm/backend.h`), A-01.4 (ptrace sketch).

## Goal

Same exercise as A-01.4: map every op in `struct um_backend_ops` to
its seccomp implementation, drawn from the merged 6.16 seccomp work
(Benjamin Berg, 2021–2025). Confirm the contract fits without per-
backend extensions or side channels.

## Background

The seccomp backend ships in 6.16. Mechanism summary (from
`os-Linux/skas/process.c` and `kernel/skas/stub_exe.c`):

- The guest runs as a child host process with a seccomp filter
  installed (`SECCOMP_RET_TRAP` on every syscall except a small
  allowlist).
- On guest syscall, kernel SIGSYS fires inside the guest process; the
  in-stub SIGSYS handler captures regs/siginfo into shared memory
  (`stub_data->sigstack[]`) and waits on a futex.
- The UML kernel side (running in a separate host process / thread)
  signals the futex when ready, then resumes the guest by writing new
  state into `stub_data` and setting the futex back to `FUTEX_IN_CHILD`.
- Stub-side syscalls (mmap/munmap/sendmsg) use a syscall queue
  (`syscall_data[]`) drained on the next kernel→guest round-trip.
- Kernel→guest FD passing for new mmap fds uses an SCM_RIGHTS sendmsg
  on the per-mm socket pair set up in `start_userspace`.

## Per-op mapping

Same legend as A-01.4: EXIST / WRAP / EXTRACT / NEW.

### Lifecycle and trap

| Op | Existing seccomp impl | Action | Source |
|---|---|---|---|
| `probe` | `init_seccomp()` (full functional probe — clones a child, installs filter, validates SIGSYS path, captures host_fp_size) | EXTRACT | os-Linux/start_up.c:293–367 |
| `init` | implicit today (`os_early_checks` does the work); for the new contract, gather the per-process initial sigstack/exec_regs setup | NEW (one entry point that drives the init chain) | os-Linux/start_up.c:463 |
| `shutdown` | `halt_skas`/`reboot_skas` — *same* impl as ptrace; the longjmp-based shutdown isn't trap-mechanism specific | WRAP (shared with ptrace) | os-Linux/skas/process.c:881,903 |
| `run_userspace` | `userspace()` seccomp branch (lines 593–646: `set_stub_state`, `wait_stub_done_seccomp`, `get_stub_state`, fault decoding) | EXTRACT | os-Linux/skas/process.c:593 |

### Memory

| Op | Existing seccomp impl | Action | Source |
|---|---|---|---|
| `mm_attach` | `start_userspace` seccomp branch (lines 487–489: `wait_stub_done_seccomp(mm_id, 1, 1)` after clone) plus the socketpair retention on line 526 | EXTRACT | os-Linux/skas/process.c:442 |
| `mm_detach` | per-mm: kill stub child + close the socketpair (`mmu->id.sock`); seccomp also has the FUTEX_IN_KERN dance from the mm_sigchld_irq handler | EXTRACT | kernel/skas/mmu.c:106–107 + os-Linux/process.c:53 |
| `mm_map` | `map()` queues into `syscall_data[]`; flushed on next `wait_stub_done_seccomp(running=0)` round-trip; FD passing via `get_stub_fd` + sendmsg | EXTRACT (the `syscall_data`/FD path is seccomp-only) | os-Linux/skas/mem.c:234 + 87–135 + 196–232 |
| `mm_unmap` | `unmap()`, symmetric | EXTRACT | os-Linux/skas/mem.c:267 |

### Scheduling

| Op | Existing seccomp impl | Action | Source |
|---|---|---|---|
| `thread_create` | Same `new_thread()` as ptrace — jmp_buf machinery is shared. Seccomp only changes how the trap is delivered, not how kernel-side threads are created. | WRAP (shared) | os-Linux/skas/process.c:793 |
| `thread_start_idle` | Same `start_idle_thread()` | WRAP (shared) | os-Linux/skas/process.c:819 |
| `context_switch` | Same `switch_threads()` | WRAP (shared) | os-Linux/skas/process.c:805 |
| `ipi_send` | Same `os_send_ipi()` | WRAP (shared) | os-Linux/smp.c:92 |

### Time

| Op | Existing seccomp impl | Action | Source |
|---|---|---|---|
| All three | identical to ptrace — the host POSIX timer API is mode-agnostic | WRAP (shared) | os-Linux/time.c |

### Debug

| Op | Existing seccomp impl | Action | Source |
|---|---|---|---|
| `init_thread_regs` | `get_safe_registers` — populated by either the ptrace probe *or* the seccomp probe (see `init_seccomp` lines 332–345 which capture exec_regs from the seccomp-trapped child); same access path for both | WRAP (shared symbol; init populated per-backend) | os-Linux/registers.c + os-Linux/start_up.c:332 |
| `read_guest_regs` | Today: `get_stub_state(regs, proc_data, NULL)` reads the snapshot stored in `stub_data->sigstack` by the SIGSYS handler. Need to formalize for KGDB-from-kernel-side reading | EXTRACT into a helper | os-Linux/skas/process.c:625 |
| `write_guest_regs` | Today: `set_stub_state(regs, proc_data, singlestepping())` — writes the regs the guest will resume with | EXTRACT into a helper | os-Linux/skas/process.c:596 |

## Side-channel check

The seccomp backend communicates with the guest via:

1. **`stub_data` shared memory** (`mm_id->stack` points to it). All
   syscall queue, signal info, register snapshot, futex word — all
   inside one struct accessible to both sides. ✅ no side channel
   beyond the per-mm `stub_data`.
2. **Socketpair** for FD passing. `mm_id->sock` carries the kernel-
   side end. ✅ inside `mm_id`.
3. **`mm_id->syscall_fd_map[]` / `syscall_fd_num`** — the per-mm FD
   slot table for SCM_RIGHTS. ✅ inside `mm_id`.

Every cross-op channel lives in `mm_id` or `stub_data`. Nothing
outside the ops table's argument list. **No side channel.**

## How the ops table accommodates seccomp's "queue then flush" model

Workstream A-01.2's decision to fold `mm_flush` into the implicit
flush of `mm_map`/`mm_unmap`/`run_userspace` works for seccomp:

- `seccomp_mm_map` queues into `syscall_data[]`, returns 0
  immediately. Same as today's `map()`.
- `seccomp_mm_unmap` likewise.
- `seccomp_run_userspace` calls the internal flush
  (today: `wait_stub_done_seccomp(running=0)` does it as part of the
  resume-guest dance) before yielding control to the guest.

The kernel never has to know flushing happened. ✅

## What the seccomp backend keeps that ptrace doesn't

**Backend-private state on `mm_id`:** `sock`, `syscall_fd_num`,
`syscall_fd_map[STUB_MAX_FDS]`. These fields are unused by the
ptrace backend (it uses `pid`+`stack` only).

Two ways to handle this in the formalized ops table:

A. **Keep one `struct mm_id` with all fields.** Document which fields
   are which-backend. Ptrace ignores `sock`/`syscall_fd_*`. Cost:
   16 bytes wasted per ptrace-only build (3 ints + an int array of 4).
   Already the situation today.
B. **Split `struct mm_id` core + per-backend extension.** `mm_id_core`
   has `pid`+`stack`; ptrace defines `mm_id_ptrace` containing the
   core; seccomp defines `mm_id_seccomp` with core + `sock` + FD map.
   Allocation site (`init_new_context`) calls
   `um_backend->mm_id_alloc()` to get the right size.
   Cost: extra op (`mm_id_alloc`/`mm_id_free`), plus accessor
   discipline.

**Recommendation: keep Option A** for now. 16 bytes is noise; no
allocator round-trip; consistent with current code. Document fields'
ownership in `Documentation/virt/uml/backend-contract.rst`. Revisit
in C-10 if the launcher refactor needs more per-backend state.

This is decision Q2 from A-01.0 / Q2 from A-01 README; recorded in
`08-decisions.md` (next).

## Files that move into `arch/um/backend/seccomp/`

```
arch/um/backend/seccomp/
├── Makefile
├── seccomp_backend.c        /* struct um_backend_ops seccomp_ops = { ... }; */
├── trap.c                   /* seccomp half of run_userspace,
                                wait_stub_done_seccomp, signal decode */
├── mm.c                     /* mm_attach (seccomp branch of start_userspace),
                                mm_detach, mm_map, mm_unmap, mm_flush
                                (queue + sendmsg machinery) */
├── thread.c                 /* mostly empty — shares with ptrace via
                                arch/um/backend/common/thread.c */
├── time.c                   /* shares with ptrace via common/ */
├── debug.c                  /* read_guest_regs (get_stub_state),
                                write_guest_regs (set_stub_state) */
├── probe.c                  /* init_seccomp extracted from start_up.c */
└── stub_load.c              /* userspace_tramp seccomp half */
```

Plus a shared layer:

```
arch/um/backend/common/
├── Makefile
├── thread.c                 /* switch_threads, new_thread,
                                start_idle_thread, jmp_buf machinery —
                                shared by ptrace + seccomp backends */
├── time.c                   /* set_timer dispatch wrapper, shared */
└── lifecycle.c              /* halt/reboot via initial_jmpbuf, shared */
```

(KVM backend will not use `common/` — it has its own thread/time
mechanisms.)

Estimated LoC migrated into seccomp backend: ~600 (seccomp branches +
the syscall_data queue/sendmsg machinery). Estimated `common/`: ~250
(jmp_buf shared bits).

## Cost-model notes

Seccomp backend `run_userspace` cost components per syscall:

- 1 SIGSYS delivery (~150 ns)
- 1 futex round-trip (~150 ns)
- shared-memory snapshot copy (~50 ns)
- Total ~350 ns vs ~1.2 µs ptrace. **3–4× faster** as advertised.

Per-mmap stub round-trip: ~350 ns (same one-way cost; queueing
amortizes multi-page mmaps).

Per architecture doc invariant I2: seccomp is the floor for prod-fast
*until* KVM lands. Workstream A's job is to not regress this number.

## Things that change for seccomp

1. `using_seccomp = 1` branches are extracted into seccomp impls.
2. The `init_seccomp` probe in `start_up.c` becomes
   `seccomp_probe`. The arbiter (`init_backend`) calls it and the
   ptrace probe in priority order.
3. The `seccomp=on/auto/off` boot param is consumed by the arbiter,
   not by the seccomp-specific code; it forwards into the new
   `backend=seccomp` / `backend=auto` / `backend=ptrace` family
   while keeping legacy `seccomp=` as an alias for one boot.
4. The `mm_sigchld_irq` handler in `kernel/skas/mmu.c` (which only
   runs work for seccomp, see line 122 comment) becomes part of
   `arch/um/backend/seccomp/mm.c` as a backend-specific IRQ handler
   registered during `seccomp_init`.

## Validation criterion

Per A-01 spec: every op maps cleanly. ✅
- All 18 ops have a current source.
- 8 of 18 (`thread_*`, `context_switch`, `ipi_send`, time family,
  `init_thread_regs`, `shutdown`) are *literally shared* with ptrace —
  they're implemented once in `common/` and referenced from both
  backends' op tables.
- The 10 backend-specific ops have distinct, side-channel-free
  implementations.

## Open questions specific to seccomp impl (defer to A-03)

- **Q-seccomp-1:** The `set_state`/`get_state` shared-memory snapshot
  protocol is currently informal (struct `stub_data` layout is the
  contract). Should it be versioned alongside `UM_BACKEND_CONTRACT_VERSION`?
  Probably not — it's intra-backend, not inter-component. Document
  in `arch/um/backend/seccomp/README.md`.
- **Q-seccomp-2:** `STUB_MAX_FDS = 4` is a hard cap on FDs passed in
  one mmap batch. The current `do_syscall_stub` flushes when full;
  good enough but may need raising for mmap-heavy workloads
  (e.g. JIT-heavy guests). Defer to perf CI in A-07.
- **Q-seccomp-3:** SMP seccomp is supported (per `Kconfig` "SMP not
  supported with PTRACE userspace"). The ops table contract doesn't
  need to express this — it's a probe-time refusal. Confirmed.

## Next subtask

A-01.6: KVM sketch — the forward-looking design exercise.
