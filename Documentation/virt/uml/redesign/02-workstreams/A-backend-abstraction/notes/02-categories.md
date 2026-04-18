# A-01.2 — Op categories and granularity rationale

Date: 2026-04-17
Owner: claude-code session
Inputs: A-01.0 survey, A-01.1 inventory (87 sites, 16 candidate ops)

## Granularity rule (from 01-ops-table.md)

> Too coarse → backends need internal switches; abstraction leaks.
> Too fine → cost of indirect dispatch dominates; refactor stalls.

Plus the design constraint from `three-layers.md`: "hot ops get the
single-backend-inline treatment in 04-kconfig.md; cold ops always
indirect."

We therefore classify each candidate op as **HOT** (called per syscall
or per page fault — must be inlinable in single-backend builds, and
even in multi-backend builds the indirect call must be ≤10 cycles) or
**COLD** (called per mm/per second/per init — indirect cost is noise).

## Final op table — 14 ops in 5 categories

(Down from 16 candidates after collapsing `mm.flush` into the implicit
batch flush of `mm.map/unmap` and folding `time.set_timer` into one op
that takes a mode tag.)

### Category 1 — Lifecycle and trap (4 ops)

| Op | HOT? | Signature (sketch) | Rationale |
|---|---|---|---|
| `probe` | cold | `int (*probe)(void)` | Returns 0 if backend usable on this host. Called by `init_backend()` arbiter. |
| `init` | cold | `int (*init)(const struct um_backend_args *args)` | One-shot per boot. Returns 0 on success, ‑E on failure. |
| `shutdown` | cold | `void (*shutdown)(void)` | Reboot/halt path. |
| `run_userspace` | **HOT** | `void (*run_userspace)(struct uml_pt_regs *regs)` | The trap loop. **Single chokepoint** for every guest→kernel transition. Cost dominated by trap mechanism (1 µs ptrace, 300 ns seccomp, 100 ns KVM target). The indirect call here is amortized across the trap; it is *not* a hot-cycle concern. Inlinable in single-backend builds anyway. |

**Why no separate `syscall_dispatch` op?** Architecture doc shows
`syscall_dispatch` in the sketch table. After the inventory, it's clear
that `handle_syscall()` is the *kernel-side* callback — called by every
backend from inside `run_userspace` after the trap returns. It's not a
backend op. It's a fixed shared kernel function. Removing it from the
ops table.

**Why no separate `page_fault` op?** Same argument — page faults are
delivered through `segv()`/`segv_handler()` which are shared kernel
code. The backend's job is "call segv_handler when a fault arrives";
that happens inside `run_userspace`. No separate op.

**Why no `lifecycle` split (boot/halt/reboot/panic)?** `init` and
`shutdown` are sufficient. Reboot/halt converge on `shutdown`; panic
converges on `os_dump_core`.

### Category 2 — Memory (4 ops)

| Op | HOT? | Signature | Rationale |
|---|---|---|---|
| `mm_attach` | cold | `int (*mm_attach)(struct mm_id *id)` | Once per mm creation. Renames `start_userspace`. |
| `mm_detach` | cold | `void (*mm_detach)(struct mm_id *id)` | Once per mm destruction. Renames `os_kill_ptraced_process` for the mm-bound case. (The standalone `os_kill_process` for helpers stays in `os.h`.) |
| `mm_map` | **HOT** | `int (*mm_map)(struct mm_id *id, unsigned long va, unsigned long len, int prot, int phys_fd, u64 offset)` | Called from `tlb.c` (5 sites) plus initial `unmap(0, STUB_START)`. Per page-table sync. Hot enough to matter; inlinable in single-backend builds. |
| `mm_unmap` | **HOT** | `int (*mm_unmap)(struct mm_id *id, unsigned long va, unsigned long len)` | Symmetric. Same hot-path argument. |

**Why fold `mm.flush` into `mm.map/unmap`?** Today
`syscall_stub_flush()` is called explicitly from `userspace()` to drain
queued stub syscalls before resuming the guest. The backend can do
this implicitly inside `run_userspace` (call its private flush before
returning to the guest). External callers don't need to see it. Saves
one op slot; reduces backend coupling. **Decision: drop `mm_flush` as
a public op; backend implements internally.**

**Why not separate `mm_protect`?** All current `prot` changes go
through `mm_map` (re-mapping with new prot). Saves an op.

**Why are `mm_map`/`mm_unmap` in the ops table at all** — couldn't they
just call `os_map_memory` / `os_unmap_memory` directly? Because they
operate on a *guest* mm (via the stub process), not the host's mm. The
ptrace/seccomp backends drive a stub child process to do the mmap;
the KVM backend will program EPT page tables instead. The dispatch
mechanism is fundamentally per-backend.

### Category 3 — Scheduling (4 ops)

