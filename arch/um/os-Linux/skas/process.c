// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Benjamin Berg <benjamin@sipsolutions.net>
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2002- 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <mem_user.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <asm/unistd.h>
#include <backend.h>
#include <as-layout.h>
#include <init.h>
#include <kern_util.h>
#include <mem.h>
#include <os.h>
#include <ptrace_user.h>
#include <registers.h>
#include <skas.h>
#include <sysdep/stub.h>
#include <sysdep/mcontext.h>
#include <linux/futex.h>
#include <linux/threads.h>
#include <timetravel.h>
#include "../internal.h"

int is_skas_winch(int pid, int fd, void *data)
{
	return pid == getpgrp();
}

/*
 * Ship the queued syscall_fd_map[] fds to the stub child via SCM_RIGHTS
 * over mm_idp->sock. Extracted from wait_stub_done_seccomp's prelude so
 * the WORKER_PROCESS=y path can run it from the spawner (those fds
 * live in the spawner's FD table) before handing the futex round-trip
 * off to the worker.
 */
void send_stub_syscall_fds(struct mm_id *mm_idp)
{
	const char byte = 0;
	struct iovec iov = {
		.iov_base = (void *)&byte,
		.iov_len  = sizeof(byte),
	};
	union {
		char data[CMSG_SPACE(sizeof(mm_idp->syscall_fd_map))];
		struct cmsghdr align;
	} ctrl;
	struct msghdr msgh = {
		.msg_iov    = &iov,
		.msg_iovlen = 1,
	};
	unsigned int fds_size;
	struct cmsghdr *cmsg;

	if (!mm_idp->syscall_fd_num)
		return;

	fds_size = sizeof(int) * mm_idp->syscall_fd_num;
	msgh.msg_control    = ctrl.data;
	msgh.msg_controllen = CMSG_SPACE(fds_size);
	cmsg = CMSG_FIRSTHDR(&msgh);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type  = SCM_RIGHTS;
	cmsg->cmsg_len   = CMSG_LEN(fds_size);
	memcpy(CMSG_DATA(cmsg), mm_idp->syscall_fd_map, fds_size);

	CATCH_EINTR(syscall(__NR_sendmsg, mm_idp->sock, &msgh, 0));
}

void wait_stub_done_seccomp(struct mm_id *mm_idp, int running, int wait_sigsys)
{
	struct stub_data *data = (void *)mm_idp->stack;
	int ret;

	do {
		const char byte = 0;
		struct iovec iov = {
			.iov_base = (void *)&byte,
			.iov_len = sizeof(byte),
		};
		union {
			char data[CMSG_SPACE(sizeof(mm_idp->syscall_fd_map))];
			struct cmsghdr align;
		} ctrl;
		struct msghdr msgh = {
			.msg_iov = &iov,
			.msg_iovlen = 1,
		};

		if (!running) {
			if (mm_idp->syscall_fd_num) {
				unsigned int fds_size =
					sizeof(int) * mm_idp->syscall_fd_num;
				struct cmsghdr *cmsg;

				msgh.msg_control = ctrl.data;
				msgh.msg_controllen = CMSG_SPACE(fds_size);
				cmsg = CMSG_FIRSTHDR(&msgh);
				cmsg->cmsg_level = SOL_SOCKET;
				cmsg->cmsg_type = SCM_RIGHTS;
				cmsg->cmsg_len = CMSG_LEN(fds_size);
				memcpy(CMSG_DATA(cmsg), mm_idp->syscall_fd_map,
				       fds_size);

				CATCH_EINTR(syscall(__NR_sendmsg, mm_idp->sock,
						&msgh, 0));
			}

			data->signal = 0;
			data->futex = FUTEX_IN_CHILD;
			CATCH_EINTR(syscall(__NR_futex, &data->futex,
					    FUTEX_WAKE, 1, NULL, NULL, 0));
		}

		do {
			/*
			 * We need to check whether the child is still alive
			 * before and after the FUTEX_WAIT call. Before, in
			 * case it just died but we still updated data->futex
			 * to FUTEX_IN_CHILD. And after, in case it died while
			 * we were waiting (and SIGCHLD woke us up, see the
			 * IRQ handler in mmu.c).
			 *
			 * Either way, if PID is negative, then we have no
			 * choice but to kill the task.
			 */
			if (UM_USER_READ_ONCE(mm_idp->pid) < 0)
				goto out_kill;

			ret = syscall(__NR_futex, &data->futex,
				      FUTEX_WAIT, FUTEX_IN_CHILD,
				      NULL, NULL, 0);
			if (ret < 0 && errno != EINTR && errno != EAGAIN) {
				printk(UM_KERN_ERR "%s : FUTEX_WAIT failed, errno = %d\n",
				       __func__, errno);
				goto out_kill;
			}
		} while (data->futex == FUTEX_IN_CHILD);

		if (UM_USER_READ_ONCE(mm_idp->pid) < 0)
			goto out_kill;

		running = 0;

		/* We may receive a SIGALRM before SIGSYS, iterate again. */
	} while (wait_sigsys && data->signal == SIGALRM);

	if (data->mctx_offset > sizeof(data->sigstack) - sizeof(mcontext_t)) {
		printk(UM_KERN_ERR "%s : invalid mcontext offset", __func__);
		goto out_kill;
	}

	if (wait_sigsys && data->signal != SIGSYS) {
		printk(UM_KERN_ERR "%s : expected SIGSYS but got %d",
		       __func__, data->signal);
		goto out_kill;
	}

	return;

out_kill:
	printk(UM_KERN_ERR "%s : failed to wait for stub, pid = %d, errno = %d\n",
	       __func__, mm_idp->pid, errno);
	/* This is not true inside start_userspace */
	if (current_mm_id() == mm_idp)
		fatal_sigsegv();
}

