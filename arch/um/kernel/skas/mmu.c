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

int um_skas_respawn_all_stubs(void)
{
	struct mm_id **arr;
	int n, i, respawned = 0, last_err = 0;

	printk(KERN_INFO "%s: entry\n", __func__);
	n = snapshot_mm_ids(&arr);
	printk(KERN_INFO "%s: snapshot returned n=%d\n", __func__, n);
	if (n <= 0)
		return n;

	for (i = 0; i < n; i++) {
		struct mm_id *id = arr[i];
		int err;

		printk(KERN_INFO "%s: about to redo id=%p pid=%d sock=%d stack=%lx\n",
		       __func__, id, id->pid, id->sock, id->stack);
		err = start_userspace_redo(id);
		printk(KERN_INFO "%s: redo returned err=%d new pid=%d\n",
		       __func__, err, id->pid);
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
