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
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait.h>

#include <os.h>
#include <skas.h>
#include <mm_id.h>
#include <sysdep/ptrace.h>
#include <sysdep/ptrace_user.h>
#include <worker_api.h>
#include <worker_ipc.h>
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
	struct task_struct	*dispatcher;	/* per-worker IPC kthread (E.3c); NULL otherwise */

	/*
	 * E.3d.1 wait-queue bounce (memo 28 Part C.E).
	 *
	 * `pending_reqs` is a FIFO of SYSCALL_REQs the dispatcher has
	 * received but not yet executed; `pending_lock` protects the
	 * list head. `reply_wait` wakes any task blocked in
	 * worker_run_pending_syscalls when either a new request lands
	 * or the dispatcher exits.
	 *
	 * Lock-ordering rule: pending_lock is INNERMOST. Never acquire
	 * pending_lock while holding workers_lock. The dispatcher runs
	 * outside workers_lock, and reap_worker_for_mm only touches
	 * pending_reqs after the dispatcher has fully exited (kthread_stop
	 * returned), so the two locks never nest in practice — this rule
	 * is asserted by construction in the call sites below.
	 */
	wait_queue_head_t	reply_wait;
	struct list_head	pending_reqs;
	spinlock_t		pending_lock;

	/*
	 * E.3d.2 vcpu_run completion. The dispatcher routes VCPU_DONE
	 * out-of-band to a completion rather than into pending_reqs:
	 * pending_reqs is a queue of equally-shaped SYSCALL_REQs that
	 * any draining task can pick up, but VCPU_DONE is the
	 * end-of-iteration edge for the one task currently in
	 * worker_drive_vcpu_run.
	 */
	struct completion	vcpu_done;
	int			vcpu_done_status;
};

/*
 * One pending SYSCALL_REQ entry. Lives on um_worker::pending_reqs.
 *
 * We allocate one per inbound SYSCALL_REQ (the dispatcher copies the
 * full message into it so it can release its stack frame). The
 * draining task in worker_run_pending_syscalls dequeues, runs
 * handle_syscall, replies, then kfrees.
 */
struct worker_pending_req {
	struct list_head		list;
	u64				task_handle;
	struct worker_msg_syscall	syscall;
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
 * Per-worker IPC dispatcher kthread (memo 28 E.3c / Part C.D / Part K.5
 * / E.3d.1 Part C.E).
 *
 * Routing-only after E.3d.1. handle_syscall is NOT called here: its
 * sys_call_table[] callees deref `current` heavily (credentials, files,
 * fs, signals, ptrace, seccomp), and the dispatcher's task_struct is a
 * kthread, not the originating guest task. Instead we copy each
 * SYSCALL_REQ into a worker_pending_req, link it on the worker's
 * pending_reqs FIFO, and wake the originating guest task that's
 * blocked in worker_run_pending_syscalls — it runs handle_syscall on
 * its own kernel stack under its own `current` and ships SYSCALL_REP
 * back over the socket.
 *
 * Exit semantics:
 *  (a) kthread_stop() bumps should_stop; reap_worker_for_mm calls it
 *      before reap_worker_process closes the socket. We loop on
 *      kthread_should_stop after every short / EINTR read so the stop
 *      is observed even if a partial read happened.
 *  (b) Socket close from the spawner side (reap path) gives os_read_file
 *      a 0 return (EOF) once the worker drains and closes its end, or
 *      -EBADF after our own close — either path falls through to the
 *      same exit branch.
 *  (c) Explicit WORKER_MSG_SHUTDOWN: not produced by E.3b/E.3c paths
 *      yet but handled defensively so the dispatcher can be retired
 *      out-of-band by future spawner-initiated tear-down.
 *
 * On exit we wake_up(&w->reply_wait) so any task blocked in
 * worker_run_pending_syscalls observes the dispatcher-gone condition
 * and returns -ENODEV.
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
		case WORKER_MSG_SYSCALL_REQ: {
			struct worker_pending_req *req;

			/*
			 * GFP_KERNEL is fine: the dispatcher kthread has no
			 * atomic-context constraints (no spinlocks held, no
			 * IRQ context). Allocation failure drops the request
			 * — the originating guest task will hang waiting for
			 * a reply, which under E.3d.2 surfaces as a stalled
			 * syscall. Better than silently producing a wrong
			 * answer.
			 */
			req = kmalloc(sizeof(*req), GFP_KERNEL);
			if (!req) {
				pr_warn_ratelimited("um: worker disp: OOM dropping SYSCALL_REQ task=%llu\n",
						    (unsigned long long)msg.task_handle);
				break;
			}

			req->task_handle = msg.task_handle;
			req->syscall     = msg.u.syscall;

			scoped_guard(spinlock, &w->pending_lock) {
				list_add_tail(&req->list, &w->pending_reqs);
			}
			wake_up(&w->reply_wait);
			break;
		}
		case WORKER_MSG_VCPU_DONE:
			/*
			 * E.3d.2 leaves the dispatcher idle while
			 * worker_drive_vcpu_run owns the socket directly, so
			 * VCPU_DONE should never reach this kthread. Keep a
			 * defensive completion-signal in place for the
			 * out-of-band case (e.g., a future async path injecting
			 * a stub-loss notification while no vcpu_run is active).
			 */
			w->vcpu_done_status = (int)msg.u.vcpu_done.status;
			complete(&w->vcpu_done);
			break;
		case WORKER_MSG_SHUTDOWN:
			goto out;
		default:
			pr_warn_ratelimited("um: worker disp: unhandled msg type %u\n",
					    (unsigned)msg.type);
			break;
		}
	}
