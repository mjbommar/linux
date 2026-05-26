// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

#include <shared/irq_kern.h>
#include <asm/backend.h>
#include <asm/pgalloc.h>
#include <asm/sections.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>
#include <asm/trace/um_backend.h>
#include <asm/um_memory.h>
#include <as-layout.h>
#include <os.h>
#include <skas.h>
#include <stub-data.h>

/* Ensure the stub_data struct covers the allocated area */
static_assert(sizeof(struct stub_data) == STUB_DATA_PAGES * UM_KERN_PAGE_SIZE);

static spinlock_t mm_list_lock;
static struct list_head mm_list;

struct mutex *__get_turnstile(struct mm_id *mm_id)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);

	return &ctx->turnstile;
}

void enter_turnstile(struct mm_id *mm_id)
{
	mutex_lock(__get_turnstile(mm_id));
}

void exit_turnstile(struct mm_id *mm_id)
{
	mutex_unlock(__get_turnstile(mm_id));
}

int init_new_context(struct task_struct *task, struct mm_struct *mm)
{
	struct mm_id *new_id = &mm->context.id;
	unsigned long stack = 0;
	int ret = -ENOMEM;

	mutex_init(&mm->context.turnstile);
	spin_lock_init(&mm->context.sync_tlb_lock);

	/*
	 * dup_mm() (fork's copy_mm path) bytewise-copies the parent
	 * mm_struct, which includes the sync_tlb_range_{from,to}
	 * window the parent had pending at fork time.  The child mm
	 * has no PTEs of its own yet, so propagating those would
	 * cause the next um_tlb_sync on the child to flush a region
	 * that's meaningful only in the parent.  Worse, a partial
	 * range can confuse the backend dispatcher into a no-op
	 * "already flushed" decision when the child later actually
	 * does need a sync.  Reset to the canonical "no range" state.
	 */
	mm->context.sync_tlb_range_from = 0;
	mm->context.sync_tlb_range_to = 0;

	/*
	 * Per-mm guest-TLB generation counter.  Same dup_mm hazard:
	 * inheriting the parent's tlb_gen value lets a vCPU whose
	 * last_seen_tlb_gen happens to match (because it last ran on
	 * a different mm at the same generation) skip the CR4.PGE
	 * flush when switching to this fresh mm.  Guest TLB then
	 * holds parent's stale GVA->GPA translations, faulting on
	 * the child's anon pages and incrementing MM_ANONPAGES on
	 * a mm whose unmap path won't see the matching PTE.
	 * Manifests as parallel-fork+exec workloads producing
	 *   BUG: Bad rss-counter state ... MM_ANONPAGES val:1
	 * + TLS-region SEGVs in libc/python.  Reset to 0 so the very
	 * first dispatch on the child mm always flushes.
	 */
	atomic64_set(&mm->context.tlb_gen, 0);
	{
		int i;

		for (i = 0; i < NR_CPUS; i++)
			atomic64_set(&mm->context.tlb_gen_seen_by[i], 0);
	}

	/*
	 * Memo §H.1b residual fix: deferred-free page list. dup_mm()
	 * bytewise-copies the parent mm including these fields, so we
	 * must reset for the new mm. See arch/um/include/asm/mmu.h
	 * for the rationale (mmu_gather frees pages before UML's
	 * deferred TLB flush completes, exposing a slab-write window).
	 */
	spin_lock_init(&mm->context.deferred_free_lock);
	INIT_LIST_HEAD(&mm->context.deferred_free_pages);
	mm->context.deferred_free_count = 0;

	/*
	 * Clear the per-mm worker pointer for a fresh mm. dup_mm() (during
	 * fork's copy_mm path) bytewise-copies the parent mm_struct,
	 * including mm->context.worker. Without this reset, the child mm
	 * inherits the parent's `struct um_worker *` and the subsequent
	 * seccomp_mm_create → worker_alloc_stub_for_mm → __spawn_worker_for_mm
	 * call trips WARN_ON_ONCE(mm->context.worker != NULL) at
	 * arch/um/kernel/spawner.c:225, then proceeds to spawn a new worker
	 * which silently overwrites the inherited pointer (leaking the
	 * parent's reference). Clearing here is the single source of truth
	 * for "fresh mm has no worker yet"; spawn_worker_for_mm /
	 * worker_alloc_stub_for_mm assign the pointer once they actually
	 * create one.
	 */
	mm->context.worker = NULL;

	stack = __get_free_pages(GFP_KERNEL | __GFP_ZERO, ilog2(STUB_DATA_PAGES));
	if (stack == 0)
		goto out;

	new_id->stack = stack;
	new_id->syscall_data_len = 0;
	new_id->syscall_fd_num = 0;
	/*
	 * sock is set to a real fd by the seccomp backend's mm_attach;
	 * initialize to -1 so destroy_context()'s teardown can use a
	 * valid-fd check (>= 0) rather than truthiness, which would
	 * mistreat fd 0 as closed.
	 */
	new_id->sock = -1;
	new_id->pid = -1;

	scoped_guard(spinlock_irqsave, &mm_list_lock) {
		/* Insert into list, used for lookups when the child dies */
		list_add(&mm->context.list, &mm_list);
	}

	ret = um_backend_dispatch(mm_create, mm);
	if (ret < 0)
		goto out_free;
	trace_um_backend_mm_create(mm);

	/* Ensure the new MM is clean and nothing unwanted is mapped */
	{
		struct um_memory_region region = {
			.va = 0,
			.len = STUB_START,
			.phys_fd = -1,
		};
		um_backend_dispatch(mm_region_removed, mm, &region);
	}

	return 0;

 out_free:
	free_pages(new_id->stack, ilog2(STUB_DATA_PAGES));
 out:
	return ret;
}

