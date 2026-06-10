// SPDX-License-Identifier: GPL-2.0
/*
 * UML per-mm host worker process, host-side helpers.
 *
 * This file is built with libc available. arch/um/kernel/spawner.c
 * calls into here for the actual clone() + socketpair() syscalls; the
 * worker side runs inside the freshly-cloned worker process and pumps
 * the IPC loop.
 *
 * STUB_ALLOC_REQ drives start_userspace() inside the worker's own VA
 * and ships the parent-side socketpair fd back via SCM_RIGHTS.
 */

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/futex.h>

#include <kern_util.h>
#include <mm_id.h>
#include <os.h>
#include <stub-data.h>
#include <sysdep/mcontext.h>
#include <sysdep/ptrace.h>

#include <worker_ipc.h>
#include <worker_user.h>

#include "internal.h"

/*
 * Pre-clone payload handed to worker_main via clone()'s 4th arg.
 * Lives on the spawner-side stack until clone() returns; worker
 * copies what it needs to its own stack and stops touching the
 * pointer.
 */
struct worker_init {
	int worker_socket_fd;	/* worker's end of the socketpair */
};

/*
 * Worker stack. Allocated per-spawn via mmap; freed on reap.
 */
#define WORKER_STACK_SIZE	(64 * 1024)

/*
 * Worker main loop: tagged-message dispatcher.
 *
 * STUB_ALLOC_REQ starts userspace inside the worker's own VA, with the
 * resulting mm_id and parent-side socketpair fd shipped back to the
 * spawner via a STUB_ALLOC_REP reply (SCM_RIGHTS for the fd). WRITE_REGS
 * and RETURN_VALUE provide simple register round-trip messages.
 *
 * SIGTERM exits cleanly with status 0.
 */
static volatile sig_atomic_t worker_should_exit;

static void worker_sigterm_handler(int sig)
{
	(void)sig;
	worker_should_exit = 1;
}

static void worker_load_mm_id(struct mm_id *dst,
			      const struct worker_msg_stub_alloc *src)
{
	int i;

	dst->pid              = (int)src->pid;
	dst->stack            = (unsigned long)src->stack;
	dst->syscall_data_len = (int)src->syscall_data_len;
	dst->sock             = (int)src->sock;
	dst->syscall_fd_num   = (int)src->syscall_fd_num;

	for (i = 0; i < STUB_MAX_FDS; i++)
		dst->syscall_fd_map[i] = (int)src->syscall_fd_map[i];
}

static int worker_send(int sock, const struct worker_msg *msg)
{
	ssize_t n = write(sock, msg, sizeof(*msg));

	if (n != (ssize_t)sizeof(*msg))
		return -1;
	return 0;
}

/*
 * Send msg over sock and (if fd >= 0) attach fd via SCM_RIGHTS
 * on the cmsg channel. The receiver's recvmsg installs the fd in its
 * own FD table; the sender retains ownership of its descriptor.
 *
 * Used by the STUB_ALLOC_REP path so the spawner can populate
 * mm_id->sock with the worker-allocated parent-side socketpair fd.
 */
static int worker_send_with_fd(int sock, const struct worker_msg *msg, int fd)
{
	struct iovec iov = { .iov_base = (void *)msg, .iov_len = sizeof(*msg) };
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} cu = { 0 };
	struct msghdr m = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	ssize_t n;

	if (fd >= 0) {
		struct cmsghdr *c;

		m.msg_control    = cu.buf;
		m.msg_controllen = sizeof(cu.buf);
		c = CMSG_FIRSTHDR(&m);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type  = SCM_RIGHTS;
		c->cmsg_len   = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(c), &fd, sizeof(int));
	}

	n = sendmsg(sock, &m, 0);
	if (n != (ssize_t)sizeof(*msg))
		return -1;
	return 0;
}

