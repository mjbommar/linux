// SPDX-License-Identifier: GPL-2.0
/*
 * UML template-pause user-space helpers.
 *
 * Thin host-syscall wrappers that the in-kernel template_pause path
 * (arch/um/kernel/template_pause.c) needs to reach across the
 * os-Linux boundary:
 *
 *   os_template_pause_stop_self():
 *	Raise SIGSTOP on the current host process and return when the
 *	supervisor sends SIGCONT.  A forked child resumes after the
 *	supervisor has written its identity blob to the memfd at
 *	UM_TEMPLATE_IDENTITY_FD.
 *
 *   os_template_pause_identity_fd():
 *	Resolve UM_TEMPLATE_IDENTITY_FD from the host environment,
 *	validate it is open, and return it.  Returns -ENOENT if the
 *	env var is unset (no identity-blob channel: pool master
 *	before fork, or non-pool boot) or -EBADF if the fd is bogus.
 *
 *   os_template_pause_read_identity(fd, buf, len):
 *	Read up to @len bytes from a memfd; lseek to 0 first so the
 *	supervisor can rewrite the blob between takes without
 *	creating a fresh fd.  Short reads (memfd shorter than @len)
 *	return the bytes actually read; -errno on host failure.
 *
 * These are host-side helpers; they must not call into kernel code.
 * The in-kernel driver lives in arch/um/kernel/template_pause.c.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <linux/compiler_attributes.h>
#include <linux/types.h>

#include <os.h>

#define UM_TEMPLATE_IDENTITY_FD_ENV "UM_TEMPLATE_IDENTITY_FD"

/*
 * SIGSTOP on self.  SIGSTOP cannot be masked or caught; the host
 * kernel suspends the process unconditionally, and execution resumes
 * here only when SIGCONT arrives.  No sigsuspend() needed: kill()
 * returns after the stop+resume cycle as if the call had blocked for
 * the duration of the suspension.
 *
 * Rationale for kill(getpid(), ...) over raise():
 *   - raise() is libc's wrapper around tgkill(getpid(), gettid(),
 *     ...) and uses pthread state.  Plain kill(getpid(), SIGSTOP)
 *     is the simplest primitive and matches existing UML process
 *     signal helpers.
 *
 * Returns 0 on success (SIGSTOP raised + SIGCONT resumed), -errno
 * on syscall failure.
 */
