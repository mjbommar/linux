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

#include <linux/err.h>
#include <linux/completion.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include <os.h>
#include <skas.h>
#include <mm_id.h>
#include <worker_api.h>
#include <worker_ipc.h>
#include <worker_user.h>

/*
 * Per-mm worker handle. Opaque to callers; only spawner.c (and
 * later spawner_user.c) reference fields directly.
 *
 * `dispatcher` is only populated for the smoke-test scaffold path
 * (UM_WORKER_SMOKE_TEST_ON_BOOT); production worker bring-up
 * (worker_alloc_stub_for_mm) leaves it NULL and uses the synchronous
 * VCPU_RUN/VCPU_DONE round-trip in worker_drive_vcpu_run instead.
 *
 * `vcpu_done` is the completion the dispatcher kthread signals when
 * it observes WORKER_MSG_VCPU_DONE — kept for the smoke-test path
 * and as a defensive sink should an asynchronous VCPU_DONE arrive
 * outside of worker_drive_vcpu_run's synchronous read.
 */
struct um_worker {
	struct list_head	list;		/* on workers list, under workers_lock */
	struct mm_struct	*mm;		/* back-ref to the guest mm we serve */
	int			pid;		/* worker process pid; -1 until E.3 spawns */
	int			ipc_sock;	/* spawner-side socket end; -1 in E.2 */
	struct task_struct	*dispatcher;	/* smoke-test kthread; NULL in production */

