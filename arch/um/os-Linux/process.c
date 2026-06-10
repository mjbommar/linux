// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include <backend.h>
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
 * Snapshot / forkserver primitives.
 *
 * These live here rather than in a separate file because they are thin
 * wrappers around host libc calls that the in-kernel snapshot.c needs
 * to reach through the os-Linux boundary. Semantics:
 *
 *   os_snapshot_fd_is_open(fd):
 *	Return 1 if the host fd is open on this process, 0 otherwise.
 *	Used to detect whether an AFL-style host launcher has pre-
 *	opened fds 198/199 before UML's main() ran.
 *
 *   os_snapshot_fork_worker(void):
 *	Raw fork() via syscall(__NR_fork). Unlike helper.c's
 *	clone(CLONE_VM), this gives the child its own copy-on-
 *	write address space, which is what the fuzz forkserver wants
 *	(child mutates RAM, parent stays pristine). Returns the host
 *	pid of the child in the parent and 0 in the child, as fork()
 *	does. A negative return carries -errno.
 *
 *   os_snapshot_{read,write}_all(fd, buf, len):
 *	Loop until len bytes have been read/written or an error
 *	occurs. Partial reads/writes are handled internally. These
 *	are the 4-byte AFL wire-protocol moves; wrappers keep the
 *	in-kernel caller free of EINTR handling.
 *
 *   os_snapshot_waitpid_status(pid):
 *	Block-reap the given pid. Returns the encoded wait status, or
 *	-errno on failure.
 *
 * All five are host-side helpers; they must not call into kernel code.
 * The in-kernel driver lives in arch/um/kernel/snapshot.c.
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
		 * Use raw wait4 so this path does not enter libc cancellation or
		 * pthread state while UML is running kernel code.  This helper is
		 * for callers that explicitly want to wait for @pid; forkserver
		 * iteration uses the non-blocking drain below.
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
		/* wait4() returned a different child despite a specific pid. */
		return -EINVAL;
	}
}

/*
 * Non-blocking zombie drain. Walks and reaps any exited children,
 * returning the count. Used between forkserver iterations to keep the
 * host zombie table empty without blocking in wait4.
 *
 * Same raw-wait4 rationale as os_snapshot_waitpid_status above;
 * the WNOHANG flag makes this safe to call from the forkserver loop.
 *
 * A -ECHILD return from wait4 just means "no children left",
 * not an error for our purposes; return zero for clean-drain
 * completion.
 */
int os_snapshot_reap_zombies(void)
{
	int status;
	long ret;
	int reaped = 0;

	for (;;) {
		ret = syscall(__NR_wait4, -1, &status, WNOHANG, NULL);
		if (ret > 0) {
			reaped++;
			continue;
		}
		if (ret == 0)
			return reaped;		/* nothing exited yet */
		if (errno == ECHILD)
			return reaped;		/* no children at all */
		if (errno == EINTR)
			continue;
		return -errno;
	}
}

