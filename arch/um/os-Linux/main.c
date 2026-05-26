// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/personality.h>
#include <as-layout.h>
#include <backend.h>
#include <init.h>
#include <kern_util.h>
#include <os.h>
#include <um_malloc.h>
#include "internal.h"

#define STACKSIZE (8 * 1024 * 1024)

static void __init set_stklim(void)
{
	struct rlimit lim;

	if (getrlimit(RLIMIT_STACK, &lim) < 0) {
		perror("getrlimit");
		exit(1);
	}
	if ((lim.rlim_cur == RLIM_INFINITY) || (lim.rlim_cur > STACKSIZE)) {
		lim.rlim_cur = STACKSIZE;
		if (setrlimit(RLIMIT_STACK, &lim) < 0) {
			perror("setrlimit");
			exit(1);
		}
	}
}

static void last_ditch_exit(int sig)
{
	/*
	 * Async-signal-handler context — only async-signal-safe
	 * operations allowed here (see signal-safety(7)).
	 *
	 * The previous implementation called uml_cleanup() from here
	 * and then exit(). That is unsafe: uml_cleanup() walks the
	 * task list under tasklist_lock, runs the __exitcall() chain
	 * (including console_exit → free_irq → __mutex_lock → might
	 * _sleep), and dispatches um_backend->shutdown() through
	 * kmalloc-capable code paths. free_irq(3) also WARN()s when
	 * called with in_interrupt() == true, which is the state a
	 * signal inherits when it preempts a kernel raw_spin_lock_
	 * irqsave region. Under PROVE_LOCKING + DEBUG_ATOMIC_SLEEP
	 * (research profile) the result is a "Trying to free IRQ
	 * from IRQ context" WARN plus a sleeping-in-atomic BUG on
	 * every SIGTERM.
	 *
	 * Instead, exit immediately. The host kernel closes all fds,
	 * unmaps all mmaps, and delivers SIGKILL to every stub child
	 * — every stub is forked with PR_SET_PDEATHSIG=SIGKILL (see
	 * arch/um/os-Linux/process.c and arch/um/kernel/skas/stub_
	 * exe.c). We lose the um_backend->shutdown() dispatch (no-op
	 * today for PTRACE / SECCOMP, nice-to-have for a future KVM
	 * backend but not load-bearing), the __exitcall chain (the
	 * host reclaims the resources those exitcalls would release),
	 * and the ptraced-task kill loop (pdeathsig covers it).
	 *
	 * install_fatal_handler() sets SA_RESETHAND, so a second
	 * signal of the same type hits the default disposition —
	 * safety net if something hangs before _exit completes.
	 *
	 * Use _exit() (async-signal-safe) rather than exit() to skip
	 * atexit() and stdio cleanup, neither of which is signal-safe.
	 */
	static const char msg[] = "UML: fatal signal; exiting\n";
	ssize_t ret;

	/*
	 * write(2) is marked warn_unused_result in glibc, and this
	 * is an async-signal-handler context — the only sane
	 * response to a short write or EINTR here is to exit anyway.
	 * Consume the return value explicitly to silence the
	 * warning; a bare `(void)write(...)` does not.
	 */
	ret = write(STDERR_FILENO, msg, sizeof(msg) - 1);
	(void)ret;
	_exit(1);
}

static void __init install_fatal_handler(int sig)
{
	struct sigaction action;

	/* All signals are enabled in this handler ... */
	sigemptyset(&action.sa_mask);

	/*
	 * ... including the signal being handled, plus we want the
	 * handler reset to the default behavior, so that if an exit
	 * handler is hanging for some reason, the UML will just die
	 * after this signal is sent a second time.
	 */
	action.sa_flags = SA_RESETHAND | SA_NODEFER;
	action.sa_restorer = NULL;
	action.sa_handler = last_ditch_exit;
	if (sigaction(sig, &action, NULL) < 0) {
		os_warn("failed to install handler for signal %d "
			"- errno = %d\n", sig, errno);
		exit(1);
	}
}

#define UML_LIB_PATH	":" OS_LIB_PATH "/uml"

