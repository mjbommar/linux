# A-01.1 — Backend-relevant call-site inventory

Date: 2026-04-17
Owner: claude-code session
Inputs: A-01.0 survey + grep passes over arch/um/

## Method

Six grep vectors, run against `arch/um/`:

1. `using_seccomp` — the existing informal "ops table" branches.
2. `\bptrace\s*\(` — every host ptrace syscall (ptrace-backend-only).
3. `secure_computing|seccomp_filter|SECCOMP_RET` — seccomp-backend-only.
4. Direct calls to skas functions: `handle_syscall`, `userspace`,
   `start_userspace`, `switch_threads`, `new_thread`, `start_idle_thread`,
   `halt_skas`, `reboot_skas`, `initial_thread_cb_skas`, `current_mm_*`,
   `new_thread_handler`, `init_new_context`, `destroy_context`.
5. `mm_id->`/`mm_idp->`/`mm->context.id`/`mmu->id` — every reach into the
   per-mm backend handle.
6. The host-OS abstraction surface that is backend-shared but
   trap-mechanism-relevant: `os_send_ipi`, `os_*_timer*`, `os_nsecs`,
   `os_kill_ptraced_process`, `os_alarm_process`, `os_futex_*`,
   `get_safe_registers`, `os_map_memory`, `os_protect_memory`,
   `os_unmap_memory`, `os_drop_memory`, signal handler table.

## Result: 87 backend-relevant sites across 23 files

Grouped by category. Each row gives: site → file:line(s) → proposed op
or "shared host" (= moves to a shared layer, not into ops table).

### Category 1 — Trap mechanism (lifecycle, syscall dispatch, fault delivery)

| # | Call site | File:line | Maps to |
|---|---|---|---|
| 1 | seccomp probe (sets `using_seccomp`) | os-Linux/start_up.c:477,488 | `ops->probe()` + `init_backend()` arbiter |
| 2 | `using_seccomp` declaration | os-Linux/skas/process.c:429 | becomes `enum um_backend_kind active_backend` |
| 3 | `using_seccomp` extern | include/shared/skas/skas.h:11 | removed; replaced by `um_backend->kind` |
| 4 | `start_userspace(mm_id)` | os-Linux/skas/process.c:442 (def); kernel/skas/mmu.c:65 (call) | `ops->mm_attach(mm_id)` |
| 5 | `userspace(regs)` trap loop | os-Linux/skas/process.c:546 (def); kernel/process.c:126,143 (calls) | `ops->run_userspace(regs)` (the hot loop) |
| 6 | `handle_trap(regs)` (ptrace internal) | os-Linux/skas/process.c:270 | folded into ptrace impl of `run_userspace` |
| 7 | `handle_syscall(regs)` | kernel/skas/syscall.c:17 (def); os-Linux/skas/process.c:275,760 (calls) | guest→kernel callback (stays out of ops table; called *by* the ops impl) |
| 8 | `secure_computing()` host-side seccomp filter | kernel/skas/syscall.c:30 | guest-side seccomp policy; not part of trap ops |
| 9 | `set_stub_state` / `get_stub_state` (seccomp) | os-Linux/skas/process.c:596,625 | internal to seccomp `run_userspace` |
| 10 | `wait_stub_done` / `wait_stub_done_seccomp` | os-Linux/skas/process.c (multiple) | internal to each backend |
| 11 | `os_kill_ptraced_process(pid,...)` | os-Linux/process.c:53 (def); kernel/reboot.c:34, kernel/skas/mmu.c:102, os-Linux/util.c:153, os-Linux/skas/process.c:533 | `ops->mm_detach(mm_id)` (renamed; current name leaks ptrace) |
| 12 | `start_userspace` clone() pattern | os-Linux/skas/process.c:477 | internal to ptrace+seccomp `mm_attach` |
| 13 | `seccomp_helper` clone() probe | os-Linux/start_up.c:318 | internal to seccomp `probe()` |
| 14 | `get_safe_registers(...)` | os-Linux/registers.c:37 (def); kernel/{exec,process}.c, os-Linux/skas/mem.c | `ops->init_thread_regs(regs)` |
| 15 | signal table `sig_info[SIG{SEGV,TRAP,BUS,FPE,ILL,WINCH}]` | os-Linux/signal.c:25-32 | `ops->install_trap_handlers()` (one-shot at init) |
| 16 | `segv_handler` SIGSEGV entry | kernel/trap.c:289; registered os-Linux/signal.c:32 | called *by* trap handler; stays kernel-side |
| 17 | `relay_signal`/`winch` | kernel/trap.c:397,426 | called *by* trap handler; stays kernel-side |
| 18 | `fatal_sigsegv()` | kernel/trap.c:266 | shared; called by both backend impls |
| 19 | `is_skas_winch(...)` | os-Linux/skas/process.c:37; drivers/chan_user.c:306 | rename to `is_backend_helper_pid`; backend-shared |
| 20 | host ptrace() syscalls (18 sites) | os-Linux/{registers,process,start_up,skas/mem,skas/process}.c | internal to ptrace backend |
| 21 | seccomp BPF filters | os-Linux/start_up.c:260-261; kernel/skas/stub_exe.c:153,162,190,193 | internal to seccomp backend |

