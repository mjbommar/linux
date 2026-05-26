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

	/*
	 * Phase G.2-fix (2026-05-01): per-mm guest-TLB generation
	 * counter, mirroring v1-archive's shadow_mm->tlb_gen pattern
	 * (kvm-v1-archive/kvm_backend.h:618). Incremented by
	 * um_tlb_sync after a successful PTE drain. Compared at each
	 * vCPU's dispatch against tlb_gen_seen_by[cpu]; on mismatch
	 * the vCPU does its CR4.PGE-toggle guest-TLB flush and
	 * updates its slot.
	 *
	 * Also used by kvm-v2's tlb_kick_others to TARGET cross-vCPU
	 * IPIs: only kick vCPUs whose tlb_gen_seen_by[cpu] is stale
	 * relative to THIS mm's tlb_gen. That narrowing differentiates
	 * this from commit C (broadcast IPI storm) and 9f0ff6257e8b
	 * (cmpxchg-dedup'd broadcast — still too eager under high
	 * mm-churn).
	 *
	 * tlb_gen_seen_by is per-(mm, cpu) — the previous per-vCPU
	 * last_seen_tlb_gen was cross-mm-contaminated: dispatching
	 * mm_B (gen=5) after mm_A (gen=3000) wrote 5 into the single
	 * counter, so returning to mm_A showed lag=2995 (false) and
	 * could also fool the kicker into skipping needed IPIs.
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