void destroy_context(struct mm_struct *mm)
{
	struct mm_context *mmu = &mm->context;

	/*
	 * If init_new_context wasn't called, this will be
	 * zero, resulting in a kill(0), which will result in the
	 * whole UML suddenly dying.  Also, cover negative and
	 * 1 cases, since they shouldn't happen either.
	 *
	 * Negative cases happen if the child died unexpectedly.
	 */
	if (mmu->id.pid >= 0 && mmu->id.pid < 2) {
		printk(KERN_ERR "corrupt mm_context - pid = %d\n",
		       mmu->id.pid);
		return;
	}

	scoped_guard(spinlock_irqsave, &mm_list_lock)
		list_del(&mm->context.list);

	/*
	 * Memo §H.1b residual fix: drain any remaining deferred-free
	 * pages. Normally drained at vcpu_run; if the mm is being torn
	 * down with pages still on the queue (e.g. exit_mmap raced with
	 * a pending sync), free them here so they don't leak. By
	 * destroy_context time the mm has no live mappings anywhere —
	 * no TLB, no PTEs, no userspace — so it's safe regardless.
	 *
	 * SMP-T20 (2026-05-02): drain now uses call_rcu. The callback
	 * (um_defer_batch_rcu_free) does NOT dereference mm — it only
	 * frees the encoded_page array (which is in the kmalloc'd batch
	 * struct, owned independently of mm) and kfrees the batch. So
	 * it's safe for the callback to outlive the mm. We deliberately
	 * do NOT call rcu_barrier() here — it would block destroy_context
	 * for one full grace period (~15ms+ under PREEMPT_VOLUNTARY with
	 * busy vCPUs), and at high mm-churn workloads (subprocess.Popen
	 * × 400) that serializes to multi-second slowdown.
	 */
	um_mmu_gather_drain(mm);

	/*
	 * mm_destroy owns per-mm teardown of backend-private state (stub
	 * child, per-mm socketpair). Don't repeat the seccomp socket
	 * close here — see seccomp_mm_destroy().
	 */
	trace_um_backend_mm_destroy(mm);
	um_backend_dispatch(mm_destroy, mm);

	free_pages(mmu->id.stack, ilog2(STUB_DATA_PAGES));
}