static void __init setup_env_path(void)
{
	char *new_path = NULL;
	char *old_path = NULL;
	int path_len = 0;

	old_path = getenv("PATH");
	/*
	 * if no PATH variable is set or it has an empty value
	 * just use the default + /usr/lib/uml
	 */
	if (!old_path || (path_len = strlen(old_path)) == 0) {
		if (putenv("PATH=:/bin:/usr/bin/" UML_LIB_PATH))
			perror("couldn't putenv");
		return;
	}

	/* append /usr/lib/uml to the existing path */
	path_len += strlen("PATH=" UML_LIB_PATH) + 1;
	new_path = malloc(path_len);
	if (!new_path) {
		perror("couldn't malloc to set a new PATH");
		return;
	}
	snprintf(new_path, path_len, "PATH=%s" UML_LIB_PATH, old_path);
	if (putenv(new_path)) {
		perror("couldn't putenv to set a new PATH");
		free(new_path);
	}
}

int __init main(int argc, char **argv, char **envp)
{
	char **new_argv;
	int ret, i, err;

	/* Disable randomization and re-exec if it was changed successfully */
	ret = personality(PER_LINUX | ADDR_NO_RANDOMIZE);
	if (ret >= 0 && (ret & (PER_LINUX | ADDR_NO_RANDOMIZE)) !=
			 (PER_LINUX | ADDR_NO_RANDOMIZE)) {
		char buf[4096] = {};
		ssize_t ret;

		ret = readlink("/proc/self/exe", buf, sizeof(buf));
		if (ret < 0 || ret >= sizeof(buf)) {
			perror("readlink failure");
			exit(1);
		}
		execve(buf, argv, envp);
	}

	set_stklim();

	/*
	 * SMP-T71 (Round 13): optionally raise RLIMIT_MEMLOCK and
	 * mlockall all current + future mappings. Combined with
	 * MAP_POPULATE in os_map_memory, this would prevent the host
	 * kernel from migrating / reclaiming / KSM-merging UML's
	 * physmem pages. Round 13 T72 disproved the working hypothesis
	 * (UML doesn't host-unmap guest user pages at all — the
	 * mmu_notifier traffic was on guest kernel VAs only), so
	 * pinning is no longer dispositive. Kept as a gated knob in
	 * case future work needs it.
	 *
	 * Gated by UM_KVM_V2_PIN_PHYSMEM env var. Note: umlctl strips
	 * the env when spawning, passing only PATH/HOME/USER/LANG/TERM
	 * plus the [env] section of the toml — neither carries this
	 * var by default. To activate, the soak harness must export it
	 * before the umlctl invocation OR add it to umlctl's
	 * passthrough list in tools/uml/uml-launcher/src/bin/umlctl/
	 * gate.rs and up.rs.
	 *
	 * Both setrlimit and mlockall require CAP_IPC_LOCK or a raised
	 * RLIMIT_MEMLOCK — non-root execution returns EPERM/ENOMEM.
	 */
	if (getenv("UM_KVM_V2_PIN_PHYSMEM")) {
		struct rlimit rl;

		rl.rlim_cur = RLIM_INFINITY;
		rl.rlim_max = RLIM_INFINITY;
		if (setrlimit(RLIMIT_MEMLOCK, &rl) < 0)
			perror("SMP-T71 setrlimit(RLIMIT_MEMLOCK)");
		if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) < 0)
			perror("SMP-T71 mlockall");
	}

	/*
	 * SMP-T80 (memo 52 §1.3): apply OOM score adjustment if
	 * UM_OOM_SCORE_ADJ env var is set. Value range [-1000, +1000];
	 * the host kernel clamps out-of-range values. Operator policy:
	 *   - Soak harness: UM_OOM_SCORE_ADJ=+500 → UML is expendable,
	 *     gets killed first when host is under memory pressure.
	 *   - Long-running research session: -500 → UML is precious.
	 *   - Production: 0 (host policy decides).
	 */
	{
		const char *oom_str = getenv("UM_OOM_SCORE_ADJ");

		if (oom_str) {
			int fd = open("/proc/self/oom_score_adj",
				      O_WRONLY | O_CLOEXEC);

			if (fd < 0) {
				perror("SMP-T80 open(/proc/self/oom_score_adj)");
			} else {
				ssize_t n = write(fd, oom_str, strlen(oom_str));

				if (n < 0)
					perror("SMP-T80 write(oom_score_adj)");
				close(fd);
			}
		}
	}

	/*
	 * SMP-T82 (memo 52 §2.2): apply process-level CPU affinity if
	 * UM_KVM_V2_CPU_AFFINITY env var is set. Format: comma-
	 * separated CPU list with optional ranges, e.g. "0-3" or
	 * "0,2,4". Confines the UML process (and all its host threads)
	 * to the listed CPU subset.
	 *
	 * Under v2's per-host-CPU vCPU pool design, this consolidates
	 * the working set: all guest tasks dispatching land on vCPUs
	 * within the listed CPU subset, which keeps the host's per-CPU
	 * KVM caches (mmu_cache, posted_interrupts) warm. Cross-vCPU
	 * transitions within the subset still occur (load balancing
	 * within the affinity mask) but the host-thread-migrates-to-
	 * a-cold-CPU class is eliminated.
	 *
	 * Parser is intentionally simple: handles "M" / "M-N" /
	 * "M,N,..." / combinations. Anything malformed → perror +
	 * inherit existing affinity (don't fail the boot).
	 */
	{
		const char *aff = getenv("UM_KVM_V2_CPU_AFFINITY");

		if (aff && *aff) {
			cpu_set_t mask;
			const char *p = aff;
			char *end;
			long lo, hi;
			int parsed_any = 0;

			CPU_ZERO(&mask);
			while (*p) {
				lo = strtol(p, &end, 10);
				if (end == p)
					break;
				p = end;
				hi = lo;
				if (*p == '-') {
					p++;
					hi = strtol(p, &end, 10);
					if (end == p)
						break;
					p = end;
				}
				if (lo >= 0 && hi >= lo && hi < CPU_SETSIZE) {
					long cpu;

					for (cpu = lo; cpu <= hi; cpu++)
						CPU_SET((int)cpu, &mask);
					parsed_any = 1;
				}
				if (*p == ',')
					p++;
				else if (*p)
					break;
			}
			if (parsed_any) {
				int rc;

				errno = 0;
				rc = sched_setaffinity(0, sizeof(mask), &mask);
				if (rc < 0 && errno != 0)
					perror("SMP-T82 sched_setaffinity");
			} else {
				fprintf(stderr,
					"SMP-T82: malformed UM_KVM_V2_CPU_AFFINITY='%s' — ignoring\n",
					aff);
			}
		}
	}

	/*
	 * Memo 06 Phase 1 / HONEST-AUDIT §7: per-vCPU host-thread
	 * affinity.  In kvm-v2's per-host-CPU pool design, vcpus[N] is
	 * already used only from smp_processor_id() == N, so the
	 * "thread N pinned to host CPU N" property is enforced by
	 * construction — there's nothing extra to sched_setaffinity()
	 * at this point.
	 *
	 * The env var is read here so the value flows into the boot
	 * log (mission Phase 4 verifies its presence + the
	 * pool-design property), and so an operator misconception
	 * ("did umlctl pass my vcpu_thread_affinity through?") has
	 * an observable answer in the boot output.  Real per-task
	 * pinning (cgroup cpuset for guest userspace tasks) is the
	 * follow-on documented in memo 52 §3.2 Tier 3.
	 */
	{
		const char *vaff = getenv("UM_KVM_V2_VCPU_AFFINITY");

		if (vaff && *vaff && strcmp(vaff, "off") != 0)
			fprintf(stderr,
				"SMP-T82b: UM_KVM_V2_VCPU_AFFINITY='%s' noted; per-vCPU pinning is enforced by the kvm-v2 per-host-CPU pool (memo 06 Phase 1).\n",
				vaff);
	}

	setup_env_path();

	setsid();

	new_argv = malloc((argc + 1) * sizeof(char *));
	if (new_argv == NULL) {
		perror("Mallocing argv");
		exit(1);
	}
	for (i = 0; i < argc; i++) {
		new_argv[i] = strdup(argv[i]);
		if (new_argv[i] == NULL) {
			perror("Mallocing an arg");
			exit(1);
		}
	}
	new_argv[argc] = NULL;

	/*
	 * Allow these signals to bring down a UML if all other
	 * methods of control fail.
	 */
	install_fatal_handler(SIGINT);
	install_fatal_handler(SIGTERM);

	scan_elf_aux(envp);

	change_sig(SIGPIPE, 0);
	ret = linux_main(argc, argv, envp);

	/*
	 * Disable SIGPROF - I have no idea why libc doesn't do this or turn
	 * off the profiling time, but UML dies with a SIGPROF just before
	 * exiting when profiling is active.
	 */
	change_sig(SIGPROF, 0);

	/*
	 * This signal stuff used to be in the reboot case.  However,
	 * sometimes a timer signal can come in when we're halting (reproducably
	 * when writing out gcov information, presumably because that takes
	 * some time) and cause a segfault.
	 */

	/* stop timers and set timer signal to be ignored */
	um_backend_dispatch(set_timer, 0, 0, UM_TIMER_DISABLE);

	/* disable SIGIO for the fds and set SIGIO to be ignored */
	err = deactivate_all_fds();
	if (err)
		os_warn("deactivate_all_fds failed, errno = %d\n", -err);

	/*
	 * Let any pending signals fire now.  This ensures
	 * that they won't be delivered after the exec, when
	 * they are definitely not expected.
	 */
	unblock_signals();

	os_info("\n");
	/* Reboot */
	if (ret) {
		execvp(new_argv[0], new_argv);
		perror("Failed to exec kernel");
		ret = 1;
	}
	return uml_exitcode;
}

