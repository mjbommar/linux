// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/smp.h>

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
	int i;

	mutex_init(&mm->context.turnstile);
	spin_lock_init(&mm->context.sync_tlb_lock);

	/*
	 * dup_mm() copies the parent's pending TLB-sync window. A new mm
	 * has no child-owned PTE updates yet, so reset it to the canonical
	 * "no range" state.
	 */
	mm->context.sync_tlb_range_from = 0;
	mm->context.sync_tlb_range_to = 0;

	/*
	 * Reset the per-mm guest-TLB generation. Inheriting the parent's
	 * generation can make a vCPU skip the first flush for this mm and
	 * observe translations from the parent.
	 */
	atomic64_set(&mm->context.tlb_gen, 0);
	for_each_possible_cpu(i)
		atomic64_set(&mm->context.tlb_gen_seen_by[i], 0);

	/*
	 * dup_mm() bytewise-copies the parent mm including the
	 * deferred-free queue, so reset it for the new mm. See
	 * arch/um/include/asm/mmu.h for the rationale.
	 */
	spin_lock_init(&mm->context.deferred_free_lock);
	INIT_LIST_HEAD(&mm->context.deferred_free_pages);
	mm->context.deferred_free_count = 0;

	/*
	 * Clear the per-mm worker pointer copied from the parent. Worker
	 * ownership is established when the backend attaches this mm.
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
		pr_err("corrupt mm_context - pid = %d\n", mmu->id.pid);
		return;
	}

	scoped_guard(spinlock_irqsave, &mm_list_lock)
		list_del(&mm->context.list);

	/*
	 * Drain any remaining deferred-free pages. Normally drained at
	 * vcpu_run; if the mm is being torn down with pages still on the
	 * queue, free them here so they do not leak. By destroy_context
	 * time the mm has no live mappings anywhere, so it is safe.
	 *
	 * um_mmu_gather_drain() uses call_rcu. The callback does not
	 * dereference mm; it only frees the independently-owned batch.
	 * Do not call rcu_barrier() here because that would serialize
	 * high mm-churn workloads on a full RCU grace period.
	 */
	um_mmu_gather_drain(mm);

	/*
	 * mm_destroy owns per-mm teardown of backend-private state (stub
	 * child, per-mm socketpair). Don't repeat the seccomp socket
	 * close here; see seccomp_mm_destroy().
	 */
	trace_um_backend_mm_destroy(mm);
	um_backend_dispatch(mm_destroy, mm);

	free_pages(mmu->id.stack, ilog2(STUB_DATA_PAGES));
}

static irqreturn_t mm_sigchld_irq(int irq, void *dev)
{
	struct mm_context *mm_context;
	pid_t pid;

	guard(spinlock)(&mm_list_lock);

	while ((pid = os_reap_child()) > 0) {
		/*
		 * A child died, check if we have an MM with the PID. This is
		 * only relevant in SECCOMP mode.
		 */
		list_for_each_entry(mm_context, &mm_list, list) {
			if (mm_context->id.pid == pid) {
				struct stub_data *stub_data;

				pr_err("Unexpectedly lost MM child; affected tasks will segfault\n");

				/* Marks the MM as dead */
				mm_context->id.pid = -1;

				stub_data = (void *)mm_context->id.stack;
				stub_data->futex = FUTEX_IN_KERN;
#if IS_ENABLED(CONFIG_SMP)
				os_futex_wake(&stub_data->futex);
#endif

				/*
				 * Syscalls already executing in affected tasks
				 * may finish normally.
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
 * Bulk stub teardown and respawn helpers.
 *
 * Two helpers used by arch/um/kernel/template_pause.c's
 * fork-on-resume loop to eliminate stub-pid aliasing across a
 * fork(2) of the host UML process:
 *
 *   - um_skas_teardown_all_stubs() - SIGKILL+wait4 every stub child
 *     in mm_list, mark each mm's id.pid = -1 (and id.sock = -1).
 *
 *   - um_skas_respawn_all_stubs() - for every mm whose id.pid is -1
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
			pr_warn("%s: reap_stub(pid=%d) failed: %d\n",
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

		/*
		 * Do not kill: the stub child is alive in the parent process
		 * and we'd disturb that. Just mark id as
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
 * Walk every present PTE in @mm and mark it _PAGE_NEEDSYNC, then trigger
 * um_tlb_sync to push them all to the backend.
 *
 * Used by template_pause's pool_member fork loop after
 * start_userspace_fresh: the new stub has only stub_code +
 * stub_data mapped (execveat reset its address space), but
 * init.sh's mm already has VMAs and present PTEs from before
 * the fork.  Without this eager push, init.sh's first userspace
 * access SIGSEGVs and gets mapped lazily one page per syscall
 * round-trip, which is too slow for any realistic workload.
 *
 * The walk mirrors update_pte_range in tlb.c but only sets the needsync
 * bit. um_tlb_sync's normal sweep then drives the push.
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
	pgd_t *pgd;
	unsigned long addr, next, lo = ~0UL, hi = 0;
	p4d_t *p4d;

	VMA_ITERATOR(vmi, mm, 0);

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

/*
 * um_skas_disown_inherited() - drop parent-owned stub references after fork.
 *
 * A pool member inherits mm_list entries whose pid and socket fields refer to
 * the parent's stub children. Mark those references detached and reset syscall
 * scratch fields; the caller may then spawn fresh stubs for the child with
 * start_userspace_fresh().
 *
 * Return: number of entries disowned, or a negative errno.
 */
int um_skas_disown_inherited(void)
{
	struct mm_id **arr;
	int n, i, disowned = 0;

	n = snapshot_mm_ids(&arr);
	if (n <= 0)
		return n;

	for (i = 0; i < n; i++) {
		struct mm_id *id = arr[i];

		/*
		 * Do not kill the stub child; it belongs to the parent host
		 * process. start_userspace_fresh() will replace id->stack with
		 * child-owned stub_data backing.
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
		 * Respawn only stubs that still have a live pid. Entries
		 * already in the detached sentinel state belong to a different
		 * teardown path and must not be revived here.
		 */
		if (id->pid <= 0)
			continue;

		err = start_userspace_redo(id);
		if (err) {
			pr_err("%s: respawn for mm %p failed: %d\n",
			       __func__, id, err);
			last_err = err;
			continue;
		}
		respawned++;
	}

	kfree(arr);
	return last_err ? last_err : respawned;
}