extern char __syscall_stub_start[];

static int stub_exe_fd;

struct tramp_data {
	struct stub_data *stub_data;
	/* 0 is inherited, 1 is the kernel side */
	int sockpair[2];
	/*
	 * If >= 0, override phys_mapping() for stub_data; pass this
	 * fd to the stub child instead of UML's shared physmem_fd.
	 * Used by start_userspace_fresh() so post-fork pool members
	 * get a per-mm stub_data backing fd (memfd) and the stub maps
	 * physically-isolated memory.  Offset is always 0 in this
	 * mode.  If -1, fall back to phys_mapping (the default).
	 */
	int stub_data_fd_override;
};

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC	(1U << 2)
#endif

static int userspace_tramp(void *data)
{
	struct tramp_data *tramp_data = data;
	char *const argv[] = { "uml-userspace", NULL };
	unsigned long long offset;
	/*
	 * Stub-child seccomp wiring. init_data.seccomp is the
	 * boolean "install the SIGSYS filter" flag the stub
	 * binary reads; the handler/restorer trampoline offsets
	 * select the SIGSYS or SIGSEGV stub entry point.
	 * Routed through um_backend->stub_child_runs_seccomp
	 * because userspace_tramp runs inside clone() of the first
	 * start_userspace call, which is post-init_backend.
	 */
	bool want_seccomp = um_backend && um_backend->stub_child_runs_seccomp;
	struct stub_init_data init_data = {
		.seccomp = want_seccomp,
		.stub_start = STUB_START,
	};
	int ret;

	if (want_seccomp) {
		init_data.signal_handler = STUB_CODE +
					   (unsigned long) stub_signal_interrupt -
					   (unsigned long) __syscall_stub_start;
		init_data.signal_restorer = STUB_CODE +
					   (unsigned long) stub_signal_restorer -
					   (unsigned long) __syscall_stub_start;
	} else {
		init_data.signal_handler = STUB_CODE +
					   (unsigned long) stub_segv_handler -
					   (unsigned long) __syscall_stub_start;
		init_data.signal_restorer = 0;
	}

	init_data.stub_code_fd = phys_mapping(uml_to_phys(__syscall_stub_start),
					      &offset);
	init_data.stub_code_offset = MMAP_OFFSET(offset);

	if (tramp_data->stub_data_fd_override >= 0) {
		/* Per-mm memfd: stub_data lives in its own backing fd
		 * (start_userspace_fresh path), not UML's shared
		 * physmem_fd.  Used to give post-fork pool members
		 * physically-isolated stub_data.
		 */
		init_data.stub_data_fd = tramp_data->stub_data_fd_override;
		init_data.stub_data_offset = 0;
	} else {
		init_data.stub_data_fd = phys_mapping(uml_to_phys(tramp_data->stub_data),
						      &offset);
		init_data.stub_data_offset = MMAP_OFFSET(offset);
	}

	/*
	 * Avoid leaking unneeded FDs to the stub by setting CLOEXEC on all FDs
	 * and then unsetting it on all memory related FDs.
	 * This is not strictly necessary from a safety perspective.
	 */
	syscall(__NR_close_range, 0, ~0U, CLOSE_RANGE_CLOEXEC);

	fcntl(init_data.stub_data_fd, F_SETFD, 0);
	/*
	 * In the override path stub_code_fd != stub_data_fd (data is
	 * a per-mm memfd; code is still UML's physmem_fd holding the
	 * stub binary text).  The default path has them equal: both
	 * are physmem_fd, so the fcntl above implicitly clears
	 * CLOEXEC on both.  Be explicit so the override case works.
	 */
	if (init_data.stub_code_fd != init_data.stub_data_fd)
		fcntl(init_data.stub_code_fd, F_SETFD, 0);

	/* dup2 signaling FD/socket to STDIN */
	if (dup2(tramp_data->sockpair[0], 0) < 0)
		exit(3);
	close(tramp_data->sockpair[0]);

	/* Write init_data and close write side */
	ret = write(tramp_data->sockpair[1], &init_data, sizeof(init_data));
	close(tramp_data->sockpair[1]);

	if (ret != sizeof(init_data))
		exit(4);

	/* Raw execveat for compatibility with older libc versions */
	syscall(__NR_execveat, stub_exe_fd, (unsigned long)"",
		(unsigned long)argv, NULL, AT_EMPTY_PATH);

	exit(5);
}

