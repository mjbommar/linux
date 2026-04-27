/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 * Copyright 2003 PathScale, Inc.
 * Derived from include/asm-i386/pgtable.h
 */

#ifndef __UM_PGTABLE_H
#define __UM_PGTABLE_H

#include <asm/page.h>
#include <linux/mm_types.h>
#include <asm/kvm_mmu_sync.h>	/* memo 15 direct shadow sync */

#define _PAGE_PRESENT	0x001
#define _PAGE_NEEDSYNC	0x002
#define _PAGE_RW	0x020
#define _PAGE_USER	0x040
#define _PAGE_ACCESSED	0x080
#define _PAGE_DIRTY	0x100
/* If _PAGE_PRESENT is clear, we use these: */
#define _PAGE_PROTNONE	0x010	/* if the user mapped it with PROT_NONE;
				   pte_present gives true */

/* We borrow bit 10 to store the exclusive marker in swap PTEs. */
#define _PAGE_SWP_EXCLUSIVE	0x400

#if CONFIG_PGTABLE_LEVELS == 4
#include <asm/pgtable-4level.h>
#elif CONFIG_PGTABLE_LEVELS == 2
#include <asm/pgtable-2level.h>
#else
#error "Unsupported number of page table levels"
#endif

extern pgd_t swapper_pg_dir[PTRS_PER_PGD];

/* Just any arbitrary offset to the start of the vmalloc VM area: the
 * current 8MB value just means that there will be a 8MB "hole" after the
 * physical memory until the kernel virtual memory starts.  That means that
 * any out-of-bounds memory accesses will hopefully be caught.
 * The vmalloc() routines leaves a hole of 4kB between each vmalloced
 * area for the same reason. ;)
 */

#ifndef COMPILE_OFFSETS
#include <as-layout.h> /* for high_physmem */
#endif

#define VMALLOC_OFFSET	(__va_space)
#define VMALLOC_START	((high_physmem + VMALLOC_OFFSET) & ~(VMALLOC_OFFSET-1))

/*
 * Under CONFIG_KMSAN, split the original VMALLOC range
 * [VMALLOC_START, TASK_SIZE - 2 * PAGE_SIZE) into four
 * equal quarters matching x86_64's layout
 * (arch/x86/include/asm/pgtable_64_types.h:124-169). This
 * is the D62-selected resolution for the D58 "dedicated
 * KMSAN shadow slab doesn't fit" breakage. See
 * `Documentation/virt/uml/redesign/02-workstreams/
 * C-profiles-and-gaps/07-port-kmsan-redesign.md` for the
 * feasibility comparison.
 *
 *   quarter 1 [VMALLOC_START, VMALLOC_END)
 *                              — the effective vmalloc area
 *                                (1/4 of original size)
 *   quarter 2 [KMSAN_VMALLOC_SHADOW_START, ...)
 *                              — shadow for vmalloc range
 *                                (1 byte per byte)
 *   quarter 3 [KMSAN_VMALLOC_ORIGIN_START, ...)
 *                              — origin for vmalloc range
 *   quarter 4 — unused on UML
 *
 * **Modules-vs-vmalloc note.** On UML, MODULES_VADDR ==
 * VMALLOC_START (see the definitions below); modules live
 * inside the same VA range as vmalloc. The generic
 * mm/kmsan/shadow.c::vmalloc_meta() checks the vmalloc
 * predicate before the module predicate, so every module
 * address is classified as vmalloc and routed through
 * quarter 2 (shadow) / quarter 3 (origin). The modules-
 * shadow and modules-origin 4th-quarter slots x86 uses
 * therefore have no corresponding consumer on UML; we
 * alias the `KMSAN_MODULES_*_START` macros to their
 * VMALLOC equivalents so any caller that does reach the
 * module branch gets a consistent address in quarters
 * 2 / 3 rather than an otherwise-unused quarter 4 region.
 * The 4th quarter is left unreserved — future subsystems
 * (e.g. a dedicated per-CPU shadow bank) can claim it.
 *
 * The generic KMSAN code (mm/kmsan/shadow.c::vmalloc_meta)
 * computes shadow/origin addresses as VMALLOC_START +
 * offset + KMSAN_VMALLOC_*_OFFSET; our macros feed that
 * arithmetic with the same shapes x86 uses.
 */
