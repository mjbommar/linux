// SPDX-License-Identifier: GPL-2.0
/*
 * UML per-mm host worker process — spawner side
 * (memo 25 refactor 4 / memo 28 commit E.2).
 *
 * The spawner is the original UML host process. After memo 25 R4
 * lands fully (E.6 commit), the spawner owns control-plane state
 * only — no guest-task execution happens in the spawner's VA
 * space. Each guest mm gets its own worker process.
 *
 * This TU is the kernel-side dispatch + list management. The
 * actual clone-without-CLONE_VM lives in
 * arch/um/os-Linux/spawner_user.c (E.3 commit).
 *
 * What this commit (E.2) provides:
 * - struct um_worker definition (opaque to callers via
 *   arch/um/include/shared/worker_api.h's forward decl).
 * - Per-spawner state: list of workers, lock.
 * - spawner_init / spawner_shutdown wired into the boot path.
 * - spawn_worker_for_mm / reap_worker_for_mm stubs that always
 *   return success without actually spawning. The seccomp
 *   mm_create path falls back to today's stub-child-in-spawner
 *   model when mm->context.worker == NULL after the call —
 *   i.e. always in this commit.
 *
 * E.3 makes spawn_worker_for_mm actually create a worker.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <worker_api.h>
#include <worker_user.h>

/*
 * Per-mm worker handle. Opaque to callers; only spawner.c (and
 * later spawner_user.c) reference fields directly.
 */
struct um_worker {
	struct list_head	list;		/* on workers list, under workers_lock */
	struct mm_struct	*mm;		/* back-ref to the guest mm we serve */
	int			pid;		/* worker process pid; -1 until E.3 spawns */
	int			ipc_sock;	/* spawner-side socket end; -1 in E.2 */

	/*
	 * Future fields (E.3+) go here:
	 *   struct task_struct *dispatcher;  // per-worker IPC thread
	 *   wait_queue_head_t   reply_wait;  // for syscall round-trip
	 *   atomic_t            quiescing;   // QUIESCE_REQ flag
	 *   ...
	 *
	 * Keep additions explicit so the wire format and lifecycle
	 * stay reviewable.
	 */
};

static LIST_HEAD(workers);
static DEFINE_SPINLOCK(workers_lock);
static bool spawner_initialized;

int spawner_init(void)
{
	if (spawner_initialized) {
		pr_warn("um: worker model: spawner_init called twice\n");
		return -EBUSY;
	}

	/* List + lock are statically initialized; nothing to do here yet. */
	spawner_initialized = true;
	pr_info("um: worker model: spawner ready (CONFIG_UM_WORKER_PROCESS=y; no workers yet — memo 28 E.2)\n");
	return 0;
}

void spawner_shutdown(void)
{
	struct um_worker *w, *tmp;
	LIST_HEAD(reap_list);

	if (!spawner_initialized)
		return;

	scoped_guard(spinlock, &workers_lock) {
		list_splice_init(&workers, &reap_list);
	}

	list_for_each_entry_safe(w, tmp, &reap_list, list) {
		/*
		 * Today (E.2) the list is always empty here because
		 * spawn_worker_for_mm doesn't actually allocate. Once
		 * E.3 lands, this loop kills + waitpid()s each worker.
		 */
		list_del(&w->list);
		kfree(w);
	}

	spawner_initialized = false;
	pr_info("um: worker model: spawner shutdown complete\n");
}

/*
 * Per-mm worker lifecycle — stubs. E.3 replaces with real impls.
 *
 * Callers (today only seccomp_mm_create / seccomp_mm_destroy via
 * mm_create / mm_destroy ops) must tolerate either branch:
 *  - mm->context.worker == NULL after spawn_worker_for_mm: fall
 *    back to the stub-child-in-spawner path.
 *  - mm->context.worker != NULL: route through the worker IPC.
 *
 * That fallback discipline is what keeps WORKER_PROCESS=y a no-op
 * until E.3 actually spawns workers, and what lets the toggle ship
 * default-y in E.6 without breaking existing seccomp setups.
 */
int spawn_worker_for_mm(struct mm_struct *mm)
{
	struct um_worker *w;
	int rc;

	if (!spawner_initialized)
		return -ENODEV;

	WARN_ON_ONCE(mm->context.worker != NULL);

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;

	w->mm       = mm;
	w->pid      = -1;
	w->ipc_sock = -1;

	rc = spawn_worker_process(&w->pid, &w->ipc_sock);
	if (rc < 0) {
		kfree(w);
		return rc;
	}

	scoped_guard(spinlock, &workers_lock) {
		list_add(&w->list, &workers);
	}

	mm->context.worker = w;

	pr_info("um: worker model: spawned worker pid=%d for mm=%p (ipc_sock=%d)\n",
		w->pid, mm, w->ipc_sock);
	return 0;
}

void reap_worker_for_mm(struct mm_struct *mm)
{
	struct um_worker *w = mm->context.worker;
	int pid, sock;

	if (!w)
		return;

	scoped_guard(spinlock, &workers_lock) {
		list_del(&w->list);
	}

	pid  = w->pid;
	sock = w->ipc_sock;

	mm->context.worker = NULL;
	kfree(w);

	reap_worker_process(pid, sock);
}

/*
 * Wire spawner_init / spawner_shutdown into the boot path.
 *
 * arch_initcall (level 3) runs after the per-arch setup_arch() so
 * the slab allocator + workqueues + spinlock_init macros are all
 * available, and before subsys_initcall where the seccomp backend
 * starts wanting to spawn things.
 */
static int __init spawner_arch_init(void)
{
	return spawner_init();
}
arch_initcall(spawner_arch_init);

/*
 * Reboot/halt path. arch/um/kernel/reboot.c::uml_cleanup() calls
 * the backend's shutdown op; we hook into the existing
 * subsys_initcall reboot ordering via a panic notifier so spawner
 * teardown runs before the host process exits.
 */
#include <linux/notifier.h>
#include <linux/panic_notifier.h>

static int spawner_panic_handler(struct notifier_block *nb,
				 unsigned long event, void *data)
{
	spawner_shutdown();
	return NOTIFY_DONE;
}

static struct notifier_block spawner_panic_nb = {
	.notifier_call	= spawner_panic_handler,
	.priority	= INT_MIN,	/* run last so other notifiers see live workers */
};

static int __init spawner_register_notifiers(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &spawner_panic_nb);
	return 0;
}
late_initcall(spawner_register_notifiers);