**Subtotal: 21 sites in 8 files.**

### Category 2 — Memory management (mmap/munmap/protect/sync)

| # | Call site | File:line | Maps to |
|---|---|---|---|
| 22 | `init_new_context(task,mm)` | include/asm/mmu_context.h:22; kernel/skas/mmu.c:43 | `ops->mm_attach()` (creates stub process) |
| 23 | `destroy_context(mm)` | include/asm/mmu_context.h:25; kernel/skas/mmu.c:80 | `ops->mm_detach()` |
| 24 | `map(mm_idp, virt, len, prot, fd, off)` | os-Linux/skas/mem.c:234 (def); kernel/tlb.c:33 wraps via `os_map_memory`; kernel/skas/mmu.c uses indirectly | `ops->mm_map(mm_idp, ...)` |
| 25 | `unmap(mm_idp, addr, len)` | os-Linux/skas/mem.c:267 (def); kernel/tlb.c:85,105,128,151,188 (5 sites); kernel/skas/mmu.c:70 | `ops->mm_unmap(mm_idp, ...)` |
| 26 | `syscall_stub_flush(mm_idp)` | os-Linux/skas/mem.c:137 (def); os-Linux/skas/process.c:651 | `ops->mm_flush(mm_idp)` |
| 27 | `syscall_stub_alloc(mm_idp)` | os-Linux/skas/mem.c:157 | internal helper to mm_map/mm_unmap |
| 28 | `syscall_stub_dump_error(mm_idp)` | os-Linux/skas/mem.c:25 (def); skas/process.c:617 | internal to backend; debug helper |
| 29 | `current_mm_id()` / `current_stub_stack()` / `current_mm_sync()` | kernel/skas/process.c:45,53,61 | shared kernel helpers; consumed by ops impls |
| 30 | `os_map_memory` (host) | os-Linux/process.c:87 (def); kernel/{tlb,physmem}.c:33,32,77 | shared host service; stays in os.h |
| 31 | `os_protect_memory` | os-Linux/process.c:103 (def); kernel/mem.c:138 | shared host service |
| 32 | `os_unmap_memory` | os-Linux/process.c:114 (def); kernel/{tlb,time}.c:41,101 | shared host service |
| 33 | `os_drop_memory` | os-Linux/process.c:128 (def); drivers/mconsole_kern.c:362 | shared host service |
| 34 | `um_tlb_sync(mm)` | kernel/tlb.c:161 (def); kernel/{trap,skas/process}.c:325,66 | shared kernel; calls `ops->mm_map/unmap` internally |
| 35 | `flush_tlb_*` family | kernel/tlb.c, asm/tlbflush.h | wrappers around `um_tlb_sync`; arch-generic |
| 36 | `enter_turnstile` / `exit_turnstile` | os-Linux/skas/mem.c, kernel/skas/mmu.c | per-mm serialization; backend-shared |
| 37 | mm_id->{stack, pid, sock, syscall_data_len, syscall_fd_*} field accesses (~30 sites in 6 files) | kernel/{smp,time,reboot}.c, kernel/skas/{mmu,process}.c, os-Linux/skas/{mem,process}.c | hide behind `mm_id` opaque + accessor inlines |

