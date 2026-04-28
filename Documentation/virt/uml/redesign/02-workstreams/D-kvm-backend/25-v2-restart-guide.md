# Memo 25 — Mechanical guide: how to restart the KVM backend (v2) and what to refactor in ARCH=um first

**Date:** 2026-04-28
**Audience:** Whoever is implementing the v2 reimplementation
**Companion memos:** 23 (concrete fix plan for v1), 24 (ELI5 + 10 clean-slate
items)

This memo is the operational playbook. It assumes the strategic decision to
restart has been made (see memo 24's items #2, #3, and #10 for the
strategic argument). It covers:

- **Part 1**: mechanical steps to archive v1 and stand up a clean v2
  scaffold without breaking the build or the seccomp default.
- **Part 2**: the 12 ARCH=um core refactors that should land BEFORE v2's
  real implementation begins, so v2 starts on a clean substrate.
- **Part 3**: sequencing, dependencies, risk register.
- **Part 4**: success criteria for declaring v2 done.

---

## Part 1 — Mechanical restart (1-2 days)

### Step 1: Tag and archive v1 in the git history

```bash
# Branch the current state for posterity (no merge target, just exists).
git checkout -b kvm-v1-final
git tag kvm-v1-archive-20260428 -m "v1 KVM backend final state — see memo 22 for known limits"
git checkout uml-redesign-plan
```

Anyone who needs to reach v1 source can `git show kvm-v1-archive-20260428:arch/um/backend/kvm/...` or check out the branch. The tag is the canonical pointer.

### Step 2: Move the implementation to an archive directory

```bash
git mv arch/um/backend/kvm arch/um/backend/kvm-v1-archive
git commit -m "um: archive kvm v1 to arch/um/backend/kvm-v1-archive/

The v1 KVM backend reaches mean 19.4/21 on the cpython-parity gate
post-A.4i (memo 22) but the residual Bug B (memo 22 §Update — Bug B
is NOT a use-after-munmap) and the structural fragility of shadow
PT (memo 24) make further incremental fixes unproductive.

The v2 reimplementation (memo 25) starts on a clean ARCH=um substrate
after the 12 prerequisite refactors land. v1 stays in-tree (not built)
as a reference for v2 implementers.

Tagged: kvm-v1-archive-20260428"
```

Keep the directory in-tree. It's reachable via `git log`, `grep`, IDE
search. **Do not delete it** — the design memos are in there as code
comments, the C reproducers reference it, and v2 implementers will
want to grep for "how did v1 handle this case."

### Step 3: Strip v1's hooks from ARCH=um core

These are the cross-cutting changes v1 made outside `arch/um/backend/kvm/`.
Each needs explicit revert because the strip affects every backend.

**File-by-file revert checklist:**

