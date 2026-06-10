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
	 * Per-mm host worker process handle.
	 * Always present (cost of a NULL pointer is negligible).
	 * Under CONFIG_UM_WORKER_PROCESS=y this points at an opaque
	 * struct um_worker owned by arch/um/os-Linux/spawner.c; under
	 * =n it stays NULL and seccomp's mm_create falls back to the
	 * stub-child-in-spawner path.
	 */
	struct um_worker *worker;

	/*
	 * UML's deferred TLB-sync model means a host page can be returned
	 * to the allocator before the guest TLB flush has actually run.
	 * flush_tlb_range() only marks sync_tlb_range_*; the actual guest
	 * flush happens at the next vcpu_run dispatch. Keep the physical
	 * pages pinned until that dispatch so slab cannot reuse a freed PFN
	 * while a guest CPU still has a stale user-half translation.
	 *
	 * UML takes ownership of the encoded_page array from mmu_gather
	 * instead of calling free_pages_and_swap_cache. It holds an extra
	 * ref on each page, queues the pages here, and drops the refs at
	 * the start of vcpu_run after the previous dispatch has flushed the
	 * guest TLB.
	 */
	spinlock_t deferred_free_lock;
	struct list_head deferred_free_pages;
	unsigned int deferred_free_count;

	/*
	 * Per-mm guest-TLB generation counter. Incremented by um_tlb_sync
	 * after a successful PTE drain. Compared at each vCPU's dispatch
	 * against tlb_gen_seen_by[cpu]; on mismatch the vCPU does its
	 * CR4.PGE-toggle guest-TLB flush and updates its slot.
	 *
	 * Also used by kvm-v2's tlb_kick_others to target cross-vCPU
	 * IPIs: only kick vCPUs whose tlb_gen_seen_by[cpu] is stale
	 * relative to this mm's tlb_gen, avoiding broadcast IPIs under
	 * high mm churn.
	 *
	 * tlb_gen_seen_by is per-(mm, cpu); a per-vCPU
	 * last_seen_tlb_gen was cross-mm-contaminated: dispatching
	 * mm_B (gen=5) after mm_A (gen=3000) wrote 5 into the shared
	 * counter, so returning to mm_A showed a false lag and could also
	 * fool the kicker into skipping needed IPIs.
	 */
	atomic64_t tlb_gen;
	atomic64_t tlb_gen_seen_by[NR_CPUS];
} mm_context_t;

#define INIT_MM_CONTEXT(mm)						\
	.context = {							\
		.turnstile = __MUTEX_INITIALIZER(mm.context.turnstile),	\
		.sync_tlb_lock = __SPIN_LOCK_INITIALIZER(mm.context.sync_tlb_lock), \
		.deferred_free_lock = __SPIN_LOCK_INITIALIZER(mm.context.deferred_free_lock), \
		.deferred_free_pages = LIST_HEAD_INIT(mm.context.deferred_free_pages), \
	}

#endif