	struct completion	vcpu_done;
	int			vcpu_done_status;
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
 * Per-worker IPC dispatcher kthread.
 *
 * E.3d.2's spawner-side reroute (set_stub_state / get_stub_state /
 * handle_syscall all run in seccomp_vcpu_run's continuation under the
 * originating guest task's `current`) and worker_drive_vcpu_run's
 * synchronous read of the IPC socket leave this kthread with no
 * production role. It survives only as the smoke-test scaffold's
 * waiter: when UM_WORKER_SMOKE_TEST_ON_BOOT drives a worker through
 * STUB_ALLOC / WRITE_REGS / RETURN_VALUE / WRITE_REGS_ACK, the
 * spawner side reads the ACK directly via worker_smoke_test rather
 * than this dispatcher — so in fact even the smoke test does not
 * route messages here. The kthread therefore acts as a defensive
 * sink: VCPU_DONE updates the completion (in case an asynchronous
 * stub-loss notification ever arrives outside a worker_drive_vcpu_run
 * call), SHUTDOWN exits cleanly, anything else (including SYSCALL_REQ,
 * which production paths no longer emit) is warn-and-dropped.
 */
static int worker_dispatcher_fn(void *arg)
{
	struct um_worker *w = arg;
	struct worker_msg msg;
	int n;

	while (!kthread_should_stop()) {
		n = os_read_file(w->ipc_sock, &msg, sizeof(msg));
		if (n == 0)
			break;	/* EOF: worker closed or we did */
		if (n == -EINTR)
			continue;
		if (n < 0)
			break;	/* socket gone (e.g. -EBADF after our close) */
		if (n != sizeof(msg)) {
			pr_warn_ratelimited("um: worker disp: short read %d (sock=%d)\n",
					    n, w->ipc_sock);
			continue;
		}

		if (msg.magic != WORKER_IPC_MAGIC) {
			pr_warn_ratelimited("um: worker disp: bad magic 0x%x type=%u\n",
					    (unsigned)msg.magic, (unsigned)msg.type);
			continue;
		}

		switch (msg.type) {
		case WORKER_MSG_VCPU_DONE:
			w->vcpu_done_status = (int)msg.u.vcpu_done.status;
			complete(&w->vcpu_done);
			break;
		case WORKER_MSG_SHUTDOWN:
			return 0;
		default:
			pr_warn_ratelimited("um: worker disp: unexpected msg type %u (production paths route VCPU_RUN/VCPU_DONE inline)\n",
					    (unsigned)msg.type);
			break;
		}
	}
	return 0;
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
/*
 * Start the per-worker dispatcher kthread. Split from
 * spawn_worker_for_mm so the E.3d.0 stub-alloc round-trip can drain
 * the IPC socket synchronously before the dispatcher takes ownership
 * of read(). Returns 0 on success or a negative errno; the caller is
 * responsible for tearing down the worker on failure.
 */
static int worker_start_dispatcher(struct um_worker *w)
{
	w->dispatcher = kthread_run(worker_dispatcher_fn, w,
				    "um-worker-disp/%d", w->pid);
	if (IS_ERR(w->dispatcher)) {
		int rc = PTR_ERR(w->dispatcher);

		w->dispatcher = NULL;
		return rc;
	}
	return 0;
}

/*
 * Allocate + spawn a worker for `mm`. When `defer_dispatcher` is
 * false (the legacy callers) the dispatcher kthread starts before
 * returning. When true (E.3d.0's seccomp_mm_create path), the caller
 * is responsible for invoking worker_start_dispatcher after any
 * dispatcher-incompatible synchronous IPC round-trip has completed.
 */
static int __spawn_worker_for_mm(struct mm_struct *mm, bool defer_dispatcher)
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

	init_completion(&w->vcpu_done);

	rc = spawn_worker_process(&w->pid, &w->ipc_sock);
	if (rc < 0) {
		kfree(w);
		return rc;
	}

	if (!defer_dispatcher) {
		rc = worker_start_dispatcher(w);
		if (rc < 0) {
			reap_worker_process(w->pid, w->ipc_sock);
			kfree(w);
			return rc;
		}
	}

	scoped_guard(spinlock, &workers_lock) {
		list_add(&w->list, &workers);
	}

	mm->context.worker = w;

	pr_info("um: worker model: spawned worker pid=%d for mm=%p (ipc_sock=%d, dispatcher=%s)\n",
		w->pid, mm, w->ipc_sock, defer_dispatcher ? "deferred" : "running");
	return 0;
}

int spawn_worker_for_mm(struct mm_struct *mm)
{
	return __spawn_worker_for_mm(mm, false);
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

	/*
	 * Order: reap_worker_process closes our socket end first, which
	 * unblocks the dispatcher's os_read_file with EOF/-EBADF; then
	 * SIGTERM+waitpid finishes the worker. After that, kthread_stop
	 * collects an already-exited kthread (kthread_stop on a returned
	 * kthread is fine — it just returns the exit code). Reversing
	 * (kthread_stop first) would deadlock since kthread_stop waits
	 * for the kthread to exit, but the kthread is blocked in read.
	 */
	reap_worker_process(pid, sock);

	if (w->dispatcher) {
		kthread_stop(w->dispatcher);
		w->dispatcher = NULL;
	}

	mm->context.worker = NULL;
	kfree(w);
}

/*
 * worker_alloc_stub_for_mm — bring up the per-mm worker AND its
 * stub child (memo 28 E.3d.0).
 *
 * The worker is spawned with the dispatcher kthread DEFERRED so this
 * function can synchronously drive the STUB_ALLOC_REQ → STUB_ALLOC_REP
 * round-trip without racing the dispatcher on the IPC socket. On
 * success the spawner-side `*id_out` is fully populated (including
 * id_out->sock, which is a fresh fd installed in this process's FD
 * table by recvmsg's SCM_RIGHTS handling) and the dispatcher kthread
 * is running.
 *
 * Failure modes (any of which leave mm->context.worker == NULL so
 * seccomp_mm_create's caller can fall back to the legacy in-spawner
 * start_userspace path):
 *   -ENODEV   spawner not initialized
 *   -EIO      short read/write on IPC socket
 *   -EPROTO   reply has wrong magic/type or no SCM_RIGHTS cmsg
 *   <other>   propagated from spawn_worker_process / start_userspace
 *             (status field of the reply, sign-extended)
 */
int worker_alloc_stub_for_mm(struct mm_struct *mm, struct mm_id *id_out)
{
	struct um_worker *w;
	struct worker_msg req;
	struct worker_msg rep;
	int sock_fd = -1;
	ssize_t n;
	int rc, i;

	rc = __spawn_worker_for_mm(mm, true /* defer_dispatcher */);
	if (rc < 0)
		return rc;

	w = mm->context.worker;

	memset(&req, 0, sizeof(req));
	req.magic = WORKER_IPC_MAGIC;
	req.type  = WORKER_MSG_STUB_ALLOC_REQ;
	/*
	 * Pass the spawner-allocated stub_data VA (init_new_context did
	 * __get_free_pages from physmem) so the worker uses the SAME
	 * shared page. The worker inherited the MAP_SHARED physmem
	 * mapping CoW from the spawner; arithmetic on this VA in
	 * start_userspace's userspace_tramp resolves through phys_mapping
	 * the same way (uml_physmem is also CoW-inherited). The futex
	 * stored at that VA is cross-process visible because the page is
	 * memfd-backed and MAP_SHARED.
	 */
	req.u.stub_alloc.stack = (worker_u64)id_out->stack;
	n = os_write_file(w->ipc_sock, &req, sizeof(req));
	if (n != sizeof(req)) {
		rc = n < 0 ? n : -EIO;
		goto out_reap;
	}

	/*
	 * recvmsg with cmsg buffer for the worker's SCM_RIGHTS reply.
	 * os_rcv_fd_msg installs the inbound fd into the spawner's FD
	 * table and returns the body length.
	 */
	n = os_rcv_fd_msg(w->ipc_sock, &sock_fd, 1, &rep, sizeof(rep));
	if (n != sizeof(rep)) {
		rc = n < 0 ? n : -EIO;
		goto out_reap;
	}

	if (rep.magic != WORKER_IPC_MAGIC ||
	    rep.type  != WORKER_MSG_STUB_ALLOC_REP) {
		pr_err("um: worker alloc: bad reply magic=0x%x type=%u\n",
		       (unsigned)rep.magic, (unsigned)rep.type);
		rc = -EPROTO;
		goto out_reap;
	}

	if (rep.u.stub_alloc_rep.status != 0) {
		pr_err("um: worker alloc: start_userspace in worker failed (status=%d)\n",
		       (int)rep.u.stub_alloc_rep.status);
		rc = (int)rep.u.stub_alloc_rep.status;
		if (rc >= 0)
			rc = -EIO;
		goto out_reap;
	}

	if (sock_fd < 0) {
		pr_err("um: worker alloc: reply OK but no SCM_RIGHTS fd\n");
		rc = -EPROTO;
		goto out_reap;
	}

	id_out->stack            = (unsigned long)rep.u.stub_alloc_rep.stack;
	id_out->pid              = (int)rep.u.stub_alloc_rep.pid;
	id_out->syscall_data_len = (int)rep.u.stub_alloc_rep.syscall_data_len;
	id_out->sock             = sock_fd;
	id_out->syscall_fd_num   = (int)rep.u.stub_alloc_rep.syscall_fd_num;
	for (i = 0; i < STUB_MAX_FDS; i++)
		id_out->syscall_fd_map[i] =
			(int)rep.u.stub_alloc_rep.syscall_fd_map[i];

	/*
	 * vcpu_run directly reads/writes the IPC socket from the
	 * originating guest task's context (single-consumer model).
	 * No dispatcher kthread is started here — leaving
	 * w->dispatcher == NULL keeps reap_worker_for_mm's
	 * kthread_stop branch a no-op.
	 */

	pr_info("um: worker model: stub alloc OK for mm=%p (worker_pid=%d stub_pid=%d sock=%d)\n",
		mm, w->pid, id_out->pid, id_out->sock);
	return 0;

out_reap:
	/*
	 * Tear down the worker so seccomp_mm_create can fall back. The
	 * dispatcher hasn't started yet (deferred), so reap_worker_for_mm
	 * is safe — it'll skip the kthread_stop branch since
	 * w->dispatcher is NULL.
	 */
	reap_worker_for_mm(mm);
	return rc;
}

/*
 * Synchronous send of a worker_msg over an mm's IPC socket.
 *
 * Production callers: worker_drive_vcpu_run sends VCPU_RUN here and
 * reads VCPU_DONE inline. Returns 0 on success or a negative errno;
 * the caller is responsible for reading the reply separately if one
 * is expected.
 */
int worker_send_msg_for_mm(struct mm_struct *mm, const struct worker_msg *msg)
{
	struct um_worker *w = mm ? mm->context.worker : NULL;
	int n;

	if (!w || w->ipc_sock < 0)
		return -ENODEV;

	n = os_write_file(w->ipc_sock, msg, sizeof(*msg));
	if (n != sizeof(*msg))
		return n < 0 ? n : -EIO;
	return 0;
}

/*
 * worker_drive_vcpu_run — ship one outer vcpu_run iteration to the
 * worker and read its VCPU_DONE reply.
 *
 * Synchronization:
 *  - The worker's main loop is single-threaded; it owns the futex
 *    round-trip with the stub child and replies VCPU_DONE when the
 *    stub re-traps. There is exactly one consumer-on-each-side per
 *    iteration, so a synchronous read on the spawner end is correct
 *    (no dispatcher kthread is started for this worker — E.3d.2).
 *  - SIGSYS dispatch happens in the spawner's seccomp_vcpu_run
 *    continuation (handle_syscall is called there, on the originating
 *    guest task's `current`). The next iteration's set_stub_state
 *    propagates the syscall return value into the stub's mcontext.
 *
 * Lifetime: caller holds current->mm pinned; reap_worker_for_mm only
 * fires after all guest tasks for the mm have stopped, so w stays
 * valid across this call.
 *
 * Returns 0 on a clean trap completion; -ENODEV if the worker is gone
 * (caller falls back to the legacy in-spawner wait_stub_done_seccomp);
 * -EIO / -EPROTO on IPC failure (caller surfaces fatal_sigsegv).
 */
int worker_drive_vcpu_run(struct uml_pt_regs *regs,
			  int single_stepping,
			  int syscall_data_len)
{
	struct mm_struct *mm = current->mm;
	struct um_worker *w = mm ? mm->context.worker : NULL;
	struct mm_id *mm_id = mm ? &mm->context.id : NULL;
	struct worker_msg msg;
	int rc, n;

	if (!w)
		return -ENODEV;

	/*
	 * Ship any queued stub-syscall fds via SCM_RIGHTS now: those fds
	 * live in the spawner's FD table, and the stub child will
	 * recvmsg them after the worker's futex_wake. Mirrors the
	 * sendmsg in wait_stub_done_seccomp's !running prelude (the
	 * legacy WORKER_PROCESS=n path).
	 */
	if (mm_id && mm_id->syscall_fd_num)
		send_stub_syscall_fds(mm_id);

	memset(&msg, 0, sizeof(msg));
	msg.magic = WORKER_IPC_MAGIC;
	msg.type  = WORKER_MSG_VCPU_RUN;
	msg.task_handle = (u64)(unsigned long)current;
	msg.u.vcpu_run.regs_va         = (u64)(unsigned long)regs;
	msg.u.vcpu_run.single_stepping = single_stepping ? 1 : 0;
	msg.u.vcpu_run.syscall_data_len = (u32)syscall_data_len;

	rc = worker_send_msg_for_mm(mm, &msg);
	if (rc < 0)
		return rc;

	/*
	 * Direct synchronous read of the IPC socket — bypass the
	 * dispatcher kthread for the duration of this vcpu_run
	 * iteration. The single-task-per-mm assumption (E.3d.2; E.4
	 * generalizes it) means there is exactly one consumer at a
	 * time. Worker emits exactly one VCPU_DONE per VCPU_RUN; the
	 * post-trap signal dispatch (including handle_syscall for
	 * SIGSYS) runs in the spawner's seccomp_vcpu_run continuation,
	 * which is already on the originating guest task's `current`.
	 */
	for (;;) {
		n = os_read_file(w->ipc_sock, &msg, sizeof(msg));
		if (n == -EINTR)
			continue;
		if (n != sizeof(msg))
			return n < 0 ? n : -EIO;

		if (msg.magic != WORKER_IPC_MAGIC)
			return -EPROTO;

		if (msg.type == WORKER_MSG_VCPU_DONE)
			return (int)msg.u.vcpu_done.status;

		pr_warn_ratelimited("um: drv: unexpected msg type %u\n",
				    (unsigned)msg.type);
	}
}

/*
 * worker_smoke_test — drive the E.3b round-trip end-to-end.
 *
 * Spawns a worker (no mm_struct involved; the test is for the IPC
 * dispatcher itself), exchanges STUB_ALLOC_REQ + WRITE_REGS +
 * RETURN_VALUE, and verifies WRITE_REGS_ACK echoes the sentinel back
 * in slot 2 (HOST_AX). Reaps the worker before returning.
 *
 * Not auto-wired. E.3c will replace this with the dispatcher thread
 * that drives real syscall round-trips.
 */
int worker_smoke_test(void)
{
	struct worker_msg msg;
	int pid = -1, sock = -1, rc, n;
	const u64 sentinel = 0x1234ULL;

	if (!spawner_initialized)
		return -ENODEV;

	rc = spawn_worker_process(&pid, &sock);
	if (rc < 0) {
		pr_err("um: worker smoke: spawn failed: %d\n", rc);
		return rc;
	}

	memset(&msg, 0, sizeof(msg));
	msg.magic = WORKER_IPC_MAGIC;
	msg.type  = WORKER_MSG_STUB_ALLOC_REQ;
	msg.u.stub_alloc.pid              = (worker_u32)pid;
	msg.u.stub_alloc.stack            = 0;
	msg.u.stub_alloc.syscall_data_len = 0;
	msg.u.stub_alloc.sock             = (worker_u32)-1;
	msg.u.stub_alloc.syscall_fd_num   = 0;
	n = os_write_file(sock, &msg, sizeof(msg));
	if (n != sizeof(msg)) {
		rc = n < 0 ? n : -EIO;
		goto out;
	}

	memset(&msg, 0, sizeof(msg));
	msg.magic = WORKER_IPC_MAGIC;
	msg.type  = WORKER_MSG_WRITE_REGS;
	msg.u.regs.slot[0] = 0xdeadULL;	/* rip */
	msg.u.regs.slot[1] = 0xbeefULL;	/* rsp */
	n = os_write_file(sock, &msg, sizeof(msg));
	if (n != sizeof(msg)) {
		rc = n < 0 ? n : -EIO;
		goto out;
	}

	memset(&msg, 0, sizeof(msg));
	msg.magic = WORKER_IPC_MAGIC;
	msg.type  = WORKER_MSG_RETURN_VALUE;
	msg.u.retval.sentinel = sentinel;
	n = os_write_file(sock, &msg, sizeof(msg));
	if (n != sizeof(msg)) {
		rc = n < 0 ? n : -EIO;
		goto out;
	}

	memset(&msg, 0, sizeof(msg));
	n = os_read_file(sock, &msg, sizeof(msg));
	if (n != sizeof(msg)) {
		pr_err("um: worker smoke: short read (%d)\n", n);
		rc = n < 0 ? n : -EIO;
		goto out;
	}

	if (msg.magic != WORKER_IPC_MAGIC ||
	    msg.type  != WORKER_MSG_WRITE_REGS_ACK ||
	    msg.u.regs.slot[2] != sentinel) {
		pr_err("um: worker smoke: ack mismatch magic=0x%x type=%u slot2=0x%llx\n",
		       (unsigned)msg.magic, (unsigned)msg.type,
		       (unsigned long long)msg.u.regs.slot[2]);
		rc = -EPROTO;
		goto out;
	}

	pr_info("um: worker smoke: round-trip OK (sentinel 0x%llx)\n",
		(unsigned long long)sentinel);
	rc = 0;

out:
	reap_worker_process(pid, sock);
	return rc;
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

#ifdef CONFIG_UM_WORKER_SMOKE_TEST_ON_BOOT
static int __init spawner_run_smoke_test(void)
{
	int rc = worker_smoke_test();

	if (rc < 0)
		pr_err("um: worker smoke (boot): FAIL rc=%d\n", rc);
	return 0;
}
late_initcall_sync(spawner_run_smoke_test);
#endif
