// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <asm/unistd.h>
#include <init.h>
#include <longjmp.h>
#include <os.h>
#include <skas/skas.h>

void os_alarm_process(int pid)
{
	if (pid <= 0)
		return;

	kill(pid, SIGALRM);
}

void os_kill_process(int pid, int reap_child)
{
	if (pid <= 0)
		return;

	/* Block signals until child is reaped */
	block_signals();

	kill(pid, SIGKILL);
	if (reap_child)
		CATCH_EINTR(waitpid(pid, NULL, __WALL));

	unblock_signals();
}

/* Kill off a ptraced child by all means available.  kill it normally first,
 * then PTRACE_KILL it, then PTRACE_CONT it in case it's in a run state from
 * which it can't exit directly.
 */

void os_kill_ptraced_process(int pid, int reap_child)
{
	if (pid <= 0)
		return;

	/* Block signals until child is reaped */
	block_signals();

	kill(pid, SIGKILL);
	ptrace(PTRACE_KILL, pid);
	ptrace(PTRACE_CONT, pid);
	if (reap_child)
		CATCH_EINTR(waitpid(pid, NULL, __WALL));

	unblock_signals();
}

pid_t os_reap_child(void)
{
	int status;

	/* Try to reap a child */
	return waitpid(-1, &status, WNOHANG);
}

/*
 * Snapshot / forkserver primitives (workstream C-09).
 *
 * These live here rather than in a separate TU because they are thin
 * wrappers around host libc calls that the in-kernel snapshot.c needs
 * to reach through the os-Linux boundary. Semantics:
 *
 *   os_snapshot_fd_is_open(fd):
 *	Return 1 if the host fd is open on this process, 0 otherwise.
 *	Used to detect whether an AFL-style host launcher has pre-
 *	opened fds 198/199 before UML's main() ran.
 *
 *   os_snapshot_fork_worker(void):
 *	Raw ``fork()`` via syscall(__NR_fork). Unlike ``helper.c``'s
 *	``clone(CLONE_VM)``, this gives the child its own copy-on-
 *	write address space, which is what the fuzz forkserver wants
 *	(child mutates RAM, parent stays pristine). Returns the host
 *	pid of the child in the parent and 0 in the child, as fork()
 *	does. A negative return carries ``-errno``.
 *
 *   os_snapshot_{read,write}_all(fd, buf, len):
 *	Loop until ``len`` bytes have been read/written or an error
 *	occurs. Partial reads/writes are handled internally. These
 *	are the 4-byte AFL wire-protocol moves; wrappers keep the
 *	in-kernel caller free of EINTR handling.
 *
 *   os_snapshot_waitpid_status(pid):
 *	Block-reap the given pid. Returns the encoded wait status, or
 *	``-errno`` on failure.
 *
 * All five are USER_OBJS-scope helpers; they must not call into
 * kernel code. The in-kernel driver lives in arch/um/kernel/snapshot.c.
 */
int os_snapshot_fd_is_open(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFD);
	if (flags < 0)
		return 0;
	return 1;
}

int os_snapshot_fork_worker(void)
{
	long ret;

	ret = syscall(__NR_fork);
	if (ret < 0)
		return -errno;
	return (int)ret;
}

ssize_t os_snapshot_read_all(int fd, void *buf, size_t len)
{
	size_t got = 0;
	ssize_t n;

	while (got < len) {
		n = read(fd, (char *)buf + got, len - got);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EPIPE;
		got += n;
	}
	return (ssize_t)got;
}

ssize_t os_snapshot_write_all(int fd, const void *buf, size_t len)
{
	size_t sent = 0;
	ssize_t n;

	while (sent < len) {
		n = write(fd, (const char *)buf + sent, len - sent);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		sent += n;
	}
	return (ssize_t)sent;
}

int os_snapshot_waitpid_status(int pid)
{
	int status;
	long ret;

	/*
	 * Use raw syscall(__NR_wait4, ...) rather than glibc's
	 * waitpid() wrapper. In commit 3d-a v1 the glibc wrapper
	 * crashed the parent with a null-jump even under UML-level
	 * signal gating; one plausible cause is glibc's cancellation-
	 * point machinery (__syscall_cancel in modern glibc) doing an
	 * indirect call through a pthread-specific pointer that is
	 * inconsistent when we call it from UML kernel context. The
	 * raw syscall bypasses all that and just returns the kernel's
	 * answer. wait4() is the canonical kernel entry (waitpid() is
	 * historically a libc wrapper over wait4 with rusage=NULL).
	 */
	for (;;) {
		ret = syscall(__NR_wait4, pid, &status, 0, NULL);
		if (ret == pid)
			return status;
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		/* Shouldn't happen for a specific pid, but be defensive. */
		return -EINVAL;
	}
}

