// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Benjamin Berg <benjamin@sipsolutions.net>
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <asm/ldt.h>
#include <asm/unistd.h>
#include <backend.h>
#include <init.h>
#include <os.h>
#include <smp.h>
#include <kern_util.h>
#include <mem_user.h>
#include <ptrace_user.h>
#include <stdbool.h>
#include <stub-data.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <sysdep/mcontext.h>
#include <sysdep/stub.h>
#include <registers.h>
#include <skas.h>
#include "internal.h"

static void fatal_perror(const char *str)
{
	perror(str);
	exit(1);
}

static void fatal(char *fmt, ...)
{
	va_list list;

	va_start(list, fmt);
	vfprintf(stderr, fmt, list);
	va_end(list);

	exit(1);
}

/*
 * The former ptrace host probes were removed with the ptrace backend.
 */

extern unsigned long host_fp_size;
extern unsigned long exec_regs[MAX_REG_NR];
extern unsigned long *exec_fp_regs;

__initdata static struct stub_data *seccomp_test_stub_data;

static void __init sigsys_handler(int sig, siginfo_t *info, void *p)
{
	ucontext_t *uc = p;

	/* Stow away the location of the mcontext in the stack */
	seccomp_test_stub_data->mctx_offset = (unsigned long)&uc->uc_mcontext -
					      (unsigned long)&seccomp_test_stub_data->sigstack[0];

	/* Prevent libc from clearing memory (mctx_offset in particular) */
	syscall(__NR_exit, 0);
}

static int __init seccomp_helper(void *data)
{
	static struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clock_nanosleep, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
	};
	static struct sock_fprog prog = {
		.len = ARRAY_SIZE(filter),
		.filter = filter,
	};
	struct sigaction sa;

	/* close_range is needed for the stub */
	if (stub_syscall3(__NR_close_range, 1, ~0U, 0))
		exit(1);

	set_sigstack(seccomp_test_stub_data->sigstack,
			sizeof(seccomp_test_stub_data->sigstack));

	sa.sa_flags = SA_ONSTACK | SA_NODEFER | SA_SIGINFO;
	sa.sa_sigaction = (void *) sigsys_handler;
	sa.sa_restorer = NULL;
	if (sigaction(SIGSYS, &sa, NULL) < 0)
		exit(2);

	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
			SECCOMP_FILTER_FLAG_TSYNC, &prog) != 0)
		exit(3);

	sleep(0);

	/* Never reached. */
	_exit(4);
}

static bool __init init_seccomp(void)
{
	int pid;
	int status;
	int n;
	unsigned long sp;

	/*
	 * We check that we can install a seccomp filter and then exit(0)
	 * from a trapped syscall.
	 *
	 * Note that we cannot verify that no seccomp filter already exists
	 * for a syscall that results in the process/thread to be killed.
	 */

	os_info("Checking that seccomp filters can be installed...");

	seccomp_test_stub_data = mmap(0, sizeof(*seccomp_test_stub_data),
				      PROT_READ | PROT_WRITE,
				      MAP_SHARED | MAP_ANON, 0, 0);

	/* Use the syscall data area as stack, we just need something */
	sp = (unsigned long)&seccomp_test_stub_data->syscall_data +
	     sizeof(seccomp_test_stub_data->syscall_data) -
	     sizeof(void *);
	pid = clone(seccomp_helper, (void *)sp, CLONE_VFORK | CLONE_VM, NULL);

	if (pid < 0)
		fatal_perror("check_seccomp : clone failed");

	CATCH_EINTR(n = waitpid(pid, &status, __WCLONE));
	if (n < 0)
		fatal_perror("check_seccomp : waitpid failed");

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		struct uml_pt_regs *regs;
		unsigned long fp_size;
		int r;

		/* Fill in the host_fp_size from the mcontext. */
		regs = calloc(1, sizeof(struct uml_pt_regs));
		get_stub_state(regs, seccomp_test_stub_data, &fp_size);
		host_fp_size = fp_size;
		free(regs);

		/* Repeat with the correct size */
		regs = calloc(1, sizeof(struct uml_pt_regs) + host_fp_size);
		r = get_stub_state(regs, seccomp_test_stub_data, NULL);

		/* Store as the default startup registers */
		exec_fp_regs = malloc(host_fp_size);
		memcpy(exec_regs, regs->gp, sizeof(exec_regs));
		memcpy(exec_fp_regs, regs->fp, host_fp_size);

		munmap(seccomp_test_stub_data, sizeof(*seccomp_test_stub_data));

		free(regs);

		if (r) {
			os_info("failed to fetch registers: %d\n", r);
			return false;
		}

		os_info("OK\n");
		return true;
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
		os_info("missing\n");
	else
		os_info("error\n");

	munmap(seccomp_test_stub_data, sizeof(*seccomp_test_stub_data));
	return false;
}