#ifndef CONFIG_KMSAN
#define VMALLOC_END	(TASK_SIZE-2*PAGE_SIZE)
#else
#define VMALLOC_QUARTER_SIZE	\
	(((TASK_SIZE - 2 * PAGE_SIZE) - VMALLOC_START) / 4)
#define VMALLOC_END		(VMALLOC_START + VMALLOC_QUARTER_SIZE - 1)

#define KMSAN_VMALLOC_SHADOW_OFFSET	VMALLOC_QUARTER_SIZE
#define KMSAN_VMALLOC_ORIGIN_OFFSET	(VMALLOC_QUARTER_SIZE << 1)

#define KMSAN_VMALLOC_SHADOW_START	\
	(VMALLOC_START + KMSAN_VMALLOC_SHADOW_OFFSET)
#define KMSAN_VMALLOC_ORIGIN_START	\
	(VMALLOC_START + KMSAN_VMALLOC_ORIGIN_OFFSET)

/*
 * Modules overlap vmalloc on UML — alias to vmalloc shadow
 * and origin. See the "Modules-vs-vmalloc note" above.
 */
#define KMSAN_MODULES_SHADOW_START	KMSAN_VMALLOC_SHADOW_START
#define KMSAN_MODULES_ORIGIN_START	KMSAN_VMALLOC_ORIGIN_START
#endif /* CONFIG_KMSAN */

#define MODULES_VADDR	VMALLOC_START
#define MODULES_END	VMALLOC_END

#define _PAGE_TABLE	(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | _PAGE_ACCESSED | _PAGE_DIRTY)
#define _KERNPG_TABLE	(_PAGE_PRESENT | _PAGE_RW | _PAGE_ACCESSED | _PAGE_DIRTY)
#define _PAGE_CHG_MASK	(PAGE_MASK | _PAGE_ACCESSED | _PAGE_DIRTY)
#define __PAGE_KERNEL_EXEC                                              \
	 (_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED)
#define PAGE_NONE	__pgprot(_PAGE_PROTNONE | _PAGE_ACCESSED)
#define PAGE_SHARED	__pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | _PAGE_ACCESSED)
#define PAGE_COPY	__pgprot(_PAGE_PRESENT | _PAGE_USER | _PAGE_ACCESSED)
#define PAGE_READONLY	__pgprot(_PAGE_PRESENT | _PAGE_USER | _PAGE_ACCESSED)
#define PAGE_KERNEL	__pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED)
#define PAGE_KERNEL_EXEC	__pgprot(__PAGE_KERNEL_EXEC)

/*
 * The i386 can't do page protection for execute, and considers that the same
 * are read.
 * Also, write permissions imply read permissions. This is the closest we can
 * get..
 */

/*
 * Memo 15 direct-shadow-sync: convert pte_clear from macro to
 * inline function so the integrated KVM backend can hook it. The
 * pte_set_val side-effect is preserved exactly. The
 * kvm_shadow_sync_pte hook is declared in <asm/kvm_mmu_sync.h>
 * which we include at the top of this file.
 */
static inline void pte_clear(struct mm_struct *mm, unsigned long addr,
			     pte_t *xp)
{
	pte_set_val(*(xp), (phys_t)0, __pgprot(_PAGE_NEEDSYNC));
	(void)kvm_shadow_sync_pte(mm, addr, *xp);
}