| File | What v1 added | Revert action |
|---|---|---|
| `arch/um/include/asm/pgtable.h` | `kvm_shadow_sync_pte` calls in `pte_clear` (line 147-152), `set_pte` (line 312-340), `set_ptes` (lines 341-393) | Remove the calls; `pte_clear` reverts to a `#define` macro; `set_pte`/`set_ptes` lose their post-write hook |
| `arch/um/include/asm/tlbflush.h` | `kvm_shadow_sync_va_atomic` in `flush_tlb_page` (line 60); `kvm_shadow_sync_range_atomic` in `flush_tlb_range` (line 67) | Remove the calls; `um_tlb_mark_sync` is the only remaining hook, used by all backends |
| `arch/um/include/asm/kvm_mmu_sync.h` | Entire header (declarations of the shadow-sync hooks) | `git rm` the file |
| `arch/um/kernel/skas/mmu.c` | KVM-specific paths in `init_new_context` (the bulk-clear that `mm.c:307` short-circuits) | Revert to seccomp/ptrace behavior |
| `arch/um/kernel/process.c` | Any `kvm_*` calls in `__switch_to` / `arch_dup_task_struct` | Remove |
| `arch/x86/um/asm/processor_64.h` | `arch_thread.kvm` field (vCPU pointer, FPU buffer); `kvm_fpu_capture_for_fork` call from `arch_copy_thread` | Remove the field and the call; `arch_thread` shrinks |
| `arch/um/include/shared/backend.h` | KVM-specific ops in the dispatch table | Strip back to seccomp/ptrace verbs (will be replaced by the cleaner ops in refactor #2 below) |
| `arch/um/include/shared/os.h` | `register_kvm_kick_signal`, `os_kvm_kick_thread`, `os_kvm_block_kick_signal` declarations | Remove |
| `arch/um/os-Linux/signal.c` | `kvm_kick_signal_noop`, `register_kvm_kick_signal` definitions | Remove |
| `arch/um/os-Linux/process.c` | `os_kvm_kick_thread`, `os_kvm_block_kick_signal` definitions | Remove (these were added during A.4f attempts, post-revert in working tree) |
| `arch/um/Kconfig` | `CONFIG_UM_BACKEND_KVM`, `CONFIG_UM_BACKEND_KVM_INTEGRATED`, `CONFIG_UM_BACKEND_KVM_GADGET`, `CONFIG_UM_BACKEND_KVM_SNAPSHOT` etc. | Remove or rename to `_V1_ARCHIVE` and gate `depends on BROKEN` |
| `arch/x86/um/shared/sysdep/mm_id.h` (or wherever `mm_id.kvm_shadow` lives) | The `kvm_shadow` pointer field | Remove |

**Verification after this step:**

```bash
make ARCH=um defconfig
# Disable any auto-enabled KVM backend bits if they remain.
make ARCH=um O=/tmp/uml-clean -j$(nproc)
# Should produce a working seccomp-only UML.

UML_BINARY=/tmp/uml-clean/linux \
  bash tools/testing/selftests/um/cpython-parity/cpython-parity.sh
# Expect: 21/21 (seccomp baseline always passes).
```

If seccomp regresses after these reverts, the strip removed something
seccomp depended on — debug before proceeding.

### Step 4: Stand up the v2 stub directory

```bash
mkdir -p arch/um/backend/kvm-v2
```

Create three files:

**`arch/um/backend/kvm-v2/Kconfig`:**
```
config UM_BACKEND_KVM_V2
    bool "KVM backend (v2 reimplementation, in-progress)"
    depends on EXPERT && X86_64 && KVM
    default n
    help
      The v2 KVM backend reimplemented from scratch on top of the
      12 ARCH=um core refactors landed during the v1 → v2 transition
      (see Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
      25-v2-restart-guide.md). Uses TDP via per-mm host worker process,
      KVM_SET_USER_MEMORY_REGION per-mapping memslots, and standard
      x86_64 memory layout.

      Currently a stub. Selecting it builds an init that registers as
      the active backend and returns -ENOSYS on every op. Use
      backend=force=seccomp on the boot command line to get a working
      system until v2 implementation lands.

      The archived v1 implementation lives at arch/um/backend/kvm-v1-archive/
      and is documented in memos 21-24 alongside this guide.
```

**`arch/um/backend/kvm-v2/Makefile`:**
```
# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_UM_BACKEND_KVM_V2) += init.o
```

**`arch/um/backend/kvm-v2/init.c`:**
```c
// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend v2 — stub. Real implementation pending.
 *
 * See Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
 * 25-v2-restart-guide.md for the design intent.
 */

#include <linux/init.h>
#include <linux/printk.h>
#include <backend.h>

static int __init kvm_v2_init(void)
{
    pr_warn("um: kvm v2 backend selected but not implemented; "
            "use backend=force=seccomp until v2 lands\n");
    /* Register as a no-op backend so dispatch doesn't oops. */
    return 0;
}

um_backend_initcall(kvm_v2_init);
```

**`arch/um/backend/kvm-v2/README.md`:**
```
# KVM backend v2

Empty namespace; real implementation begins after the 12 ARCH=um core
refactors in memo 25 land.

Read first:
- Memo 24: clean-slate architectural items
- Memo 25: this guide
- Memo 22: v1 bug diagnosis (especially Bug B)
- Memo 21: v1 failed-fix log (so we don't repeat)
- arch/um/backend/kvm-v1-archive/: v1 source for reference
```

Hook it into the existing backend Kconfig/Makefile chain at
`arch/um/backend/{Kconfig,Makefile}` so the stub builds.

### Step 5: Update top-level docs

- `Documentation/virt/uml/index.rst`: add a "KVM backend status" section
  pointing at `kvm-v1-archive/` and `kvm-v2/` with their respective
  READMEs.
- `Documentation/virt/uml/redesign/README.md` (if exists): note the
  restart and link to memo 25.

### Step 6: Verify and commit

```bash
make ARCH=um defconfig
make ARCH=um O=/tmp/uml-restart -j$(nproc)
UML_BINARY=/tmp/uml-restart/linux \
  bash tools/testing/selftests/um/cpython-parity/cpython-parity.sh
# Still expect 21/21 (still on seccomp).

git add -A
git commit -m "um: archive kvm v1; clean ARCH=um core; stub v2 namespace

See Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
25-v2-restart-guide.md for the v2 plan."
```

### Step 7: Keep what's still useful

The five C reproducers in
`tools/testing/selftests/um/cpython-parity/repros/` stay untouched.
They are backend-agnostic correctness tests:
- `single_dlopen.c` → 30s deterministic Bug B repro under v1; v2 must pass
- `mt_dlopen_repro.c`, `mt_anon_exec.c`, `mt_file_noexec.c`,
  `mt_mmap_repro.c` → orthogonal stress tests

The cpython-parity gate at `tools/testing/selftests/um/cpython-parity/cpython-parity.sh`
stays as the canonical bar. v2's success criterion is **24h continuous
gate, zero flakes** (memo 23 Phase 4.3).

---

## Part 2 — 12 ARCH=um core refactors (6-10 weeks)

These should all land BEFORE v2's actual implementation begins. Each is
independently mergeable, each improves seccomp's quality along the way,
and v2 plugs into the resulting clean substrate.

### Refactor 1: Move `uml_physmem` to PML4[256+]

**Why.** Item #1 from memo 24. The §9.5 blocker. The single most important
enabler for v2 using TDP cleanly. Eliminates the bug class behind A
(memo 22), BUG.1 (memo 22), and the 1GB-huge-page CR3-swap triple-fault.

**Touch points** (estimated 50-200 sites):
- `arch/um/kernel/mem.c` — `uml_physmem` allocation (`setup_physmem`,
  `uml_reserved`).
- `arch/x86/um/asm/processor_64.h` — `TASK_SIZE` related macros may
  need adjustment if they currently equal `uml_physmem`.
- `arch/um/include/asm/pgtable.h` — `VMALLOC_START`, `VMALLOC_END`,
  `STACK_TOP_MAX` (line 96, 99).
- Every `__pa()` / `__va()` site that hardcodes assumptions about the
  layout. Use `git grep -l '__pa\|__va' arch/um arch/x86/um` for the
  full list.
- Boot code (`arch/um/kernel/um_arch.c`'s `setup_arch`) that maps
  early pages.
- Linker script `arch/um/kernel/uml.lds.S` if it has hardcoded
  addresses.

**Verification.**
- KUnit test: assert `init_mm.pgd[0]` is empty (no kernel-half entries
  in user-half PGD slot).
- KUnit test: assert `init_mm.pgd[256]` covers
  `[uml_physmem, uml_physmem + physmem_size)`.
- Seccomp gate must still pass post-relocation.
- Confirm `objdump -h vmlinux` shows kernel sections at the new high
  addresses.

**Estimated cost.** 1-2 weeks. Boot-order sensitivity is the main
unknown.

**Can land independently of:** everything (this is a pure UML core
change, no backend impact other than future).

#### Update — 2026-04-28: literal interpretation incompatible with seccomp

A surface-mapping pass (Explore subagent, ~73 reference sites
across arch/um/, arch/x86/um/, drivers) found that R1 as written —
"allocate `uml_physmem` (and the entire kernel direct map) at
PML4[256+]" — **cannot be implemented for seccomp mode**, and
therefore cannot land as a pre-v2 refactor without breaking the
seccomp gate.

The constraint: UML runs as a Linux *user* process. The host
kernel reserves PML4[256..511] (canonical kernel-half,
`0xffff_8000_0000_0000+`) and rejects any user mmap into it. Today,
`uml_physmem` is BOTH the kernel direct-map base AND the host VA
where UML's mapped pages physically live (`uml_physmem =
__binary_start & PAGE_MASK` in `arch/um/kernel/um_arch.c:381`).
Generic kernel code dereferences `__va(phys)` results
(`page_address(p)`, `memset((void *)__va(p), 0, PAGE_SIZE)`, etc.);
moving uml_physmem to PML4[256+] makes those derefs target
addresses the host kernel will not let UML access.

The *structural* fix this refactor describes — kernel half at
PML4[256+], user half at PML4[0], no overlap — is meaningful only
when the guest's page table is **separate** from the host process's
page table. That separation arrives with v2 + TDP + memslots (memo
26 Phase B): KVM walks `mm->pgd` directly, with kernel pages
mapped into PML4[256+] of the *guest* pgd, while the host process
keeps its physical memory wherever the kernel allocated it (low
host VA, untouched). For seccomp mode there is no guest pgd
distinct from the host pgd; the structural fix is structurally
absent.

Three viable rescopings:

A. **Soft / preparatory R1.** Define a constant
   `UM_KERNEL_VA_BASE = 0xffff_888000_000000` (or chosen value) for
   v2's eventual guest-pgd kernel-half. Add documentation comments
   in `arch/um/include/asm/page.h` and `arch/um/include/shared/mem.h`
   distinguishing host-VA usage from guest-pgd-VA usage. Add a KUnit
   test that's skipped today and gates v2's pgd setup later. No
   runtime change. ~half a day.

B. **Defer R1 to memo 26 Phase B.** v2's TDP + memslot work
   inherently sets up the guest pgd with kernel half at PML4[256+].
   R1 becomes part of that phase rather than a standalone refactor.
   Reorder the sequencing diagram so R2/R3/R4/R5/R7/R8/R9 land
   first, then v2 Phase B does the layout work in context.

C. **Strategy 2 anyway.** Bump uml_physmem to high VA, introduce
   `__binary_start_hva` for the ~12 host-VA sites that must keep
   working. Risky: depends on auditing every `__va()` deref site to
   confirm none run on seccomp's hot path. The subagent's report
   identified driver paths (`virtio_uml.c`, `vfio_user.c`) and
   `os-Linux/main.c:302` that would need explicit splitting, plus
   any generic-kernel `__va()` dereference. High likelihood of
   subtle seccomp regressions.

**Recommendation: option B.** The structural fix the memo wants is
the v2 Phase B work; doing R1 separately just to do it pre-v2
delivers cosmetic value (KUnit test that already needs v2 to
pass) at the cost of either no runtime change (option A) or a
risky audit (option C). Folding R1 into v2 Phase B keeps the
refactor sequence honest about where the layout decoupling
actually pays off.

#### Resolution — 2026-04-28: implement R1 as a Kconfig-gated abstraction

User insight: "can't we refactor this so that the kconfig flags
cleanly abstract this constraint between seccomp and kvm? it's
just another virtual memory abstraction layer, right?"

Yes — that's the correct framing. The "constraint" isn't
fundamental; it's that today's UML conflates two distinct concepts
under a single name. R1 introduces the conceptual split:

- `uml_physmem` — kernel direct-map base in the *guest pgd* (used
  by `__pa()` / `__va()` / `PAGE_OFFSET`). Today this is the host
  VA where UML loaded; under v2 KVM this becomes a high constant.

- `__binary_start_hva` — host VA where UML's pages physically live.
  Today equals `uml_physmem`; under v2 it stays low while
  `uml_physmem` moves high.

This commit lands the rename + abstraction without flipping any
Kconfig gate. Today's seccomp / dynamic builds see no behavioral
change (both anchors equal `__binary_start & PAGE_MASK`). When v2
KVM is built (memo 26 Phase B), the Kconfig gate flips and the
runtime split takes effect. Sites that mean "host VA" use
`__binary_start_hva`; sites that mean "kernel pgd VA" use
`uml_physmem`. The 5 sites identified by the surface-mapping pass
have been updated:

  - arch/um/os-Linux/main.c:302 — kfree-vs-vfree range check
    (host-VA semantics; uses `__binary_start_hva` lower bound)
  - arch/um/drivers/virtio_uml.c:652 — vhost-user offset
    (host-VA math; uses `__binary_start_hva`)
  - arch/um/drivers/vfio_user.c:56 — VFIO DMA offset
    (same; uses `__binary_start_hva`)
  - arch/um/include/shared/mem.h — abstraction header
  - arch/um/kernel/um_arch.c — both anchors initialized

Build verified: defconfig (DYNAMIC=y, SECCOMP=y) LINK linux clean.
Boot smoke under seccomp prints banner and runs Python.
Seccomp gate post-R1: 21/21 (pass=21 fail=0).

The `__binary_end_hva` companion (for upper-bound host-VA
comparisons) is deferred until a site actually needs it; today
high_physmem == __binary_start_hva + physmem_size and the existing
high_physmem suffices. v2 Phase B will introduce __binary_end_hva
when high_physmem and __binary_start_hva + physmem_size diverge.

The Kconfig gate that flips the runtime split lives at
arch/um/include/asm/page.h (currently a no-op `#define PAGE_OFFSET
(uml_physmem)`); v2's Phase B work redefines it under
`#ifdef CONFIG_UM_BACKEND_KVM_V2_HIGH_VA` (or similar) to point at
the high constant. The 35 kernel-PT users of __pa/__va inherit the
new value transparently; the 5 host-VA users above already use the
correct symbol.

### Refactor 2: Backend ops abstraction cleanup

**Why.** Item #5 from memo 24's strategic frame. Current
`arch/um/include/shared/backend.h` ops were shaped by what existed
(seccomp-style verbs). v1 jammed shadow-PT-specific concerns into
this interface. v2 needs cleaner verbs.

**Proposed new ops:**
```c
struct um_backend_ops {
    /* Lifecycle */
    int  (*init)(void);
    void (*shutdown)(void);

    /* Per-mm.  Backend allocates whatever it needs:
     *   seccomp:  fork stub-child host process
     *   ptrace:   fork ptraced-child host process
     *   kvm-v2:   fork per-mm worker process + per-mm KVM context
     */
    int  (*mm_create)(struct mm_struct *mm);
    void (*mm_destroy)(struct mm_struct *mm);

    /* Generic memory-region notification.  Backend translates: */
    int  (*mm_region_added)(struct mm_struct *mm, unsigned long va,
                            unsigned long len, int prot,
                            int phys_fd, u64 off);
    int  (*mm_region_removed)(struct mm_struct *mm, unsigned long va,
                              unsigned long len);
    int  (*mm_region_protected)(struct mm_struct *mm, unsigned long va,
                                unsigned long len, int new_prot);

    /* vCPU dispatch.  Returns when guest exits to host. */
    int  (*vcpu_run)(struct uml_pt_regs *regs);
};
```

**Touch points.**
- `arch/um/include/shared/backend.h` — new struct definition,
  deprecate old fields with a transition window.
- `arch/um/backend/seccomp/` — implement the new ops.
- `arch/um/backend/ptrace/` — implement (or mark deprecated).
- `arch/um/kernel/skas/mmu.c` — call new ops.
- Backend dispatch macros in `arch/um/include/asm/backend.h`.

**Estimated cost.** 1-2 weeks. Mostly mechanical.

**Depends on:** none.
**Blocks:** v2 (v2 implements against the new ops).

### Refactor 3: Decouple TLB sync from backends

**Why.** v1's hook-everywhere model (kvm_shadow_sync_pte in pgtable.h,
kvm_shadow_sync_range_atomic in tlbflush.h) is a major source of race
classes. Generalize.

**Plan.**
- `pte_clear`, `set_pte`, `set_ptes`, `pmd_clear`, etc. mark VAs as
  needing sync via the existing `um_tlb_mark_sync` mechanism. **No
  per-PTE backend callback.**
- `flush_tlb_*` triggers backend's `mm_region_*` callbacks for the
  affected ranges.
- Backends only see coarse range notifications, never per-PTE writes.

**Touch points.**
- `arch/um/include/asm/pgtable.h` — remove all backend hooks; pte_clear
  reverts to a macro.
- `arch/um/include/asm/tlbflush.h` — generic dispatch via backend ops.
- `arch/um/kernel/tlb.c` — `um_tlb_sync` orchestration.

**Estimated cost.** 1 week.

**Depends on:** Refactor 2 (needs the new ops).
**Blocks:** v2.

### Refactor 4: Standardize threading model — per-mm host worker process

**Why.** Items #2 and #8 from memo 24. The current
`clone(CLONE_VM | CLONE_VFORK | SIGCHLD)` model gives every UML task
its own host process, shared VM but separate signal tables — worst of
both worlds.

**Plan.**
- One host "spawner" process per UML kernel.
- For each guest mm: spawner forks a "worker" process. Worker has its
  own VA space (no CLONE_VM), its own signal table, its own KVM
  context if v2 is the backend.
- Inside each worker, multiple guest tasks share the worker's VA space
  via pthreads (CLONE_VM|CLONE_THREAD|CLONE_SIGHAND|CLONE_FILES).
- A guest task migrating between mms = signal sent to the new mm's
  worker, task picks it up there.

**Touch points.**
- `arch/um/os-Linux/skas/process.c` — `userspace_tramp`, `clone()`
  call.
- `arch/um/kernel/skas/mmu.c` — `init_new_context` (creates new
  worker on fork).
- Backend ops `mm_create`/`mm_destroy` map to spawn/reap worker.
- Inter-process syscall dispatch (worker → spawner) via UNIX socket
  or shared-memory ring.

**Estimated cost.** 3-4 weeks. This is the deepest refactor.

**Depends on:** Refactor 2.
**Blocks:** v2 (v2 piggybacks on the per-mm worker model).

### Refactor 5: Generic memory-region abstraction

**Why.** Stage B's "per-mapping memslot" work was v1-specific. Generalize
so seccomp/ptrace get the same primitive (they ignore it; v2 implements
it as `KVM_SET_USER_MEMORY_REGION`).

**Plan.**
```c
struct um_memory_region {
    unsigned long  va;
    unsigned long  len;
    int            prot;
    int            phys_fd;     /* -1 for anonymous */
    u64            offset;
    void          *backend_data;  /* opaque, backend-managed */
};
```

`mm_region_added/removed/protected` ops take a `struct um_memory_region`.
Seccomp/ptrace use it for their host-VA tracking. v2 uses it as the
memslot allocation key.

**Touch points.**
- `arch/um/include/asm/um_memory.h` (new file).
- Backend ops definition.
- All backend implementations.

**Estimated cost.** 1 week.

**Depends on:** Refactor 2.

### Refactor 6: Standardize signal handling

**Why.** v1 stumbled on per-process signal tables three times (A.4f
v1/v2/v2-with-sigprocmask). Refactor 4's threading model fixes most
of this — workers use CLONE_SIGHAND so all threads in a worker share
handlers — but signal infrastructure code in `os-Linux/signal.c`
needs cleanup too.

**Plan.**
- Establish handlers in the spawner. Workers inherit via fork.
- Inside a worker, all threads share the table.
- Per-thread signal masks via `pthread_sigmask`.
- Document the contract in `os-Linux/signal.c` headerdoc.

**Touch points.**
- `arch/um/os-Linux/signal.c`.
- `arch/um/os-Linux/process.c`.
- Anywhere `sigprocmask` vs `pthread_sigmask` matters.

**Estimated cost.** 1 week.

**Depends on:** Refactor 4.

### Refactor 7: Build observability infrastructure

**Why.** Item #9 from memo 24. v1 invented ad-hoc telemetry per-bug.
v2 should use standard tracing.

**Plan.**
```c
/* arch/um/include/asm/trace.h */
TRACE_EVENT(um_backend_mm_create, ...);
TRACE_EVENT(um_backend_mm_destroy, ...);
TRACE_EVENT(um_backend_mm_region_added, ...);
TRACE_EVENT(um_backend_mm_region_removed, ...);
TRACE_EVENT(um_backend_mm_region_protected, ...);
TRACE_EVENT(um_backend_vcpu_run_enter, ...);
TRACE_EVENT(um_backend_vcpu_run_exit, ...);   /* exit_reason, RIP */
TRACE_EVENT(um_backend_syscall_dispatch, ...); /* nr, args[6] */
TRACE_EVENT(um_backend_signal_received, ...);  /* sig, source */
```

Wire ftrace tracepoints. Test with `trace-cmd record -e um:*` then
`trace-cmd report`.

**Touch points.**
- `arch/um/include/asm/trace.h` (new).
- Every backend's hot paths.
- Documentation + example trace commands in
  `Documentation/virt/uml/tracing.rst`.

**Estimated cost.** 1 week.

**Depends on:** Refactor 2 (for clean op naming).

### Refactor 8: Clean up `um_tlb_sync`

**Why.** Currently `um_tlb_sync` knows about backend specifics. Make it
a generic VA-range queue drainer that calls backend ops.

**Plan.**
- The deferred-sync queue stores `struct um_memory_region` operations.
- `um_tlb_sync` walks the queue, calls `backend->mm_region_*` for
  each entry.
- No backend-specific knowledge in mm-arbiter layer.

**Touch points.**
- `arch/um/kernel/tlb.c`.
- Whichever data structure holds the deferred queue.

**Estimated cost.** 3-5 days.

**Depends on:** Refactor 2, Refactor 5.

### Refactor 9: Syscall dispatch path cleanup

**Why.** `arch/um/kernel/skas/syscall.c`'s `handle_syscall` is monolithic.
v1's `kvm_decode_syscall` short-circuited some classes. Generalize.

**Plan.**
- Define syscall classes generically (PASSTHROUGH, VCPU_STATE,
  SIGFRAME, TRAP, GADGET).
- Class table in a backend-agnostic header.
- Dispatcher reads class, routes to appropriate handler.
- Backends register class-specific handlers if they want to override.

**Touch points.**
- New header: `arch/um/include/asm/syscall_class.h`.
- `arch/um/kernel/skas/syscall.c`.
- v1's `arch/um/backend/kvm-v1-archive/syscall_class.c` is the design
  reference.

**Estimated cost.** 1 week.

**Depends on:** Refactor 2.

#### Update — 2026-04-28: premise largely obsolete, class table deferred

`arch/um/kernel/skas/syscall.c::handle_syscall` is ~100 lines and
quite linear: ptrace trace_enter → seccomp_check → sys_call_table
dispatch → trace_exit. The "monolithic" framing the memo uses
referred to v1's `kvm_decode_syscall` short-circuit path, which is
gone with the archive. Today there is no backend that overrides
syscall handling, so the proposed class table + registration
mechanism would add infrastructure with no consumer.

The substantive R9 work — define
`enum um_syscall_class { PASSTHROUGH, VCPU_STATE, SIGFRAME, TRAP,
GADGET }`, the per-syscall class table, and the backend
registration hook — naturally lands with v2 Phase D (memo 26)
where the vmcall hypercall path needs class-specific handling for
syscalls that need vCPU state, signal-frame manipulation, etc.
Building the abstraction pre-v2 just adds dead infrastructure;
landing it with v2's first consumer keeps the design honest.

**Marked done as a no-op for the pre-v2 substrate.** v2's Phase D
will introduce the class table at the same time as the first
backend that uses it.

### Refactor 10: Kill `harness.c` and `!CONFIG_UM_BACKEND_KVM_INTEGRATED`

**Why.** Already on the cleanup list (T.4). 1526 LoC of dead scaffold
from when KVM was prototype-only. v2 starts INTEGRATED-only — no
reason to carry the legacy.

**Plan.**
- `git rm arch/um/backend/kvm-v1-archive/harness.c` (it's now in the
  archive but still building).
- Remove `CONFIG_UM_BACKEND_KVM_INTEGRATED` from Kconfig (after Step
  3 of Part 1 already removed `CONFIG_UM_BACKEND_KVM_*`, this is
  cleanup).

**Touch points.**
- `arch/um/backend/kvm-v1-archive/harness.c` (delete).
- Kconfig (any straggling references).

**Estimated cost.** 1 day.

**Depends on:** Part 1 Step 3 (v1 archived).

### Refactor 11: Deprecate (or remove) ptrace backend

**Why.** Fewer backends to keep working through the refactors. Seccomp
+ v2-stub is enough scope.

**Plan.** Either:
- A. **Mark BROKEN**: `CONFIG_UM_BACKEND_PTRACE` gains
  `depends on BROKEN`. Code stays for archeology but isn't built.
- B. **Remove entirely**: `git rm -r arch/um/backend/ptrace`. More
  invasive but cleaner.

Recommend A for now (preserves option to revive). B in a future cleanup.

**Touch points.**
- `arch/um/backend/Kconfig`.
- `arch/um/Kconfig` (if backend selection is conditioned).

**Estimated cost.** Half a day.

**Depends on:** none.

### Refactor 12: Documentation refresh

**Why.** The architecture changes from refactors 1-11 should be reflected
in user-facing docs.

**Plan.**
- `Documentation/virt/uml/index.rst` — describe the new backend ops
  contract.
- `Documentation/virt/uml/backends.rst` (new) — per-backend status
  (seccomp: production; ptrace: deprecated; kvm-v1-archive: archived;
  kvm-v2: in-progress).
- `Documentation/virt/uml/tracing.rst` (from refactor 7).
- Update any relevant entries in `MAINTAINERS`.
- Headerdoc in `arch/um/include/shared/backend.h` for the new ops
  contract.

**Touch points.**
- `Documentation/virt/uml/`.
- `MAINTAINERS`.
- Header comments in changed source files.

**Estimated cost.** 2-3 days.

**Depends on:** Refactors 1-11 substantially complete.

---

## Part 3 — Sequencing, dependencies, risk

### Dependency graph

```
                        [Part 1 Step 1-7: archive v1, stub v2]
                                       │
        ┌──────────┬──────────┬────────┼────────┬──────────┐
        │          │          │        │        │          │
       [R1:       [R2:       [R7:    [R10:    [R11:      [R12:
        uml_       backend    obs.    kill     ptrace     docs
        physmem    ops        infra]  harness] BROKEN]    refresh]
        relocate]  cleanup]      │       │
        │          │  │          │       │
        │          │  └──────────┤       │
        │          ▼             │       │
        │      [R3: TLB sync     │       │
        │       decoupling]      │       │
        │          │             │       │
        │          ▼             │       │
        │      [R4: per-mm       │       │
        │       worker proc]     │       │
        │          │             │       │
        │          ▼             │       │
        │      [R6: signals]     │       │
        │          │             │       │
        │          ▼             │       │
        │      [R5: memory       │       │
        │       region]          │       │
        │          │             │       │
        │          ▼             │       │
        │      [R8: um_tlb_sync  │       │
        │       cleanup]         │       │
        │          │             │       │
        │          ▼             │       │
        │      [R9: syscall      │       │
        │       dispatch]        │       │
        │          │             │       │
        └──────────┴─────────────┴───────┴───────────────────┐
                                                              │
                                                              ▼
                                              [v2 implementation begins
                                               on clean substrate]
```

### Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| Refactor 1 (uml_physmem move) breaks boot in subtle ways | High | Land behind a Kconfig knob first; switch default after gate-validation |
| Refactor 4 (per-mm worker) breaks seccomp performance | Medium | Benchmark seccomp before/after; if regression > 20%, optimize the IPC ring |
| Refactor 4 introduces fork-storm under heavy load | Medium | Pool workers; reuse on mm exec/exit |
| Refactor 7 tracepoints add hot-path overhead | Low | Use static branches (jump label); off by default |
| v1 archive code rots and stops compiling | Low | Skip building it (`obj-` empty); or mark `depends on BROKEN`; or build under CI separately |
| Out-of-tree consumers of v1 hooks break | Low | Search code.search.devel for references; coordinate with maintainers |
| Refactor 9 (syscall dispatch) breaks gadget path | Low | v1 archive has reference; preserve gadget API |
| ptrace deprecation surprises users | Low | Mention in MAINTAINERS, release notes |

### Suggested sequencing (12 weeks total)

```
Week 1-2:    Part 1 (archive + stub) + R10 (kill harness) + R11 (ptrace BROKEN)
Week 3-4:    R1 (uml_physmem relocation) + R2 (backend ops) in parallel
Week 5:      R3 (TLB sync decoupling) + R7 (observability)
Week 6-9:    R4 (per-mm worker process) — the long pole
Week 10:     R5 (memory region) + R6 (signals) + R8 (um_tlb_sync) + R9 (syscall)
Week 11-12:  R12 (docs) + integration testing + soak

Then: v2 implementation begins.
```

Most refactors are independently mergeable to the seccomp baseline. The
gate stays at 21/21 (seccomp) throughout — any refactor that breaks
seccomp gets reverted, fixed, re-landed.

---

## Part 4 — v2 success criteria

The v2 reimplementation is **done** when all of the following hold:

1. **Functional**: cpython-parity gate at 21/21 single-pass across 100
   trials. Zero divergences.
2. **Stable**: 24h continuous gate run, zero flakes (memo 23 Phase 4.3).
3. **Performant**: cpython gate completes in ≤ 1.2× the wall-clock time
   of the seccomp baseline. (v1 is currently ~2× faster than seccomp on
   syscall-heavy workloads but that's offset by occasional retries.)
4. **Tier 1 ready**: pytest of `requests` + `cryptography` + `numpy`
   under v2, 100% pass.
5. **Clean diff**: v2 implementation is < 1500 LoC (vs v1's ~6000 LoC)
   thanks to the cleaner ARCH=um substrate.
6. **Observable**: every guest exit has a tracepoint; ftrace-based
   debugging works.
7. **SMP**: gate passes with `--with-cpus=4` boot arg, parallel kernel
   build inside guest works.

When all 7 hold, the project ships v2, marks v1-archive as historical
reference, deprecates v2 → v3 only if a major architecture shift
(like ARM64 port, or moving away from per-mm worker processes) requires
it.

---

## Part 5 — Open questions for the implementer

1. **R1 specifics**: which exact PML4 slot for `uml_physmem`? PML4[256]
   matches x86_64 standard, but check no UML-specific assumption breaks.
2. **R4 specifics**: pthread vs fork for vCPU threads inside a worker?
   pthreads cleaner, but signal model is touchier.
3. **R5 specifics**: should `um_memory_region` live in arch-generic
   include or under arch/um? (Probably the latter for now.)
4. **R7 specifics**: tracepoint naming convention. `um:` prefix? Or
   `um_kvm:` for backend-specific?
5. **v2 specifics**: per-vCPU-host-thread model — pin vCPU to host CPU
   via `sched_setaffinity`?

These should be resolved in a follow-up design memo (memo 26?) before
v2 implementation begins.