static irqreturn_t mm_sigchld_irq(int irq, void* dev)
{
	struct mm_context *mm_context;
	pid_t pid;

	guard(spinlock)(&mm_list_lock);

	while ((pid = os_reap_child()) > 0) {
		/*
		* A child died, check if we have an MM with the PID. This is
		* only relevant in SECCOMP mode (as ptrace will fail anyway).
		*
		* See wait_stub_done_seccomp for more details.
		*/
		list_for_each_entry(mm_context, &mm_list, list) {
			if (mm_context->id.pid == pid) {
				struct stub_data *stub_data;
				printk("Unexpectedly lost MM child! Affected tasks will segfault.");

				/* Marks the MM as dead */
				mm_context->id.pid = -1;

				stub_data = (void *)mm_context->id.stack;
				stub_data->futex = FUTEX_IN_KERN;
#if IS_ENABLED(CONFIG_SMP)
				os_futex_wake(&stub_data->futex);
#endif

				/*
				 * NOTE: Currently executing syscalls by
				 * affected tasks may finish normally.
				 */
				break;
			}
		}
	}

	return IRQ_HANDLED;
}

static int __init init_child_tracking(void)
{
	int err;

	spin_lock_init(&mm_list_lock);
	INIT_LIST_HEAD(&mm_list);

	err = request_irq(SIGCHLD_IRQ, mm_sigchld_irq, 0, "SIGCHLD", NULL);
	if (err < 0)
		panic("Failed to register SIGCHLD IRQ: %d", err);

	return 0;
}
early_initcall(init_child_tracking)

/*
 * Memo 09 Phase 2a — bulk stub teardown + respawn.
 *
 * Two helpers used by arch/um/kernel/template_pause.c's
 * fork-on-resume loop to eliminate stub-pid aliasing across a
 * fork(2) of the host UML process:
 *
 *   - um_skas_teardown_all_stubs() — SIGKILL+wait4 every stub child
 *     in mm_list, mark each mm's id.pid = -1 (and id.sock = -1).
 *
 *   - um_skas_respawn_all_stubs() — for every mm whose id.pid is -1
 *     (sentinel set by the teardown), call start_userspace_redo()
 *     to clone a fresh stub child.
 *
 * The snapshot-then-iterate pattern (mirrors mm_sigchld_irq's lock
 * discipline): hold mm_list_lock only long enough to copy
 * struct mm_id * pointers into a kmalloc'd array, drop the lock,
 * then call host syscalls.  See the contract comment on the
 * extern declarations in shared/skas/skas.h.
 */

/* Build a snapshot of mm_id pointers from mm_list.  Returns the
 * number of entries written to @out (caller pre-sized via a count
 * pass) on success, or -ENOMEM on alloc failure.  Callers free @out
 * with kfree().
 */
static int snapshot_mm_ids(struct mm_id ***out)
{
	struct mm_context *ctx;
	struct mm_id **arr;
	int n = 0, i = 0;

	/* Count under the lock so the allocation matches. */
	scoped_guard(spinlock_irqsave, &mm_list_lock) {
		list_for_each_entry(ctx, &mm_list, list)
			n++;
	}

	if (n == 0) {
		*out = NULL;
		return 0;
	}

	arr = kmalloc_array(n, sizeof(*arr), GFP_KERNEL);
	if (!arr)
		return -ENOMEM;

	scoped_guard(spinlock_irqsave, &mm_list_lock) {
		list_for_each_entry(ctx, &mm_list, list) {
			if (i < n)
				arr[i++] = &ctx->id;
		}
	}

	*out = arr;
	return i;
}

int um_skas_teardown_all_stubs(void)
{
	struct mm_id **arr;
	int n, i, dead = 0;

	n = snapshot_mm_ids(&arr);
	if (n <= 0)
		return n;

	for (i = 0; i < n; i++) {
		struct mm_id *id = arr[i];
		int err;

		if (id->pid <= 0)
			continue;

		err = os_skas_reap_stub(id);
		if (err) {
			printk(KERN_WARNING
			       "%s: reap_stub(pid=%d) failed: %d\n",
			       __func__, id->pid, err);
			continue;
		}
		dead++;
	}

	kfree(arr);
	return dead;
}

int um_skas_other_mm_mid_syscall(struct mm_id *caller)
{
	struct mm_context *ctx;
	int busy = 0;

	scoped_guard(spinlock_irqsave, &mm_list_lock) {
		list_for_each_entry(ctx, &mm_list, list) {
			struct stub_data *stub_data;

			if (&ctx->id == caller)
				continue;
			if (ctx->id.pid <= 0)
				continue;
			stub_data = (void *)ctx->id.stack;
			if (stub_data->futex == FUTEX_IN_KERN) {
				busy = 1;
				break;
			}
		}
	}
	return busy;
}

