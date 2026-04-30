/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __UM_TLBFLUSH_H
#define __UM_TLBFLUSH_H

#include <linux/mm.h>

/*
 * UML's TLB-sync contract (memo 25 R3 — backend decoupled).
 *
 * UML "syncs the TLB" by reaching out from the kernel into the
 * stub-child process (or, under v2 KVM, the per-mm worker process)
 * and replaying mmap/munmap syscalls so the host VA layout matches
 * the guest pgd. The set_ptes + flush_tlb_* call sites mark the
 * affected VA range via um_tlb_mark_sync(); um_tlb_sync() is the
 * single drain point that walks the pgd, computes per-PTE
 * (prot, fd, offset) tuples, and dispatches to the backend's
 * mm_region_added / mm_region_removed ops.
 *
 * Per memo 25 R3, this layer carries NO backend-specific knowledge:
 * pte_clear / set_pte / set_ptes / pmd_clear etc. mark VAs as
 * needing sync; they never call a backend op directly. Backends
 * see range notifications at drain time, never per-PTE writes.
 * (v1's kvm_shadow_sync_pte hook-everywhere model — a major source
 * of race classes — was stripped in memo 25 Step 3.)
 *
 * The only special case is that flush_tlb_kernel_range() drains
 * immediately (kernel-VA mappings have no later synchronization
 * point); user-mm flushes wait for the next set_pte / segfault /
 * task switch before draining.
 *
 *  - flush_tlb_all() flushes all processes TLBs
 *  - flush_tlb_mm(mm) flushes the specified mm context TLB's
 *  - flush_tlb_page(vma, vmaddr) flushes one page
 *  - flush_tlb_range(vma, start, end) flushes a range of pages
 *  - flush_tlb_kernel_range(start, end) flushes a range of kernel pages
 */

extern int um_tlb_sync(struct mm_struct *mm);

extern void flush_tlb_all(void);
extern void flush_tlb_mm(struct mm_struct *mm);

/*
 * Memo §H.1b residual fix: deferred free for mmu_gather pages.
 * mmu_gather's tlb_batch_pages_flush calls um_mmu_gather_defer
 * to hand pages off to the per-mm deferred queue; the active
 * backend's vcpu_run calls um_mmu_gather_drain after KVM_RUN's
 * CR4.PGE flush has executed, freeing the deferred pages safely.
 */
struct encoded_page;
unsigned int um_mmu_gather_defer(struct mm_struct *mm,
				 struct encoded_page **encoded,
				 unsigned int nr);
void um_mmu_gather_drain(struct mm_struct *mm);

static inline void flush_tlb_page(struct vm_area_struct *vma,
				  unsigned long address)
{
	um_tlb_mark_sync(vma->vm_mm, address, address + PAGE_SIZE);
}

static inline void flush_tlb_range(struct vm_area_struct *vma,
				   unsigned long start, unsigned long end)
{
	um_tlb_mark_sync(vma->vm_mm, start, end);
}

static inline void flush_tlb_kernel_range(unsigned long start,
					  unsigned long end)
{
	um_tlb_mark_sync(&init_mm, start, end);

	/* Kernel needs to be synced immediately */
	um_tlb_sync(&init_mm);
}

#endif
