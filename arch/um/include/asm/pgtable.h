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

/*
 * UML PTE bit layout - aligned with x86 hardware paging (memo 26 Phase E.3.6).
 *
 * Before this change, UML's _PAGE_* assignments were software-only book-
 * keeping: bits picked arbitrarily relative to x86 hardware. With the v2
 * KVM backend (which hands UML's pgd directly to the CPU as the guest CR3)
 * those positions collide with x86 paging semantics:
 *
 *   - UML _PAGE_RW=0x020 sat at x86 bit 5, which the CPU reads as A.
 *   - UML _PAGE_USER=0x040 sat at x86 bit 6, which on a leaf is D and on
 *     a non-leaf is ignored. The value never quite matched.
 *   - UML _PAGE_ACCESSED=0x080 sat at x86 bit 7, which on a leaf is the
 *     PS-bit (2MB/1GB large page). CPU walking a UML pgd would mistake
 *     a young 4K leaf for a large page, causing silent map corruption.
 *   - UML _PAGE_DIRTY=0x100 sat at x86 bit 8, the G-bit on leaves and
 *     ignored on non-leaves; mostly benign but still a value mismatch.
 *
 * Bits 0..6 now match x86 architectural PTE positions. The software-only
 * bits (NEEDSYNC, PROTNONE, SWP_EXCLUSIVE) move into the AVL window
 * (bits 9..11) where x86 ignores them entirely on hardware walks.
 *
 *   bit  0  P    _PAGE_PRESENT       (matches x86)
 *   bit  1  R/W  _PAGE_RW            (was 0x020)
 *   bit  2  U/S  _PAGE_USER          (was 0x040)
 *   bit  3  PWT  -                   (reserved for x86 cacheability)
 *   bit  4  PCD  -                   (reserved for x86 cacheability)
 *   bit  5  A    _PAGE_ACCESSED      (was 0x080)
 *   bit  6  D    _PAGE_DIRTY         (was 0x100; leaf-only on x86)
 *   bit  7  PS   -                   (large page; UML always 4K leaves)
 *   bit  8  G    -                   (global; not used by UML)
 *   bit  9  AVL  _PAGE_NEEDSYNC      (sw-only; was 0x002)
 *   bit 10  AVL  _PAGE_PROTNONE      (sw-only; was 0x010, only if P=0)
 *   bit 11  AVL  _PAGE_SWP_EXCLUSIVE (sw-only; was 0x400, swap PTEs only)
 *
 * Consequences:
 *
 * - kernel-core code (tlb.c, trap.c, mmu.c, mem.c, skas/uaccess.c) goes
 *   through the named pte_/pmd_/p4d_ accessors and is bit-position-
 *   agnostic; no source change required outside this file.
 * - The seccomp backend never references _PAGE_ macros directly: it is
 *   unaffected by this change.
 * - The KVM v2 backend can now pass __pa(active_mm->pgd) to KVM as CR3
 *   without any shadow-PT translation: UML's leaf entries are valid x86
 *   PTEs by construction.
 * - The swap-PTE encoding moves: type and offset fields can no longer
 *   overlap _PAGE_NEEDSYNC at bit 9. See __swp_type / __swp_offset and
 *   the format comment near the bottom of this file.
 */
#define _PAGE_PRESENT	0x001	/* x86 P   (bit 0) */
#define _PAGE_RW	0x002	/* x86 R/W (bit 1) */
#define _PAGE_USER	0x004	/* x86 U/S (bit 2) */
#define _PAGE_ACCESSED	0x020	/* x86 A   (bit 5) */
#define _PAGE_DIRTY	0x040	/* x86 D   (bit 6, leaf only) */

/* Software-only bits in the x86 AVL window (bits 9..11). x86 hardware
 * never inspects these on a walk, so KVM-v2 can hand UML's pgd straight
 * to the CPU even with these set on present entries.
 */
#define _PAGE_NEEDSYNC		0x200	/* AVL bit 9; cleared at sync time */
#define _PAGE_PROTNONE		0x400	/* AVL bit 10; only valid when P=0
					 * (hw treats P=0 entries as not
					 * present, sw uses PROTNONE to keep
					 * pte_present() returning true).
					 */
/* Bit 11 (AVL) stores the exclusive marker on swap PTEs (which have P=0). */
#define _PAGE_SWP_EXCLUSIVE	0x800

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

#define pte_clear(mm, addr, xp) \
	pte_set_val(*(xp), (phys_t)0, __pgprot(_PAGE_NEEDSYNC))

