# SMP-T31 — TDP coherence fix plan (UML-only, no upstream KVM changes)

**Date:** 2026-05-03
**Status:** PLAN. Implements α first; δ is fallback if α inconclusive.

## Problem (recap)

Three-way comparison establishes the bug as v2-specific:

| Configuration | mt-mini SMP T=8 × 30 | flake |
|---|---|---|
| Bare host | 30 / 30 | 0% |
| UML + seccomp SMP T=8 ncpus=4 | 30 / 30 | 0% |
| UML + kvm-v2 SMP T=8 ncpus=4 | 22 / 30 | 27% |

Failure: a worker pthread reads `0` from a 4 KB page it just wrote `tid` to (same thread, back-to-back `memset` then verify).

Diagnosis (both opus + codex agents converge): KVM's per-VM mmu_notifier IS registered against the spawner mm at `kvm_create_vm` (`virt/kvm/kvm_main.c:887-891`) and DOES fire for HVA changes via host mm operations (mprotect, munmap, reclaim, etc.). But UML's `set_pte()` (`arch/um/include/asm/pgtable.h:326-363`) is a plain data write to a physmem-backed page — NOT a host mm operation. KVM never sees a signal that the GVA→GPA mapping changed. Under multi-threaded mmap churn, vCPU B's TDP/EPT shadow can serve stale translations even after vCPU A's PTE update + the G.2 IPI kick.

## Existing related work to be aware of

- `arch/um/os-Linux/process.c:355-374` already provides `os_drop_caching(addr, len)` = `madvise(addr, len, MADV_DONTNEED)`. The comment block explicitly notes "this fires mmu_notifier in the host kernel, which causes KVM to invalidate any cached EPT/TDP entries pointing to the dropped HPAs."
- SMP-T26 H_E experiment (`arch/um/backend/kvm-v2/syscall_trap.c:1471-1484`) tried calling `os_drop_caching` per-faulted-page from `handle_io_pf` after `segv_handler`. **Did not change** the threaded-fork-malloc fail rate. Conclusion was "TDP cache aliasing for the FAULTED page is not the mechanism — corruption may be on adjacent pages, or not TDP-cache at all."
- SMP-T26 G.2 ablation: disabling `tlb_kick_others` (which forces remote vCPU CR4.PGE→INVVPID) **did not change** mt-mini fail rate either (`arch/um/kernel/tlb.c:561-565`).

These two negative results weaken the "pure TDP coherence" hypothesis. T31 proceeds anyway because:

1. Both prior experiments invalidated only ONE page or only the hardware TLB. T31 invalidates the entire drained range proactively in `um_tlb_sync` (after the batch of PTE writes is complete, before remote vCPUs see any of them).
2. The next-most-likely hypothesis (pure UML state-tracking bug in `set_pte` ordering or `mmu_gather` drain) has been investigated under T19/T20/T22 with partial fixes; deeper investigation is more expensive than a clean A/B of T31.

If T31 (α and δ) both fail, the working hypothesis becomes "the bug is NOT in KVM TDP/EPT coherence at all" and the next direction is `set_pte` ordering / `mmu_gather` / UML memory model investigation.

## Option α — `madvise(MADV_DONTNEED)` on the drained user-VA range, mapped to physmem HVAs

### Mechanism

After `um_tlb_sync` completes its drain successfully, walk the just-drained user-VA range `[sync_tlb_range_from, sync_tlb_range_to)` over the mm's pgd. For each leaf PTE present in the range, compute `host_va = uml_physmem + pte_pfn(pte) * PAGE_SIZE` and call `os_drop_caching(host_va, PAGE_SIZE)`. The host kernel's `madvise(MADV_DONTNEED)` zaps the spawner mm's host PTE for those pages, which fires `mmu_notifier_invalidate_range_*` callbacks. KVM's existing per-VM notifier intersects the HVA range with slot 0 and zaps any TDP/EPT entries pointing at those HPAs. Next vCPU access faults; KVM walks afresh, refills EPT, hits the page (still in the tmpfs page cache — same data — since physmem is `MAP_SHARED` over a tmpfs file).

### Why this might fix what T26 H_E didn't

- T26 H_E madvised one page (the faulted page) at handle_io_pf. T31 madvises the WHOLE just-drained range (typically multiple pages), proactively, before any remote vCPU could see a stale TDP entry derived from those PTE writes.
- T26 H_E ran on KVM_EXIT_IO PF path (reactive, single-page). T31 runs on `um_tlb_sync` post-drain (proactive, batched).