out:
	/*
	 * Wake any task blocked in worker_run_pending_syscalls so it can
	 * observe the dispatcher-gone condition (we're about to return,
	 * so kthread_should_stop is on the way to true OR we've hit EOF).
	 * The waiter checks list_empty + dispatcher state under
	 * pending_lock; the wake is the edge it observes.
	 */
	wake_up(&w->reply_wait);
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

	init_waitqueue_head(&w->reply_wait);
	INIT_LIST_HEAD(&w->pending_reqs);
	spin_lock_init(&w->pending_lock);
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
	struct worker_pending_req *req, *tmp;
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

	/*
	 * Now that the dispatcher has fully exited (kthread_stop returned)
	 * no one else is touching pending_reqs — drain whatever is left.
	 * Wake any straggling waiter one more time (the dispatcher's exit
	 * path already did this, but cheap and idempotent) so that a task
	 * which raced into worker_run_pending_syscalls between the
	 * list_del above and now observes -ENODEV via list_empty +
	 * dispatcher == NULL.
	 *
	 * Lock ordering: we are NOT holding workers_lock here (the
	 * scoped_guard above released it at the end of its block). Taking
	 * pending_lock is therefore unconstrained — see the lock-ordering
	 * note on struct um_worker.
	 */
	scoped_guard(spinlock, &w->pending_lock) {
		list_for_each_entry_safe(req, tmp, &w->pending_reqs, list) {
			list_del(&req->list);
			kfree(req);
		}
	}
	wake_up(&w->reply_wait);

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
	 * E.3d.2: vcpu_run directly reads/writes the IPC socket from
	 * the originating guest task's context (single-consumer model;
	 * E.4 generalizes when multiple tasks share one mm). The
	 * dispatcher kthread that E.3d.1 introduced is therefore not
	 * started here — leaving w->dispatcher == NULL keeps
	 * reap_worker_for_mm's kthread_stop branch a no-op. E.3d.1's
	 * smoke-test scaffold (worker_run_pending_syscalls) returns
	 * -ENODEV against a NULL dispatcher, which is the correct
	 * "no kthread routing today" answer.
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
 * E.3d's seccomp integration is the production caller: it builds a
 * SYSCALL_REQ and waits for SYSCALL_REP. For E.3b this is also the
 * primitive driving worker_smoke_test(). Returns 0 on success or a
 * negative errno; the caller is responsible for reading the reply
 * separately if one is expected.
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
 * Drain one worker_pending_req from `w` and execute the syscall under
 * the calling task's `current` (memo 28 Part C.E "wait-queue bounce").
 *
 * Returns 0 if a request was consumed (caller should keep looping),
 * -EAGAIN if the queue was empty AND the dispatcher is still alive
 * (caller should re-block on reply_wait), or -ENODEV if the
 * dispatcher has exited and the queue is drained (caller should
 * return).
 *
 * Why pt_regs on the stack: handle_syscall does container_of on its
 * uml_pt_regs argument. The fp[] flex tail is unused by the syscall
 * dispatch path itself (Part K.5), so a stack-local pt_regs without
 * FP storage is sufficient for routing. E.3d.2's vcpu_run path will
 * pass the trapping task's actual pt_regs once that lands.
 */
