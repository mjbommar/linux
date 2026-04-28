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

#include <os.h>
#include <skas.h>
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
	 * Future fields (E.4+) go here:
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
 * Per-worker IPC dispatcher kthread (memo 28 E.3c / Part C.D / Part K.5).
 *
 * Why a kthread: handle_syscall takes a uml_pt_regs and operates only
 * on its argument — no current/task_struct dereference for the syscall
 * path itself (Part K.5). A kernel thread therefore has full access to
 * UML's syscall dispatch surface without bouncing through the
 * originating task. One thread per worker (Part C.D).
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
			/*
			 * struct pt_regs wraps a single uml_pt_regs and is
			 * what handle_syscall does container_of on. The fp[]
			 * flex tail is unused by the syscall dispatch path
			 * (Part K.5), so a stack-local pt_regs without FP is
			 * safe for E.3c smoke purposes; E.3d will route real
			 * traps which already carry properly-sized regs.
			 */
			struct pt_regs regs;
			struct worker_msg rep;

			memset(&regs, 0, sizeof(regs));
			regs.regs.is_user = 1;
			regs.regs.gp[HOST_ORIG_AX] = msg.u.syscall.nr;
			regs.regs.gp[HOST_DI]      = msg.u.syscall.args[0];
			regs.regs.gp[HOST_SI]      = msg.u.syscall.args[1];
			regs.regs.gp[HOST_DX]      = msg.u.syscall.args[2];
			regs.regs.gp[HOST_R10]     = msg.u.syscall.args[3];
			regs.regs.gp[HOST_R8]      = msg.u.syscall.args[4];
			regs.regs.gp[HOST_R9]      = msg.u.syscall.args[5];

			handle_syscall(&regs.regs);

			memset(&rep, 0, sizeof(rep));
			rep.magic = WORKER_IPC_MAGIC;
			rep.type  = WORKER_MSG_SYSCALL_REP;
			rep.task_handle = msg.task_handle;
			rep.u.reply.retval = regs.regs.gp[HOST_AX];

			n = os_write_file(w->ipc_sock, &rep, sizeof(rep));
			if (n != sizeof(rep)) {
				pr_warn_ratelimited("um: worker disp: SYSCALL_REP write failed %d\n",
						    n);
				if (n < 0 && n != -EINTR)
					return 0;
			}
			break;
		}
		case WORKER_MSG_SHUTDOWN:
			return 0;
		default:
			pr_warn_ratelimited("um: worker disp: unhandled msg type %u\n",
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

	w->dispatcher = kthread_run(worker_dispatcher_fn, w,
				    "um-worker-disp/%d", w->pid);
	if (IS_ERR(w->dispatcher)) {
		rc = PTR_ERR(w->dispatcher);
		w->dispatcher = NULL;
		reap_worker_process(w->pid, w->ipc_sock);
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