**Subtotal: 16 sites + ~30 field accesses across 9 files.**

### Category 3 — Scheduling (context switch, IPI, idle)

| # | Call site | File:line | Maps to |
|---|---|---|---|
| 38 | `switch_threads(prev_jb, next_jb)` | os-Linux/skas/process.c:805 (def); kernel/process.c:78 (__switch_to) | `ops->context_switch(prev,next)` (hot path) |
| 39 | `new_thread(stack, jb, handler)` | os-Linux/skas/process.c:793 (def); kernel/{process,smp}.c:173,160 | `ops->thread_create(...)` |
| 40 | `start_idle_thread(stack, jb)` | os-Linux/skas/process.c:819 (def); kernel/skas/process.c:41 (start_uml) | `ops->thread_start_idle(...)` |
| 41 | `new_thread_handler()` | kernel/process.c:110 (def); um_arch.c:255 | shared kernel; called by ops post-switch |
| 42 | `initial_thread_cb_skas(proc,arg)` | os-Linux/skas/process.c:863 (def); kernel/process.c:190 | `ops->thread_initial_cb(...)` |
| 43 | `os_send_ipi(cpu,vector)` | os-Linux/smp.c:92 (def); kernel/smp.c:34,39,47,57 (4 sites) | `ops->ipi_send(cpu,vector)` |
| 44 | `ipi_sig_handler` | os-Linux/smp.c:120 | internal to ipi_send install |
| 45 | `os_alarm_process(pid)` | os-Linux/process.c:25 (def); kernel/{smp,time}.c:69,876 | `ops->ipi_self()` (preempt poke) |
| 46 | `os_idle_sleep` / `os_idle_prepare` | os-Linux/time.c | shared host; called by `arch_cpu_idle*` |
| 47 | `time_travel_sleep` | kernel/time.c | layer-2 (gated); not in ops table |

**Subtotal: 10 sites in 6 files.**

### Category 4 — Time

| # | Call site | File:line | Maps to |
|---|---|---|---|
| 48 | `os_nsecs()` (monotonic) | os-Linux/time.c:103 (def); kernel/time.c:904 | `ops->read_clock_ns()` (cheap; hot) |
| 49 | `os_timer_create()` | os-Linux/time.c:44 (def); kernel/time.c:921 | `ops->timer_init()` |
| 50 | `os_timer_set_interval(cpu,ns)` | os-Linux/time.c:61 (def); kernel/time.c:642,814 | `ops->timer_set_interval(cpu,ns)` |
| 51 | `os_timer_one_shot(cpu,ns)` | os-Linux/time.c:77 (def); kernel/time.c:644,834 | `ops->timer_one_shot(cpu,ns)` |
| 52 | `os_timer_disable(cpu)` | os-Linux/time.c:95 (def); kernel/time.c:631,793; os-Linux/main.c:170 | `ops->timer_disable(cpu)` |
| 53 | `os_persistent_clock_emulation` | os-Linux/time.c | `ops->read_persistent_clock_ns()` (cold) |

**Subtotal: 6 sites in 3 files.** (Bundle 49–52 into one `timer_*` op family if granularity rule allows; see A-01.2.)

### Category 5 — I/O surfaces (host I/O, sigio, irq)