static int worker_run_one_pending(struct um_worker *w)
{
	struct worker_pending_req *req;
	struct pt_regs regs;
	struct worker_msg rep;
	int n;

	scoped_guard(spinlock, &w->pending_lock) {
		req = list_first_entry_or_null(&w->pending_reqs,
					       struct worker_pending_req, list);
		if (req)
			list_del(&req->list);
	}

	if (!req) {
		/*
		 * Queue is empty. If the dispatcher has already returned,
		 * the wake from the exit path means no more requests will
		 * arrive — caller exits with -ENODEV. Otherwise caller
		 * blocks again.
		 *
		 * READ_ONCE keeps the compiler from CSE'ing this against
		 * an earlier load; the dispatcher field is set/cleared
		 * with normal stores under workers_lock or kthread
		 * lifecycle but for this read we only care about NULL vs
		 * non-NULL atomicity, not full ordering.
		 */
		return READ_ONCE(w->dispatcher) ? -EAGAIN : -ENODEV;
	}

	memset(&regs, 0, sizeof(regs));
	regs.regs.is_user = 1;
	regs.regs.gp[HOST_ORIG_AX] = req->syscall.nr;
	regs.regs.gp[HOST_DI]      = req->syscall.args[0];
	regs.regs.gp[HOST_SI]      = req->syscall.args[1];
	regs.regs.gp[HOST_DX]      = req->syscall.args[2];
	regs.regs.gp[HOST_R10]     = req->syscall.args[3];
	regs.regs.gp[HOST_R8]      = req->syscall.args[4];
	regs.regs.gp[HOST_R9]      = req->syscall.args[5];

	/*
	 * THIS is the whole point of E.3d.1: handle_syscall runs under
	 * the calling task's `current`. credentials, files, fs, signals,
	 * seccomp, ptrace — all see the originating guest task, exactly
	 * as today's stub-child-in-spawner path does.
	 */
	handle_syscall(&regs.regs);

	memset(&rep, 0, sizeof(rep));
	rep.magic       = WORKER_IPC_MAGIC;
	rep.type        = WORKER_MSG_SYSCALL_REP;
	rep.task_handle = req->task_handle;
	rep.u.reply.retval = regs.regs.gp[HOST_AX];

	n = os_write_file(w->ipc_sock, &rep, sizeof(rep));
	if (n != sizeof(rep))
		pr_warn_ratelimited("um: worker run: SYSCALL_REP write failed %d\n",
				    n);

	kfree(req);
	return 0;
}

int worker_run_pending_syscalls(struct mm_struct *mm)
{
	struct um_worker *w = mm ? mm->context.worker : NULL;
	int rc;

	if (!w)
		return -ENODEV;

	/*
	 * Lifetime contract (E.3d.2 callers must honour): the worker
	 * pointer is stable for the duration of this call because
	 * reap_worker_for_mm only fires from mm-destroy paths after all
	 * guest tasks belonging to `mm` have stopped — i.e. no task
	 * inside this loop. Until E.3d.2 plumbs that lifecycle through
	 * vcpu_run we have no production caller, so the contract is
	 * stated here and verified by audit when E.3d.2 lands.
	 */
	for (;;) {
		rc = worker_run_one_pending(w);
		if (rc == 0)
			continue;
		if (rc == -ENODEV)
			return -ENODEV;

		/*
		 * Empty queue, dispatcher alive: block until either a
		 * request lands or the dispatcher exits. Interruptible
		 * so a fatal signal terminates the loop cleanly.
		 *
		 * We must re-check both conditions inside the wait
		 * predicate to avoid losing a wake that fires between
		 * worker_run_one_pending's check and the schedule().
		 */
		rc = wait_event_interruptible(
			w->reply_wait,
			!list_empty(&w->pending_reqs) ||
				READ_ONCE(w->dispatcher) == NULL);
		if (rc == -ERESTARTSYS)
			return -EINTR;
	}
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