extern char stub_exe_start[];
extern char stub_exe_end[];

extern char *tempdir;

#define STUB_EXE_NAME_TEMPLATE "/uml-userspace-XXXXXX"

#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

static int __init init_stub_exe_fd(void)
{
	size_t tmpfile_len;
	size_t written = 0;
	char *tmpfile = NULL;

	stub_exe_fd = memfd_create("uml-userspace",
				   MFD_EXEC | MFD_CLOEXEC | MFD_ALLOW_SEALING);

	if (stub_exe_fd < 0) {
		printk(UM_KERN_INFO "Could not create executable memfd, using temporary file!");

		tmpfile_len = strlen(tempdir) + strlen(STUB_EXE_NAME_TEMPLATE) + 1;
		tmpfile = malloc(tmpfile_len);
		if (tmpfile == NULL)
			panic("Failed to allocate memory for stub binary name");

		snprintf(tmpfile, tmpfile_len, "%s%s", tempdir,
			 STUB_EXE_NAME_TEMPLATE);

		stub_exe_fd = mkstemp(tmpfile);
		if (stub_exe_fd < 0)
			panic("Could not create fallback file for stub binary: %d",
			      -errno);
	}

	while (written < stub_exe_end - stub_exe_start) {
		ssize_t res = write(stub_exe_fd, stub_exe_start + written,
				    stub_exe_end - stub_exe_start - written);
		if (res < 0) {
			if (errno == EINTR)
				continue;

			if (tmpfile)
				unlink(tmpfile);
			panic("Failed write stub binary: %d", -errno);
		}

		written += res;
	}

	if (!tmpfile) {
		fcntl(stub_exe_fd, F_ADD_SEALS,
		      F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
	} else {
		if (fchmod(stub_exe_fd, 00500) < 0) {
			unlink(tmpfile);
			panic("Could not make stub binary executable: %d",
			      -errno);
		}

		close(stub_exe_fd);
		/*
		 * FD disposition: inherit. The stub
		 * binary fd is the fexecve() target for every
		 * userspace stub spawn and is held for the UML
		 * kernel's entire lifetime. Workers inherit it CoW
		 * and reuse it to spawn their own stubs. O_CLOEXEC
		 * here is correct: start_userspace() separately
		 * unshares fds into the stub child's file table
		 * via the tramp socketpair.
		 */
		stub_exe_fd = open(tmpfile, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (stub_exe_fd < 0) {
			unlink(tmpfile);
			panic("Could not reopen stub binary: %d", -errno);
		}

		unlink(tmpfile);
		free(tmpfile);
	}

	return 0;
}
__initcall(init_stub_exe_fd);

int using_seccomp;

/**
 * start_userspace() - prepare a new userspace process
 * @mm_id: The corresponding struct mm_id
 *
 * Sets up a scratch stack page used while userspace_tramp() runs.
 * Clones the kernel process into a new userspace process, with FDs only.
 *
 * Return: positive process id of the new userspace process, or a
 * negative errno.
 */
int start_userspace(struct mm_id *mm_id)
{
	struct stub_data *proc_data = (void *)mm_id->stack;
	struct tramp_data tramp_data = {
		.stub_data = proc_data,
		.stub_data_fd_override = -1,
	};
	void *stack;
	unsigned long sp;
	int err;

	/* Set up a scratch stack page. */
	stack = mmap(NULL, UM_KERN_PAGE_SIZE,
		     PROT_READ | PROT_WRITE | PROT_EXEC,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) {
		err = -errno;
		printk(UM_KERN_ERR "%s : mmap failed, errno = %d\n",
		       __func__, errno);
		return err;
	}

	/* set stack pointer to the end of the stack page, so it can grow downwards */
	sp = (unsigned long)stack + UM_KERN_PAGE_SIZE;

	/*
	 * Socket pair for init data and SECCOMP FD passing.
	 * FD disposition: exec-transmit. No
	 * SOCK_CLOEXEC on purpose; the userspace stub child
	 * exec()s into the stub binary and must inherit this fd to
	 * receive init data and (optionally) a seccomp fd from the
	 * UML kernel. This is the one socketpair() in arch/um that
	 * intentionally survives exec; do not "fix" it.
	 */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, tramp_data.sockpair)) {
		err = -errno;
		printk(UM_KERN_ERR "%s : socketpair failed, errno = %d\n",
		       __func__, errno);
		return err;
	}

	/*
	 * Pre-clone futex seed; only meaningful for backends
	 * whose stub dispatch uses the futex-wait_stub_done_
	 * seccomp round-trip. Routed through
	 * um_backend->stub_syscall_uses_futex.
	 */
	if (um_backend && um_backend->stub_syscall_uses_futex)
		proc_data->futex = FUTEX_IN_CHILD;

	mm_id->pid = clone(userspace_tramp, (void *) sp,
		    CLONE_VFORK | CLONE_VM | SIGCHLD,
		    (void *)&tramp_data);
	if (mm_id->pid < 0) {
		err = -errno;
		printk(UM_KERN_ERR "%s : clone failed, errno = %d\n",
		       __func__, errno);
		goto out_close;
	}

	/*
	 * Wait for the stub child to reach its initial ready state via
	 * the futex round-trip. KVM doesn't reach this code path.
	 */
	wait_stub_done_seccomp(mm_id, 1, 1);

	if (munmap(stack, UM_KERN_PAGE_SIZE) < 0) {
		err = -errno;
		printk(UM_KERN_ERR "%s : munmap failed, errno = %d\n",
		       __func__, errno);
		goto out_kill;
	}

	close(tramp_data.sockpair[0]);
	/*
	 * Retain the parent-side sockpair FD only for backends
	 * that use it for subsequent SCM_RIGHTS FD passing to
	 * the stub child (seccomp). Ptrace closes it; KVM never
	 * gets here. Routed through um_backend->has_syscall_
	 * stub_fd_map.
	 */
	if (um_backend && um_backend->has_syscall_stub_fd_map)
		mm_id->sock = tramp_data.sockpair[1];
	else
		close(tramp_data.sockpair[1]);

	return 0;

out_kill:
	os_kill_ptraced_process(mm_id->pid, 1);
out_close:
	close(tramp_data.sockpair[0]);
	close(tramp_data.sockpair[1]);

	mm_id->pid = -1;

	return err;
}