#define pmd_none(x)	(!((unsigned long)pmd_val(x) & ~_PAGE_NEEDSYNC))
#define	pmd_bad(x)	((pmd_val(x) & (~PAGE_MASK & ~_PAGE_USER)) != _KERNPG_TABLE)

#define pmd_present(x)	(pmd_val(x) & _PAGE_PRESENT)

/*
 * pmd_clear / pud_clear / p4d_clear notify the KVM shadow PT
 * INDIRECTLY, via the flush_tlb_range hook in <asm/tlbflush.h>.
 * Direct notification would require a VA we don't have here (the
 * macro receives only the entry pointer, not the VA range it
 * covers). The invariant relied on:
 *
 *   Every call site that does pmd_clear / pud_clear / p4d_clear
 *   on a USER VA range MUST be followed by a flush_tlb_range over
 *   the same VA range BEFORE the next KVM_RUN entry. Generic mm
 *   code (mm/memory.c, mm/mremap.c, mm/madvise.c) follows this
 *   discipline; flush_tlb_range's KVM-backend hook calls
 *   kvm_shadow_sync_range_atomic, which clears or refreshes the
 *   corresponding shadow leaves.
 *
 * For ranges >= 2 MiB (= 512 pages, the PMD coverage),
 * kvm_shadow_sync_range_atomic short-circuits to
 * needs_full_resync=true rather than walking page-by-page (memo
 * 15 F9 threshold). pud_clear (1 GiB) and p4d_clear (512 GiB)
 * always exceed this threshold, so they always trigger the full
 * resync repair on the next kvm_enter_guest. Safe but heavy.
 *
 * Task #96: if a future patch adds a call site that does
 * pmd_clear without a paired flush_tlb_range, the shadow leaves
 * for that range will remain stale until the next mm_unmap or
 * fill — and the guest can read deleted data through them.
 * Instrument with kvm_shadow_audit_va before merging.
 */
#define pmd_clear(xp)	do { pmd_val(*(xp)) = _PAGE_NEEDSYNC; } while (0)

#define pmd_needsync(x)   (pmd_val(x) & _PAGE_NEEDSYNC)
#define pmd_mkuptodate(x) (pmd_val(x) &= ~_PAGE_NEEDSYNC)

#define pud_needsync(x)   (pud_val(x) & _PAGE_NEEDSYNC)
#define pud_mkuptodate(x) (pud_val(x) &= ~_PAGE_NEEDSYNC)

#define p4d_needsync(x)   (p4d_val(x) & _PAGE_NEEDSYNC)
#define p4d_mkuptodate(x) (p4d_val(x) &= ~_PAGE_NEEDSYNC)

#define pmd_pfn(pmd) (pmd_val(pmd) >> PAGE_SHIFT)
#define pmd_page(pmd) phys_to_page(pmd_val(pmd) & PAGE_MASK)

#define pte_page(x) pfn_to_page(pte_pfn(x))

#define pte_present(x)	pte_get_bits(x, (_PAGE_PRESENT | _PAGE_PROTNONE))

/*
 * =================================
 * Flags checking section.
 * =================================
 */

static inline int pte_none(pte_t pte)
{
	return pte_is_zero(pte);
}

/*
 * The following only work if pte_present() is true.
 * Undefined behaviour if not..
 */
static inline int pte_read(pte_t pte)
{
	return((pte_get_bits(pte, _PAGE_USER)) &&
	       !(pte_get_bits(pte, _PAGE_PROTNONE)));
}

static inline int pte_exec(pte_t pte){
	return((pte_get_bits(pte, _PAGE_USER)) &&
	       !(pte_get_bits(pte, _PAGE_PROTNONE)));
}

static inline int pte_write(pte_t pte)
{
	return((pte_get_bits(pte, _PAGE_RW)) &&
	       !(pte_get_bits(pte, _PAGE_PROTNONE)));
}

static inline int pte_dirty(pte_t pte)
{
	return pte_get_bits(pte, _PAGE_DIRTY);
}