static void __init check_coredump_limit(void)
{
	struct rlimit lim;
	int err = getrlimit(RLIMIT_CORE, &lim);

	if (err) {
		perror("Getting core dump limit");
		return;
	}

	os_info("Core dump limits :\n\tsoft - ");
	if (lim.rlim_cur == RLIM_INFINITY)
		os_info("NONE\n");
	else
		os_info("%llu\n", (unsigned long long)lim.rlim_cur);

	os_info("\thard - ");
	if (lim.rlim_max == RLIM_INFINITY)
		os_info("NONE\n");
	else
		os_info("%llu\n", (unsigned long long)lim.rlim_max);
}

void  __init get_host_cpu_features(
		void (*flags_helper_func)(char *line),
		void (*cache_helper_func)(char *line))
{
	FILE *cpuinfo;
	char *line = NULL;
	size_t len = 0;
	int done_parsing = 0;

	cpuinfo = fopen("/proc/cpuinfo", "r");
	if (cpuinfo == NULL) {
		os_info("Failed to get host CPU features\n");
	} else {
		while ((getline(&line, &len, cpuinfo)) != -1) {
			if (strstr(line, "flags")) {
				flags_helper_func(line);
				done_parsing++;
			}
			if (strstr(line, "cache_alignment")) {
				cache_helper_func(line);
				done_parsing++;
			}
			free(line);
			line = NULL;
			if (done_parsing > 1)
				break;
		}
		fclose(cpuinfo);
	}
}

/*
 * backend= boot param. Parsed early in linux_main
 * via __uml_setup; consumed by init_backend() in arch/um/kernel/backend.c.
 *
 *   backend=auto                  - Kconfig default + using_seccomp probe
 *   backend=ptrace                - prefer ptrace; use seccomp if unavailable
 *   backend=seccomp               - prefer seccomp; use seccomp if unavailable
 *   backend=kvm                   - prefer KVM; use seccomp if unavailable
 *   backend=force=ptrace          - require ptrace; panic if N/A
 *   backend=force=seccomp         - require seccomp; panic if N/A
 *   backend=force=kvm             - require KVM; panic if N/A
 *
 * The seccomp=on/auto/off boot param is accepted as a compatibility
 * alias; backend= takes precedence when both are set.
 */
int backend_arg_requested __initdata;	/* enum um_backend_kind */
int backend_arg_force __initdata;

static int __init uml_backend_config(char *line, int *add)
{
	*add = 0;

	if (strcmp(line, "auto") == 0) {
		backend_arg_requested = UM_BACKEND_KIND_NONE;
		backend_arg_force = 0;
	} else if (strcmp(line, "ptrace") == 0) {
		backend_arg_requested = UM_BACKEND_KIND_PTRACE;
		backend_arg_force = 0;
	} else if (strcmp(line, "seccomp") == 0) {
		backend_arg_requested = UM_BACKEND_KIND_SECCOMP;
		backend_arg_force = 0;
	} else if (strcmp(line, "kvm") == 0 || strcmp(line, "kvm-v2") == 0) {
		/*
		 * "kvm-v2" is a synonym for "kvm"; there is only one KVM
		 * backend selector in this tree.
		 */
		backend_arg_requested = UM_BACKEND_KIND_KVM;
		backend_arg_force = 0;
	} else if (strcmp(line, "force=ptrace") == 0) {
		backend_arg_requested = UM_BACKEND_KIND_PTRACE;
		backend_arg_force = 1;
	} else if (strcmp(line, "force=seccomp") == 0) {
		backend_arg_requested = UM_BACKEND_KIND_SECCOMP;
		backend_arg_force = 1;
	} else if (strcmp(line, "force=kvm") == 0 ||
		   strcmp(line, "force=kvm-v2") == 0) {
		backend_arg_requested = UM_BACKEND_KIND_KVM;
		backend_arg_force = 1;
	} else {
		static const char valid[] =
			"auto ptrace seccomp kvm kvm-v2 force=<ptrace|seccomp|kvm|kvm-v2>";

		fatal("Invalid backend option '%s'; valid: %s\n", line, valid);
	}
	return 0;
}