/*
 * Drive one futex round-trip with the worker's stub child. The spawner
 * has already done set_stub_state and (for
 * batched stub-syscall cases) the sendmsg for syscall_fd_map[]; this
 * function flips data->futex from FUTEX_IN_KERN to FUTEX_IN_CHILD,
 * wakes the stub, and blocks until the stub re-traps. The spawner's
 * seccomp_vcpu_run runs get_stub_state and the post-trap signal
 * dispatch; same one-trap-per-call shape as the WORKER_PROCESS=n path.
 * The worker exists in the loop because the stub child is a
 * CLONE_VM peer of the worker (not the spawner): SIGCHLD on stub
 * crash and signal-mask ownership of the trap-reception path are
 * worker-local concerns.
 *
 * Returns 0 if the stub trapped cleanly; -EAGAIN if the stub child
 * pid went negative (spawner surfaces as fatal_sigsegv).
 */
static int worker_vcpu_trap_iter(struct mm_id *local_mm_id)
{
	struct stub_data *data = (void *)local_mm_id->stack;
	int ret;

	/*
	 * Spawner has already done set_stub_state. We do the
	 * data->signal / data->futex prelude + futex round-trip here
	 * (the stub child is a CLONE_VM peer of this worker, so the
	 * wake/wait cycle should run from the worker's address space).
	 * Cross-process futex on the shared memfd page works either
	 * way; locating it in the worker keeps signal-mask ownership with
	 * the process that owns the stub.
	 */
	data->signal = 0;
	data->futex  = FUTEX_IN_CHILD;

	syscall(__NR_futex, &data->futex, FUTEX_WAKE, 1, NULL, NULL, 0);

	for (;;) {
		if (UM_USER_READ_ONCE(local_mm_id->pid) < 0)
			return -EAGAIN;

		ret = syscall(__NR_futex, &data->futex,
			      FUTEX_WAIT, FUTEX_IN_CHILD,
			      NULL, NULL, 0);
		if (ret < 0 && errno != EINTR && errno != EAGAIN)
			return -EAGAIN;
		if (data->futex != FUTEX_IN_CHILD)
			break;
	}

	if (UM_USER_READ_ONCE(local_mm_id->pid) < 0)
		return -EAGAIN;

	if (data->mctx_offset > sizeof(data->sigstack) - sizeof(mcontext_t))
		return -EAGAIN;

	return 0;
}

static void worker_init_ack(struct worker_msg *ack,
			    enum worker_msg_type type,
			    worker_u64 task_handle)
{
	memset(ack, 0, sizeof(*ack));
	ack->magic = WORKER_IPC_MAGIC;
	ack->type = type;
	ack->task_handle = task_handle;
}

static int worker_handle_stub_alloc(int sock, struct mm_id *local_mm_id,
				    const struct worker_msg *msg)
{
	struct worker_msg ack;
	unsigned int i;

	/*
	 * Bring up a real stub child inside the worker's own VA.
	 * start_userspace clones the stub child with CLONE_VM against the
	 * worker, runs userspace_tramp init, then waits for the
	 * FUTEX_IN_CHILD handshake. On success local_mm_id has a valid .sock
	 * for the per-mm socketpair; pass that fd to the spawner via
	 * SCM_RIGHTS so the spawner-side mm_id resolves to the same kernel
	 * file as the worker's.
	 *
	 * Globals like stub_exe_fd and um_backend->* are already
	 * CoW-inherited from the spawner, so userspace_tramp can run inside
	 * the worker.
	 *
	 * A zero stack marks a dry-run allocation request; skip
	 * start_userspace and keep the zeroed snapshot. worker_alloc_stub_for_mm()
	 * always passes a non-zero stack.
	 */
	worker_load_mm_id(local_mm_id, &msg->u.stub_alloc);

	if (local_mm_id->stack == 0)
		return 0;

	if (start_userspace(local_mm_id) != 0) {
		worker_init_ack(&ack, WORKER_MSG_STUB_ALLOC_REP,
				msg->task_handle);
		ack.u.stub_alloc_rep.status = (worker_u32)(-1);
		return worker_send(sock, &ack);
	}

	worker_init_ack(&ack, WORKER_MSG_STUB_ALLOC_REP, msg->task_handle);
	ack.u.stub_alloc_rep.stack = (worker_u64)local_mm_id->stack;
	ack.u.stub_alloc_rep.pid = (worker_u32)local_mm_id->pid;
	ack.u.stub_alloc_rep.syscall_data_len =
		(worker_u32)local_mm_id->syscall_data_len;
	ack.u.stub_alloc_rep.syscall_fd_num =
		(worker_u32)local_mm_id->syscall_fd_num;
	for (i = 0; i < STUB_MAX_FDS; i++)
		ack.u.stub_alloc_rep.syscall_fd_map[i] =
			(worker_u32)local_mm_id->syscall_fd_map[i];
	ack.u.stub_alloc_rep.status = 0;

	return worker_send_with_fd(sock, &ack, local_mm_id->sock);
}

