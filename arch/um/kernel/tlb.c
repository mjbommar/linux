// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 *
 * UML mm-arbiter: TLB-sync drain orchestration (memo 25 R8).
 *
 * Contract (post memo 25 R3 / R5 / R8):
 *
 * - This layer carries NO backend-specific knowledge. The drain
 *   loop dispatches through um_backend->mm_region_added /
 *   mm_region_removed for user mms (via the vm_ops table loaded
 *   once at the top of um_tlb_sync), or kern_map / kern_unmap
 *   for init_mm.
 *
 * - The "deferred sync queue" today is the compact (from, to)
 *   range stashed in struct mm_context::sync_tlb_range_{from,to}
 *   and updated by um_tlb_mark_sync() (called from set_pte,
 *   set_ptes, flush_tlb_*, etc.). At drain time, um_tlb_sync()
 *   walks the per-mm pgd over [from, to), inspects pte_needsync()
 *   on each leaf, and emits a struct um_memory_region per
 *   needsync PTE. Equivalent to "queue of regions" but stored as
 *   bounds + on-demand pgd walk.
 *
 * - Backends see only struct um_memory_region values — never
 *   struct mm_id (memo 25 R2 cleanup), never per-PTE callbacks
 *   (memo 25 R3 + Step 3 strip), never v1's shadow_sync_pte
 *   hook chain (gone since memo 25 Step 3, b19444243944).
 *
 * Future (memo 26 Phase B): when v2's per-mapping memslot path
 * lands, a higher-level mmap-time notification at the vma layer
 * may augment this drain loop so v2 sees one mm_region_added per
 * user mmap (vs today's per-page emission). The drain orchestration
 * itself stays — that path remains the synchronous "force backend
 * to catch up before the next access" point.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/swap.h>

#include <asm/backend.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>
#include <asm/trace/um_backend.h>
#include <asm/um_memory.h>
#include <as-layout.h>
#include <mem_user.h>
#include <os.h>
#include <skas.h>
#include <kern_util.h>

/*
 * Memo §H.1b residual fix: deferred-free queue.
 *
 * UML's flush_tlb_range only marks (sync_tlb_range_*); the actual
 * guest-TLB flush is the CR4.PGE toggle on the next vcpu_run
 * dispatch (kvm-v2/vcpu.c:1148). mmu_gather's tlb_batch_pages_flush
 * (mm/mmu_gather.c) frees pages back to buddy BEFORE that flush —
 * which violates the standard mm contract and exposes a window
 * where kernel slab can take a freed PFN and write data while the
 * guest CPU's user-half TLB still has stale translations to it.
 *
 * Diagnostic confirmed (memo §H.1b ROOT-CAUSED, 2026-04-30): under
 * mt-mmap-stress 3T×50 iters, the failing PFN appears in
 * tlb_batch_pages_flush with sync_tlb_range_to non-empty.
 *
 * Fix shape: instead of letting mmu_gather call
 * free_pages_and_swap_cache, we transfer the batch's encoded_page
 * entries into per-mm deferred_free batches. We do NOT touch
 * refcounts here — the encoded_page array's "ownership" of each
 * page (the ref that mmu_gather holds during the unmap) is
 * preserved by transferring the entries. At drain time (next
 * vcpu_run, after KVM_RUN's CR4.PGE flush has executed), we call
 * free_pages_and_swap_cache on the deferred batches — the same
 * operation mmu_gather would have done, just deferred until the
 * TLB has actually been flushed.
 *
 * Each um_defer_batch holds up to UM_DEFER_BATCH_NR encoded_page
 * entries. Sized to fit comfortably in a kmalloc-1024 slab and
 * cover a typical 64KB unmap (16 pages) in a single batch.
 */
#define UM_DEFER_BATCH_NR	120	/* 8*120 + 16 = 976 bytes */

struct um_defer_batch {
	struct list_head list;
	struct rcu_head rcu;	/* SMP-T20: RCU-deferred free (memo §SMP-T20) */
	unsigned int nr;
	struct encoded_page *pages[UM_DEFER_BATCH_NR];
};

/*
 * Append a single encoded_page entry to mm->context.deferred_free_pages.
 * Caller holds context.deferred_free_lock.
 *
 * The encoded_page array's "ownership" of the page (the ref that
 * mmu_gather held during the unmap) must be PRESERVED in the deferred
 * list — we transfer that ref here without taking a new one. The
 * caller then sets batch->nr to skip the matching entry in
 * __tlb_batch_free_encoded_pages, so the standard release_pages
 * decrement does NOT run on this entry. The deferred drain calls
 * free_pages_and_swap_cache to do that decrement at the safe time.
 *
 * Returns 0 on success; on kmalloc failure returns -ENOMEM and the
 * caller MUST fall back to the immediate-free path (otherwise we'd
 * leak the ref).
 */
static int __um_defer_append_locked(struct mm_context *ctx,
				    struct encoded_page *enc)
{
	struct um_defer_batch *b;

	if (!list_empty(&ctx->deferred_free_pages)) {
		b = list_last_entry(&ctx->deferred_free_pages,
				    struct um_defer_batch, list);
		if (b->nr < UM_DEFER_BATCH_NR) {
			b->pages[b->nr++] = enc;
			ctx->deferred_free_count++;
			return 0;
		}
	}

	b = kmalloc(sizeof(*b), GFP_ATOMIC);
	if (!b)
		return -ENOMEM;

	b->nr = 1;
	b->pages[0] = enc;
	list_add_tail(&b->list, &ctx->deferred_free_pages);
	ctx->deferred_free_count++;
	return 0;
}

/*
 * Hand mmu_gather's encoded_page array off to the per-mm deferred
 * queue. Returns the number of entries successfully deferred; the
 * caller frees the rest immediately. Called from
 * tlb_batch_pages_flush in mm/mmu_gather.c.
 *
 * On entry, batch->encoded_pages[0..batch->nr) holds the entries.
 * On full success we set batch->nr = 0 (caller skips its own free).
 * On partial success (kmalloc OOM mid-batch), we set batch->nr to
 * the number of REMAINING entries shifted to the front of the
 * array — caller resumes the standard free flow on those.
 */
unsigned int um_mmu_gather_defer(struct mm_struct *mm,
				 struct encoded_page **encoded,
				 unsigned int nr)
{
	struct mm_context *ctx;
	unsigned long flags;
	unsigned int deferred = 0;
	unsigned int i;

	if (!mm || !nr)
		return 0;

	/*
	 * init_mm is special — it's the kernel's own mm and runs
	 * outside of vcpu_run (kernel-half VAs). The deferred-free
	 * mechanism only makes sense for user-half mappings draining
	 * before guest TLB flush. Init_mm pages get freed normally.
	 */
	if (mm == &init_mm)
		return 0;

	ctx = &mm->context;

	scoped_guard(spinlock_irqsave, &ctx->deferred_free_lock) {
		for (i = 0; i < nr; i++) {
			if (__um_defer_append_locked(ctx, encoded[i]) < 0) {
				/* kmalloc OOM mid-batch: stop deferring,
				 * shift remaining entries to front so caller
				 * frees them inline. */
				unsigned int rem = nr - i;
				memmove(&encoded[0], &encoded[i],
					rem * sizeof(encoded[0]));
				return deferred;
			}
			deferred++;
		}
	}
	return deferred;
}

/*
 * RCU callback: free a deferred batch after a full RCU grace
 * period has elapsed. By this point every CPU has passed through
 * a quiescent state (= every vCPU has exited and re-entered
 * KVM_RUN, which flushes guest TLB via CR4.PGE toggle), so any
 * stale guest-TLB entries pointing at these pages are gone.
 */
static void um_defer_batch_rcu_free(struct rcu_head *rh)
{
	struct um_defer_batch *b = container_of(rh, struct um_defer_batch, rcu);

	free_pages_and_swap_cache((struct encoded_page **)b->pages, b->nr);
	kfree(b);
}

/*
 * Drain the per-mm deferred-free queue. Called from the active
 * backend's vcpu_run AFTER KVM_RUN's CR4.PGE flush has executed
 * (so the GUEST TLB no longer caches stale translations to these
 * pages).
 *
 * SMP-T20 (2026-05-02): switched from immediate kfree to call_rcu.
 * Reason: the local-CR4.PGE flush at THIS dispatch's KVM_RUN
 * entry only flushes THIS vCPU's guest TLB. Other vCPUs running
 * tasks in the same mm may still cache stale GVA→guest-PA
 * translations to these pages until their own next dispatch.
 * Without a grace period, those stale TLB entries can alias the
 * recycled physical page, yielding the high-cr2 user faults seen
 * in fork-stress (Angle 2 / mt-mmap-stress flake class).
 *
 * call_rcu defers the actual free until every CPU has passed
 * through a quiescent state. The migrate_disable() in vcpu_run is
 * a preempt-disable section under PREEMPT=n, which is itself an
 * RCU read-side critical section — so RCU's grace period waits
 * for every running vCPU to exit KVM_RUN at least once before
 * firing the callback. By that point every vCPU has executed the
 * CR4.PGE toggle and flushed its guest TLB → safe to free.
 *
 * The local TLB-kick (tlb.c:um_tlb_sync) re-fired by SMP-T13 stays
 * in place; its job changes from "ensure flush before free" to
 * "ensure forward progress of the grace period under CPU-bound
 * guest workloads" — without it a vCPU spinning in guest user
 * code wouldn't reach a quiescent state until its next SIGALRM.
 *
 * Splices the queue under the lock so the registration runs
 * without holding the lock.
 */
void um_mmu_gather_drain(struct mm_struct *mm)
{
	struct mm_context *ctx;
	struct um_defer_batch *b, *tmp;
	LIST_HEAD(local);

	if (!mm || mm == &init_mm)
		return;

	ctx = &mm->context;

	scoped_guard(spinlock_irqsave, &ctx->deferred_free_lock) {
		if (list_empty(&ctx->deferred_free_pages))
			return;
		list_splice_init(&ctx->deferred_free_pages, &local);
		ctx->deferred_free_count = 0;
	}

	list_for_each_entry_safe(b, tmp, &local, list) {
		list_del(&b->list);
		call_rcu(&b->rcu, um_defer_batch_rcu_free);
	}
}

/*
 * vm_ops: per-drain dispatch table. Carries the mm pointer (or
 * NULL for init_mm) and the two HOT region ops as function
 * pointers. Loaded once at um_tlb_sync entry; the inner
 * update_*_range() helpers populate stack-local
 * struct um_memory_region descriptors and pass them through.
 */
struct vm_ops {
	struct mm_struct *mm;

	int (*mmap)(struct mm_struct *mm,
		    const struct um_memory_region *region);
	int (*unmap)(struct mm_struct *mm,
		     const struct um_memory_region *region);
};

static int kern_map(struct mm_struct *mm,
		    const struct um_memory_region *region)
{
	/* TODO: Why is executable needed to be always set in the kernel? */
	return os_map_memory((void *)region->va, region->phys_fd,
			     region->offset, region->len,
			     region->prot & UM_PROT_READ,
			     region->prot & UM_PROT_WRITE,
			     1);
}

static int kern_unmap(struct mm_struct *mm,
		      const struct um_memory_region *region)
{
	return os_unmap_memory((void *)region->va, region->len);
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
			struct um_memory_region region;

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

			region = (struct um_memory_region){
				.va = addr,
				.len = PAGE_SIZE,
				.prot = prot,
				.phys_fd = fd,
				.offset = offset,
			};
			trace_um_backend_mm_region_added(ops->mm, addr,
							 PAGE_SIZE, prot,
							 fd, offset);
			ret = ops->mmap(ops->mm, &region);
		} else {
			struct um_memory_region region = {
				.va = addr,
				.len = PAGE_SIZE,
				.phys_fd = -1,
			};
			trace_um_backend_mm_region_removed(ops->mm, addr,
							   PAGE_SIZE);
			ret = ops->unmap(ops->mm, &region);
		}

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
				struct um_memory_region region = {
					.va = addr,
					.len = next - addr,
					.phys_fd = -1,
				};
				ret = ops->unmap(ops->mm, &region);
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
				struct um_memory_region region = {
					.va = addr,
					.len = next - addr,
					.phys_fd = -1,
				};
				ret = ops->unmap(ops->mm, &region);
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
				struct um_memory_region region = {
					.va = addr,
					.len = next - addr,
					.phys_fd = -1,
				};
				ret = ops->unmap(ops->mm, &region);
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

	guard(spinlock_irqsave)(&mm->page_table_lock);
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
				struct um_memory_region region = {
					.va = addr,
					.len = next - addr,
					.phys_fd = -1,
				};
				ret = ops.unmap(ops.mm, &region);
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
	/*
	 * Phase G.2-fix (2026-05-01): bump per-mm tlb_gen so each
	 * vCPU's load_user_sregs can detect "drained-since-last-flush"
	 * and update its last_seen_tlb_gen. KICK ACTIVATION DEFERRED:
	 * even with v1-pattern narrowing (current_mm match + last_seen
	 * gen check + cmpxchg dedup), calling tlb_kick_others from here
	 * regressed mt-mini T=4/ncpus=4 60→42 and T=8 18→7. The IPI
	 * itself disrupts forward progress under high mm-churn,
	 * regardless of how aggressively it's targeted/dedup'd.
	 *
	 * Conclusion: T>=N stress flake is NOT cross-vCPU guest-TLB
	 * stale (since per-vCPU CR4.PGE on every dispatch already
	 * flushes locally). The actual root cause is elsewhere —
	 * needs deeper investigation.
	 *
	 * The gen counter still gets bumped (cheap) so future code
	 * can use it for diagnostics or a non-IPI mechanism (e.g.
	 * shared-memory generation polling at vmexit boundaries).
	 *
	 * Skip init_mm: kernel-mm syncs go via kern_map/kern_unmap.
	 */
	if (ret == 0 && mm != &init_mm) {
		atomic64_inc(&mm->context.tlb_gen);
		/*
		 * SMP-T13 followup (2026-05-02): activate the cross-vCPU
		 * tlb kicker now that the migrate_disable fix has closed
		 * the dominant SMP T>=N race. Earlier activation attempts
		 * regressed T=4 from 60/60 to 18/60 because the IPI storm
		 * piled on top of the migration race. With the migration
		 * race gone, the kicker should help close the residual
		 * "stale guest TLB on remote vCPU" window that produces
		 * mt-mini's `got=0 expect=tid` symptoms.
		 *
		 * SMP-T26 ablation (2026-05-02): disabling this kicker
		 * leaves the threaded-fork-malloc fail rate unchanged
		 * (G.2-on: 6/6 boots × ~6 fails; G.2-off: 6/6 × ~5).
		 * H1 (cross-vCPU TLB stale) is therefore NOT the SMP-T26
		 * mechanism. Kicker stays active for defensive correctness;
		 * see Layer 14 memo for next-step hypothesis ranking.
		 */
		if (um_backend->tlb_kick_others)
			um_backend->tlb_kick_others(mm);
	}

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