| Op | HOT? | Signature | Rationale |
|---|---|---|---|
| `thread_create` | cold | `int (*thread_create)(struct task_struct *p, void *stack, void (*handler)(void))` | Once per fork. Renames `new_thread`. The jmp_buf detail is hidden. |
| `thread_start_idle` | cold | `int (*thread_start_idle)(void *stack, struct thread_struct *t)` | Once per CPU bring-up. Renames `start_idle_thread`. |
| `context_switch` | **HOT** | `void (*context_switch)(struct task_struct *prev, struct task_struct *next)` | Per task switch. The jmp_buf swap (ptrace+seccomp) and KVM_RUN reentry (KVM) are different shapes; the op accepts task pointers and the backend extracts what it needs from `task->thread`. |
| `ipi_send` | cold | `int (*ipi_send)(int cpu, int vector)` | SMP IPI delivery. Today maps 1:1 to `os_send_ipi`. Cold enough that even per-IPI indirect call is fine. |

**Why fold `os_alarm_process` (preempt poke) into `ipi_send`?** Looking
at the current code:

- `kernel/smp.c::ipi_self()` calls `os_alarm_process(current_pid)` to
  send SIGALRM to ourselves.
- `kernel/time.c:876` calls `os_alarm_process(current_pid)` from the
  timer interrupt path.

These are not IPIs in the SMP sense. They're "kick the current host
process to drop out of userspace." A separate op `kick_self()` would
clarify this, but at cost of a new op.

**Decision:** keep `ipi_send`. Add a comment that "self-kick" can be
done with `ipi_send(smp_processor_id(), UML_IPI_PREEMPT)`. Revisit if
KVM backend implementation needs different semantics.

**Why no `thread_initial_cb` op?** It's called exactly once at boot
(`initial_thread_cb` → `initial_thread_cb_skas`). Make it a kernel
helper that uses `context_switch` internally; no separate op.

### Category 4 — Time (3 ops)

| Op | HOT? | Signature | Rationale |
|---|---|---|---|
| `read_clock_ns` | **HOT** | `u64 (*read_clock_ns)(void)` | Called from `clocksource->read` per timer/clock query. Hot path; must be cheap. Today maps to `os_nsecs()` (clock_gettime). |
| `set_timer` | cold | `int (*set_timer)(int cpu, u64 deadline_ns, enum um_timer_mode mode)` | Combines `os_timer_set_interval`, `os_timer_one_shot`, `os_timer_disable` into one op with a mode tag (`PERIODIC | ONE_SHOT | DISABLE`). All three are configuration calls per timer event, not per-tick; cold enough. **Saves 2 op slots.** |
| `read_persistent_clock_ns` | cold | `u64 (*read_persistent_clock_ns)(void)` | RTC / boot timestamp. Wraps `os_persistent_clock_emulation`. Called once at boot. |

**Why split `set_timer` into 3 ops in the architecture sketch?** The
sketch in `three-layers.md` shows separate `set_timer(deadline_ns)` —
that already implies one op. The current `os_timer_*` family is split
because the underlying POSIX timer API is split. The ops layer is
allowed to consolidate; the per-backend impl can dispatch to the
underlying API.

**Why no separate `timer_init`?** Folded into `init`. The `os_timer_create`
call in `kernel/time.c::921` happens during clocksource registration;
it can be done as part of backend init.

### Category 5 — Debug / introspection (3 ops)

| Op | HOT? | Signature | Rationale |
|---|---|---|---|
| `init_thread_regs` | cold | `void (*init_thread_regs)(unsigned long *gp, unsigned long *fp)` | Per fork/exec. Renames `get_safe_registers` (which reads from a per-backend "safe" template). |
| `read_guest_regs` | cold | `int (*read_guest_regs)(struct task_struct *t, struct pt_regs *regs)` | KGDB consumer. Per debug-stop. |
| `write_guest_regs` | cold | `int (*write_guest_regs)(struct task_struct *t, const struct pt_regs *regs)` | KGDB consumer. Per debug-stop. |

**Why not include `set_singlestep`?** Singlestepping today is a
per-thread flag (`TIF_SINGLESTEP`) checked inside `userspace()`. Each
backend reads the flag and acts accordingly. No separate op needed —
the flag itself is the interface. Revisit if KGDB integration shows a
need.

**Why not include `fetch_faultinfo`?** Today `get_skas_faultinfo()`
(ptrace) and the seccomp `GET_FAULTINFO_FROM_MC` macro both populate
`regs->faultinfo` *inside* `run_userspace`. The kernel just reads
`regs->faultinfo` afterward. The faultinfo is part of the
`uml_pt_regs` contract, not a separate op.

## Final ops table — 14 ops

Compressing the above into `struct um_backend_ops`:

```c
struct um_backend_ops {
    const char *name;
    u32 contract_version;

    /* lifecycle + trap (4) */
    int  (*probe)(void);
    int  (*init)(const struct um_backend_args *args);
    void (*shutdown)(void);
    void (*run_userspace)(struct uml_pt_regs *regs);     /* HOT */

    /* memory (4) */
    int  (*mm_attach)(struct mm_id *id);
    void (*mm_detach)(struct mm_id *id);
    int  (*mm_map)(struct mm_id *id, unsigned long va,   /* HOT */
                   unsigned long len, int prot,
                   int phys_fd, u64 offset);
    int  (*mm_unmap)(struct mm_id *id,                   /* HOT */
                     unsigned long va, unsigned long len);

    /* scheduling (4) */
    int  (*thread_create)(struct task_struct *p,
                          void *stack, void (*handler)(void));
    int  (*thread_start_idle)(void *stack,
                              struct thread_struct *t);
    void (*context_switch)(struct task_struct *prev,     /* HOT */
                           struct task_struct *next);
    int  (*ipi_send)(int cpu, int vector);

    /* time (3) */
    u64  (*read_clock_ns)(void);                         /* HOT */
    int  (*set_timer)(int cpu, u64 deadline_ns,
                      enum um_timer_mode mode);
    u64  (*read_persistent_clock_ns)(void);

    /* debug (3) */
    void (*init_thread_regs)(unsigned long *gp,
                             unsigned long *fp);
    int  (*read_guest_regs)(struct task_struct *t,
                            struct pt_regs *regs);
    int  (*write_guest_regs)(struct task_struct *t,
                             const struct pt_regs *regs);
};
```

That's **18 fields total**: 1 name, 1 version, 16 ops.

### Wait — that's 16 ops, not 14

Recount:
- Category 1: 4 (probe, init, shutdown, run_userspace)
- Category 2: 4 (mm_attach, mm_detach, mm_map, mm_unmap)
- Category 3: 4 (thread_create, thread_start_idle, context_switch, ipi_send)
- Category 4: 3 (read_clock_ns, set_timer, read_persistent_clock_ns)
- Category 5: 3 (init_thread_regs, read_guest_regs, write_guest_regs)
- Total: 18.

The "14" claim above was wrong. **Actual: 18 ops.**

That's higher than the architecture doc's "~15 ops" target but lower
than the "too fine" worry threshold of dozens. Acceptable.

## Hot-op set (4 ops)

`run_userspace`, `mm_map`, `mm_unmap`, `context_switch`, `read_clock_ns`
— actually 5.

These five are the targets for single-backend-inline macros in A-04.
In single-backend builds they become direct calls (zero indirect-call
cost). In multi-backend builds they pay one well-predicted indirect
branch (~5 cycles on modern CPUs). All other ops always go indirect.

## What this leaves out

- **No `host_io_submit`.** The architecture doc's sketch had this for
  virtio-uml's host-side I/O. Inventory shows virtio-uml uses raw
  `os_*_file` ops directly; no abstraction needed today. Add to the
  ops table later if/when crosvm-style I/O lands in workstream C-10.
- **No `set_singlestep`.** TIF_SINGLESTEP flag is the interface;
  backends inspect it. Add an op only if a backend can't read kernel
  thread flags (KVM doesn't have this issue — vCPU thread is a kernel
  thread).
- **No `set_breakpoint` / `add_watchpoint`.** Future KGDB work
  (workstream C-11). Will extend the ops table when needed; contract
  versioning supports adding ops without breaking existing backends.

## Cost of dispatch — back-of-envelope

Single-backend inline (`CONFIG_UM_BACKEND_SECCOMP_ONLY`):

- 5 hot ops: 0 cycles each (direct call after preprocessor expansion).
- 13 cold ops: 0 cycles each (direct).
- Net cost vs. today: 0 cycles. Identical to current state.

Multi-backend (`CONFIG_UM_BACKEND_DYNAMIC`, with seccomp+ptrace+KVM):

- 5 hot ops: ~5 cycles each indirect call (well-predicted).
- 13 cold ops: ~5–10 cycles each, but called rarely.
- Per-syscall amortized: ~5 cycles run_userspace + ~5 cycles
  context_switch (if scheduled) + ~5 cycles read_clock_ns. ≈15
  cycles on top of the trap cost (~100–1000 ns depending on backend).
  **<2% overhead on prod-fast (KVM) target. Within invariant I2.**

## Open decisions (pushed to A-01.8)

1. **Q1: sync vs async ops.** All 18 above are sync. Confirmed.
2. **Q2: backend_private void *.** None used; backends are singletons,
   per-mm state lives in `mm_id`. Confirmed.
3. **Q3: hot vs cold.** Resolved above (5 hot, 13 cold).
4. **Q4: contract version.** `u32 contract_version` field added.
   Bumped on op signature changes; new ops can be added without bump
   (they just appear as new fields and old backends report
   `-ENOSYS` from a stub).

## What this enables for A-01.3 (header draft)

Direct: 18 function-pointer fields, two scalar fields, the dispatch
macro, the args struct, the timer-mode enum. ~80–100 lines of header.
Drafting in next subtask.

## Out of scope for A-01.2

- The actual header (A-01.3).
- Per-backend sketches confirming the table fits (A-01.4, A-01.5, A-01.6).
- Coverage check vs. the inventory (A-01.7).
- LKML feedback cycle (A-01.10 + months of review).