#define pmd_none(x)	(!((unsigned long)pmd_val(x) & ~_PAGE_NEEDSYNC))
#define	pmd_bad(x)	((pmd_val(x) & (~PAGE_MASK & ~_PAGE_USER)) != _KERNPG_TABLE)

#define pmd_present(x)	(pmd_val(x) & _PAGE_PRESENT)

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
	return !pte_get_bits(pte, _PAGE_PROTNONE);
}

static inline int pte_exec(pte_t pte)
{
	return !pte_get_bits(pte, _PAGE_PROTNONE);
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
	/*
	 * Publish the final PTE value (with _PAGE_NEEDSYNC set) in a
	 * SINGLE store. The previous shape did two stores —
	 *   pte_copy(*pteptr, pteval);
	 *   *pteptr = pte_mkneedsync(*pteptr);
	 * — leaving a transient window where another reader of the PTE
	 * (KVM TDP walker on a different host CPU during v2's mt-mmap-
	 * stress workload, concurrent um_tlb_sync on a sibling thread
	 * of the same mm, or hardware A/D-bit update) could observe
	 * PRESENT-without-NEEDSYNC, and the second store's read-modify-
	 * write could clobber any concurrent update.
	 *
	 * Also marks _PAGE_NEEDSYNC on swap entries so update_pte_range
	 * knows to unmap them.
	 *
	 * Caught by tools/testing/selftests/um/mt-mmap-stress (3 pthreads
	 * × 100 iters of mmap+memset+munmap) which fails ~100% under v2
	 * with the previous two-store form — post-memset readback returns
	 * bytes from a sibling thread or stale physmem because PTE
	 * publication interleaved with KVM's TDP walks.
	 *
	 * Seccomp doesn't have a hardware page-table consumer (the stub
	 * child uses host syscalls, not direct PT walks), so the two-
	 * store window is harmless there. v2's KVM TDP walker reads
	 * UML's pgd as part of every guest VA → host PA translation,
	 * so the transient state escapes to KVM's caches.
	 *
	 * MAP_POPULATE workaround masks the bug by resolving anon faults
	 * eagerly under mmap_write_lock — no concurrent lazy-PF
	 * publication, no transient window.
	 *
	 * Single-store fix attributed to codex (gpt-5.5 xhigh) audit
	 * 2026-04-30, memo §H.1b.
	 */
	pte_copy(*pteptr, pte_mkneedsync(pteval));
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
	 * Bugfix vs the version in commit bcf3d957c63d ("um: refactor TLB
	 * update handling"): the original advance formula was
	 *   pte = __pte(pte_val(pte) + (nr << PFN_PTE_SHIFT));
	 * but `nr` has just been decremented, so for nr_in >= 3 each
	 * subsequent PTE picks up an extra (nr_remaining-1) pages of
	 * PFN drift. Fix matches include/linux/pgtable.h's generic
	 * set_ptes which advances by exactly one page per iteration
	 * via pte_next_pfn().
	 */
	size_t length = nr * PAGE_SIZE;

	for (;;) {
		set_pte(ptep, pte);
		if (--nr == 0)
			break;
		ptep++;
		pte = __pte(pte_val(pte) + PAGE_SIZE);
	}

	um_tlb_mark_sync(mm, addr, addr + length);
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
 * Format of swap PTEs (post memo 26 Phase E.3.6 x86-aligned bit layout):
 *
 *   6 6 5 5 5 5 5 5 5 5 5 5 4 4         1 1 1 1 1 1 1 1 1 1
 *   3 2 1 0 9 8 7 6 5 4 3 2 1 0 ...     1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 *   <----------------- offset ----------------> E S 0 0 0 0 0 < type > 0
 *
 *   bit  0     = _PAGE_PRESENT = 0  (this is a swap entry, not a present PTE)
 *   bits 1..5  = swap type (5 bits → 32 types; matches the old encoding)
 *   bits 6..8  = 0 (UML never sets D / PS / G on swap entries)
 *   bit  9 (S) = _PAGE_NEEDSYNC = 1 (set by set_pte() so the next sync drains
 *                                    the swap-out)
 *   bit 10     = _PAGE_PROTNONE = 0 (swap entries must NOT look like
 *                                    pte_present)
 *   bit 11 (E) = _PAGE_SWP_EXCLUSIVE
 *   bits 12+   = offset
 */
#define __swp_type(x)			(((x).val >> 1) & 0x1f)
#define __swp_offset(x)			((x).val >> 12)

#define __swp_entry(type, offset) \
	((swp_entry_t) { (((type) & 0x1f) << 1) | ((offset) << 12) })
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