static inline int pte_young(pte_t pte)
{
	return pte_get_bits(pte, _PAGE_ACCESSED);
}

static inline int pte_needsync(pte_t pte)
{
	return pte_get_bits(pte, _PAGE_NEEDSYNC);
}

/*
 * =================================
 * Flags setting section.
 * =================================
 */

static inline pte_t pte_mkclean(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_DIRTY);
	return(pte);
}

static inline pte_t pte_mkold(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_ACCESSED);
	return(pte);
}

static inline pte_t pte_wrprotect(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_RW);
	return pte;
}

static inline pte_t pte_mkread(pte_t pte)
{
	pte_set_bits(pte, _PAGE_USER);
	return pte;
}

static inline pte_t pte_mkdirty(pte_t pte)
{
	pte_set_bits(pte, _PAGE_DIRTY);
	return(pte);
}

static inline pte_t pte_mkyoung(pte_t pte)
{
	pte_set_bits(pte, _PAGE_ACCESSED);
	return(pte);
}

static inline pte_t pte_mkwrite_novma(pte_t pte)
{
	pte_set_bits(pte, _PAGE_RW);
	return pte;
}

static inline pte_t pte_mkuptodate(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_NEEDSYNC);
	return pte;
}

static inline pte_t pte_mkneedsync(pte_t pte)
{
	pte_set_bits(pte, _PAGE_NEEDSYNC);
	return(pte);
}

static inline void set_pte(pte_t *pteptr, pte_t pteval)
{
	pte_copy(*pteptr, pteval);

	/* If it's a swap entry, it needs to be marked _PAGE_NEEDSYNC so
	 * update_pte_range knows to unmap it.
	 */

	*pteptr = pte_mkneedsync(*pteptr);
}

#define PFN_PTE_SHIFT		PAGE_SHIFT

static inline void um_tlb_mark_sync(struct mm_struct *mm, unsigned long start,
				    unsigned long end)
{
	guard(spinlock_irqsave)(&mm->context.sync_tlb_lock);

	if (!mm->context.sync_tlb_range_to) {
		mm->context.sync_tlb_range_from = start;
		mm->context.sync_tlb_range_to = end;
	} else {
		if (start < mm->context.sync_tlb_range_from)
			mm->context.sync_tlb_range_from = start;
		if (end > mm->context.sync_tlb_range_to)
			mm->context.sync_tlb_range_to = end;
	}
}

#define set_ptes set_ptes
static inline void set_ptes(struct mm_struct *mm, unsigned long addr,
			    pte_t *ptep, pte_t pte, int nr)
{
	/*
	 * Basically the default implementation.
	 *
	 * Bugfix vs the version in commit bcf3d957c63d ("um: refactor TLB
	 * update handling"): the original advance formula was
	 *   pte = __pte(pte_val(pte) + (nr << PFN_PTE_SHIFT));
	 * but `nr` has just been decremented, so for nr_in >= 3 each
	 * subsequent PTE picks up an extra (nr_remaining-1) pages of
	 * PFN drift. For nr_in=3 the installed PFNs are P, P+2, P+3
	 * instead of P, P+1, P+2; for nr_in=4 they are P, P+3, P+5,
	 * P+6. Fix matches include/linux/pgtable.h's generic set_ptes
	 * which advances by exactly one page per iteration via
	 * pte_next_pfn().
	 *
	 * Memo 15 / direct shadow sync: in addition to the existing
	 * NEEDSYNC + um_tlb_mark_sync deferred chain (which non-KVM
	 * backends still depend on), call kvm_shadow_sync_pte for
	 * each PTE. On builds without integrated KVM the call is an
	 * inline no-op. On integrated-KVM builds the shadow leaf is
	 * updated synchronously to match the new UML PTE — closing
	 * the producer/consumer split that let the deferred chain
	 * leave stale shadow leaves.
	 */
	size_t length = nr * PAGE_SIZE;
	unsigned long sync_addr = addr;
	pte_t sync_pte = pte;
	pte_t *sync_ptep = ptep;
	int sync_nr = nr;

	for (;;) {
		set_pte(ptep, pte);
		if (--nr == 0)
			break;
		ptep++;
		pte = __pte(pte_val(pte) + PAGE_SIZE);
	}

	um_tlb_mark_sync(mm, addr, addr + length);

	/*
	 * Direct shadow sync after set_pte loop. Doing this after
	 * the loop (not interleaved) keeps the existing PTE-write
	 * sequence atomic w.r.t. host-side observers; the shadow
	 * sync runs on the now-final pgd state.
	 */
	for (; sync_nr > 0; sync_nr--, sync_ptep++,
	     sync_addr += PAGE_SIZE,
	     sync_pte = __pte(pte_val(sync_pte) + PAGE_SIZE)) {
		(void)kvm_shadow_sync_pte(mm, sync_addr, *sync_ptep);
	}
}