/*
 * Gate UML's in-kernel signal dispatch for the duration of the
 * forkserver loop body. Not the host sigprocmask; that only stops
 * host delivery, and when unblocked UML's sig_handler runs queued
 * signals back-to-back from whatever context we happen to be in,
 * which is exactly the crash mode this path avoids. UML provides
 * its own TLS flag (signals_enabled in arch/um/os-Linux/signal.c)
 * that sig_handler checks on every delivery; when it's 0, the
 * handler stores the signal in signals_pending and returns
 * without running any UML kernel code. When we flip it back to 1,
 * unblock_signals() drains signals_pending synchronously and
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
	/*
	 * Unreachable; if the syscall somehow returns, panic the host
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
	int flags;
	const char *thp_knob;

	prot = (r ? PROT_READ : 0) | (w ? PROT_WRITE : 0) |
		(x ? PROT_EXEC : 0);

	/*
	 * Use MAP_POPULATE so the host PTEs are installed at mmap time
	 * rather than on first touch.
	 *
	 * MAP_LOCKED additionally tells the host kernel to keep the resulting
	 * pages resident. Gate it on UM_KVM_V2_PIN_PHYSMEM so launchers can
	 * choose the residency policy at runtime.
	 *
	 * If UM_HUGEPAGES=2M or =1G, request explicit hugepage backing
	 * via MAP_HUGETLB. Massive TLB pressure
	 * reduction (1 GiB physmem at 4K = 262144 PTEs vs 512 at 2M vs
	 * 1 at 1G). Falls back to 4K (with a perror) if the hugepage pool
	 * is empty; reserve /proc/sys/vm/nr_hugepages before launch when
	 * hugepage backing is required.
	 */
	flags = MAP_SHARED | MAP_FIXED | MAP_POPULATE;
	if (getenv("UM_KVM_V2_PIN_PHYSMEM"))
		flags |= MAP_LOCKED;

	{
		const char *hp = getenv("UM_HUGEPAGES");

		if (hp) {
			int huge_shift = 0;

			if (!strcmp(hp, "2M"))
				huge_shift = 21;	/* MAP_HUGE_2MB */
			else if (!strcmp(hp, "1G"))
				huge_shift = 30;	/* MAP_HUGE_1GB */
			/* anything else (off/auto/empty): keep 4K */

			if (huge_shift)
				flags |= MAP_HUGETLB | (huge_shift << MAP_HUGE_SHIFT);
		}
	}

	loc = mmap64((void *)virt, len, prot, flags, fd, off);
	if (loc == MAP_FAILED && (flags & MAP_HUGETLB)) {
		/*
		 * Hugepage pool exhausted, typically because
		 * /proc/sys/vm/nr_hugepages was not reserved. Retry with 4K so
		 * the run proceeds and the warning identifies the missing host
		 * reservation.
		 */
		perror("um: UM_HUGEPAGES requested but mmap returned ENOMEM; falling back to 4K");
		flags &= ~(MAP_HUGETLB | (0x3fU << MAP_HUGE_SHIFT));
		loc = mmap64((void *)virt, len, prot, flags, fd, off);
	}
	if (loc == MAP_FAILED)
		return -errno;

	/*
	 * Tell host KSM to skip this range.
	 * KSM (Kernel Same-page Merging) is a host-side feature that
	 * scans VM memory looking for identical pages and merges them
	 * via COW. Removes a known noise source for hypervisor
	 * workloads (latency spikes when the guest writes to a merged
	 * page and the host has to un-share). Apply unconditionally;
	 * EINVAL on a host without KSM configured is fine.
	 */
	if (madvise(loc, len, MADV_UNMERGEABLE) < 0 && errno != EINVAL)
		perror("um: madvise(MADV_UNMERGEABLE)");

	/*
	 * Apply Transparent Huge Pages policy for predictability.
	 * UM_THP=off -> NOHUGEPAGE (predictable, no khugepaged scan
	 * latency spikes); =on -> HUGEPAGE (throughput-oriented,
	 * eager defrag); =auto/unset -> inherit the system default.
	 *
	 * Distinct from MAP_HUGETLB: MAP_HUGETLB
	 * guarantees specific page sizes from the pre-reserved
	 * hugetlbfs pool; MADV_HUGEPAGE is opportunistic via
	 * khugepaged on regular 4K-backed memory.
	 */
	thp_knob = getenv("UM_THP");
	if (thp_knob) {
		if (!strcmp(thp_knob, "off")) {
			if (madvise(loc, len, MADV_NOHUGEPAGE) < 0)
				perror("um: madvise(MADV_NOHUGEPAGE)");
		} else if (!strcmp(thp_knob, "on")) {
			if (madvise(loc, len, MADV_HUGEPAGE) < 0)
				perror("um: madvise(MADV_HUGEPAGE)");
		}
		/* "auto"/anything else: no madvise, inherit default. */
	}

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