__uml_setup("backend=", uml_backend_config,
	    "backend=<auto|seccomp|kvm|force=seccomp|force=kvm>\n"
	    "    Pick the trap mechanism. auto (default) uses Kconfig plus\n"
	    "    runtime probe. Bare names are preferences that resolve\n"
	    "    to seccomp if the requested backend is not built. force= makes\n"
	    "    the choice mandatory and panics if the requested backend is not\n"
	    "    compiled in or fails its probe.\n"
	    "\n"
	    "    ptrace is parsed for compatibility, but the ptrace backend is\n"
	    "    not built by this tree. kvm selects the KVM v2 backend when\n"
	    "    CONFIG_UM_BACKEND_KVM_V2 is enabled.\n"
	    "\n"
	    "    Replaces seccomp=on/auto/off, which is still accepted as a\n"
	    "    compatibility alias.\n\n");

static int seccomp_config __initdata;

static int __init uml_seccomp_config(char *line, int *add)
{
	*add = 0;

	if (strcmp(line, "off") == 0)
		seccomp_config = 0;
	else if (strcmp(line, "auto") == 0)
		seccomp_config = 1;
	else if (strcmp(line, "on") == 0)
		seccomp_config = 2;
	else
		fatal("Invalid seccomp option '%s', expected on/auto/off\n",
		      line);

	return 0;
}

__uml_setup("seccomp=", uml_seccomp_config,
"seccomp=<on/auto/off>\n"
"    Configure whether or not SECCOMP is used. With SECCOMP, userspace\n"
"    processes work collaboratively with the kernel instead of being\n"
"    traced using ptrace. All syscalls from the application are caught and\n"
"    redirected using a signal. This signal handler in turn is permitted to\n"
"    do the selected set of syscalls to communicate with the UML kernel and\n"
"    do the required memory management.\n"
"\n"
"    This method is overall faster than the ptrace based userspace, primarily\n"
"    because it reduces the number of context switches for (minor) page faults.\n"
"\n"
"    However, the SECCOMP filter is not (yet) restrictive enough to prevent\n"
"    userspace from reading and writing all physical memory. Userspace\n"
"    processes could also trick the stub into disabling SIGALRM which\n"
"    prevents it from being interrupted for scheduling purposes.\n"
"\n"
"    This is insecure and should only be used with a trusted userspace\n\n"
);

void __init os_early_checks(void)
{
	/* Print out the core dump limits early */
	check_coredump_limit();

	/* Need to check this early because mmapping happens before the
	 * kernel is running.
	 */
	check_tmpexec();

	/* If seccomp is not compiled in, there is nothing to probe. */
	if (!IS_ENABLED(CONFIG_UM_BACKEND_SECCOMP))
		return;

	/*
	 * Run the seccomp probe whenever CONFIG_UM_BACKEND_SECCOMP is
	 * compiled in. backend=force=ptrace skips the probe so
	 * init_backend() can report the removed backend cleanly.
	 * init_backend() (called from linux_main() right after this
	 * function) consumes the using_seccomp result + boot params
	 * and selects the backend authoritatively.
	 */
	if (IS_ENABLED(CONFIG_UM_BACKEND_SECCOMP) &&
	    backend_arg_requested != UM_BACKEND_KIND_PTRACE) {
		if (init_seccomp()) {
			using_seccomp = 1;
			return;
		}

		if (seccomp_config == 2)
			fatal("SECCOMP userspace requested but not functional!\n");
		}

	/*
	 * No ptrace fallback is available; seccomp is the only stub-child
	 * backend in tree.
	 */
	fatal("seccomp probe failed and no fallback backend is available\n");
}