static int worker_handle_vcpu_run(int sock, struct mm_id *local_mm_id,
				  const struct worker_msg *msg)
{
	struct worker_msg ack;
	int rc;

	/*
	 * The spawner has already done set_stub_state. We own the
	 * data->signal/data->futex prelude + futex round trip because the
	 * stub child is a CLONE_VM peer of this worker, not the spawner. Once
	 * the stub re-traps the spawner reads back proc_data and calls
	 * get_stub_state + signal dispatch.
	 */
	rc = worker_vcpu_trap_iter(local_mm_id);

	worker_init_ack(&ack, WORKER_MSG_VCPU_DONE, msg->task_handle);
	ack.u.vcpu_done.status = (worker_u32)rc;
	return worker_send(sock, &ack);
}

static int worker_handle_return_value(int sock, worker_u64 *local_regs,
				      const struct worker_msg *msg)
{
	struct worker_msg ack;
	unsigned int i;

	/*
	 * Slot 2 mirrors HOST_AX in the regs frame; this is the path
	 * set_stub_state() / get_stub_state() drive. The sentinel path writes
	 * into the local regs buffer and ships it back so the spawner can
	 * verify the round-trip.
	 */
	local_regs[2] = msg->u.retval.sentinel;

	worker_init_ack(&ack, WORKER_MSG_WRITE_REGS_ACK, msg->task_handle);
	for (i = 0; i < WORKER_REGS_SLOTS; i++)
		ack.u.regs.slot[i] = local_regs[i];
	return worker_send(sock, &ack);
}

static int worker_dispatch_msg(int sock, const struct worker_msg *msg,
			       struct mm_id *local_mm_id,
			       worker_u64 *local_regs)
{
	unsigned int i;

	switch (msg->type) {
	case WORKER_MSG_STUB_ALLOC_REQ:
		return worker_handle_stub_alloc(sock, local_mm_id, msg);
	case WORKER_MSG_WRITE_REGS:
		for (i = 0; i < WORKER_REGS_SLOTS; i++)
			local_regs[i] = msg->u.regs.slot[i];
		return 0;
	case WORKER_MSG_VCPU_RUN:
		return worker_handle_vcpu_run(sock, local_mm_id, msg);
	case WORKER_MSG_RETURN_VALUE:
		return worker_handle_return_value(sock, local_regs, msg);
	case WORKER_MSG_SHUTDOWN:
		return 1;
	default:
		os_info("um: worker: unhandled msg type %u\n",
			(unsigned int)msg->type);
		return 0;
	}
}

static void worker_install_signal_handlers(void)
{
	struct sigaction sa = { 0 };

	sa.sa_handler = worker_sigterm_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
}

static int worker_main(void *arg)
{
	struct worker_init *init = arg;
	int sock = init->worker_socket_fd;
	struct worker_msg msg;
	struct mm_id local_mm_id = { .pid = -1 };
	worker_u64 local_regs[WORKER_REGS_SLOTS] = { 0 };
	ssize_t n;

	/*
	 * PDEATHSIG so an orphaned worker self-terminates if the spawner
	 * crashes before SIGTERM is delivered. Stub child clones inherit it.
	 */
	prctl(PR_SET_PDEATHSIG, SIGTERM);

	worker_install_signal_handlers();

	while (!worker_should_exit) {
		n = read(sock, &msg, sizeof(msg));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		if (n != (ssize_t)sizeof(msg))
			continue;

		if (msg.magic != WORKER_IPC_MAGIC) {
			os_info("um: worker: bad ipc magic 0x%x, dropping\n",
				(unsigned int)msg.magic);
			continue;
		}

		if (worker_dispatch_msg(sock, &msg, &local_mm_id,
					local_regs) != 0)
			goto out;
	}

out:
	/*
	 * local_mm_id holds the worker's stub-child handle from
	 * start_userspace(); leave it allocated until exit. Process exit
	 * reaps the stub child via the inherited PR_SET_PDEATHSIG.
	 */
	close(sock);
	exit(0);
}