/*
 * os_remap_region_shared() - atomically swap the backing fd for a
 * MAP_SHARED region.  Used by pool-member fork-child entry to
 * isolate physmem: replicate master's physmem content into a fresh
 * memfd, then point the kernel's MAP_SHARED mapping at the new fd.
 *
 * MAP_SHARED is preserved across the swap (kernel/stub coherence
 * within a member relies on both ends mapping the same fd
 * MAP_SHARED; see arch/um/kernel/skas/stub.c which always uses
 * MAP_SHARED | MAP_FIXED for STUB_SYSCALL_MMAP).
 *
 * The replacement is VMA-atomic at the host kernel level; kernel
 * code executing from the affected VA range continues because the
 * new mapping's pages have identical bytes (replicated via
 * copy_file_range before this call).
 *
 * Returns 0 on success, -errno on failure (old mapping intact).
 */
int os_remap_region_shared(void *addr, int fd, unsigned long long off,
			   unsigned long len)
{
	void *loc;

	loc = mmap64(addr, len, PROT_READ | PROT_WRITE | PROT_EXEC,
		     MAP_SHARED | MAP_FIXED | MAP_POPULATE, fd, off);
	if (loc == MAP_FAILED)
		return -errno;
	if (loc != addr)
		return -EINVAL;
	/* Mirror os_map_memory()'s MADV_UNMERGEABLE so KSM doesn't
	 * scan and merge the replicated pages (latency noise source).
	 */
	(void)madvise(loc, len, MADV_UNMERGEABLE);
	return 0;
}

/*
 * Step the kernel-VA mapping through an anonymous mmap before
 * landing on the destination fd.  Sequence:
 *   1. memcpy original content into a scratch heap-side anonymous mmap
 *   2. mmap MAP_ANONYMOUS|MAP_SHARED|MAP_FIXED over original VA
 *   3. memcpy scratch back into the (now anonymous) VA range
 *   4. write new fd's content over the anonymous range via memcpy
 *      from new_fd's MAP_SHARED scratch
 *   5. mmap-FIXED swap to new fd
 *
 * The intermediate anonymous mapping resets host-kernel state tied to
 * the original inode before the destination fd is installed.
 *
 * Returns 0 on success, -errno on failure (original mapping may be
 * left in an intermediate state on partial failure; caller must
 * treat any failure as fatal).
 */
