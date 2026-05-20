// SPDX-License-Identifier: GPL-2.0
/*
 * UML template-pause user-space helpers (Memo 09 Phase 1a).
 *
 * Thin host-syscall wrappers that the in-kernel template_pause path
 * (arch/um/kernel/template_pause.c) needs to reach across the
 * os-Linux boundary:
 *
 *   os_template_pause_stop_self():
 *	Raise SIGSTOP on the current host process and return when the
 *	supervisor SIGCONTs us (which, for a forked child, happens
 *	after the supervisor has written the per-child identity blob
 *	to the memfd at UM_TEMPLATE_IDENTITY_FD).
 *
 *   os_template_pause_identity_fd():
 *	Resolve UM_TEMPLATE_IDENTITY_FD from the host environment,
 *	validate it is open, and return it.  Returns -ENOENT if the
 *	env var is unset (no identity-blob channel — pool master
 *	before fork, or non-pool boot) or -EBADF if the fd is bogus.
 *
 *   os_template_pause_read_identity(fd, buf, len):
 *	Read up to @len bytes from a memfd; lseek to 0 first so the
 *	supervisor can rewrite the blob between takes without
 *	creating a fresh fd.  Short reads (memfd shorter than @len)
 *	return the bytes actually read; -errno on host failure.
 *
 * These are USER_OBJS-scope helpers — they must not call into kernel
 * code.  The in-kernel driver lives in arch/um/kernel/template_pause.c.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <linux/types.h>

#include <os.h>

#define UM_TEMPLATE_IDENTITY_FD_ENV "UM_TEMPLATE_IDENTITY_FD"

/*
 * SIGSTOP on self.  SIGSTOP cannot be masked or caught — the host
 * kernel suspends the process unconditionally, and we resume here
 * only when SIGCONT arrives.  No sigsuspend() needed: kill() returns
 * after the stop+resume cycle as if the call had blocked for the
 * duration of the suspension.
 *
 * Rationale for kill(getpid(), ...) over raise():
 *   - raise() is libc's wrapper around tgkill(getpid(), gettid(),
 *     ...) and uses pthread state we don't want to depend on at
 *     this point.  Plain kill(getpid(), SIGSTOP) is the simplest
 *     possible primitive and is what existing UML code uses
 *     (compare os_alarm_process / os_kill_process).
 *
 * Returns 0 on success (SIGSTOP raised + SIGCONT resumed), -errno
 * on syscall failure.
 */
static int __attribute__((__noinline__))
os_template_pause_stop_self_inner(void)
{
	register long rax asm("rax");
	long pid;

	/*
	 * Inline-asm getpid + kill.  Bypasses glibc's syscall()
	 * wrappers — both of which can route through
	 * __syscall_cancel, which has been observed to write to an
	 * internal libc cancellation pipe whose reader pthread does
	 * NOT exist in a forked child (raw fork only duplicates the
	 * calling thread).  Direct syscall instructions have zero
	 * libc state.
	 */

	/* getpid */
	rax = __NR_getpid;
	asm volatile (
		"syscall\n\t"
		: "+r" (rax)
		:
		: "rcx", "r11", "memory"
	);
	if (rax <= 0)
		return -EINVAL;
	pid = rax;

	/* kill(pid, SIGSTOP) */
	{
		register long rax_k asm("rax") = __NR_kill;
		register long rdi_k asm("rdi") = pid;
		register long rsi_k asm("rsi") = SIGSTOP;

		asm volatile (
			"syscall\n\t"
			: "+r" (rax_k)
			: "r" (rdi_k), "r" (rsi_k)
			: "rcx", "r11", "memory"
		);
		if (rax_k < 0 && rax_k > -4096)
			return (int)rax_k;
	}
	return 0;
}

int os_template_pause_stop_self(void)
{
	return os_template_pause_stop_self_inner();
}

/*
 * Parse UM_TEMPLATE_IDENTITY_FD and return the fd, or a negative
 * errno if unavailable.  Validates the fd is currently open via
 * fcntl(F_GETFD) so a stale env var (parent exec'd into us with a
 * dead fd) shows up cleanly rather than failing later on read().
 */