static noinline int
os_template_pause_stop_self_inner(void)
{
	register long rax asm("rax");
	long pid;

	/*
	 * Inline-asm getpid + kill.  Bypasses glibc's syscall()
	 * wrappers; both of which can route through
	 * __syscall_cancel, which has been observed to write to an
	 * internal libc cancellation pipe whose reader thread does
	 * not exist in a forked child (raw fork only duplicates the
	 * calling thread).  Direct syscall instructions have zero
	 * libc state.
	 */

	/* getpid */
	rax = __NR_getpid;
	asm volatile("syscall\n\t"
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

		asm volatile("syscall\n\t"
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
 * fcntl(F_GETFD) so an inherited environment variable with a closed
 * fd fails cleanly before the read path.
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
 * os_snapshot_fork_worker: bypass glibc's cancellation-point and
 * per-thread state machinery from UML kernel context.
 * Returns the child pid in the parent, 0 in the child, or
 * a negative errno.
 *
 * Fork-on-resume uses this for every taken pool member. The KVM-backend
 * restriction is enforced in-kernel (see
 * arch/um/kernel/template_pause.c assert_fork_safety); under KVM
 * fork() aliases /dev/kvm fds and per-vCPU mmap state and corrupts
 * both parent and child.
 */
static noinline int
os_template_pause_fork_inner(void)
{
	register long rax asm("rax") = __NR_fork;

	asm volatile("syscall\n\t"
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
 * Fork primitive: __NR_clone with private MAP_PRIVATE child stack.
 * Giving the child its own stack region prevents the master's
 * post-fork stack writes from corrupting the child's view.
 *
 * Semantics:
 *   * Parent returns: child PID (positive) or -errno.  Same as fork.
 *   * Child returns: does not return.  Exits via __NR_exit_group(0)
 *     inline.  The child is on a private stack; it cannot do ret
 *     because the private stack is empty.  Any C code that would
 *     run in the child must be inline-asm-only, never touching
 *     locals or function calls.
 *
 * Stack-cache discipline: the 8 KiB private stack is allocated once
 * (file-static) and reused across all iterations.  This is safe
 * because master never writes to the stack (only the child does,
 * via the inline-asm post-clone path, and the child's writes
 * trigger CoW; master's page stays clean).  Without caching, a
 * long-running master accumulates one 8 KiB VMA per iteration
 * (12k iters -> 96 MB virtual / thousands of /proc/<pid>/maps
 * entries).
 */
/*
 * 8 KiB is enough for the only thing the child does (one __NR_exit_group
 * inline asm).  16-byte top offset reserves the x86_64 ABI's red-zone
 * scratch + alignment headroom even though the child does not use C
 * stack frames.
 */
#define TEMPLATE_PAUSE_CLONE_STACK_BYTES	8192
#define TEMPLATE_PAUSE_CLONE_STACK_TOP_OFF	16

/* x86_64 syscall numbers used by the inline clone+exit dance.  Hard-
 * coded rather than pulled from <asm/unistd.h> because UML's os-Linux
 * side is built against the host's libc headers; the host's libc
 * numbering matches the x86_64 ABI targeted by this helper.
 */
#define TEMPLATE_PAUSE_NR_CLONE_X86_64		56
#define TEMPLATE_PAUSE_NR_EXIT_GROUP_X86_64	231
#define TEMPLATE_PAUSE_CLONE_FLAGS_FORK_LIKE	17 /* SIGCHLD */

static void *os_template_pause_clone_stack_base;

int os_template_pause_fork_clone(void)
{
	unsigned long child_stack_top;
	long ret;

	if (!os_template_pause_clone_stack_base) {
		void *base = mmap(NULL, TEMPLATE_PAUSE_CLONE_STACK_BYTES,
				  PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS,
				  -1, 0);
		if (base == MAP_FAILED)
			return -errno;
		os_template_pause_clone_stack_base = base;
	}
	child_stack_top = (unsigned long)os_template_pause_clone_stack_base +
			   TEMPLATE_PAUSE_CLONE_STACK_BYTES -
			   TEMPLATE_PAUSE_CLONE_STACK_TOP_OFF;

	/*
	 * Raw __NR_clone. In the child, rax = 0 and RSP =
	 * child_stack_top; in the parent, rax = child pid.
	 *
	 * After the syscall, both halves resume in this function. We
	 * must distinguish them and route the child to exit_group via
	 * raw inline asm only; the child has no usable C stack.
	 */
	{
		register long rax asm("rax") = TEMPLATE_PAUSE_NR_CLONE_X86_64;
		register long rdi asm("rdi") =
			TEMPLATE_PAUSE_CLONE_FLAGS_FORK_LIKE;
		register long rsi asm("rsi") = child_stack_top;
		register long rdx asm("rdx") = 0;  /* parent_tid */
		register long r10 asm("r10") = 0;  /* child_tid */
		register long r8  asm("r8")  = 0;  /* tls */

		asm volatile("syscall\n\t"
			"testq %%rax, %%rax\n\t"
			"jnz 1f\n\t"
			/* CHILD path: rax = 0.  Do raw exit_group(0).
			 * Cannot use C: RSP is the private stack, no
			 * usable locals.  Cannot ret.
			 */
			"movq %[exit_nr], %%rax\n\t"
			"xorq %%rdi, %%rdi\n\t"
			"syscall\n\t"
			"2: jmp 2b\n\t"           /* unreachable */
			"1:\n\t"
			: "+r" (rax)
			: "r" (rdi), "r" (rsi), "r" (rdx),
			  "r" (r10), "r" (r8),
			  [exit_nr] "i" (TEMPLATE_PAUSE_NR_EXIT_GROUP_X86_64)
			: "rcx", "r11", "memory"
		);
		ret = rax;
	}

	/*
	 * Parent only reaches here.  The cached
	 * os_template_pause_clone_stack_base is preserved for the next
	 * call; see the static-cache rationale above.
	 */
	if (ret < 0 && ret > -4096)
		return (int)ret;
	return (int)ret;
}

/*
 * Variant of os_template_pause_fork_clone() that, in the child, does
 * not exit_group(0); instead jmpq's into a caller-supplied @entry on
 * the private stack.
 *
 * This is the stack-pivot primitive applied to the post-clone child
 * path.  The child's %rsp points at a fresh MAP_PRIVATE region; it
 * does not share saved return slots on the parent's kernel stack.
 *
 * Contract:
 *   - @entry must not return.  Mark it __noreturn.
 *   - @entry runs on an 8 KiB MAP_PRIVATE stack; no large C frames.
 *   - @entry runs with the kernel's data segment fully visible (post-
 *     fork, all pages are present) but with master's locks/preempt
 *     state inherited.  The first call inside @entry should be
 *     preempt_enable() if master held it.
 *   - Returns child PID in the parent, or -errno on clone failure.
 *
 * The stack is allocated once and reused across calls.  The fork-on-resume
 * loop calls this many times per master lifetime, and re-mmap'ing
 * costs ~5us per call.  Children that survive past the first jmpq
 * own their stack page until the child process exits. Concurrent
 * pool-member callers would need a per-call mmap.
 */
typedef void __noreturn (*os_template_pause_child_entry_t)(void);

int os_template_pause_fork_clone_to(os_template_pause_child_entry_t entry)
{
	unsigned long child_stack_top;
	long ret;

	if (!entry)
		return -EINVAL;

	if (!os_template_pause_clone_stack_base) {
		void *base = mmap(NULL, TEMPLATE_PAUSE_CLONE_STACK_BYTES,
				  PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS,
				  -1, 0);
		if (base == MAP_FAILED)
			return -errno;
		os_template_pause_clone_stack_base = base;
	}
	child_stack_top = (unsigned long)os_template_pause_clone_stack_base +
			   TEMPLATE_PAUSE_CLONE_STACK_BYTES -
			   TEMPLATE_PAUSE_CLONE_STACK_TOP_OFF;

	/*
	 * Raw __NR_clone.  In the CHILD, rax = 0 and RSP = child_stack_top;
	 * the CHILD then does:
	 *	xorq %rbp, %rbp
	 *	jmpq *%[entry]
	 * No ret, no call, no C frame: the
	 * child never touches the parent's kernel stack again.
	 */
	{
		register long rax asm("rax") = TEMPLATE_PAUSE_NR_CLONE_X86_64;
		register long rdi asm("rdi") =
			TEMPLATE_PAUSE_CLONE_FLAGS_FORK_LIKE;
		register long rsi asm("rsi") = child_stack_top;
		register long rdx asm("rdx") = 0;
		register long r10 asm("r10") = 0;
		register long r8  asm("r8")  = 0;

		asm volatile("syscall\n\t"
			"testq %%rax, %%rax\n\t"
			"jnz 1f\n\t"
			/*
			 * CHILD path: rax = 0.  Pivot to clean state and
			 * jmpq into @entry.  RSP is already the private
			 * stack (clone() set it from rsi).
			 */
			"xorq %%rbp, %%rbp\n\t"
			"jmpq *%[entry]\n\t"
			"2: jmp 2b\n\t"           /* unreachable */
			"1:\n\t"
			: "+r" (rax)
			: "r" (rdi), "r" (rsi), "r" (rdx),
			  "r" (r10), "r" (r8),
			  [entry] "r" (entry)
			: "rcx", "r11", "memory"
		);
		ret = rax;
	}

	if (ret < 0 && ret > -4096)
		return (int)ret;
	return (int)ret;
}

/*
 * Host-level signal block.  os_snapshot_block_iter_signals only
 * blocks UML's TLS-level signals_enabled dispatch flag; the host
 * kernel still delivers signals to the UML host process, and UML's
 * hard_handler -> sig_handler runs on the interrupted task's
 * kernel stack.  When the stack is tight, the handler's own C
 * frame can overwrite ancestor saved-RIP slots, corrupting the
 * ret of um_template_pause_enter's epilogue post-fork.
 *
 * Raw __NR_rt_sigprocmask bypasses glibc; glibc's sigprocmask
 * routes through __syscall_cancel which has its own post-fork
 * hazards (the cancellation pipe).
 *
 * The mask excludes SIGSTOP/SIGCONT/SIGKILL (cannot be blocked
 * anyway) and SIGSEGV/SIGBUS/SIGILL/SIGFPE (blocking synchronous
 * signals is undefined; they would still be delivered and would
 * still corrupt the stack.  This path assumes those signals are not
 * raised.
 *
 * The saved mask is file scope so the matching restore can find it
 * without the caller having to track sigset_t.
 * Single-threaded use only (template-pause master + forked child
 * each have their own .data copy via CoW).
 */
/*
 * Kernel sigset_t is 8 bytes on x86_64 (64-bit mask).  glibc's
 * userspace sigset_t is 128 bytes (1024 bits). When
 * calling rt_sigprocmask via raw syscall, the size arg must be the
 * kernel size (8), not sizeof(sigset_t) which is glibc's. Use a
 * raw u64 to hold the mask so the helper does not depend on sigset_t
 * representation.
 */
#define KERNEL_SIGSET_BYTES 8

static unsigned long os_template_pause_saved_sigmask;
static int os_template_pause_sigmask_armed;

int os_template_pause_signals_block_host(void)
{
	unsigned long fillmask;
	long ret;
	unsigned long *saveptr;

	/*
	 * Idempotent: if already armed, do not overwrite
	 * os_template_pause_saved_sigmask; that snapshot must
	 * reflect the pre-first-block state so restore returns to
	 * the unblocked mask. Subsequent calls just re-set the same
	 * mask (no-op at kernel level).
	 */
	saveptr = os_template_pause_sigmask_armed ?
		  NULL : &os_template_pause_saved_sigmask;

	/* Block everything except SIGKILL (can't be blocked anyway),
	 * SIGSTOP / SIGCONT (need for pause/resume), and the
	 * synchronous fault signals (their delivery is undefined when
	 * blocked).  Signal numbers on x86_64 Linux:
	 *   SIGKILL=9, SIGSEGV=11, SIGFPE=8, SIGBUS=7, SIGILL=4,
	 *   SIGSTOP=19, SIGCONT=18.
	 */
	fillmask = ~0UL;
	fillmask &= ~(1UL << (9 - 1));   /* SIGKILL */
	fillmask &= ~(1UL << (19 - 1));  /* SIGSTOP */
	fillmask &= ~(1UL << (18 - 1));  /* SIGCONT */
	fillmask &= ~(1UL << (11 - 1));  /* SIGSEGV */
	fillmask &= ~(1UL << (7 - 1));   /* SIGBUS */
	fillmask &= ~(1UL << (4 - 1));   /* SIGILL */
	fillmask &= ~(1UL << (8 - 1));   /* SIGFPE */

	ret = syscall(__NR_rt_sigprocmask, SIG_SETMASK,
		      &fillmask, saveptr,
		      KERNEL_SIGSET_BYTES);
	if (ret < 0)
		return -errno;
	os_template_pause_sigmask_armed = 1;
	return 0;
}

int os_template_pause_signals_restore_host(void)
{
	long ret;

	if (!os_template_pause_sigmask_armed)
		return 0;
	ret = syscall(__NR_rt_sigprocmask, SIG_SETMASK,
		      &os_template_pause_saved_sigmask, NULL,
		      KERNEL_SIGSET_BYTES);
	os_template_pause_sigmask_armed = 0;
	if (ret < 0)
		return -errno;
	return 0;
}

/*
 * Write the just-forked child's host pid into the identity memfd
 * at offset @offset (typically sizeof(struct um_template_identity)
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
	 * Raw __NR_pwrite64 bypasses glibc's cancellation-point
	 * machinery (__syscall_cancel) from post-fork UML kernel context.
	 *
	 * pwrite is positional; no separate lseek call is needed, so this
	 * avoids a second cancellation-point-affected call.
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
 * Raw __NR_exit_group helper so a forked-child UML kernel
 * can exit cleanly without going through glibc's at_exit handlers
 * (which are not safe to invoke from UML kernel context post-fork,
 * for the same reasons that wait4 / kill via glibc hit the
 * cancellation-point hazard).
 */
void os_template_pause_child_exit(int code)
{
	/* Inline-asm syscall: bypasses glibc.  exit_group never
	 * returns, so there is no need to capture rax.  Args: edi = code.
	 */
	register long rax_in asm("rax") = __NR_exit_group;
	register long rdi_in asm("rdi") = (long)code;
	asm volatile("syscall\n\t"
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
 * member, so seek to 0 before reading.  Short reads (memfd is
 * smaller than @len) are tolerated; caller asserts magic + version
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