/*
 * os_skas_reap_stub() - SIGKILL + wait4 the stub child attached to a
 * given mm_id, close the parent-side socketpair end, and zero the
 * stub_data round-trip fields so a subsequent start_userspace()
 * sees fresh state.
 *
 * Used by the template-pause fork-on-resume loop
 * BOTH:
 *   * Pre-fork in the master, where every stub child must be killed
 *     so fork() does not alias their pids into the forked-child
 *     UML's mm_list (where two processes would race to drive them).
 *   * As the first half of start_userspace_redo() below.
 *
 * Idempotent: if mm_id->pid <= 0, the kill/wait is skipped and only
 * the sock close / stub_data zero / id field reset runs.
 *
 * Uses raw __NR_wait4 to avoid glibc's cancellation-point waitpid
 * wrapper from UML kernel context.
 *
 * Returns 0 on success, -errno on kill/wait failure (other than
 * ESRCH/ECHILD which are treated as "stub already gone" and silently
 * tolerated).  On error, leaves @mm_id in a partially-reaped state;
 * the caller is responsible for not driving the stub further.
 */
int os_skas_reap_stub(struct mm_id *mm_id)
{
	struct stub_data *proc_data = (void *)mm_id->stack;
	long ret;
	int err;

	if (mm_id->pid > 0) {
		if (kill(mm_id->pid, SIGKILL) < 0 && errno != ESRCH) {
			err = -errno;
			printk(UM_KERN_ERR "%s: kill(%d, SIGKILL) failed: %d\n",
			       __func__, mm_id->pid, err);
			return err;
		}
		for (;;) {
			ret = syscall(__NR_wait4, mm_id->pid, NULL, __WALL, NULL);
			if (ret == mm_id->pid)
				break;
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				/* ECHILD is fine: stub already reaped by
				 * an earlier SIGCHLD path or was never our
				 * direct child after a fork race.
				 */
				if (errno == ECHILD)
					break;
				err = -errno;
				printk(UM_KERN_ERR "%s: wait4(%d) failed: %d\n",
				       __func__, mm_id->pid, err);
				return err;
			}
		}
	}

	if (mm_id->sock >= 0) {
		close(mm_id->sock);
		mm_id->sock = -1;
	}

	/* Zero the round-trip fields so the new stub's first futex
	 * handshake sees fresh state.  Leave the rest of stub_data
	 * alone; pages, code, mctx storage, fault info; those are
	 * stub-binary-owned and respawn doesn't alter them.
	 */
	proc_data->futex = 0;
	proc_data->signal = 0;
	proc_data->si_offset = 0;
	proc_data->mctx_offset = 0;
	proc_data->syscall_data_len = 0;

	mm_id->pid = -1;
	mm_id->syscall_data_len = 0;
	mm_id->syscall_fd_num = 0;

	return 0;
}