int os_template_pause_identity_fd(void)
{
	const char *v;
	char *end;
	long fd;

	v = getenv(UM_TEMPLATE_IDENTITY_FD_ENV);
	if (!v || !*v)
		return -ENOENT;

	fd = strtol(v, &end, 10);
	if (*end != '\0' || fd < 0 || fd > INT32_MAX)
		return -EINVAL;

	if (fcntl((int)fd, F_GETFD) < 0)
		return -EBADF;

	return (int)fd;
}

/*
 * Fork via raw __NR_fork.  Identical mechanics to
 * os_snapshot_fork_worker (workstream C-09): bypass glibc's
 * cancellation-point + per-thread state machinery that has
 * historically misbehaved when called from UML kernel context.
 * Returns the child pid in the parent, 0 in the child, or
 * a negative errno.
 *
 * Phase 2a (fork-on-resume) uses this for every taken pool member.
 * The KVM-backend restriction is enforced in-kernel (see
 * arch/um/kernel/template_pause.c assert_fork_safety) — under KVM
 * fork() aliases /dev/kvm fds and per-vCPU mmap state and corrupts
 * both parent and child.
 */
static int __attribute__((__noinline__))
os_template_pause_fork_inner(void)
{
	register long rax asm("rax") = __NR_fork;

	asm volatile (
		"syscall\n\t"
		: "+r" (rax)
		:
		: "rcx", "r11", "memory"
	);
	if (rax < 0 && rax > -4096)
		return (int)rax;
	return (int)rax;
}

int os_template_pause_fork(void)
{
	return os_template_pause_fork_inner();
}

/*
 * Write the just-forked child's host pid into the identity memfd
 * at offset @offset (typically `sizeof(struct um_template_identity)`
 * so the supervisor can read it back without conflicting with the
 * blob at offset 0).  Used by the parent side of the fork-on-resume
 * loop to report the new pool-member pid to the supervisor.
 *
 * Returns 0 on success, -errno on syscall failure.
 */
int os_template_pause_write_child_pid(int fd, off_t offset, int child_pid)
{
	__u32 le = (__u32)child_pid;
	long n;

	if (fd < 0)
		return -EBADF;

	/*
	 * Raw __NR_pwrite64 — bypasses glibc's cancellation-point
	 * machinery (__syscall_cancel) that has historically misbehaved
	 * when called from post-fork UML kernel context (see
	 * os_template_pause_stop_self for the same rationale, and
	 * arch/um/kernel/snapshot.c's wait4 hazard commentary).
	 *
	 * pwrite is positional — no separate lseek call needed, so we
	 * don't need to make two cancellation-point-affected calls.
	 */
	for (;;) {
		n = syscall(__NR_pwrite64, fd, &le, sizeof(le), (off_t)offset);
		if (n == (long)sizeof(le))
			return 0;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		/* Implausible short write on memfd; bail. */
		return -EIO;
	}
}

/*
 * Bisect helper: raw __NR_exit_group so a forked-child UML kernel
 * can exit cleanly without going through glibc's at_exit handlers
 * (which are not safe to invoke from UML kernel context post-fork,
 * for the same reasons that wait4 / kill via glibc hit the
 * cancellation-point hazard).
 */
void os_template_pause_child_exit(int code)
{
	/* Inline-asm syscall: bypasses glibc.  exit_group never
	 * returns, so we don't need to capture rax.  Args: edi = code.
	 */
	register long rax_in asm("rax") = __NR_exit_group;
	register long rdi_in asm("rdi") = (long)code;
	asm volatile (
		"syscall\n\t"
		:
		: "r" (rax_in), "r" (rdi_in)
		: "rcx", "r11", "memory"
	);
	/* unreachable */
	for (;;)
		;
}

/*
 * Read identity blob from a memfd.  Supervisor lays out exactly one
 * struct um_template_identity at offset 0 each time it takes a pool
 * member, so we seek to 0 before reading.  Short reads (memfd is
 * smaller than @len) are tolerated — caller asserts magic + version
 * before trusting any field.
 */
ssize_t os_template_pause_read_identity(int fd, void *buf, size_t len)
{
	ssize_t got = 0, n;

	if (fd < 0)
		return -EBADF;
	if (!buf || !len)
		return -EINVAL;

	if (lseek(fd, 0, SEEK_SET) < 0)
		return -errno;

	while ((size_t)got < len) {
		n = read(fd, (char *)buf + got, len - got);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			break;	/* short read: memfd shorter than @len */
		got += n;
	}

	return got;
}