### Files I will modify

#### 1. `arch/um/include/shared/backend.h`

Insert after line 225 (after `tlb_kick_others`):

```c
/*
 * Optional explicit guest-translation invalidation. Called from
 * um_tlb_sync after a successful drain, BEFORE tlb_kick_others.
 * Backends with a separate guest-PT-walk cache (kvm-v2 EPT shadow)
 * use this to invalidate cached translations for the drained range.
 * Seccomp leaves NULL — its mm_region ops already mediate via
 * standard host mm machinery which fires mmu_notifier.
 *
 * SMP-T31 (2026-05-03): KVM's mmu_notifier is registered against
 * the spawner mm but UML's set_pte is a plain data write that
 * never triggers a host mm operation; without this hook KVM has
 * no signal that GVA→GPA mappings changed.
 */
int (*tlb_invalidate_guest)(struct mm_struct *mm,
                            unsigned long start, unsigned long end);
```

#### 2. `arch/um/kernel/tlb.c`

In `um_tlb_sync()` (line 450-579), insert call BEFORE `tlb_kick_others` at line 567. Read the range BEFORE the success-path clear at line 571-573:

```c
    if (ret == 0 && mm != &init_mm) {
        atomic64_inc(&mm->context.tlb_gen);

        /*
         * SMP-T31 (2026-05-03): tell backend to invalidate any
         * cached guest-translation state over the just-drained
         * range. Must happen BEFORE the IPI kick so other vCPUs,
         * on waking, walk the freshly-invalidated translation
         * rather than racing the invalidate.
         */
        if (um_backend->tlb_invalidate_guest) {
            unsigned long s = mm->context.sync_tlb_range_from;
            unsigned long e = mm->context.sync_tlb_range_to;
            smp_wmb();   /* PTE writes visible before invalidate */
            um_backend->tlb_invalidate_guest(mm, s, e);
        }

        if (um_backend->tlb_kick_others)
            um_backend->tlb_kick_others(mm);
    }
```

#### 3. `arch/um/backend/kvm-v2/region.c`

Add new function (probably at end of file, with other tlb-related helpers):

```c
/*
 * SMP-T31 (2026-05-03): walk the just-drained user-VA range over
 * mm's pgd, collect leaf PFNs, and madvise(MADV_DONTNEED) on each
 * PFN's HVA in the spawner mm. Triggers KVM's existing mmu_notifier
 * (registered at vm_create against the spawner mm) which zaps EPT
 * entries pointing at those HPAs.
 *
 * Cost: one madvise per present-leaf PTE in the drained range.
 * Acceptable for typical drains (a few PTEs); large drains (e.g.,
 * exit_mmap) walk thousands of PTEs but only on process exit which
 * is not a hot path.
 *
 * Returns 0 always (best-effort; madvise failure is logged but
 * non-fatal — falls back to the existing CR4.PGE-toggle defense
 * which still fires on next dispatch).
 */
int kvm_v2_tlb_invalidate_guest(struct mm_struct *mm,
                                unsigned long start, unsigned long end)
{
    unsigned long addr;
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte, ent;

    if (!mm || mm == &init_mm) return 0;
    if (start >= end) return 0;

    addr = start;
    while (addr < end) {
        pgd = pgd_offset(mm, addr);
        if (!pgd_present(*pgd)) { addr = (addr + PGDIR_SIZE) & PGDIR_MASK; continue; }
        p4d = p4d_offset(pgd, addr);
        if (!p4d_present(*p4d)) { addr = (addr + P4D_SIZE) & P4D_MASK; continue; }
        pud = pud_offset(p4d, addr);
        if (!pud_present(*pud)) { addr = (addr + PUD_SIZE) & PUD_MASK; continue; }
        pmd = pmd_offset(pud, addr);
        if (!pmd_present(*pmd)) { addr = (addr + PMD_SIZE) & PMD_MASK; continue; }
        pte = pte_offset_kernel(pmd, addr);
        ent = *pte;
        if (pte_present(ent)) {
            unsigned long pfn = pte_pfn(ent);
            void *hva = (void *)(uml_physmem + (pfn << PAGE_SHIFT));
            (void)os_drop_caching(hva, PAGE_SIZE);
        }
        addr += PAGE_SIZE;
    }
    return 0;
}
```

