# 06 — Backend Comparison: Why KVM Fails Where ptrace and seccomp Succeed

Status: architecture review (perspective: backend comparison / structural design)

Date: 2026-04-26

Scope: explain the structural differences between the three UML backends
(`ptrace`, `seccomp`, `kvm`) and identify which of those differences plausibly
cause the KVM-only "wild-pointer" failure pattern observed under
`backend=force=kvm` for medium workloads (e.g. `python3 -c "import unittest"`).

The fact that `backend=force=ptrace` and `backend=force=seccomp` are bit-perfect
on every workload is the load-bearing observation. It tells us:

- The UML kernel's mm code, scheduler, syscall handlers, signal delivery,
  fork/exec and the shared `userspace()` trap loop are all correct.
- The `mm_id` lifecycle, `init_new_context()` / `destroy_context()` shape is
  correct.
- The `um_backend_dispatch()` framework and the contract are correct.
- All four "HOT" ops (`run_userspace`, `mm_map`, `mm_unmap`, `context_switch`)
  do something coherent in ptrace and seccomp.

So the bug is something the KVM backend *adds* on top of (or *replaces*) the
shared mechanism. This document compares each axis of the backend contract,
points at the file:line where KVM diverges, and proposes a structural fix.

---

## 1. Executive Summary — Top 3 Structural Differences

### Difference 1 (root cause): The KVM backend has NO separate guest address space