/*
 * Gate UML's in-kernel signal dispatch for the duration of the
 * forkserver loop body. Not the host sigprocmask — that only stops
 * host delivery, and when unblocked UML's sig_handler runs queued
 * signals back-to-back from whatever context we happen to be in,
 * which is exactly the crash mode commit 3d-a v1 hit. UML provides
 * its own TLS flag (`signals_enabled` in arch/um/os-Linux/signal.c)
 * that sig_handler checks on every delivery; when it's 0, the
 * handler stores the signal in `signals_pending` and returns
 * without running any UML kernel code. When we flip it back to 1,
 * `unblock_signals()` drains `signals_pending` synchronously and
 * in a deterministic order that UML expects.
 *
 * Using um_set_signals(0)/um_set_signals(saved) gives us that
 * synchronous-drain behavior for free. One static saves the old
 * state across the paired calls; the forkserver runs on exactly
 * one thread so the save slot is race-free.
 */
static int os_snapshot_signals_save;

void os_snapshot_block_iter_signals(void)
{
	os_snapshot_signals_save = um_set_signals(0);
}

void os_snapshot_unblock_iter_signals(void)
{
	um_set_signals(os_snapshot_signals_save);
}

/*
 * Terminate the current host process immediately with the given
 * status. Used by the snapshot-forkserver worker child so it exits
 * cleanly without running any further UML init or kernel shutdown
 * machinery. Called only from the forked child after the parent has
 * reported its pid over the AFL status fd.
 *
 * Uses exit_group() rather than exit() so all host threads the child
 * inherited from the parent (e.g. UML IRQ driver helper threads that
 * fork did not sever) terminate together. Does not return.
 */
void os_snapshot_worker_exit(int status)
{
	syscall(__NR_exit_group, status);
	/* unreachable; if the syscall somehow returns, panic the host
	 * process via a raw abort so the parent sees a clean SIGABRT
	 * rather than the child hanging in a half-alive state.
	 */
	__builtin_trap();
}

/* Don't use the glibc version, which caches the result in TLS. It misses some
 * syscalls, and also breaks with clone(), which does not unshare the TLS.
 */

int os_getpid(void)
{
	return syscall(__NR_getpid);
}

int os_map_memory(void *virt, int fd, unsigned long long off, unsigned long len,
		  int r, int w, int x)
{
	void *loc;
	int prot;

	prot = (r ? PROT_READ : 0) | (w ? PROT_WRITE : 0) |
		(x ? PROT_EXEC : 0);

	loc = mmap64((void *) virt, len, prot, MAP_SHARED | MAP_FIXED,
		     fd, off);
	if (loc == MAP_FAILED)
		return -errno;
	return 0;
}

int os_protect_memory(void *addr, unsigned long len, int r, int w, int x)
{
	int prot = ((r ? PROT_READ : 0) | (w ? PROT_WRITE : 0) |
		    (x ? PROT_EXEC : 0));

	if (mprotect(addr, len, prot) < 0)
		return -errno;

	return 0;
}

int os_unmap_memory(void *addr, int len)
{
	int err;

	err = munmap(addr, len);
	if (err < 0)
		return -errno;
	return 0;
}

#ifndef MADV_REMOVE
#define MADV_REMOVE KERNEL_MADV_REMOVE
#endif

int os_drop_memory(void *addr, int length)
{
	int err;

	err = madvise(addr, length, MADV_REMOVE);
	if (err < 0)
		err = -errno;
	return err;
}

int __init can_drop_memory(void)
{
	void *addr;
	int fd, ok = 0;

	printk(UM_KERN_INFO "Checking host MADV_REMOVE support...");
	fd = create_mem_file(UM_KERN_PAGE_SIZE);
	if (fd < 0) {
		printk(UM_KERN_ERR "Creating test memory file failed, "
		       "err = %d\n", -fd);
		goto out;
	}

	addr = mmap64(NULL, UM_KERN_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		printk(UM_KERN_ERR "Mapping test memory file failed, "
		       "err = %d\n", -errno);
		goto out_close;
	}

	if (madvise(addr, UM_KERN_PAGE_SIZE, MADV_REMOVE) != 0) {
		printk(UM_KERN_ERR "MADV_REMOVE failed, err = %d\n", -errno);
		goto out_unmap;
	}

	printk(UM_KERN_CONT "OK\n");
	ok = 1;

out_unmap:
	munmap(addr, UM_KERN_PAGE_SIZE);
out_close:
	close(fd);
out:
	return ok;
}

void init_new_thread_signals(void)
{
	set_handler(SIGSEGV);
	set_handler(SIGTRAP);
	set_handler(SIGFPE);
	set_handler(SIGILL);
	set_handler(SIGBUS);
	signal(SIGHUP, SIG_IGN);
	set_handler(SIGIO);
	/* We (currently) only use the child reaper IRQ in seccomp mode */
	if (using_seccomp)
		set_handler(SIGCHLD);
	signal(SIGWINCH, SIG_IGN);
}

void os_set_pdeathsig(void)
{
	prctl(PR_SET_PDEATHSIG, SIGKILL);
}

int os_futex_wait(void *uaddr, unsigned int val)
{
	int r;

	CATCH_EINTR(r = syscall(__NR_futex, uaddr, FUTEX_WAIT, val,
				NULL, NULL, 0));
	return r < 0 ? -errno : r;
}

int os_futex_wake(void *uaddr)
{
	int r;

	CATCH_EINTR(r = syscall(__NR_futex, uaddr, FUTEX_WAKE, INT_MAX,
				NULL, NULL, 0));
	return r < 0 ? -errno : r;
}
