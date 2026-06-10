// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
	 * Async-signal-handler context: only async-signal-safe
	 * operations allowed here (see signal-safety(7)).
	 *
	 * Do not call uml_cleanup(), exit(), __exitcall handlers, or
	 * um_backend->shutdown() from this path. They may take locks,
	 * allocate memory, or run console/IRQ teardown code, none of
	 * which is signal-safe.
	 *
	 * Use _exit() and leave process resource cleanup to the host
	 * kernel. Stub children are created with PR_SET_PDEATHSIG=SIGKILL,
	 * so they die with the UML parent. install_fatal_handler() sets
	 * SA_RESETHAND, making a repeated signal fall back to the default
	 * disposition. _exit() also skips atexit() and stdio cleanup.
	 */
	static const char msg[] = "UML: fatal signal; exiting\n";
	ssize_t ret;

	/*
	 * write(2) is marked warn_unused_result in glibc, and this
	 * is an async-signal-handler context; the only sane
	 * response to a short write or EINTR here is to exit anyway.
	 * Consume the return value explicitly to silence the
	 * warning; a bare (void)write(...) does not.
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
		os_warn("failed to install handler for signal %d - errno = %d\n",
			sig, errno);
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
	if (old_path)
		path_len = strlen(old_path);
	if (!path_len) {
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

static void __init disable_aslr_and_reexec(char **argv, char **envp)
{
	int ret;

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
}

static void __init maybe_pin_physmem(void)
{
	/*
	 * Optionally raise RLIMIT_MEMLOCK and mlockall all current and
	 * subsequent mappings. Combined with MAP_POPULATE in os_map_memory,
	 * this prevents the host kernel from migrating, reclaiming, or
	 * KSM-merging UML's physmem pages. This is a launcher-controlled
	 * tuning knob.
	 *
	 * Gated by UM_KVM_V2_PIN_PHYSMEM. Launchers that sanitize the
	 * environment must pass it explicitly.
	 *
	 * Both setrlimit and mlockall require CAP_IPC_LOCK or a raised
	 * RLIMIT_MEMLOCK; non-root execution returns EPERM/ENOMEM.
	 */
	if (getenv("UM_KVM_V2_PIN_PHYSMEM")) {
		struct rlimit rl;

		rl.rlim_cur = RLIM_INFINITY;
		rl.rlim_max = RLIM_INFINITY;
		if (setrlimit(RLIMIT_MEMLOCK, &rl) < 0)
			perror("um: setrlimit(RLIMIT_MEMLOCK)");
		if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) < 0)
			perror("um: mlockall");
	}
}

static void __init maybe_set_oom_score_adj(void)
{
	/*
	 * Apply OOM score adjustment if UM_OOM_SCORE_ADJ is set. Value
	 * range is [-1000, +1000]; the host kernel clamps out-of-range
	 * values. Positive values make UML easier to reclaim under memory
	 * pressure; negative values protect it.
	 */
	const char *oom_str = getenv("UM_OOM_SCORE_ADJ");
	int fd;

	if (!oom_str)
		return;

	fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);

	if (fd < 0) {
		perror("um: open(/proc/self/oom_score_adj)");
		return;
	}

	if (write(fd, oom_str, strlen(oom_str)) < 0)
		perror("um: write(oom_score_adj)");
	close(fd);
}

static int __init parse_cpu_range(const char **pos, cpu_set_t *mask)
{
	char *end;
	long lo, hi;
	long cpu;

	lo = strtol(*pos, &end, 10);
	if (end == *pos)
		return 0;

	*pos = end;
	hi = lo;
	if (**pos == '-') {
		(*pos)++;
		hi = strtol(*pos, &end, 10);
		if (end == *pos)
			return 0;
		*pos = end;
	}

	if (lo < 0 || hi < lo || hi >= CPU_SETSIZE)
		return 0;

	for (cpu = lo; cpu <= hi; cpu++)
		CPU_SET((int)cpu, mask);
	return 1;
}

static int __init parse_cpu_affinity(const char *aff, cpu_set_t *mask)
{
	const char *p = aff;
	int parsed_any = 0;

	CPU_ZERO(mask);
	while (*p) {
		if (!parse_cpu_range(&p, mask))
			return parsed_any;
		parsed_any = 1;

		if (*p == ',')
			p++;
		else if (*p)
			return parsed_any;
	}

	return parsed_any;
}

