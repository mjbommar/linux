// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched/signal.h>

#include <asm/backend.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>
#include <as-layout.h>
#include <mem_user.h>
#include <os.h>
#include <skas.h>
#include <kern_util.h>

/*
 * Memo 25 R2: vm_ops carries `struct mm_struct *` (or NULL for the
 * init_mm kernel direct-map path) so the backend ops it holds match
 * the new um_backend_ops contract. The function-pointer types match
 * `mm_region_added` / `mm_region_removed` exactly, so update_*_range
 * helpers can store the two HOT ops verbatim.
 */
struct vm_ops {
	struct mm_struct *mm;

	int (*mmap)(struct mm_struct *mm,
		    unsigned long virt, unsigned long len, int prot,
		    int phys_fd, u64 offset);
	int (*unmap)(struct mm_struct *mm,
		     unsigned long virt, unsigned long len);
};

static int kern_map(struct mm_struct *mm,
		    unsigned long virt, unsigned long len, int prot,
		    int phys_fd, u64 offset)
{
	/* TODO: Why is executable needed to be always set in the kernel? */
	return os_map_memory((void *)virt, phys_fd, offset, len,
			     prot & UM_PROT_READ, prot & UM_PROT_WRITE,
			     1);
}

static int kern_unmap(struct mm_struct *mm,
		      unsigned long virt, unsigned long len)
{
	return os_unmap_memory((void *)virt, len);
}

void report_enomem(void)
{
	printk(KERN_ERR "UML ran out of memory on the host side! "
			"This can happen due to a memory limitation or "
			"vm.max_map_count has been reached.\n");
}

static inline int update_pte_range(pmd_t *pmd, unsigned long addr,
				   unsigned long end,
				   struct vm_ops *ops)
{
	pte_t *pte;
	int ret = 0;

	pte = pte_offset_kernel(pmd, addr);
	do {
		if (!pte_needsync(*pte))
			continue;

		if (pte_present(*pte)) {
			__u64 offset;
			unsigned long phys = pte_val(*pte) & PAGE_MASK;
			int fd = phys_mapping(phys, &offset);
			int r, w, x, prot;

			r = pte_read(*pte);
			w = pte_write(*pte);
			x = pte_exec(*pte);
			if (!pte_young(*pte)) {
				r = 0;
				w = 0;
			} else if (!pte_dirty(*pte))
				w = 0;

			prot = (r ? UM_PROT_READ : 0) |
			       (w ? UM_PROT_WRITE : 0) |
			       (x ? UM_PROT_EXEC : 0);

			ret = ops->mmap(ops->mm, addr, PAGE_SIZE,
					prot, fd, offset);
		} else
			ret = ops->unmap(ops->mm, addr, PAGE_SIZE);

		/*
		 * Only mark the PTE uptodate if the backend op succeeded.
		 * Otherwise the next sync would skip this PTE thinking it
		 * was already synced — leaving the host VA mapping (and,
		 * under integrated KVM, the shadow PT) divergent from the
		 * pgd permanently.
		 */
		if (!ret)
			*pte = pte_mkuptodate(*pte);
	} while (pte++, addr += PAGE_SIZE, ((addr < end) && !ret));
	return ret;
}

static inline int update_pmd_range(pud_t *pud, unsigned long addr,
				   unsigned long end,
				   struct vm_ops *ops)
{
	pmd_t *pmd;
	unsigned long next;
	int ret = 0;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (!pmd_present(*pmd)) {
			if (pmd_needsync(*pmd)) {
				ret = ops->unmap(ops->mm, addr,
						 next - addr);
				if (!ret)
					pmd_mkuptodate(*pmd);
			}
		}
		else ret = update_pte_range(pmd, addr, next, ops);
	} while (pmd++, addr = next, ((addr < end) && !ret));
	return ret;
}

static inline int update_pud_range(p4d_t *p4d, unsigned long addr,
				   unsigned long end,
				   struct vm_ops *ops)
{
	pud_t *pud;
	unsigned long next;
	int ret = 0;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (!pud_present(*pud)) {
			if (pud_needsync(*pud)) {
				ret = ops->unmap(ops->mm, addr,
						 next - addr);
				if (!ret)
					pud_mkuptodate(*pud);
			}
		}
		else ret = update_pmd_range(pud, addr, next, ops);
	} while (pud++, addr = next, ((addr < end) && !ret));
	return ret;
}