int os_remap_region_via_anon(void *addr, int new_fd, unsigned long long off,
			     unsigned long len)
{
	void *loc, *scratch_anon, *new_view;
	int err;

	/* Stash original content. */
	scratch_anon = mmap64(NULL, len, PROT_READ | PROT_WRITE,
			      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (scratch_anon == MAP_FAILED)
		return -errno;
	memcpy(scratch_anon, addr, len);

	/* Mmap a fresh new_fd view into a separate scratch VA to read its
	 * content (the destination has been pre-populated by caller).
	 */
	new_view = mmap64(NULL, len, PROT_READ | PROT_WRITE,
			  MAP_SHARED, new_fd, off);
	if (new_view == MAP_FAILED) {
		err = -errno;
		munmap(scratch_anon, len);
		return err;
	}

	/* Step 1: replace original VA with anonymous shared mapping.
	 * MAP_ANONYMOUS|MAP_SHARED creates a shared anon segment;
	 * the kernel allocates fresh anon pages (no inode binding).
	 */
	loc = mmap64(addr, len, PROT_READ | PROT_WRITE | PROT_EXEC,
		     MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED, -1, 0);
	if (loc != addr) {
		err = (loc == MAP_FAILED) ? -errno : -EINVAL;
		munmap(scratch_anon, len);
		munmap(new_view, len);
		return err;
	}

	/* Step 2: restore content (now backed by anon pages). */
	memcpy(addr, scratch_anon, len);
	munmap(scratch_anon, len);

	/* Step 3: final swap to new fd. */
	loc = mmap64(addr, len, PROT_READ | PROT_WRITE | PROT_EXEC,
		     MAP_SHARED | MAP_FIXED | MAP_POPULATE, new_fd, off);
	munmap(new_view, len);
	if (loc != addr)
		return (loc == MAP_FAILED) ? -errno : -EINVAL;
	(void)madvise(loc, len, MADV_UNMERGEABLE);
	return 0;
}

/*
 * os_create_memfd() - create a fresh, anonymous memfd sized to
 * @size bytes.  Returns the new fd on success, -errno on failure.
 * Caller owns the fd.
 *
 * Used by per-pool-member physmem isolation: caller mmaps the new
 * fd into a scratch VA, memcpy's master's physmem content into it,
 * then re-mmaps the kernel-side physmem region MAP_SHARED|MAP_FIXED
 * over the new fd (see um_pool_replicate_physmem).
 *
 * memfd_create is preferred over a tmpfs tempfile because it
 * (a) is anonymous (no name collisions, no /tmp pressure) and
 * (b) cannot be backdoored via path interception.
 */
/*
 * os_mmap_rw_scratch() - mmap @fd at @off for @len bytes as a
 * scratch VA (host-chosen address, MAP_SHARED, RW).  Stores the
 * resulting VA in *@out_addr.  Returns 0 on success or -errno on
 * failure (*@out_addr untouched).
 *
 * Used by per-pool-member physmem isolation: caller mmaps the
 * fresh memfd as scratch, memcpy's master's physmem content into
 * it, then re-mmaps the kernel VA over the new fd.
 */
int os_mmap_rw_scratch(int fd, unsigned long long off, unsigned long len,
		       void **out_addr)
{
	void *loc;

	loc = mmap64(NULL, len, PROT_READ | PROT_WRITE,
		     MAP_SHARED, fd, off);
	if (loc == MAP_FAILED)
		return -errno;
	*out_addr = loc;
	return 0;
}

/*
 * Drain any pending signals via sigtimedwait with a zero timeout.
 * This forces a get_signal()-equivalent pass that may clear stuck
 * signal-delivery state, such as TIF_NOTIFY_SIGNAL from inherited
 * io_uring task_work. Returns the count of signals drained.
 */
int os_drain_pending_signals(void)
{
	sigset_t all;
	siginfo_t si;
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
	int count = 0, r;

	sigfillset(&all);
	while ((r = syscall(__NR_rt_sigtimedwait, &all, &si, &ts,
			    sizeof(sigset_t))) > 0) {
		count++;
		if (count > 64)
			break;	/* sanity cap */
	}
	return count;
}

int os_create_memfd(const char *name, unsigned long long size)
{
	int fd, err;

	fd = syscall(__NR_memfd_create, name, 0);
	if (fd < 0)
		return -errno;

	if (ftruncate(fd, size) < 0) {
		err = -errno;
		close(fd);
		return err;
	}

	return fd;
}

/*
 * os_create_tmpfile() - create an unnamed tmpfs file via
 * open(O_TMPFILE) on @dir, sized to @size bytes.  Parallel to
 * os_create_memfd() but using a tmpfs file, the same mechanism
 * setup_physmem uses for the boot-time physmem_fd.
 */
int os_create_tmpfile(const char *dir, unsigned long long size)
{
	int fd, err;

	fd = open(dir, O_CLOEXEC | O_RDWR | O_EXCL | O_TMPFILE, 0600);
	if (fd < 0)
		return -errno;

	if (ftruncate(fd, size) < 0) {
		err = -errno;
		close(fd);
		return err;
	}

	return fd;
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

/*
 * madvise(MADV_DONTNEED) drops host kernel PT entries for the range
 * without disturbing file content
 * (safe on MAP_SHARED tmpfs/physmem). Crucially, this fires
 * mmu_notifier in the host kernel, which causes KVM to invalidate
 * any cached EPT/TDP entries pointing to the dropped HPAs.
 *
 * Used by kvm-v2's handle_io_pf to force TDP invalidation after a
 * fresh anon page is allocated for a faulting GVA, preventing the
 * stale TDP writes.
 */
int os_drop_caching(void *addr, int length)
{
	int err;

	err = madvise(addr, length, MADV_DONTNEED);
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
	/*
	 * Only install the SIGCHLD reaper when the active backend
	 * uses the child-reaper IRQ. The seccomp backend does; KVM has
	 * no host stub child. Reads the ops-table capability flag rather
	 * than a backend-specific global.
	 */
	if (um_backend && um_backend->uses_stub_reaper)
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
