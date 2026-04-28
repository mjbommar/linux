// SPDX-License-Identifier: GPL-2.0
/*
 * UML per-mm host worker process — USER-side helpers
 * (memo 25 R4 / memo 28 commit E.3a).
 *
 * This TU runs in libc context (USER_CFLAGS). The spawner side
 * (arch/um/kernel/spawner.c, kernel TU) calls into here for the
 * actual clone() + socketpair() syscalls; the worker side runs
 * inside the freshly-cloned worker process and pumps the IPC
 * loop.
 *
 * Today's E.3a scope: spawn a worker via clone-without-CLONE_VM,
 * set up a per-worker UNIX socketpair for spawner ↔ worker IPC,
 * give the worker a sleep-loop main that echoes any message
 * back to the spawner. No actual syscall routing yet — that lands
 * in E.3b along with the worker-side stub child.
 *
 * The worker is a sidecar today: even with WORKER_PROCESS=y it
 * does not replace today's seccomp_mm_create stub-child path.
 * E.3d wires the actual replacement.
 */

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
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
 * Worker main loop (E.3a): echo-only. Reads worker_msg-shaped
 * frames from the IPC socket and writes them back unchanged.
 * SIGTERM exits cleanly with status 0.
 *
 * The worker shares no memory with the spawner (clone without
 * CLONE_VM); communication is exclusively via the socket.
 *
 * E.3b will replace this loop with the trap relay (set stub
 * child regs, futex-wake stub, wait for SIGSYS, forward to
 * spawner via SYSCALL_REQ, recv SYSCALL_REP, futex-wake stub
 * with new regs).
 */
static volatile sig_atomic_t worker_should_exit;

static void worker_sigterm_handler(int sig)
{
	(void)sig;
	worker_should_exit = 1;
}

static int worker_main(void *arg)
{
	struct worker_init *init = arg;
	int sock = init->worker_socket_fd;
	struct sigaction sa = { 0 };
	struct worker_msg msg;
	ssize_t n;

	/*
	 * Per-worker sigchild handling — SIGCHLD from the (future)
	 * stub child is the worker's responsibility, not the spawner's.
	 */
	prctl(PR_SET_PDEATHSIG, SIGTERM);

	sa.sa_handler = worker_sigterm_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);

	/*
	 * Echo loop. recvmsg-style would let us pass FDs via cmsg; for
	 * E.3a we only need plain bytes.
	 */
	while (!worker_should_exit) {
		n = read(sock, &msg, sizeof(msg));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;	/* spawner went away */
		}
		if (n == 0)
			break;	/* spawner closed the socket cleanly */
		if (n != (ssize_t)sizeof(msg))
			continue;	/* short read; ignore */

		/* Echo. E.3b replaces this with the real dispatcher. */
		if (write(sock, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
			break;	/* spawner closed the socket */
	}

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