| # | Call site | File:line | Maps to |
|---|---|---|---|
| 54 | `os_*_epoll_*` family | os-Linux/irq.c | shared host service (all backends use epoll-on-host) |
| 55 | `os_event_mask(irq_type)` | os-Linux/irq.c | shared host service |
| 56 | `add_sigio_fd` / `ignore_sigio_fd` | os-Linux/sigio.c | shared host service |
| 57 | `os_run_helper_thread` | os-Linux/helper.c (clone CLONE_VM) | shared host service |
| 58 | `os_*_file*` (~40 wrappers) | os-Linux/file.c, include/shared/os.h | shared host service |
| 59 | `os_kill_process(pid, reap)` | os-Linux/process.c:33 (def); drivers/{port,line,xterm,chan}.* | shared host service |
| 60 | `os_reap_child()` | os-Linux/process.c:70 (def); kernel/skas/mmu.c:119 | shared host; SIGCHLD-driven |
| 61 | `os_futex_{wait,wake}` | os-Linux/process.c:195,204 (def); kernel/{smp,skas/mmu}.c | shared host |
| 62 | virtio-uml device emulation | drivers/virtio_uml.c | layer-3 (independent of trap mechanism) |

**Subtotal: I/O is overwhelmingly shared host services. None enter the ops table.**
**Result: 0 ops in I/O category.** (Confirms architecture doc — `host_io_submit` slot is reserved for future use; today not needed.)

### Category 6 — Debug / introspection

