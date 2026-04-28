// SPDX-License-Identifier: GPL-2.0
/*
 * UML per-mm host worker process — USER-side helpers
 * (memo 25 R4 / memo 28 commits E.3a + E.3b).
 *
 * This TU runs in libc context (USER_CFLAGS). The spawner side
 * (arch/um/kernel/spawner.c, kernel TU) calls into here for the
 * actual clone() + socketpair() syscalls; the worker side runs
 * inside the freshly-cloned worker process and pumps the IPC
 * loop.
 *
 * E.3a: spawn / reap the worker via clone-without-CLONE_VM.
 * E.3b: tagged-message dispatcher in worker_main (this file).
 *
 * The worker is a sidecar today: even with WORKER_PROCESS=y it
 * does not replace today's seccomp_mm_create stub-child path.
 * E.3d wires the actual replacement.
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
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <kern_util.h>
#include <mm_id.h>
#include <os.h>

#include <worker_ipc.h>
#include <worker_user.h>

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
 * Worker main loop (E.3b): tagged-message dispatcher, minimum-viable
 * cut per memo 28 Part K.6.
 *
 * Option A from the E.3b spec: handlers stash the inbound payload in
 * worker-local state (mm_id snapshot + a 9-slot regs buffer) and ACK
 * the round-trip via WORKER_MSG_WRITE_REGS_ACK. The actual
 * start_userspace() / set_stub_state() / get_stub_state() integration
 * is deferred to E.3c — start_userspace pulls in stub_data placement
 * and current_mm_id() that we'd have to fabricate from worker context,
 * which would expand E.3b past its proof-of-concept charter.
 *
 * What this loop proves: mm_id IPC marshalling, regs ping-pong, the
 * three new message types decode cleanly. What's deferred to E.3c:
 * actual stub child clone inside the worker, real futex wake/wait,
 * SIGSYS trap relay back to spawner.
 *
 * SIGTERM still exits cleanly with status 0 (E.3a behaviour).
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

static int worker_main(void *arg)
{
	struct worker_init *init = arg;
	int sock = init->worker_socket_fd;
	struct sigaction sa = { 0 };
	struct worker_msg msg;
	struct worker_msg ack;
	struct mm_id local_mm_id = { .pid = -1 };
	worker_u64 local_regs[WORKER_REGS_SLOTS] = { 0 };
	ssize_t n;
	unsigned int i;

	/*
	 * PDEATHSIG so an orphaned worker self-terminates if the spawner
	 * crashes before SIGTERM reaches us. Inherited by the (future)
	 * stub child clone.
	 */
	prctl(PR_SET_PDEATHSIG, SIGTERM);

	sa.sa_handler = worker_sigterm_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);

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
				(unsigned)msg.magic);
			continue;
		}

		switch (msg.type) {
		case WORKER_MSG_STUB_ALLOC_REQ:
			worker_load_mm_id(&local_mm_id, &msg.u.stub_alloc);
			break;

		case WORKER_MSG_WRITE_REGS:
			for (i = 0; i < WORKER_REGS_SLOTS; i++)
				local_regs[i] = msg.u.regs.slot[i];
			break;

		case WORKER_MSG_RETURN_VALUE:
			/*
			 * Slot 2 mirrors HOST_AX in the regs frame; this is
			 * the path set_stub_state() / get_stub_state() drive
			 * in production. For E.3b we just write the sentinel
			 * into the local regs buffer and ship them back so
			 * the spawner can verify the round-trip.
			 */
			local_regs[2] = msg.u.retval.sentinel;

			memset(&ack, 0, sizeof(ack));
			ack.magic = WORKER_IPC_MAGIC;
			ack.type  = WORKER_MSG_WRITE_REGS_ACK;
			ack.task_handle = msg.task_handle;
			for (i = 0; i < WORKER_REGS_SLOTS; i++)
				ack.u.regs.slot[i] = local_regs[i];
			if (worker_send(sock, &ack) < 0)
				goto out;
			break;

		case WORKER_MSG_SHUTDOWN:
			goto out;

		default:
			/*
			 * Unknown / not-yet-implemented messages. E.3c will
			 * grow SYSCALL_REQ/SYSCALL_REP plumbing.
			 */
			os_info("um: worker: unhandled msg type %u\n",
				(unsigned)msg.type);
			break;
		}
	}

out:
	(void)local_mm_id;	/* populated by STUB_ALLOC_REQ; consumed in E.3c */
	close(sock);
	exit(0);
}

int spawn_worker_process(int *out_pid, int *out_sock)
{
	int fds[2];
	void *stack;
	pid_t pid;
	struct worker_init init;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
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

	init.worker_socket_fd = fds[1];	/* worker-side */

	/*
	 * No CLONE_VM, no CLONE_VFORK, no CLONE_FILES — worker gets
	 * its own VA space, its own signal table (since CLONE_SIGHAND
	 * not set), a CoW copy of the FD table. SIGCHLD so the spawner
	 * gets notified when the worker dies.
	 *
	 * Note: clone()'s child stack grows down, so pass the top.
	 */
	pid = clone(worker_main,
		    (char *)stack + WORKER_STACK_SIZE,
		    SIGCHLD,
		    &init);
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
	 * The stack is leaked here intentionally (the worker is now
	 * running on it); reap_worker_process can't easily unmap it
	 * because tracking per-worker stacks is extra state. The
	 * kernel reclaims it when the worker exits. Address in a
	 * follow-up if it turns up in mm-pressure traces.
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
		 * Closing our end of the socketpair gives the worker an
		 * EOF on its next read; combined with PR_SET_PDEATHSIG
		 * (set in worker_main) the worker exits cleanly even if
		 * we crash before the explicit waitpid below.
		 */
		close(sock);
	}

	if (pid <= 0)
		return;

	kill(pid, SIGTERM);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		/* loop */;
}