int um_skas_forget_all_stubs(void)
{
	struct mm_id **arr;
	int n, i, forgotten = 0;

	n = snapshot_mm_ids(&arr);
	if (n <= 0)
		return n;

	for (i = 0; i < n; i++) {
		struct mm_id *id = arr[i];
		struct stub_data *stub_data = (void *)id->stack;

		/* DO NOT kill — the stub child is alive in the parent
		 * process and we'd disturb that.  Just mark id as
		 * detached.
		 */
		id->pid = -1;
		if (id->sock >= 0) {
			os_close_file(id->sock);
			id->sock = -1;
		}
		stub_data->futex = 0;
		stub_data->signal = 0;
		stub_data->si_offset = 0;
		stub_data->mctx_offset = 0;
		stub_data->syscall_data_len = 0;
		id->syscall_data_len = 0;
		id->syscall_fd_num = 0;
		forgotten++;
	}

	kfree(arr);
	return forgotten;
}

/*
 * um_skas_disown_inherited() — post-fork helper for pool-member
 * children.
 *
 * After the Path A clone(), the child UML kernel inherits the
 * parent's mm_list with each entry's stub_pid pointing at the
 * parent's stub host-child, AND each id->stack pointing at the
 * parent-allocated stub_data page.  The stub_data page is in
 * UML's physmem and the stub processes mmap it via the inherited
 * stub_data_fd — so even though CoW separates the kernel's VA,
 * the stub_data fd resolves to the SAME PHYSICAL PAGE for master,
 * master's stub, AND the child's would-be stub.  That triple-
 * share is the inherited-stub-aliasing corruption.
 *
 * This helper makes the child's mm_list ENTRY-OWNING again:
 *   1. id->pid = -1 (do NOT kill — that's master's host-child).
 *   2. id->sock = -1 (close inherited fd locally).
 *   3. ALLOCATE FRESH __get_free_pages for id->stack.  This is
 *      the critical step — the new pages live in the child's
 *      kernel physmem (a CoW-private region after fork), so the
 *      physmem fd that phys_mapping() resolves for it is
 *      DIFFERENT from master's.  A subsequent start_userspace()
 *      clones a stub that mmaps THIS fd, getting a private
 *      stub_data page.
 *   4. Reset id syscall scratch fields.
 *
 * The OLD id->stack page is leaked from the child's perspective
 * — it still belongs to master's stub, and master's master mm
 * still references it.  Per-iteration leak budget: STUB_DATA_PAGES
 * pages.  Acceptable for pool dispatch where the child is short-
 * to-medium lived.
 *
 * Caller must then call um_skas_respawn_all_stubs() to
 * start_userspace_redo each entry — at that point id->pid is -1
 * so respawn will call start_userspace which clones a fresh
 * stub using the new id->stack physmem fd.
 *
 * Returns count of mm_id entries disowned, or -errno on alloc
 * failure (in which case the partially-disowned state is left as-
 * is; caller should not respawn).
 */
/*
 * Walk every present PTE in @mm and mark it _PAGE_NEEDSYNC, then
 * trigger um_tlb_sync to push them all to the backend.
 *
 * Used by template_pause's pool_member fork loop after
 * start_userspace_fresh: the new stub has only stub_code +
 * stub_data mapped (execveat reset its address space), but
 * init.sh's mm already has VMAs and present PTEs from before
 * the fork.  Without this eager push, init.sh's first userspace
 * access SIGSEGVs and gets mapped lazily one page per syscall
 * round-trip — too slow for any realistic workload.
 *
 * The walk mirrors update_pte_range in tlb.c but only sets
 * the needsync bit (does NOT push directly).  um_tlb_sync's
 * normal sweep then drives the push.
 */
static void mm_force_resync_pmd(pmd_t *pmd, unsigned long addr,
				unsigned long end)
{
	pte_t *pte;

	pte = pte_offset_kernel(pmd, addr);
	do {
		if (pte_present(*pte))
			*pte = pte_mkneedsync(*pte);
	} while (pte++, addr += PAGE_SIZE, addr < end);
}

