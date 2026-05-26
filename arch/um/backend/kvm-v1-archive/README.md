# UML KVM backend v1 — archive

This directory holds the **v1** implementation of the UML KVM backend,
preserved as-is for v2 implementer reference. It is **not built**:
`CONFIG_UM_BACKEND_KVM_V1_ARCHIVE` depends on `BROKEN`, and the parent
Makefile no longer routes `obj-y` into this directory.

The active KVM backend work happens in [`../kvm-v2/`](../kvm-v2/).

## What v1 was

Stage A redesign (per-task vCPU + `KVM_SET_SIGNAL_MASK`, landed
2026-04-27) plus the A.4i bootstrap-VA relocation (landed 2026-04-28
on top of Stage A) reached **mean 19.4/21** on the cpython-parity
gate. The implementation totals ~6000 LoC across these files:

- `lifecycle.c` — backend probe / init / shutdown, shadow PGD
  allocator, per-mm shadow apparatus
- `thread.c` — guest-entry loop, sregs/regs marshal, IDT/TSS/LSTAR
  handlers, IST stack handling, `kvm_run_userspace`
- `mm.c` — `kvm_mm_attach/detach/map/unmap`, `kvm_mm_map_collides_kernel`
  shield (BUG.1)
- `shadow_sync.c` — direct shadow-PT-leaf sync from `set_pte` /
  `flush_tlb_*` hooks (memo 15)
- `sregs.c` — long-mode segment / CR / EFER setup
- `record.c`, `snapshot.c` — record-replay and snapshot scaffolding
- `kvm_backend.c` / `kvm_backend.h` — ops table singleton + internal
  header

(`harness.c` — the D-04b.1b diagnostic scaffold — was deleted by
memo 25 refactor 10; it was 1526 LoC of bring-up test code that
never ran outside the early-2026 spike. Reachable via `git show
kvm-v1-archive-20260428:arch/um/backend/kvm/harness.c` if needed.)
- `syscall_class.c` — gadget syscall classification (memo 11)
- `stubs.c`, `time.c` — backend-specific stubs and clocksource

## Why it was archived

Three structural issues kept v1 below 21/21:

1. **Shadow PT staleness** (Bug B, memo 22 §"Update — Bug B is NOT a
   use-after-munmap"): residual ~5-10% per-module flake from a
   shadow-PT-served stale-pointer source we couldn't localize after
   six diagnostic rounds (A.4f v1/v2/v2-with-sigprocmask, A.4h
   diag1/diag2, A.4j register-marshal audit).
2. **uml_physmem at PML4[0]** (memo 24 item #1): the kernel direct
   map at `[0x60000000, 0x80000000)` overlaps user VAs, requiring
   the BUG.1 mm-shield, the bootstrap-VA-in-user-half class of bugs
   (Bug A → A.4i), and blocking direct CR3 = `__pa(mm->pgd)` because
   `init_mm.pgd` PUD[1] is a 1GB US=0 huge page covering the same
   range.
3. **Per-task vCPU on per-CPU model**: KVM expects 1 host thread = 1
   vCPU for life. v1 came close with Stage A but kept UML's
   cooperative `switch_threads`/longjmp scheduler underneath,
   limiting how clean the per-vCPU isolation could be.

Memos 21-22 detail the v1 fix-attempt history; memo 24 abstracts to
10 clean-slate items; memos 25-26 lay out the v2 plan.

## v2 design (one paragraph)

KVM walks `mm->pgd` directly via Two-Dimensional Paging (TDP). Each
guest mm is its own host worker process (no shadow PT to maintain).
Each host CPU gets one vCPU thread; UML's scheduler dispatches tasks
onto vCPUs the way Linux dispatches threads onto CPUs.
`KVM_SET_USER_MEMORY_REGION` adds/removes per-mapping memslots —
KVM's mmu_notifier handles cross-vCPU EPT invalidation for free.
`uml_physmem` moves to PML4[256+] (refactor 1) so user VAs and
kernel direct map can no longer collide. Guest syscalls trap via
`vmcall` → `KVM_EXIT_HYPERCALL` (no LSTAR trampoline, no bootstrap
pages installed in user mms). Target ~1500 LoC.

## v1 lifecycle

- **2026-04-28**: archive (`git mv` to `kvm-v1-archive/`, parent
  Makefile drops the obj-y line, Kconfig renames to `_V1_ARCHIVE
  depends on BROKEN`).
- **2026-04-28 + 6 months**: scheduled deletion. By then v2 should
  be in production and v1 will have served its referent role.
  Memos 21-27 stay in `Documentation/virt/uml/redesign/` permanently.

## How to use this archive

- **`grep` / `git log` / IDE-search**: free. v1's design notes are
  embedded as code comments and remain useful context for v2.
- **Borrow code verbatim**: don't. The structural shape changes
  enough that line-level borrow-and-paste is a category error. Read
  v1 to understand the problem domain, then write v2 fresh.
- **Resurrect to compare behaviour**: switch
  `CONFIG_UM_BACKEND_KVM_V1_ARCHIVE` off `depends on BROKEN`, restore
  the parent Makefile's `obj-$(...) += kvm-v1-archive/` line, and
  rebuild. The hot-path hooks are gone from ARCH=um core so a fresh
  resurrection will need them re-introduced under their own
  config-gate; this is intentional friction.

For full context, start with
[`../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/24-eli5-and-clean-slate.md`](../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/24-eli5-and-clean-slate.md).