/*
 * start_userspace_fresh() - spawn a stub with private stub_data
 * backing.
 *
 * Standard start_userspace() relies on UML's global physmem_fd
 * for stub_data: the kernel allocates a page via __get_free_pages,
 * phys_mapping() resolves that to (physmem_fd, offset), the stub
 * mmaps physmem_fd at that offset.  For pool-member children
 * post-fork, physmem_fd is MAP_SHARED with the master and sibling
 * pool members, so inherited stub_data would alias across processes.
 *
 * This variant creates a per-call memfd for stub_data, mmaps it
 * MAP_SHARED into the calling process (which becomes the new
 * id->stack VA), and passes the memfd to the stub via the
 * stub_data_fd_override path so the stub mmaps the same memfd
 * (in CLONE_VM child context, the mmap is shared with this
 * process).  Each pool member gets physically-isolated stub_data.
 *
 * Memfd lifecycle: the kernel-side mmap keeps the memfd alive
 * through the mm's destroy_context (where id->stack is freed via
 * munmap path).  The stub's mmap independently keeps the
 * underlying pages alive across the stub's lifetime.  The fd
 * number itself is closed in the parent after passing to the stub.
 *
 * Returns 0 on success or -errno on failure.
 */
int start_userspace_fresh(struct mm_id *mm_id)
{
	const size_t map_size = STUB_DATA_PAGES * UM_KERN_PAGE_SIZE;
	struct stub_data *proc_data;
	struct tramp_data tramp_data;
	void *stack;
	unsigned long sp;
	int data_fd, err;

	/* Per-mm stub_data memfd. */
	data_fd = syscall(__NR_memfd_create, "um-pool-stubdata", 0);
	if (data_fd < 0)
		return -errno;
	if (ftruncate(data_fd, map_size) < 0) {
		err = -errno;
		close(data_fd);
		return err;
	}

	/* mmap MAP_SHARED into this UML kernel's address space.  The
	 * resulting VA replaces mm_id->stack; both kernel-side
	 * accesses and the stub (via CLONE_VM share with same VA)
	 * see the same physical memory backed by data_fd.
	 */
	proc_data = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, data_fd, 0);
	if (proc_data == MAP_FAILED) {
		err = -errno;
		close(data_fd);
		return err;
	}

	/* Zero the new page (memfd_create + ftruncate gives zeros
	 * already, but be explicit for clarity).
	 */
	memset(proc_data, 0, map_size);

	/*
	 * Replace the inherited id->stack with the private mapping.  The
	 * inherited page still belongs to the parent-side stub state.
	 */
	mm_id->stack = (unsigned long)proc_data;

	tramp_data.stub_data = proc_data;
	tramp_data.stub_data_fd_override = data_fd;

	/* Scratch stack for userspace_tramp. */
	stack = mmap(NULL, UM_KERN_PAGE_SIZE,
		     PROT_READ | PROT_WRITE | PROT_EXEC,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) {
		err = -errno;
		goto out_close_data;
	}
	sp = (unsigned long)stack + UM_KERN_PAGE_SIZE;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, tramp_data.sockpair)) {
		err = -errno;
		munmap(stack, UM_KERN_PAGE_SIZE);
		goto out_close_data;
	}

	if (um_backend && um_backend->stub_syscall_uses_futex)
		proc_data->futex = FUTEX_IN_CHILD;

	mm_id->pid = clone(userspace_tramp, (void *)sp,
			   CLONE_VFORK | CLONE_VM | SIGCHLD,
			   (void *)&tramp_data);
	if (mm_id->pid < 0) {
		err = -errno;
		close(tramp_data.sockpair[0]);
		close(tramp_data.sockpair[1]);
		munmap(stack, UM_KERN_PAGE_SIZE);
		goto out_close_data;
	}

	wait_stub_done_seccomp(mm_id, 1, 1);

	if (munmap(stack, UM_KERN_PAGE_SIZE) < 0) {
		err = -errno;
		goto out_kill;
	}

	close(tramp_data.sockpair[0]);

	if (um_backend && um_backend->has_syscall_stub_fd_map)
		mm_id->sock = tramp_data.sockpair[1];
	else
		close(tramp_data.sockpair[1]);

	/* Parent no longer needs the fd reference; stub has its own
	 * mmap keeping the backing alive; our mmap (proc_data) keeps
	 * a kernel-side reference too.
	 */
	close(data_fd);
	return 0;