static void mm_force_resync_pud(pud_t *pud, unsigned long addr,
				unsigned long end)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (!pmd_none(*pmd) && !pmd_bad(*pmd))
			mm_force_resync_pmd(pmd, addr, next);
	} while (pmd++, addr = next, addr < end);
}

static void mm_force_resync_p4d(p4d_t *p4d, unsigned long addr,
				unsigned long end)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (!pud_none(*pud) && !pud_bad(*pud))
			mm_force_resync_pud(pud, addr, next);
	} while (pud++, addr = next, addr < end);
}

int um_skas_force_resync_mm(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);
	pgd_t *pgd;
	unsigned long addr, next, lo = ~0UL, hi = 0;
	p4d_t *p4d;

	if (!mm)
		return -EINVAL;

	mmap_read_lock(mm);
	for_each_vma(vmi, vma) {
		addr = vma->vm_start;
		if (addr < lo)
			lo = addr;
		if (vma->vm_end > hi)
			hi = vma->vm_end;
		pgd = pgd_offset(mm, addr);
		do {
			next = pgd_addr_end(addr, vma->vm_end);
			if (!pgd_none(*pgd) && !pgd_bad(*pgd)) {
				p4d = p4d_offset(pgd, addr);
				mm_force_resync_p4d(p4d, addr, next);
			}
		} while (pgd++, addr = next, addr < vma->vm_end);
	}
	mmap_read_unlock(mm);

	if (lo < hi)
		um_tlb_mark_sync(mm, lo, hi);
	return um_tlb_sync(mm);
}

int um_skas_disown_inherited(void)
{
	struct mm_id **arr;
	int n, i, disowned = 0;

	n = snapshot_mm_ids(&arr);
	if (n <= 0)
		return n;

	for (i = 0; i < n; i++) {
		struct mm_id *id = arr[i];

		/* Forget the parent's stub-pid + sock (don't kill — that
		 * stub belongs to the parent host process).  Caller is
		 * expected to follow with start_userspace_fresh(), which
		 * will overwrite id->stack with a per-mm memfd-backed
		 * page and clone a fresh stub.
		 *
		 * Do NOT __get_free_pages here — the post-fork page
		 * allocator state is CoW'd from master and allocating
		 * trips __del_page_from_free_list corruption.
		 */
		id->pid = -1;
		if (id->sock >= 0) {
			os_close_file(id->sock);
			id->sock = -1;
		}
		id->syscall_data_len = 0;
		id->syscall_fd_num = 0;
		disowned++;
	}

	kfree(arr);
	return disowned;
}

int um_skas_respawn_all_stubs(void)
{
	struct mm_id **arr;
	int n, i, respawned = 0, last_err = 0;

	n = snapshot_mm_ids(&arr);
	if (n <= 0)
		return n;

	for (i = 0; i < n; i++) {
		struct mm_id *id = arr[i];
		int err;

		/*
		 * Skip entries that were already torn down (pid <= 0) by
		 * a previous um_skas_teardown_all_stubs() call.  Without
		 * this guard, respawn would call start_userspace_redo on
		 * a dead mm_id whose stack/sock state is in the post-
		 * teardown sentinel state; clone() then sets up a stub
		 * child that shares VM with the caller and crashes at
		 * IP=0 (the M-fork CoW + CLONE_VM triple-share corrupts
		 * the stub binary's entry-point lookup).  Diagnosed
		 * 2026-05-20 via template-pause-fork-stress reproducing
		 * a master-death at iter ~11 every run.
		 *
		 * The caller's contract is: respawn revives the stubs
		 * the caller previously asked to teardown for fork
		 * safety.  A dead entry that the caller did NOT pair
		 * with teardown is the caller's bug — don't blindly
		 * revive it.
		 */
		if (id->pid <= 0)
			continue;

		err = start_userspace_redo(id);
		if (err) {
			printk(KERN_ERR
			       "%s: respawn for mm %p failed: %d\n",
			       __func__, id, err);
			last_err = err;
			continue;
		}
		respawned++;
	}

	kfree(arr);
	return last_err ? last_err : respawned;
}