static void __init maybe_set_cpu_affinity(void)
{
	const char *aff = getenv("UM_KVM_V2_CPU_AFFINITY");
	cpu_set_t mask;

	if (!aff || !*aff)
		return;

	/*
	 * Apply process-level CPU affinity if
	 * UM_KVM_V2_CPU_AFFINITY env var is set. Format: comma-
	 * separated CPU list with optional ranges, e.g. "0-3" or
	 * "0,2,4". Confines the UML process (and all its host threads)
	 * to the listed CPU subset.
	 *
	 * Under v2's per-host-CPU vCPU pool design, this consolidates
	 * the working set: all guest tasks dispatch onto vCPUs
	 * within the listed CPU subset, which keeps the host's per-CPU
	 * KVM caches (mmu_cache, posted_interrupts) warm. Cross-vCPU
	 * transitions within the subset still occur (load balancing
	 * within the affinity mask) but the host-thread-migrates-to-
	 * a-cold-CPU class is eliminated.
	 *
	 * Parser is intentionally simple: handles "M" / "M-N" /
	 * "M,N,..." / combinations. Anything malformed means perror +
	 * inherit existing affinity (don't fail the boot).
	 */
	if (parse_cpu_affinity(aff, &mask)) {
		errno = 0;
		if (sched_setaffinity(0, sizeof(mask), &mask) < 0 &&
		    errno != 0)
			perror("um: sched_setaffinity");
	} else {
		fprintf(stderr,
			"um: malformed UM_KVM_V2_CPU_AFFINITY='%s'; ignoring\n",
			aff);
	}
}

static void __init maybe_note_vcpu_affinity(void)
{
	const char *vaff = getenv("UM_KVM_V2_VCPU_AFFINITY");

	if (!vaff || !*vaff || strcmp(vaff, "off") == 0)
		return;

	/*
	 * Per-vCPU host-thread affinity. In kvm-v2's per-host-CPU pool
	 * design, vcpus[N] is
	 * already used only from smp_processor_id() == N, so the
	 * "thread N pinned to host CPU N" property is enforced by
	 * construction; there is nothing extra to sched_setaffinity()
	 * at this point.
	 *
	 * The env var is read here so the value flows into the boot
	 * log and so configuration mistakes have
	 * an observable answer in the boot output.  Real per-task
	 * pinning (cgroup cpuset for guest userspace tasks) is separate.
	 */
	fprintf(stderr,
		"um: UM_KVM_V2_VCPU_AFFINITY='%s' noted; per-vCPU pinning is enforced by the kvm-v2 per-host-CPU pool.\n",
		vaff);
}

static void __init apply_host_resource_env(void)
{
	maybe_pin_physmem();
	maybe_set_oom_score_adj();
	maybe_set_cpu_affinity();
	maybe_note_vcpu_affinity();
}

static char **__init copy_argv(int argc, char **argv)
{
	char **new_argv;
	int i;

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
	return new_argv;
}

static void __init prepare_fatal_handlers(void)
{
	/*
	 * Allow these signals to bring down a UML if all other methods of
	 * control fail.
	 */
	install_fatal_handler(SIGINT);
	install_fatal_handler(SIGTERM);
}

static void shutdown_host_io(void)
{
	int err;

	/*
	 * Disable SIGPROF before shutdown. Profiling timers can otherwise
	 * deliver SIGPROF just before exit when profiling is active.
	 */
	change_sig(SIGPROF, 0);

	/*
	 * A timer signal can arrive while halting, particularly while writing
	 * gcov data, and run against partially torn-down state.
	 */

	/* stop timers and set timer signal to be ignored */
	um_backend_dispatch(set_timer, 0, 0, UM_TIMER_DISABLE);

	/* disable SIGIO for the fds and set SIGIO to be ignored */
	err = deactivate_all_fds();
	if (err)
		os_warn("deactivate_all_fds failed, errno = %d\n", -err);

	/*
	 * Let any pending signals fire now. This ensures that they are not
	 * delivered after the exec, when they are definitely not expected.
	 */
	unblock_signals();
}

int __init main(int argc, char **argv, char **envp)
{
	char **new_argv;
	int ret;

	disable_aslr_and_reexec(argv, envp);
	set_stklim();
	apply_host_resource_env();
	setup_env_path();
	setsid();
	new_argv = copy_argv(argc, argv);
	prepare_fatal_handlers();

	scan_elf_aux(envp);

	change_sig(SIGPIPE, 0);
	ret = linux_main(argc, argv, envp);

	shutdown_host_io();

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

/* Prototypes for linker-wrapped allocation hooks. */
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
	else
		ret = vmalloc(size);

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
	 * freed. This is done by seeing what region of memory the pointer is in:
	 *   physical memory - kmalloc/kfree
	 *   kernel virtual memory - vmalloc/vfree
	 *   anywhere else - malloc/free
	 * If kmalloc is unavailable, then either high_physmem and/or
	 * end_vm are still 0 (as at startup), in which case we call free, or
	 * we have set them, but anyway addr has not been allocated from those
	 * areas. So, in both cases __real_free is called.
	 *
	 * CAN_KMALLOC is checked because it would be bad to free a buffer
	 * with kmalloc/vmalloc after they have been turned off during
	 * shutdown. If CAN_KMALLOC is temporarily disabled, this path may
	 * leak memory rather than freeing through a disabled allocator.
	 */

	/*
	 * Host-VA range check: was this pointer kmalloc'd from the physmem
	 * region? Both bounds are host VAs in the current layout.
	 */
	if (addr >= __binary_start_hva && addr < high_physmem) {
		if (kmalloc_ok)
			kfree(ptr);
	} else if (addr >= start_vm && addr < end_vm) {
		if (kmalloc_ok)
			vfree(ptr);
	} else {
		__real_free(ptr);
	}
}