out_kill:
	os_kill_ptraced_process(mm_id->pid, 1);
out_close_data:
	munmap(proc_data, map_size);
	close(data_fd);
	mm_id->stack = 0;
	mm_id->pid = -1;
	return err;
}

/*
 * start_userspace_redo() - replace a stub child after a fork(2).
 *
 * Equivalent to os_skas_reap_stub() followed by start_userspace().
 * Idempotent w.r.t. the kill side: a previously-reaped mm
 * (pid == -1) is just respawned.  Always respawns.
 *
 * Returns 0 on success, -errno on clone/socketpair failure.  Failure
 * leaves @mm_id->pid == -1 so subsequent vcpu_run sees the dead-mm
 * state that the existing mm_sigchld_irq logic already handles.
 */
int start_userspace_redo(struct mm_id *mm_id)
{
	int err = os_skas_reap_stub(mm_id);

	if (err)
		return err;
	return start_userspace(mm_id);
}

/*
 * Counter shared between both backends' run_userspace impls and
 * reset by switch_threads (below) on every context switch. It
 * implements the time-travel-mode rate-limit on extra scheduler
 * jiffies; counting UNSCHEDULED iterations of the per-backend trap
 * loop body.
 */
unsigned int unscheduled_userspace_iterations;

/*
 * Trap loop. Per-iteration body lives in the active backend's
 * run_userspace op; this function owns the scheduler-facing loop.
 *
 * The dispatch macro is the single source of truth for which backend
 * runs: in *_ONLY builds it expands to a direct call to the chosen
 * backend; in DYNAMIC builds init_backend() set um_backend to
 * match the host probe (using_seccomp) before this function is
 * ever entered.
 */