Header includes to verify present (likely already there from existing region.c uses):
- `<linux/mm.h>`, `<linux/pgtable.h>`, `<asm/pgtable.h>` for pgd/p4d/pud/pmd/pte walkers
- `<shared/os.h>` for `os_drop_caching` and `uml_physmem`

#### 4. `arch/um/backend/kvm-v2/ops.c`

Add line after `.tlb_kick_others` at line 139:

```c
    .tlb_invalidate_guest = kvm_v2_tlb_invalidate_guest,
```

#### 5. `arch/um/backend/kvm-v2/kvm_v2_backend.h`

Add extern decl with the other tlb helpers (~line 575):

```c
int kvm_v2_tlb_invalidate_guest(struct mm_struct *mm,
                                unsigned long start, unsigned long end);
```

### Build + test

1. Build SMP variant in **`~/src/uml-builds/uml-smp-t31a/`** (separate from production `uml-smp` and the H2-narrow `uml-smp-h2narrow`).
2. **Primary**: `mt-mini SMP T=8 × 30` → success = ≥27/30 (statistically distinguishable from baseline 22/30 at p<0.05; ideally 30/30 or close).
3. **Regression**: `threaded-fork-malloc × 5 boots × 4000 forks` must remain 0 / 20000 fork-fails.
4. **Regression**: `cpython-parity 21-mod × 1` must remain 21/21.
5. **Regression**: substrate gate (SMP) must remain `25/3/3`, matching seccomp.

### Decision tree

- **mt-mini ≥27/30 and no regression** → α is the fix. Land it. Move to narrowing/perf.
- **mt-mini 22-26/30 (no improvement) but no regression** → α isn't the right hook. Try δ (mprotect toggle). The hypothesis remains "TDP coherence", just with a stronger trigger.
- **mt-mini regresses or fork-malloc/parity/substrate regresses** → α is actively harmful. Revert immediately. Investigate why madvise is breaking previously-working paths (most likely: `os_drop_caching` is dropping a page that another vCPU is concurrently writing, and the host re-fault races with the guest write — though MAP_SHARED tmpfs should make this safe).

## Option δ — `mprotect()` RW→R→RW toggle on the drained HVA range (fallback if α inconclusive)

Same skeleton as α — same backend op, same tlb.c hook, same per-PFN walk — but instead of `os_drop_caching(hva, PAGE_SIZE)`, do:

```c
mprotect(hva, PAGE_SIZE, PROT_READ);
mprotect(hva, PAGE_SIZE, PROT_READ | PROT_WRITE);
```

Two syscalls instead of one. Each transition fires `mmu_notifier_invalidate_range_*` reliably. No page eviction (just a permission toggle). Cleaner semantics for shared memory.

**Why δ is fallback not first**: two syscalls per page vs one. If α works, δ's extra cost is wasted.

## Option β / γ (future, only if both α and δ fail)

- **β (slot 0 DELETE+CREATE)**: forces full TDP zap on every drain. Atomicity gap. Heavy. Documented for completeness; skip unless α and δ both fail.
- **γ (revert E.5, restore per-region memslots)**: the path v1 implicitly relied on. Significant code revert. Re-introduces the per-region churn that motivated E.5's drop. Skip unless α and δ both fail AND we're willing to revisit E.5.

## Out of scope (explicitly NOT pursuing)

- Adding a new KVM UAPI ioctl (touches upstream KVM, kernel ABI for everyone). Considered and rejected per session 2026-05-03; UML must work on a vanilla host kernel.

## Validation: definition of "closed"

T31 is "closed" when ALL of the following hold across one validation run:

1. mt-mini SMP T=8 × 30 = 30/30 PASS (or 29-30/30 with documented isolated outlier of a different bug class)
2. threaded-fork-malloc 30-boot soak = 0 / N forks fail (no T29 regression)
3. cpython-parity 21-mod × 5 boots = 105/105 PARITY
4. Wide cpython-parity 135 modules = 0 KVM_REGRESSION
5. Substrate gate (SMP) = 25/3/3 matches seccomp

If only (1) closes but others regress, the fix is harmful and needs narrowing or revert. If (1) doesn't close and others stay green, T31 didn't fix the right thing — re-examine the hypothesis.