#define __HAVE_ARCH_PTE_SAME
static inline int pte_same(pte_t pte_a, pte_t pte_b)
{
	return !((pte_val(pte_a) ^ pte_val(pte_b)) & ~_PAGE_NEEDSYNC);
}

#define __virt_to_page(virt) phys_to_page(__pa(virt))
#define virt_to_page(addr) __virt_to_page((const unsigned long) addr)

static inline pte_t pfn_pte(unsigned long pfn, pgprot_t pgprot)
{
	pte_t pte;

	pte_set_val(pte, pfn_to_phys(pfn), pgprot);

	return pte;
}

static inline pte_t pte_modify(pte_t pte, pgprot_t newprot)
{
	pte_set_val(pte, (pte_val(pte) & _PAGE_CHG_MASK), newprot);
	return pte;
}

/*
 * the pmd page can be thought of an array like this: pmd_t[PTRS_PER_PMD]
 *
 * this macro returns the index of the entry in the pmd page which would
 * control the given virtual address
 */
#define pmd_page_vaddr(pmd) ((unsigned long) __va(pmd_val(pmd) & PAGE_MASK))

struct mm_struct;
extern pte_t *virt_to_pte(struct mm_struct *mm, unsigned long addr);

#define update_mmu_cache(vma,address,ptep) do {} while (0)
#define update_mmu_cache_range(vmf, vma, address, ptep, nr) do {} while (0)

/*
 * Encode/decode swap entries and swap PTEs. Swap PTEs are all PTEs that
 * are !pte_none() && !pte_present().
 *
 * Format of swap PTEs:
 *
 *   3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1
 *   1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 *   <--------------- offset ----------------> E < type -> 0 0 0 1 0
 *
 *   E is the exclusive marker that is not stored in swap entries.
 *   _PAGE_NEEDSYNC (bit 1) is always set to 1 in set_pte().
 */
#define __swp_type(x)			(((x).val >> 5) & 0x1f)
#define __swp_offset(x)			((x).val >> 11)

#define __swp_entry(type, offset) \
	((swp_entry_t) { (((type) & 0x1f) << 5) | ((offset) << 11) })
#define __pte_to_swp_entry(pte) \
	((swp_entry_t) { pte_val(pte_mkuptodate(pte)) })
#define __swp_entry_to_pte(x)		((pte_t) { (x).val })

static inline bool pte_swp_exclusive(pte_t pte)
{
	return pte_get_bits(pte, _PAGE_SWP_EXCLUSIVE);
}

static inline pte_t pte_swp_mkexclusive(pte_t pte)
{
	pte_set_bits(pte, _PAGE_SWP_EXCLUSIVE);
	return pte;
}

static inline pte_t pte_swp_clear_exclusive(pte_t pte)
{
	pte_clear_bits(pte, _PAGE_SWP_EXCLUSIVE);
	return pte;
}

#endif
