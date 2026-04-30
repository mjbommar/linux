/* SPDX-License-Identifier: GPL-2.0 */
/* 
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __ARCH_UM_MMU_H
#define __ARCH_UM_MMU_H

#include "linux/types.h"
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <mm_id.h>

struct um_worker;

typedef struct mm_context {
	struct mm_id id;
	struct mutex turnstile;

	struct list_head list;

	/* Address range in need of a TLB sync */
	spinlock_t sync_tlb_lock;
	unsigned long sync_tlb_range_from;
	unsigned long sync_tlb_range_to;

	/*
	 * Per-mm host worker process handle (memo 25 R4 / memo 28).
	 * Always present (cost of a NULL pointer is negligible).
	 * Under CONFIG_UM_WORKER_PROCESS=y this points at an opaque
	 * struct um_worker owned by arch/um/os-Linux/spawner.c; under
	 * =n it stays NULL and seccomp's mm_create falls back to
	 * today's stub-child-in-spawner path. The runtime split lands
	 * when memo 28's E.6 commit flips the defconfig toggle.
	 */
	struct um_worker *worker;

	/*
	 * Memo §H.1b residual fix: mmu_gather free-after-flush gap.
	 *
	 * UML's deferred TLB-sync model violates mmu_gather's contract
	 * that the TLB flush completes BEFORE the page is returned to
	 * buddy. flush_tlb_range only marks (sync_tlb_range_*); the
	 * actual flush is the CR4.PGE toggle at next vcpu_run dispatch
	 * (kvm-v2/vcpu.c:1148). tlb_batch_pages_flush(mm/mmu_gather.c)
	 * frees pages BEFORE that flush — kernel slab can take a freed
	 * PFN and write data while the guest CPU's user-half TLB still
	 * has a stale translation pointing at it.
	 *
	 * Fix: under UML, mmu_gather hands the encoded_page array to
	 * us instead of calling free_pages_and_swap_cache. We hold an
	 * extra ref on each (preventing buddy reuse), queue them on
	 * deferred_free, and call put_page at the start of vcpu_run —
	 * which is AFTER the previous dispatch's CR4.PGE flush has
	 * already executed inside KVM_RUN.
	 *
	 * Confirmed via mm/mmu_gather.c:tlb_batch_pages_flush printk
	 * (memo §H.1b ROOT-CAUSED entry, 2026-04-30).
	 */
	spinlock_t deferred_free_lock;
	struct list_head deferred_free_pages;
	unsigned int deferred_free_count;
} mm_context_t;

#define INIT_MM_CONTEXT(mm)						\
	.context = {							\
		.turnstile = __MUTEX_INITIALIZER(mm.context.turnstile),	\
		.sync_tlb_lock = __SPIN_LOCK_INITIALIZER(mm.context.sync_tlb_lock), \
		.deferred_free_lock = __SPIN_LOCK_INITIALIZER(mm.context.deferred_free_lock), \
		.deferred_free_pages = LIST_HEAD_INIT(mm.context.deferred_free_pages), \
	}

#endif