int spawn_worker_process(int *out_pid, int *out_sock)
{
	int fds[2];
	void *stack;
	pid_t pid;
	struct worker_init *init;

	/*
	 * Use SOCK_CLOEXEC on both ends of the socketpair. Without it,
	 * every spawner-side fds[0] (kept open for the lifetime of the
	 * worker) is inherited via the CoW fd-table by subsequent clones,
	 * then carried through start_userspace's stub-child clone,
	 * and finally lands in the user process's fd table at
	 * execve. Guest code that happens to recvmsg on one of those
	 * leaked fd numbers can block forever waiting for an IPC message
	 * that the kernel-side dispatcher kthread is supposed to receive.
	 *
	 * SOCK_CLOEXEC closes both ends at the next execve in any
	 * descendant. The worker_main() loop never execs (it's
	 * a clone child running umlctl logic in a read() loop), so
	 * the CLOEXEC bit is harmless to it. start_userspace's
	 * stub-child execve closes the leaked fds before user code
	 * sees them.
	 */
	if (socketpair(AF_UNIX, SOCK_STREAM, SOCK_CLOEXEC, fds) < 0)
		return -errno;

	stack = mmap(NULL, WORKER_STACK_SIZE,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
		     -1, 0);
	if (stack == MAP_FAILED) {
		int err = -errno;

		close(fds[0]);
		close(fds[1]);
		return err;
	}

	/*
	 * Place the init payload at the BOTTOM of the worker's own
	 * stack; guaranteed to be in a page the worker can read
	 * without depending on the spawner's stack-page CoW state
	 * (the spawner's call stack lives in UML-kernel-task storage
	 * that is unstable across the spawner's subsequent activity;
	 * the worker observed init.worker_socket_fd zeroed out by
	 * the time it dereferenced &init on the spawner stack).
	 */
	init = stack;
	init->worker_socket_fd = fds[1];

	/*
	 * No CLONE_VM, no CLONE_VFORK, no CLONE_FILES; worker gets
	 * its own VA space, its own signal table (since CLONE_SIGHAND
	 * not set), a CoW copy of the FD table. SIGCHLD so the spawner
	 * gets notified when the worker dies.
	 *
	 * Note: clone()'s child stack grows down, so pass the top.
	 */
	pid = clone(worker_main,
		    (char *)stack + WORKER_STACK_SIZE,
		    SIGCHLD,
		    init);
	if (pid < 0) {
		int err = -errno;

		munmap(stack, WORKER_STACK_SIZE);
		close(fds[0]);
		close(fds[1]);
		return err;
	}

	/*
	 * Spawner closes the worker-side fd; worker keeps it open via
	 * its own copy of the FD table. fds[0] is the spawner's end.
	 *
	 * Keep the parent mapping for the life of the worker handle.
	 * The child has its own mapping after clone without CLONE_VM;
	 * the parent side is a fixed-size bookkeeping cost.
	 */
	close(fds[1]);
	(void)stack;	/* leaked until worker-process exit */

	*out_pid  = pid;
	*out_sock = fds[0];

	return 0;
}

void reap_worker_process(int pid, int sock)
{
	int status;

	if (sock >= 0) {
		/*
		 * Closing this end of the socketpair gives the worker an EOF on
		 * its next read; combined with PR_SET_PDEATHSIG (set in
		 * worker_main) the worker exits cleanly even if this process
		 * exits before the explicit waitpid below.
		 */
		close(sock);
	}

	if (pid <= 0)
		return;

	kill(pid, SIGTERM);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		continue;
}