extern void *__real_malloc(int);
extern void __real_free(void *);

/* workaround for -Wmissing-prototypes warnings */
void *__wrap_malloc(int size);
void *__wrap_calloc(int n, int size);
void __wrap_free(void *ptr);

void *__wrap_malloc(int size)
{
	void *ret;

	if (!kmalloc_ok)
		return __real_malloc(size);
	else if (size <= UM_KERN_PAGE_SIZE)
		/* finding contiguous pages can be hard*/
		ret = uml_kmalloc(size, UM_GFP_KERNEL);
	else ret = vmalloc(size);

	/*
	 * glibc people insist that if malloc fails, errno should be
	 * set by malloc as well. So we do.
	 */
	if (ret == NULL)
		errno = ENOMEM;

	return ret;
}

void *__wrap_calloc(int n, int size)
{
	void *ptr = __wrap_malloc(n * size);

	if (ptr == NULL)
		return NULL;
	memset(ptr, 0, n * size);
	return ptr;
}

void __wrap_free(void *ptr)
{
	unsigned long addr = (unsigned long) ptr;

	/*
	 * We need to know how the allocation happened, so it can be correctly
	 * freed.  This is done by seeing what region of memory the pointer is
	 * in -
	 * 	physical memory - kmalloc/kfree
	 *	kernel virtual memory - vmalloc/vfree
	 * 	anywhere else - malloc/free
	 * If kmalloc is not yet possible, then either high_physmem and/or
	 * end_vm are still 0 (as at startup), in which case we call free, or
	 * we have set them, but anyway addr has not been allocated from those
	 * areas. So, in both cases __real_free is called.
	 *
	 * CAN_KMALLOC is checked because it would be bad to free a buffer
	 * with kmalloc/vmalloc after they have been turned off during
	 * shutdown.
	 * XXX: However, we sometimes shutdown CAN_KMALLOC temporarily, so
	 * there is a possibility for memory leaks.
	 */

	/*
	 * Host-VA range check: was this pointer kmalloc'd from the physmem
	 * region? Both bounds are host VAs. Today high_physmem ==
	 * __binary_start_hva + physmem_size; under v2 that equality
	 * breaks (high_physmem stays a kernel-pgd-VA concept) and this
	 * upper bound will need a __binary_end_hva sibling. Memo 25 R1.
	 */
	if ((addr >= __binary_start_hva) && (addr < high_physmem)) {
		if (kmalloc_ok)
			kfree(ptr);
	}
	else if ((addr >= start_vm) && (addr < end_vm)) {
		if (kmalloc_ok)
			vfree(ptr);
	}
	else __real_free(ptr);
}