ptrace and seccomp run user code in a **separate host process** (the "stub
child") forked from UML by `start_userspace()` (`arch/um/os-Linux/skas/process.c:444`).
That child has its own host page table with only the user-VA mappings (plus
the stub trampoline near `STUB_START`). UML's own host process — the parent —
does NOT have user mappings at user VAs. The kernel and the guest user
address space are physically separate processes.

KVM runs the guest under `KVM_RUN` **inside the same host process as the UML
kernel**. The guest's view of memory is the per-mm shadow page table
(`struct kvm_shadow_mm` in `arch/um/backend/kvm/kvm_backend.h:439`), which
the host kernel hand-builds and feeds to the vCPU as CR3. Worse, `kvm_mm_map`
*also* calls `os_map_memory()` (`arch/um/backend/kvm/mm.c:135`), which is a
`mmap(MAP_SHARED|MAP_FIXED)` (`arch/um/os-Linux/process.c:313`) into the
**parent UML process's own VA space** at the same user VA the guest sees.

This produces the hazard the working backends literally cannot have: a guest
user VA `0x4000_0000_0000` in KVM is **also** a valid host VA in the UML
parent process and therefore a valid pointer into UML's own libc / heap /
allocator state if it ever falls through the cracks. A wild pointer that
would crash a *separate* process by going off the end of its empty mappings
will instead silently land on a UML-kernel allocation (or near-NULL pages
that the parent has). This is the architectural source of "NULL+offset" and
"45-bit garbage that almost decodes" failure shapes — they are not random
garbage, they are valid host VAs.

### Difference 2: KVM owns its own MMU and translates UML PTEs by hand

Under ptrace and seccomp, the host kernel walks the stub child's host page
table on every guest access. UML's responsibility is just to keep the host
page table in sync via `mmap()`/`munmap()` against the stub child. There is
**no second translation layer.** The host hardware MMU enforces protection;
host kernel signal delivery converts faults to SIGSEGV.

Under KVM, UML must build a **second** page table — the shadow PT — that the
guest CPU uses as CR3. This is a hand-rolled 4-level x86_64 page table built
by `kvm_shadow_map_page()` (`arch/um/backend/kvm/lifecycle.c:1360`) from
hand-translated PTEs computed by `kvm_um_pte_to_x86()`
(`arch/um/backend/kvm/lifecycle.c:1063`). A bit-for-bit translator that
silently maps `0x000` to `0x000` looks fine but is one of three things that
must match. The UML pgd, the host VA mapping (`os_map_memory`), and the KVM
shadow PT all must agree. The redesign memo (15-direct-shadow-sync.md)
explicitly calls this "three views that must agree" and documents it as the
known-broken structure.

### Difference 3: Per-task vCPU state lives nowhere durable

ptrace and seccomp get per-task isolation **for free** because each `mm_id`
literally owns a different host child process (different `mm_id->pid`).
Per-task FPU/XSTATE, FS/GS, signal mask, segment caches — all live in the
child kernel task struct. UML never has to multiplex anything onto a shared
register file.

KVM multiplexes **every guest task in UML** onto a **single vCPU** (`vcpu0_fd`
in `arch/um/backend/kvm/kvm_backend.h:36`, enforced `ncpus=1` at
`arch/um/backend/kvm/lifecycle.c:142`). That means CR3, FS/GS base, FPU
(XMM/AVX state), MSRs, segment caches, EPT/shadow TLB, and pending
exception state all belong to the vCPU and have to be saved/restored by hand
on every UML context switch. The backend has fixed FPU
(`kvm_fpu_save_for_task` at `thread.c:132/150`, hashed by task pointer), CR3
(`cached_cr3_gpa`), and FS/GS base (`cached_fs_base/gs_base`) — but the list
is open-ended. Anything that a real Linux task would inherit from a clean
fork is, on KVM, inherited from "whatever the previous vCPU run left
behind." When a single bit is missed, the guest computes pointers using
register state that belongs to a different task.

These three differences are **structural**, not implementation bugs. Per-PTE
fixes in `kvm_um_pte_to_x86()` and per-event fixes in
`kvm_shadow_invalidate_va_range()` do not change them. The redesign memo
acknowledges this directly:

> "The deeper problem is that the architecture makes these bugs likely.
> Correctness is distributed across PTE helpers, pending range coalescing,
> host `mmap`, KVM shadow invalidation, full shadow refill, CR3-cache
> invalidation, and page-fault recovery."
> — `15-direct-shadow-sync.md` lines 100-104

---

## 2. Side-by-Side Comparison Table

| Axis | ptrace | seccomp | kvm |
| --- | --- | --- | --- |
| Where guest user code runs | separate host process (`mm_id->pid`) | separate host process (`mm_id->pid`) | same host process, under `KVM_RUN` on a shared vCPU |
| Per-mm host address space | yes — child's own pgd | yes — child's own pgd | **no** — parent process VA + a hand-built shadow PT |
| Translation layer | host MMU walks child host pgd | host MMU walks child host pgd | guest MMU walks **shadow PT** (`struct kvm_shadow_mm.pgd`) |
| `mm_map` mechanism | queue stub-side `mmap` syscall, flush via PTRACE_CONT | queue stub-side `mmap` syscall, flush via futex/SIGSYS | `os_map_memory()` (parent `mmap` MAP_FIXED at user VA) **plus** invalidate shadow PT |
| `mm_unmap` mechanism | queue stub-side `munmap`, flush | queue stub-side `munmap`, flush | parent `munmap` **plus** clear shadow leaf range |
| Code execution | real x86 hw, child runs at CPL=3 under PTRACE_SYSEMU | real x86 hw, child runs at CPL=3 under seccomp filter | KVM `KVM_RUN`, vCPU at CPL=3 under hand-built CR3/SREGS |
| Syscall trap mechanism | `SIGTRAP+0x80` from PTRACE_SYSEMU; no actual syscall executes | `SIGSYS` from seccomp filter; futex back to UML | LSTAR trampoline → `out %al,$0xf4` → `KVM_EXIT_IO` (`thread.c:3125`) |
| #PF (page fault) handling | host kernel delivers SIGSEGV; UML reads `faultinfo` from stub stack | host kernel delivers SIGSEGV via `SA_SIGINFO` mcontext | hand-rolled in-guest IDT[14] handler that `out %al,$0xfb` → `KVM_EXIT_IO` (`thread.c:3152`) |
| #GP, #DF, #UD | host kernel delivers SIGSEGV / SIGILL | host kernel delivers SIGSEGV / SIGILL | hand-rolled IDT[8] / IDT[13] entries → `out` ports `0xfa`/`0xf9` (`thread.c:3648`/`3707`) |
| SIGFPE, SIGBUS, SIGILL | host signal delivery → `relay_signal` | host signal delivery → `relay_signal` | **no path** — would have to be added to bootstrap IDT and the dispatcher |
| FPU/XSTATE per task | free (separate task struct) | free (separate task struct) | manual save/restore at `kvm_context_switch` via per-task hash (`thread.c:104,132,150,292`) |
| FS/GS base per task | free | free | manual cache + KVM_SET_SREGS (`thread.c:2231`, `kvm_backend.h:81`) |
| TLB flush | host kernel handles via real CR3 reload at task switch | same | manual: `shadow->dirty` flag + `KVM_SET_SREGS` reload of CR3 (`thread.c:2234,2249`) |
| `init_new_context` `mm_unmap(0, STUB_START)` | clears the stub child's address space below STUB_START (correct) | same as ptrace | **destructive on parent** — would munmap ~128 TB of UML's own VA. Worked around by a heuristic at `mm.c:211` |
| Stub child reaper | not used (waitpid inline) | uses SIGCHLD reaper IRQ (`uses_stub_reaper=true`) | not used; no stub child exists |
| Context-switch primitive | `switch_threads()` jmp_buf | `switch_threads()` jmp_buf | `switch_threads()` jmp_buf **plus** vCPU FPU dance + um_tlb_sync drain + shadow user-half clear |
| Number of memory views that must agree | 2 (UML pgd, child host pgd) | 2 (UML pgd, child host pgd) | **3** (UML pgd, parent host pgd via os_map_memory, KVM shadow PT) |
| Failure mode when one view drifts | host kernel delivers SIGSEGV in stub; UML sees a clean signal | same | guest CPU walks shadow PT, gets the wrong PFN, **silently reads/writes the wrong physical page** |

---

## 3. Specific Structural Bugs in the KVM Backend Not Present in the Others

These are bugs whose source is the structure, not a typo or off-by-one.
Each is something the working backends literally cannot have.

### 3.1 `kvm_mm_map()` writes into the parent UML process VA

`arch/um/backend/kvm/mm.c:119-181` — `kvm_mm_map()` calls
`os_map_memory((void *)virt, phys_fd, ...)` which performs
`mmap(MAP_SHARED|MAP_FIXED)` *in the UML parent process*. Comment at
mm.c:122-134 records that an attempt to skip this (`#276 reverted`) crashed
inside libc. The comment correctly identifies that *something* in the parent
UML process — "probably copy_to_user fallback or io_uring fixed-buffer
setup" — still depends on the parent VA being mapped at the user VA.

Consequence: **the UML parent process has user-VA mappings for every
mm_id concurrently**. ptrace/seccomp do not — those mappings are scoped
to whichever stub-child host process the mapping was queued against.
Cross-mm contamination of the parent's view is the architectural floor.

This is the single most damaging structural difference. It is the source of
NULL+offset and 45-bit-garbage symptoms because the parent's own malloc
arena, kernel image, and bootstrap pages live at addresses that the guest's
shadow PT could resolve to identical offsets — when the guest computes a
wild pointer, it hits something rather than crashing cleanly.

### 3.2 `kvm_mm_unmap()` cannot honor `init_new_context`'s STUB_START sweep

`arch/um/kernel/skas/mmu.c:86` calls `mm_unmap(new_id, 0, STUB_START)` for
every freshly attached mm. `STUB_START` ≈ 128 TB on x86_64. Under ptrace
and seccomp, this clears the stub *child's* address space, which is correct.

Under KVM there is no stub child, so the call would `munmap(0, ~128TB)` in
the parent UML process — which is destructive. `kvm_mm_unmap()` recognizes
the signature with a heuristic at `arch/um/backend/kvm/mm.c:211`:

```c
if (virt == 0 && len >= (1UL << 46)) {
    pr_info_once(...);
    return 0;
}
```

The fact that this heuristic exists is itself a structural-mismatch
indicator. ptrace and seccomp do not need it because the stub-child model
naturally scopes `mm_unmap(0, STUB_START)` to a process that has nothing
mapped in that range. The heuristic also leaks: legitimate user-mode
`munmap()` calls that happen to span [0, 64TB) silently become no-ops in the
shadow path even though the UML pgd has cleared them.

### 3.3 Three concurrent address-space representations, each with its own freshness state

`arch/um/include/asm/pgtable.h:270` (`set_pte`) marks PTE updates with
`_PAGE_NEEDSYNC` and queues them via `um_tlb_mark_sync()`.
`arch/um/kernel/tlb.c:173` (`um_tlb_sync()`) later drains the queue by calling
backend `mm_map`/`mm_unmap`. The *deferred* drain is correct for a backend
where the host kernel is the authoritative MMU, but in the KVM backend it
means there is a window during which:

- UML pgd has the new PTE
- parent host VA still has the old mapping (`os_map_memory` not yet called)
- shadow PT still has the stale leaf (`kvm_shadow_invalidate_va_range` not
  yet called)

Any KVM_RUN entered during that window walks a stale shadow PT and reads or
writes the wrong physical page. The "memo 15" direct-shadow-sync
(`shadow_sync.c`) is an attempt to close half of this hole by short-circuiting
the chain; the other half (parent host VA freshness) remains.

ptrace and seccomp do not have this window because there is no second
translation layer. Their drain delay only affects when the stub child's
host pgd gets the mapping — and the stub child is not running while UML's
trap-handler is running, so by the time the stub resumes, the drain has
completed.

### 3.4 Per-mm shadow PT is allocated, but bootstrap/gadget pages are mapped per-entry

`arch/um/backend/kvm/thread.c:1939-2044` — every single `kvm_enter_guest`
call re-maps four bootstrap pages (LSTAR trampoline, IST stack, gadget
state, gadget vvar) into the active mm's shadow PT via four
`kvm_shadow_map_page()` calls.

The pages themselves are allocated once at boot but their **shadow PT
entries** are reinstalled per-entry. This means:

- A bug in `kvm_shadow_map_page()` (`lifecycle.c:1360`) gets executed many
  times per syscall.
- The `KVM_X86_PTE_P` bit is being re-OR-ed into intermediate PUD/PMD entries
  (`kvm_shadow_table_step` at `lifecycle.c:1043`) every call — so a leaf
  install racing against a concurrent invalidate can leave intermediate
  tables present but leaf cleared.
- The bootstrap-alias VA (`kvm_bootstrap_va`) lives in the *kernel*
  allocator's range and goes into PGD slot 0 alongside user mappings.
  `kvm_shadow_pgd_clear_user()` (`lifecycle.c:1459`) had to be made a
  no-op (D275 #275) because it could clear bootstrap leaves.

ptrace and seccomp have no bootstrap. They use the host kernel's IDT.

### 3.5 `kvm_shadow_sync_pte()` walks pgtables in atomic context without locks

`arch/um/backend/kvm/shadow_sync.c:153-285` — `kvm_shadow_sync_pte()` is
called from `set_ptes()` while a pte spinlock is held. It does
single-`u64` writes to leaf slots without taking `shadow->fill_lock`,
relying on the comment-stated "single-host-thread invariant" of UML.

This is correct for the cooperative single-threaded UML model, but the
comment at `shadow_sync.c:31-35` acknowledges the race is between concurrent
fill (which DOES take `fill_lock`) and direct sync (which does NOT). The fill
walk in `kvm_shadow_fill_from_uml_pgd()` (`lifecycle.c:1141`) reads each
intermediate `*pmd` then writes each leaf — between those two steps a
direct sync may flip the leaf to a new value, and the fill will then
overwrite it with the freshly-translated value. Net behavior: under fill +
direct-sync interleaving the most-recent direct-sync write to a given leaf
can be *replaced* by a slightly-older translated value from the fill.

ptrace and seccomp do not have shadow PTs at all. The host hardware MMU is
its own authoritative state.

### 3.6 `kvm_um_pte_to_x86()` translation gaps

`arch/um/backend/kvm/lifecycle.c:1063-1133` — the bit translator strips
`_PAGE_NEEDSYNC` correctly but is intentionally lossy:

- A clean writable PTE (`_PAGE_RW` set, `_PAGE_DIRTY` clear) is mapped
  read-only in the shadow (`lifecycle.c:1115`). Correct for emulating
  software dirty bits, but means the guest *will* take a #PF on first
  write, which routes through `KVM_EXIT_IO port=0xfb` and the bootstrap
  #PF handler — a path with much more code than a host signal.
- Any UML PTE bit that the translator does not know about is dropped.
  The translator is hard-coded against UML PTE bit positions, with a
  comment at `lifecycle.c:1052-1054` warning that `arch/um/include/asm/
  pgtable.h` ever reshuffling positions silently breaks this.

ptrace and seccomp use UML's own `mm_map` → `mmap(prot)` chain and the host
kernel's PTE encoder. There is no place to drop a bit.

### 3.7 Signal/exception delivery has its own dispatch table

`arch/um/backend/kvm/thread.c:3123-4093` — `kvm_run_userspace()` decodes
KVM exit reasons and routes #PF / #GP / #DF / SYSCALL through
`UM_KVM_*_PORT` constants. The list at `kvm_backend.h:680-682` and the
follow-on `UM_KVM_PF_PORT` / `UM_KVM_GP_PORT` / `UM_KVM_DF_PORT` constants
are referenced from `thread.c` but defined elsewhere; the dispatcher only
handles those ports. Anything else (#UD, #NM, #MC, host SIGFPE delivered
inside KVM_RUN) is routed to `panic("um: kvm run_userspace: KVM_EXIT_IO
port=0x%x (unknown)")` at `thread.c:3757`.

By contrast, ptrace's run_userspace (`arch/um/backend/ptrace/trap_user.c
:194-253`) and seccomp's (`arch/um/backend/seccomp/trap_user.c:124-156`)
dispatch through the kernel's `sig_info[]` table, which already handles
`SIGSEGV / SIGSYS / SIGTRAP / SIGTRAP+0x80 / SIGALRM / SIGIO / SIGILL /
SIGBUS / SIGFPE / SIGWINCH`. No bootstrap in-guest IDT. No port number
bookkeeping. The kernel knows how to deliver every architecturally defined
signal.

---

## 4. Architecture Diagram (text)

### ptrace backend

```
   UML host process (parent)                     stub child (per-mm host process)
   +-------------------------+                    +---------------------------+
   | UML kernel text/data    |                    |  guest user TEXT/DATA     |
   | UML allocator/heap      |                    |  guest user STACK         |
   | (no user mappings)      |                    |  STUB_CODE / STUB_DATA    |
   +-------------------------+                    +---------------------------+
            |                                                 ^
            | mm_map: queue stub-side mmap                    | host CPU runs
            +---->  syscall_stub (do_syscall_stub)  --------->| guest code
            |                                                 | natively
            | run_userspace: PTRACE_SYSEMU pid                |
            +---->  waitpid SIGTRAP+0x80, SIGSEGV  <----------+
            |       reads faultinfo from stub stack
            +-->>>  handle_syscall / segv via sig_info[]
```

### seccomp backend (essentially identical structure)

```
   UML host process (parent)                     stub child (per-mm host process)
   +-------------------------+                    +---------------------------+
   | UML kernel text/data    |                    |  guest user TEXT/DATA     |
   | UML allocator/heap      |                    |  STUB_CODE / STUB_DATA    |
   | (no user mappings)      |                    |  seccomp BPF filter       |
   +-------------------------+                    +---------------------------+
            |                                                 ^
            | mm_map: queue stub-side mmap                    |
            +-->>> futex/sockpair to stub  ------------------>| host CPU runs
            |                                                 | guest code
            | run_userspace: futex wake stub                  | natively
            +-->>> stub runs until SIGSYS, SIGSEGV  <---------+
            |      mcontext copied via SA_SIGINFO sigstack
            +-->>> handle_syscall / segv via sig_info[]
```

### kvm backend

```
   UML host process (parent + KVM hypervisor client)
   +----------------------------------------------------------+
   | UML kernel text / data / heap                            |
   |                                                          |
   | parent VA mapping for EVERY mm_id's user pages           | <-- structural
   | (os_map_memory MAP_SHARED | MAP_FIXED at user VA)        |     hazard
   |                                                          |
   | per-mm shadow PT page tree (kvm_shadow_mm.pgd)           |
   |   [ rebuilt by kvm_shadow_fill_from_uml_pgd ]            |
   |   [ patched by kvm_shadow_sync_pte (atomic ctx) ]        |
   |   [ patched by kvm_shadow_invalidate_va_range ]          |
   |                                                          |
   | bootstrap pages (LSTAR / IDT / GDT / IST / gadget state) |
   |                                                          |
   | KVM /dev/kvm fd, single VM fd, single vCPU0 fd           |
   |   .... CR3 / FS / GS / FPU / MSRs / TLB live IN the vCPU |
   +----------------------------------------------------------+
                  |
                  | KVM_RUN (vCPU walks shadow PT as CR3)
                  v
   +----------------------------------------------------------+
   |  vCPU0 — guest CPL=3 user code, CPL=0 hand-rolled        |
   |          IDT/LSTAR/IST trampolines                       |
   |  exits via KVM_EXIT_IO (port 0xf4 syscall, 0xfb #PF,     |
   |                          0xf9 #GP, 0xfa #DF, ...)        |
   |  also: KVM_EXIT_MMIO (EPT-level fault), KVM_EXIT_HLT     |
   +----------------------------------------------------------+
                  |
                  | run_userspace: decode IO port → faultinfo
                  | → sig_info[] dispatch (SIGSEGV only) OR
                  | → handle_syscall
                  v
              ... back to userspace() loop ...

  The vCPU is shared across every UML task. Per-task FPU is hashed in
  kvm_fpu_hash[] (thread.c:104). Per-task FS/GS is in regs->gp[].
  Everything else (segment caches, MSRs not in our save list, pending
  exception state, KVM internal vCPU bookkeeping) is whatever the
  previous task's KVM_RUN left behind.
```

The structural distance between the working backends and KVM is enormous.
ptrace and seccomp have a clear delegation contract: "host kernel is the
hypervisor; UML hands it a child process and host signals tell us what
happened." KVM has UML *being* the hypervisor while also being the kernel
— the abstraction layer sits inside our code, not below it.

---

## 5. Per-Question Answers

### 5.1 Stub child process model — what the address-space split gives you for free

The stub-child model gives **isolation as a side effect of the OS process
abstraction**:

- **Disjoint address spaces.** A wild pointer in user code dereferences
  into the stub child's address space. If that address is not mapped, the
  host kernel signals SIGSEGV. The UML parent process's libc / heap /
  allocator state is in a *different process* and is not reachable.
- **Per-mm isolation by construction.** Different `mm_id`s have different
  `pid`s have different host pgds. There is no possibility of one mm's
  mapping leaking into another's view because they live in different
  processes.
- **Per-task register state by construction.** Each stub child is a real
  Linux task with its own task_struct. FPU, FS/GS, signal mask, segment
  caches, fdtable — all owned by the host kernel per task.
- **Real x86 behavior on every architecturally defined trap.** SIGFPE,
  SIGILL, SIGBUS, SIGSEGV, SIGTRAP, SIGSYS — the host kernel's signal
  delivery machinery handles them. UML just registers `sig_info[]`
  handlers and the right one runs.
- **Real fork() COW semantics.** Each mm's stub child has its own COW
  pages. `do_wp_page` on the host translates correctly to a child-private
  copy without any plumbing in UML.

KVM has none of this for free. It builds the equivalent of:

- The shadow PT (rebuilds the address-space isolation that processes give
  you).
- The bootstrap IDT/LSTAR/IST (rebuilds signal/syscall delivery).
- `kvm_fpu_save_for_task()` and the per-task hash (rebuilds per-task FPU).
- The `cached_fs_base/gs_base/cr3_gpa` cache (rebuilds per-task segment
  state).

Every one of these is a chance to leave a bit out. The list is open-ended:
any architecturally visible CPU state that a real Linux task would inherit
from a fork is, on KVM, inherited from "whatever the previous KVM_RUN left
behind." The failure mode is silent because there is no host-side guard.

### 5.2 Address-space layout / user-memory access path

**ptrace/seccomp path.** UML kernel writes to user memory via
`raw_copy_to_user` which calls `__put_user_*` which uses UML's
`copy_chunk_to_user` in `arch/um/kernel/skas/uaccess.c:64`
(`do_op_one_page`) which `maybe_map`'s the page and walks the UML pgd to
the physical page, then `kmap_local_page` + memcpy. The host kernel never
touches the stub child's pgd from this path. The stub child's pgd is only
mutated via `mm_map`/`mm_unmap` queued ops that the stub child runs
itself.

**kvm path.** UML kernel writes to user memory exactly the same way — same
`raw_copy_to_user` chain. But `kvm_mm_map()` *additionally* registers the
mapping in the parent UML process's own VA via `os_map_memory()` so that
"some path (probably copy_to_user fallback or io_uring fixed-buffer setup)"
(`mm.c:128-133`) does not crash. The redundant parent-VA mapping is the
specific structural mistake.

The shadow PT exists for the *guest CPU* under KVM_RUN. The host UML kernel
doesn't read or write through the shadow PT; it walks UML pgd directly.
The redundant parent VA mapping is therefore **never read by the guest CPU**
(it goes through the shadow), and **may not be needed** by the host
either (uaccess.c walks the UML pgd). The 02-19 reverted attempt to drop
it failed because *something* still uses it; the audit recommendation
should be to find and fix that "something" (memo 15 §"Host VA Mapping
Policy" Option A) rather than keep the redundant mapping.

### 5.3 MM mapping primitives — could KVM run mappings in a separate process?

Yes — memo 15 §"Host VA Mapping Policy" Option B describes a per-mm
"host-mapping worker" process that owns the parent-VA mapping the way the
ptrace stub child does. The mapping ops would `socketpair`/`SCM_RIGHTS`
the phys_fd to the worker and the worker would `mmap`/`munmap`. This is
heavy: it recreates much of the existing stub machinery. The memo's own
recommendation is to try Option A first (eliminate parent-VA mappings) and
fall back to Option B only if the audit shows too many host-VA users.

The advantage of Option B as a *structural* migration is that it brings
KVM's per-mm isolation into line with the working backends: every UML mm
becomes equivalent to a separate host process, and per-mm mapping
contamination becomes impossible. The KVM_RUN call itself stays in the
parent (it doesn't need user-VA mappings — it walks the shadow PT).

### 5.4 Code-execution model

ptrace and seccomp let real x86 hardware execute the user code under the
host kernel's existing task scheduler. Address translation, fault
handling, MSR programming, FPU/XSTATE save+restore, signal delivery — all
of it is host kernel functionality that UML reuses.

KVM's vCPU executes the user code under our hand-built CR3 (shadow PT),
our hand-built IDT (bootstrap page), our hand-built GDT, our hand-built
LSTAR trampoline, our hand-built IST stack, our hand-built gadget state.
We program the SYSCALL MSRs (`thread.c:1662`), the FS/GS base
(`thread.c:1563,1618`), the CPUID (`lifecycle.c:363`), and we have to mask
host CPU features that we cannot service (XSAVE/AVX at
`lifecycle.c:419-516`).

Failure mode comparison:

| Failure | ptrace/seccomp | kvm |
| --- | --- | --- |
| Wild jump to unmapped | host #PF → SIGSEGV → kernel signal | guest #PF → in-guest IDT[14] → port 0xfb → IST frame parse → maybe handle_page_fault, maybe SIGSEGV |
| Privileged instruction in user code | host SIGILL | guest #GP → in-guest IDT[13] → port 0xf9 → SIGSEGV (NOT SIGILL) |
| Divide by zero | host SIGFPE | guest #DE → triple-fault (no IDT[0] handler) → KVM_EXIT_SHUTDOWN → panic |
| Misaligned access on misalign-checked region | host SIGBUS | guest #AC → triple-fault (no IDT[17] handler) → panic |
| AVX instruction in user code (CPUID was masked but binary uses it anyway) | host SIGILL | guest #UD → triple-fault → panic |
| Host preemption | host kernel reschedules; signal delivery is bookkeeping | KVM_RUN returns -EINTR; we KVM_GET_REGS; partially executed instruction state may be in a weird intermediate place |

The KVM column is materially worse on every line.

### 5.5 Signal handling

ptrace and seccomp deliver every architecturally defined signal cleanly
because they let the host kernel do it. The dispatcher (trap_user.c) just
forwards `siginfo_t` to UML's `sig_info[]` table. Every signal type the
kernel knows how to deliver is handled.

KVM only deliberately handles SIGSEGV (and only via #PF / #GP / EPT MMIO
fault). SIGFPE, SIGILL, SIGBUS are not wired to bootstrap IDT entries —
they would `panic("unknown exit reason")` or triple-fault. Even SIGSEGV
delivery is degraded: the guest #GP handler at `thread.c:3648` delivers
SIGSEGV (not SIGILL or SIGBUS), losing fidelity.

This is not an oversight — it is structural. Wiring a real IDT for every
fault vector means writing handler code in `kvm_bootstrap_*_bytes[]` and
adding port-decode cases in the dispatcher. Each new case is a chance for
a bug. The working backends require zero such code.

### 5.6 Thread / scheduling integration — what the multiplexing misses

Per-task vCPU state that has to be saved/restored on every UML context
switch:

- General-purpose regs — done via `regs->gp[]` (lives on UML task struct)
- RFLAGS — done via regs
- RIP — done via `regs->gp[HOST_IP]`
- RSP — done via `regs->gp[HOST_SP]`
- FS_BASE / GS_BASE — done via `cached_fs_base/gs_base` cache (`kvm_backend.h:81`)
- FPU/XSAVE — done via `kvm_fpu_hash[]` (`thread.c:104,132,150`)
- CR3 — done via per-mm shadow PGD
- MSR_KERNEL_GS_BASE — done, points at gadget state per-task on entry

What is NOT explicitly tracked per task:

- **Segment cache contents** for CS/DS/ES/SS beyond base/limit — KVM
  serializes these in SREGS but we only push them when sregs-skip cache
  misses. Seg-cache drift across tasks is undefined.
- **Pending exception state** in `KVM_GET_VCPU_EVENTS`. If task A took a
  recoverable #PF and KVM_RUN returned with an "injected" exception
  pending, switching to task B without clearing the pending exception
  re-injects A's exception into B. There is no `kvm_vcpu_events` save
  in `kvm_context_switch`.
- **Guest TLB state.** A KVM_SET_SREGS reload of CR3 flushes the vCPU TLB
  — but the cache (`cached_cr3_gpa`) skips KVM_SET_SREGS when CR3 is
  unchanged. If task A's mappings (within the same shadow_mm? no — but
  task A and task B can share an mm under fork+CLONE_VM) leave a TLB
  entry, task B sees it.
- **MXCSR** — saved as part of FPU save, but only when
  `kvm_diag_skip_fpu_save` is off and the fast path is taken.
- **Performance counters** — vPMU is enabled by default
  (`lifecycle.c:271-279`); per-task PMU counters are not isolated.
- **Debug registers (DR0..DR7).** Not saved per task at all.
- **The vCPU's view of MSR_LSTAR / MSR_FMASK / MSR_STAR** — these are
  reprogrammed at every entry but only after `msrs_primed` is false
  (`kvm_backend.h:48`). If a SET_MSRS goes through, it persists across
  tasks; the cache does not know about per-task MSR values.

The list is open-ended: any architecturally visible CPU state is potentially
shared until proven isolated. That is the inverse of how the working
backends operate, where everything is per-task by default and sharing
requires explicit kernel API use (e.g. CLONE_VM).

### 5.7 Failure modes when the structure is wrong

ptrace fails loudly:

- A bad ptrace call returns `errno` and the dispatcher goes through
  `fatal_sigsegv()` (`trap_user.c:55`).
- A SIGSEGV delivered to the stub is read out via `PTRACE_GETSIGINFO` and
  always carries a faultinfo. A wrong faultinfo is detected by UML's
  segv() because the fault address must lie in a valid VMA or it is a
  hard kernel crash.
- A stub crash is reaped via waitpid; the parent immediately knows.

seccomp fails loudly:

- The futex round-trip times out or the stub's signal frame is dumped
  into `proc_data->sigstack` and the dispatcher panics on a malformed
  offset (`trap_user.c:104`).
- A stub crash is reaped via SIGCHLD IRQ.

KVM fails silently:

- A wrong shadow PT entry produces a wrong `phys_addr` from the guest CR3
  walk. KVM resolves that to a host VA via the Policy A memslot. The
  guest reads/writes that VA. The result is wrong data, no signal, no
  error.
- A missed shadow invalidation lets the guest TLB serve a stale mapping
  for as long as the TLB entry survives.
- A wrong FS/GS base means `mov %fs:tcb_field, %rax` reads a *different*
  task's TCB. The instruction completes; the value in rax is wrong; the
  computation continues with corrupted state for arbitrary distance
  before crashing somewhere unrelated.
- Per-task FPU drift lets `movdqa` read XMM state from a different task.
  No fault. Bytes copied are wrong.

The asymmetry is fundamental: the working backends rely on host-kernel
guards that catch broken state at the source. KVM's broken state is in
*data* — page table entries, register values — that the guest CPU
faithfully consumes.

### 5.8 Per-mm host-VA isolation (memo Phase 4 Option B)

A per-mm host-mapping worker would:

- **Pros:**
  - Eliminate the parent-VA-contamination hazard. Parent UML process has
    no user mappings; wild pointers crash the worker, not UML.
  - Bring KVM's per-mm isolation into structural parity with ptrace/seccomp.
  - Make `kvm_mm_unmap(0, STUB_START)` correct — the worker has nothing
    mapped in [0, STUB_START), so it is a no-op naturally.
  - Allow killing/restarting a corrupted worker without taking down UML.

- **Cons:**
  - Recreates much of `start_userspace()` machinery for KVM.
  - Adds per-mm RPC overhead on `mm_map`/`mm_unmap`.
  - Does not solve shadow PT correctness — the shadow still has to
    mirror the worker's mapping decisions.
  - Per-syscall hot path stays in the parent (KVM_RUN). The worker is
    only used for mapping ops, so the cross-process cost is amortized.

Memo 15 currently recommends Option A (KVM-native uaccess that drops
parent VA mappings entirely) over Option B. Both are improvements; Option
B is the more conservative choice if the uaccess audit is expensive.

### 5.9 Identity memslot policy

Today: one big Policy A memslot maps `[0, physmem_size)` host VA → `[0,
physmem_size)` guest physical (`lifecycle.c:610-616`). All mms share this
slot. `kvm_mm_map`/`kvm_mm_unmap` issue NO `KVM_SET_USER_MEMORY_REGION`
ioctls; they only update parent host VA + shadow PT.

Alternative A: per-mm memslot range. Each mm's mappings live in a
disjoint guest-physical range, which the per-mm shadow CR3 references.
Cross-mm contamination is impossible at the EPT level. Cost: KVM has a
fixed limit on memslots (KVM_USER_MEM_SLOTS, currently 32 on x86) — enough
for a few hundred mms only with batching.

Alternative B: per-mm KVM VM. Each `mm_attach` calls `KVM_CREATE_VM` and
gets its own EPT. Maximal isolation. Cost: KVM_CREATE_VM is ~ms, which
is not viable for `fork()`.

Alternative C: keep one VM, one memslot, but treat the **single shared
parent-VA mapping** as the source of truth and stop trying to keep three
views in sync. This is the memo 15 direct-shadow-sync approach plus
parent VA mapping reduction. The implication is that the shadow PT is
no longer structural — it is purely a TLB cache that KVM owns and which
we resync from UML pgd at deterministic points.

The structural improvement is C, then A. C is what memo 15 recommends.
A requires KVM memslot count rework and is heavier.

### 5.10 Failure-mode analysis — simplest structural change

Wild-pointer pattern + NULL+offset + 45-bit garbage is consistent with:

1. The guest CPU walking a stale or wrong shadow PT entry, reading data
   from a host page that happens to be mapped at the same physical
   address by some other mapping (parent-VA contamination).
2. A wrong FS/GS base making `%fs:offset` resolve to a different task's
   TCB or to garbage.
3. A pending exception leak from one task to another (less likely; would
   show as cascading faults rather than wild pointers).

The simplest structural change that would either eliminate the failure or
make it loud:

**Switch to a per-mm host-mapping worker (Option B), and have the worker
die loudly on segfault.**

This collapses the three failure modes into one — any wild pointer
crashes the worker, the host kernel reaps it via SIGCHLD, and UML knows
immediately. The shadow PT bug becomes "guest CPU read garbage from a
host VA in a process whose only mappings are user pages at user VAs;
worker host kernel SIGSEGV's the worker." The diagnostic surface goes
from "we crash inside libc 1MB downstream of the actual bug" to "worker
died at exact RIP of bad access."

This does not fix the shadow PT bugs, but it makes them **loud**
instead of silent. After that, the shadow PT bugs become bisectable
with normal kernel crash dumps because every wild dereference is caught
at the moment it goes off the rails, not minutes later in unrelated code.

A simpler stopgap: have the parent UML process *unmap* its user-VA region
(or `mprotect` it PROT_NONE) before every KVM_RUN, and restore it after.
That makes any uaccess-from-parent crash at the moment it happens. Not as
clean as Option B but achievable in a single-digit-line patch.

---

## 6. Recommended Structural Redesign

### KVM should be more like ptrace because per-mm address space isolation is what makes the working backends correct.

The shadow PT is a necessary evil for KVM (the guest CPU needs *some* CR3),
but the parent-VA mapping is not — and it is the source of the silent
wild-pointer hazard. The redesign should:

1. **Adopt the per-mm host-mapping worker model (memo 15 Phase 4 Option B).**
   Each `mm_attach` clones a worker process via the same `start_userspace`
   primitive ptrace/seccomp use. The worker's only job is to own the
   user-VA mappings (it never executes guest code).

2. **Make the parent UML process have NO user-VA mappings ever.** All
   `kvm_mm_map`/`kvm_mm_unmap` go through SCM_RIGHTS or a per-worker RPC
   to the worker. uaccess in the parent goes through the
   walk-UML-pgd-to-physical path (`uaccess.c:64`) only.

3. **Treat the shadow PT as a derived cache, not a source of truth.**
   Memo 15 Phase 1-3 already prescribes this. After Option B, the shadow
   PT only has to mirror the WORKER'S host pgd, which is the same thing
   ptrace/seccomp's host MMU does naturally. The "three views must
   agree" problem becomes "two views must agree" — which is exactly what
   ptrace/seccomp have.

4. **Fall back from `panic()` to `signal-the-task` for any unrecognized
   trap.** A KVM exit we don't recognize should deliver SIGSEGV /
   SIGILL / SIGBUS to the offending task and let UML schedule another
   one. Crashing UML on an in-guest #UD or #DE is an unforced error;
   ptrace and seccomp would just SIGILL the user process and continue.

The shadow PT, the bootstrap IDT/LSTAR, the per-task FPU hash — all these
can stay. They are necessary because KVM is what it is. What changes is
that the hazards become bounded: a stale shadow leaf cannot send the
guest into UML's allocator anymore because UML's allocator is in a
different process.

---

## 7. Migration Plan (Current Architecture → Recommended)

### Phase 1: Make broken loud (1-2 weeks)

- Add `mprotect(PROT_NONE)` over `[0, TASK_SIZE)` in the parent UML
  process before every `KVM_RUN` and `mprotect(PROT_READ|PROT_WRITE)`
  back after. Any in-parent uaccess that depended on the parent-VA
  mapping now SIGSEGVs immediately.
- Audit the remaining failures. Memo 15 §"Host VA Mapping Policy"
  Option A is a list of paths to inspect: futex atomics,
  `strncpy_from_user`, signal-frame builder, io_uring fixed buffers,
  `raw_copy_*_user`.
- Patch the in-parent paths to use the UML-pgd-walk uaccess
  (`arch/um/kernel/skas/uaccess.c:64`) instead of dereferencing through
  user VAs.

This phase reuses the existing structure but converts a class of silent
corruption into immediate crashes. It is the smallest change that buys
a real diagnostic improvement.

### Phase 2: Direct shadow synchronization (memo 15) (2-4 weeks)

- Implement direct sync from `set_pte_at` / `pte_clear` / range
  helpers (memo 15 Phase 2).
- Remove the deferred chain's correctness role; keep it as a host VA
  freshness mechanism only.
- Make `kvm_enter_guest` a verifier rather than a repair pass.

This collapses the shadow PT from an eventually-consistent mirror into a
synchronously-consistent one.

### Phase 3: Per-mm host worker (memo 15 Option B) (4-8 weeks)

- Add a `kvm_um_worker` field to `mm_id` paralleling `kvm_shadow`.
- Have `kvm_mm_attach` clone a worker via `start_userspace` (the same
  primitive ptrace/seccomp use; the worker's `userspace_tramp` does
  nothing — just blocks on a sockpair waiting for mapping commands).
- Route `kvm_mm_map`/`kvm_mm_unmap` to the worker via SCM_RIGHTS.
- Drop parent-VA `os_map_memory` calls.
- Drop the `STUB_START` heuristic in `kvm_mm_unmap()` — it is no longer
  needed because the worker has no STUB region.

This is the structural fix. After Phase 3, KVM has the same per-mm
isolation the working backends have.

### Phase 4: Per-mm vCPU state save/restore (4 weeks, parallel to 3)

- Add `kvm_vcpu_events` save/restore in `kvm_context_switch`.
- Add per-task DR0..DR7 save/restore.
- Audit MSR_FMASK / MSR_STAR / MSR_LSTAR for per-task drift; either save
  per-task or assert they are invariant.
- Wire SIGFPE / SIGILL / SIGBUS / SIGTRAP delivery via dedicated
  bootstrap IDT entries + port codes.

### Phase 5: Drop the shadow PT eager-fill from the hot path (2 weeks)

- After Phases 2-3, every PTE mutation is direct-sync'd into the shadow.
  `kvm_shadow_fill_from_uml_pgd` becomes a debug audit command rather
  than the per-entry path. `kvm_enter_guest`'s SREGS-skip cache handles
  every fast-path entry without a fill.

Each phase is independently shippable. Phase 1 is a strict diagnostic
improvement that does not change correctness. Phases 2-5 are the
structural redesign.

---

## 8. What I Would Do First

**Concretely: Phase 1.** A 50-line patch that mprotects the parent's
user-VA region PROT_NONE around every `KVM_RUN`. This is:

- One change to `kvm_run_userspace()` in `arch/um/backend/kvm/thread.c`
  before the `os_ioctl_generic(vcpu_fd, KVM_RUN, 0)` call (around
  line 3053).
- One `mprotect` call over `[0, task_size)` to PROT_NONE.
- Symmetric `mprotect(PROT_READ|PROT_WRITE|PROT_EXEC)` after the
  KVM_RUN returns.
- Run `python3 -c "import unittest"` and observe where it crashes.

If the workload still crashes inside the parent UML process at user VAs,
we have proven that the parent-VA mapping is being dereferenced *while
the guest is running*. That is a bug independent of KVM and identifying
the caller is a 30-minute job with `gdb`/strace/`pr_warn_ratelimited()`
at the suspected sites.

If the workload crashes inside the guest (ring-3) but at a deterministic
location (e.g. always inside `_PyObject_GenericGetAttrWithDict` at the
same instruction reading at NULL+0x10), we have proven that the parent
VA is not the source — the bug is in the shadow PT or vCPU state, and
Phase 2 is the next step.

If the workload still works (no crashes, just bit-perfect output), we
have proven that the parent-VA mapping was never load-bearing for the
workload and `os_map_memory` can be eliminated for KVM unconditionally.
That is a 5-line patch and the structural hazard goes away.

This experiment is cheap, deterministic, and discriminates between three
hypotheses with one boot. It is the single highest-leverage thing to do
with the next available test cycle.

---

## Appendix: File:Line Map of the Structural Bugs

| Symptom | File:Line | Description |
| --- | --- | --- |
| Parent VA contamination | `arch/um/backend/kvm/mm.c:135` | `os_map_memory` MAP_FIXED into parent process at user VA |
| Three views must agree | `arch/um/include/asm/pgtable.h:270`, `arch/um/kernel/tlb.c:173`, `arch/um/backend/kvm/mm.c:165`, `arch/um/backend/kvm/lifecycle.c:1141` | UML pgd ↔ parent host VA ↔ shadow PT — three independently mutable representations |
| STUB_START sweep heuristic | `arch/um/backend/kvm/mm.c:211` | Heuristic to ignore destructive `init_new_context` mm_unmap |
| Single vCPU multiplexes all tasks | `arch/um/backend/kvm/lifecycle.c:142`, `arch/um/backend/kvm/kvm_backend.h:36` | `ncpus=1` enforced; vcpu0_fd is singleton |
| Per-task FPU not free | `arch/um/backend/kvm/thread.c:104,132,150,292` | Hand-rolled per-task FPU hash + save/restore |
| Per-task FS/GS not free | `arch/um/backend/kvm/kvm_backend.h:81`, `arch/um/backend/kvm/thread.c:2231` | Hand-rolled cache + KVM_SET_SREGS |
| Per-task vCPU events not saved | (absent) `kvm_context_switch` at `arch/um/backend/kvm/thread.c:192` | KVM_GET/SET_VCPU_EVENTS not invoked across task switches |
| In-guest IDT for #PF/#GP/#DF | `arch/um/backend/kvm/thread.c:1055,1088,1118` | Hand-rolled IDT handlers |
| No SIGFPE/SIGILL/SIGBUS path | `arch/um/backend/kvm/thread.c:3757` | Unrecognized port → panic |
| Shadow PT translation lossy | `arch/um/backend/kvm/lifecycle.c:1063` | Hand-translated PTE bits; any UML PTE bit not enumerated is dropped |
| Shadow sync atomic-context unlocked | `arch/um/backend/kvm/shadow_sync.c:153,29-35` | Direct sync skips `fill_lock`, races against fill |
| Bootstrap pages re-mapped per entry | `arch/um/backend/kvm/thread.c:1939-2044` | Four `kvm_shadow_map_page` calls per `kvm_enter_guest` |

The bug surface is structural. Per-PTE fixes do not move it.