void userspace(struct uml_pt_regs *regs)
{
	/* Handle any immediate reschedules or signals */
	interrupt_end();

	while (1)
		um_backend_dispatch(vcpu_run, regs);
}

void new_thread(void *stack, jmp_buf *buf, void (*handler)(void))
{
	(*buf)[0].JB_IP = (unsigned long) handler;
	(*buf)[0].JB_SP = (unsigned long) stack + UM_THREAD_SIZE -
		sizeof(void *);
}

#define INIT_JMP_NEW_THREAD 0
#define INIT_JMP_CALLBACK 1
#define INIT_JMP_HALT 2
#define INIT_JMP_REBOOT 3

void switch_threads(jmp_buf *me, jmp_buf *you)
{
	unscheduled_userspace_iterations = 0;

	if (UML_SETJMP(me) == 0)
		UML_LONGJMP(you, 1);
}

static jmp_buf initial_jmpbuf;

static __thread void (*cb_proc)(void *arg);
static __thread void *cb_arg;
static __thread jmp_buf *cb_back;

int start_idle_thread(void *stack, jmp_buf *switch_buf)
{
	int n;

	set_handler(SIGWINCH);

	/*
	 * Can't use UML_SETJMP or UML_LONGJMP here because they save
	 * and restore signals, with the possible side-effect of
	 * trying to handle any signals which came when they were
	 * blocked, which can't be done on this stack.
	 * Signals must be blocked when jumping back here and restored
	 * after returning to the jumper.
	 */
	n = setjmp(initial_jmpbuf);
	switch (n) {
	case INIT_JMP_NEW_THREAD:
		(*switch_buf)[0].JB_IP = (unsigned long) uml_finishsetup;
		(*switch_buf)[0].JB_SP = (unsigned long) stack +
			UM_THREAD_SIZE - sizeof(void *);
		break;
	case INIT_JMP_CALLBACK:
		(*cb_proc)(cb_arg);
		longjmp(*cb_back, 1);
		break;
	case INIT_JMP_HALT:
		kmalloc_ok = 0;
		return 0;
	case INIT_JMP_REBOOT:
		kmalloc_ok = 0;
		return 1;
	default:
		printk(UM_KERN_ERR "Bad sigsetjmp return in %s - %d\n",
		       __func__, n);
		fatal_sigsegv();
	}
	longjmp(*switch_buf, 1);

	/* unreachable */
	printk(UM_KERN_ERR "impossible long jump!");
	fatal_sigsegv();
	return 0;
}

void initial_thread_cb_skas(void (*proc)(void *), void *arg)
{
	jmp_buf here;

	cb_proc = proc;
	cb_arg = arg;
	cb_back = &here;

	initial_jmpbuf_lock();
	if (UML_SETJMP(&here) == 0)
		UML_LONGJMP(&initial_jmpbuf, INIT_JMP_CALLBACK);
	initial_jmpbuf_unlock();

	cb_proc = NULL;
	cb_arg = NULL;
	cb_back = NULL;
}

void halt_skas(void)
{
	initial_jmpbuf_lock();
	UML_LONGJMP(&initial_jmpbuf, INIT_JMP_HALT);
	/* unreachable */
}

static bool noreboot;

static int __init noreboot_cmd_param(char *str, int *add)
{
	*add = 0;
	noreboot = true;
	return 0;
}

__uml_setup("noreboot", noreboot_cmd_param,
"noreboot\n"
"    Rather than rebooting, exit always, akin to QEMU's -no-reboot option.\n"
"    This is useful if you're using CONFIG_PANIC_TIMEOUT in order to catch\n"
"    crashes in CI\n\n");

void reboot_skas(void)
{
	initial_jmpbuf_lock();
	UML_LONGJMP(&initial_jmpbuf, noreboot ? INIT_JMP_HALT : INIT_JMP_REBOOT);
	/* unreachable */
}