static inline int update_p4d_range(pgd_t *pgd, unsigned long addr,
				   unsigned long end,
				   struct vm_ops *ops)
{
	p4d_t *p4d;
	unsigned long next;
	int ret = 0;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (!p4d_present(*p4d)) {
			if (p4d_needsync(*p4d)) {
				ret = ops->unmap(ops->mm, addr,
						 next - addr);
				if (!ret)
					p4d_mkuptodate(*p4d);
			}
		} else
			ret = update_pud_range(p4d, addr, next, ops);
	} while (p4d++, addr = next, ((addr < end) && !ret));
	return ret;
}

int um_tlb_sync(struct mm_struct *mm)
{
	pgd_t *pgd;
	struct vm_ops ops;
	unsigned long addr, next;
	int ret = 0;

	guard(spinlock_irqsave)(&mm->context.sync_tlb_lock);

	if (mm->context.sync_tlb_range_to == 0)
		return 0;

	ops.mm = mm;
	if (mm == &init_mm) {
		ops.mmap = kern_map;
		ops.unmap = kern_unmap;
	} else {
		/*
		 * User-mm sync goes through the active backend. The
		 * function-pointer load is one-shot per um_tlb_sync()
		 * call; the inner update_*_range() helpers continue to
		 * indirect through ops.{mmap,unmap} as before.
		 *
		 * Memo 25 R2: the HOT memory ops are now mm_region_added /
		 * mm_region_removed, taking struct mm_struct * directly.
		 */
		ops.mmap = um_backend->mm_region_added;
		ops.unmap = um_backend->mm_region_removed;
	}

	addr = mm->context.sync_tlb_range_from;
	pgd = pgd_offset(mm, addr);
	do {
		next = pgd_addr_end(addr, mm->context.sync_tlb_range_to);
		if (!pgd_present(*pgd)) {
			if (pgd_needsync(*pgd)) {
				ret = ops.unmap(ops.mm, addr,
						next - addr);
				/*
				 * #274 issue #12: gate pgd_mkuptodate on
				 * success, mirroring the per-PTE/PMD/PUD/P4D
				 * fix. An -ENOMEM or backend invalidate
				 * failure here would otherwise leave the
				 * pgd entry marked synced even though the
				 * unmap didn't actually happen.
				 */
				if (!ret)
					pgd_mkuptodate(*pgd);
			}
		} else
			ret = update_p4d_range(pgd, addr, next, &ops);
	} while (pgd++, addr = next,
		 ((addr < mm->context.sync_tlb_range_to) && !ret));

	if (ret == -ENOMEM)
		report_enomem();

	/*
	 * #274 issue #11: only clear the pending sync range on
	 * SUCCESS. If a backend op failed mid-sweep, we left some
	 * portion of the range un-drained — clearing the from/to
	 * fields here would lose that pending work and the next
	 * sync would think there's nothing to do, leaving the host
	 * VA / shadow PT divergent from the pgd permanently.
	 *
	 * On failure, narrow the pending range to start at `addr`
	 * (the first VA we did NOT successfully drain) so the next
	 * sync resumes from there. The remaining un-drained range
	 * is [addr, sync_tlb_range_to). On success addr equals
	 * sync_tlb_range_to (the loop exited via the addr<end
	 * condition with addr having advanced to end), so the
	 * narrowing is a no-op-equivalent clear.
	 */
	if (ret == 0) {
		mm->context.sync_tlb_range_from = 0;
		mm->context.sync_tlb_range_to = 0;
	} else {
		mm->context.sync_tlb_range_from = addr;
	}

	return ret;
}

void flush_tlb_all(void)
{
	/*
	 * Don't bother flushing if this address space is about to be
	 * destroyed.
	 */
	if (atomic_read(&current->mm->mm_users) == 0)
		return;

	flush_tlb_mm(current->mm);
}

void flush_tlb_mm(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);

	for_each_vma(vmi, vma)
		um_tlb_mark_sync(mm, vma->vm_start, vma->vm_end);
}