| # | Call site | File:line | Maps to |
|---|---|---|---|
| 63 | `get_safe_registers` (already counted as #14) | — | `ops->init_thread_regs` |
| 64 | `ptrace_setregs` (ptrace-only) / register get/set in seccomp via `set/get_stub_state` | os-Linux/skas/{mem,process}.c | `ops->{read,write}_guest_regs()` (cold) |
| 65 | `get_skas_faultinfo()` | os-Linux/skas/process.c:251 | `ops->fetch_faultinfo()` (called from segv path) |
| 66 | KGDB / KDB hooks | (not present today) | future `ops->{read,write}_guest_regs` consumers |
| 67 | `singlestepping()` plumbing | kernel/process.c:258; passed to backend in trap loop | `ops->set_singlestep(thread, enable)` (cold) |

**Subtotal: 3 ops needed; rest are folded into others.**

## Site count summary

| Category | Distinct call sites (rough) | Files touched |
|---|---|---|
| 1. Trap | 21 | 8 |
| 2. Memory | 16 (+~30 field accesses) | 9 |
| 3. Scheduling | 10 | 6 |
| 4. Time | 6 | 3 |
| 5. I/O | (shared, not in ops) | — |
| 6. Debug | 3 | 4 |
| **Total** | **~56 ops-relevant; ~50 if we collapse field accesses to "make mm_id opaque"** | **~17 unique** |

Within the spec target of ~50 backend-relevant call sites. Rough
ops-table size estimate before A-01.2 grouping pass: **~13–15 ops**.

## Cross-file impact heatmap

Files that will gain `#include <asm/backend.h>` and call through ops:

- `arch/um/kernel/process.c` — context_switch, run_userspace
- `arch/um/kernel/skas/mmu.c` — mm_attach, mm_detach (rename + delegate)
- `arch/um/kernel/tlb.c` — mm_map, mm_unmap, mm_flush
- `arch/um/kernel/smp.c` — ipi_send, ipi_self
- `arch/um/kernel/time.c` — timer ops
- `arch/um/kernel/reboot.c` — mm_detach
- `arch/um/kernel/exec.c` — init_thread_regs

Files that move into per-backend dirs:

- `arch/um/os-Linux/skas/process.c` → split into
  `arch/um/backend/ptrace/process.c` + `arch/um/backend/seccomp/process.c`,
  with the loop body extracted per-backend. The `userspace()` function
  is the heart of A-02 (refactor; 6 wk).
- `arch/um/os-Linux/skas/mem.c` → mostly seccomp-aware already; split
  into per-backend `mm_*` impls.
- `arch/um/os-Linux/start_up.c` (lines 41-244 ptrace probe; 246-498
  seccomp probe) → per-backend `probe()` impls.

Files that stay backend-neutral (just rename / no functional change):

- `arch/um/include/shared/os.h` — keep as the shared host-services
  surface (file/sigio/irq/futex/fork/timer); no ops dispatch.
- `arch/um/include/shared/skas/{mm_id,stub-data}.h` — `mm_id` becomes
  the opaque per-mm backend handle; field access via inlines.
- `arch/um/kernel/trap.c`, `kernel/process.c::new_thread_handler` —
  consumers of the ops, not implementers.

## Surprises and notes for downstream tasks

1. **`os.h` is *not* the abstraction.** It's the shared host-services
   layer — both backends use the same `os_*` API. The actual divergence
   lives behind a single `using_seccomp` runtime check. Don't conflate
   "make `os.h` the ops table" with the redesign goal.

2. **`map()`/`unmap()` are the worst-named functions in arch/um.**
   Bare names. Used by all backends. Will need renaming to
   `um_backend_map_user`/`unmap_user` (matching the architecture doc's
   `map_user`/`unmap_user` slot names) when promoted into the ops table.
   Risk: name collisions with `<linux/mman.h>`. A-02 must check.

3. **`new_thread`/`switch_threads` rely on `jmp_buf` + setjmp/longjmp.**
   Inherently host-process-based. The KVM backend (workstream D) cannot
   use the same primitive — it needs vCPU thread + KVM_RUN. So
   `ops->context_switch` and `ops->thread_*` have radically different
   shapes per backend. **The ops table contract must accept this** —
   the prev/next arguments are kernel-task-struct level, not jmp_buf
   level. The jmp_buf is an implementation detail of ptrace+seccomp.

4. **mm_id field overlap.** `mm_id` carries fields used by *both*
   backends (`pid`, `stack`) and fields used by *only seccomp* (`sock`,
   `syscall_fd_map`, `syscall_fd_num`). Two paths:
   - Keep one struct, document which fields are which-backend-only.
   - Make `mm_id` an opaque pointer with backend-private allocation.
   Decision deferred to A-01.2 (granularity) and A-01.8 Q2 (state
   ownership). Leaning toward keep-one-struct because fields are tiny
   and avoid an allocator round-trip per fault.

5. **`is_skas_winch` is misnamed.** It's a probe for "is this PID one
   of our backend helper threads?" Used by `chan_user.c` to decide
   whether to register a winch IRQ handler. Becomes shared, renamed.

6. **`secure_computing()` in `kernel/skas/syscall.c` is *guest* seccomp,
   not backend seccomp.** This is `linux/seccomp.h` filtering applied
   to syscalls *issued by the guest*, identical to native x86. Not
   related to the seccomp backend. Stays as-is.

7. **`time_travel_*` lives in `userspace()` and `handle_syscall()`
   today.** It's already a runtime branch, identical to a layer-2
   gate but without the static-key infrastructure. Workstream B will
   convert it. Don't try to absorb into Layer 1.

8. **No backend struct exists today.** The grep for
   `struct (host_ops|kern_handlers|sig_info)` returns nothing. We are
   building the abstraction from a typed flag (`int using_seccomp`).
   The abstraction does not have to fight existing typed code.

## Inputs to A-01.2 (categorization)

Proposed op grouping (15 ops; refined in next task):

```
trap.probe                   /* per-backend; called by init_backend() */
trap.init                    /* one-time backend init */
trap.shutdown                /* teardown */
trap.run_userspace           /* the hot loop */
mm.attach                    /* per-mm: clone stub */
mm.detach                    /* per-mm: kill stub */
mm.map                       /* page table → host mapping */
mm.unmap                     /* tear down mapping */
mm.flush                     /* drain pending stub syscalls */
sched.thread_create          /* per-thread setup */
sched.context_switch         /* hot */
sched.ipi_send               /* SMP only */
time.read_clock_ns           /* hot */
time.set_timer               /* periodic + one-shot folded */
debug.read_guest_regs        /* cold; KGDB consumer */
debug.write_guest_regs       /* cold */
```

That's 16; A-01.2 will decide whether `time.set_timer` stays one or
splits into `interval`/`one_shot`/`disable`, and whether `mm.flush`
stays exposed or becomes implicit on `mm.map/unmap`.

## Out of scope for A-01.1 (deferred)

- Mapping each call site to *which* op (granularity decision in A-01.2,
  validation in A-01.7).
- Performance cost of indirect dispatch at each site (covered by
  A-07 perf CI).
- KVM-backend-specific ops (D workstream).
