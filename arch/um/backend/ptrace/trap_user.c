// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace backend: run_userspace (one trap-loop iteration).
 *
 * Workstream A-02.HOT-1. Extracted from arch/um/os-Linux/skas/process.c::
 * userspace() (the !using_seccomp branch). The seccomp branch is lifted
 * symmetrically by workstream A-03.
 *
 * Per Documentation/virt/uml/backend-contract.rst, run_userspace is one
 * round-trip: resume the guest, wait for the next trap, fetch register
 * state and (for fault-bearing exits) faultinfo, then dispatch the
 * kernel-side handler before returning. The caller (a tight while-loop
 * in userspace()) does only `interrupt_end()` once before entering the
 * loop; everything else — per-mm turnstile, time-travel iteration
 * accounting, current_mm_sync(), the trap mechanism, signal dispatch,
 * and the SYSCALL_NR cleanup — lives here so the backend is
 * self-contained.
 *
 * This TU is a USER object (`_user.c` suffix → USER_CFLAGS via
 * arch/um/scripts/Makefile.rules) so it can call libc ptrace/waitpid
 * directly, mirroring the existing arch/um/os-Linux/skas/ pattern.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <as-layout.h>
#include <kern_util.h>
#include <os.h>
#include <ptrace_user.h>
#include <registers.h>
#include <skas.h>
#include <sysdep/ptrace.h>
#include <sysdep/stub.h>
#include <timetravel.h>
#include <backend.h>
#include "../../os-Linux/internal.h"

extern unsigned long tt_extra_sched_jiffies;

/*
 * Fetch the SIGSEGV faultinfo from the stub stack page after a SIGSEGV
 * has been delivered to the stub child. Used by the ptrace SIGSEGV
 * path. Mirrors the body of get_skas_faultinfo() previously in
 * arch/um/os-Linux/skas/process.c.
 */
static void get_skas_faultinfo(int pid, struct faultinfo *fi)
{
	int err;

	err = ptrace(PTRACE_CONT, pid, 0, SIGSEGV);
	if (err) {
		printk(UM_KERN_ERR "Failed to continue stub, pid = %d, errno = %d\n",
		       pid, errno);
		fatal_sigsegv();
	}
	wait_stub_done(pid);

	/*
	 * faultinfo is prepared by the stub_segv_handler at start of the
	 * stub stack page. We just have to copy it.
	 */
	memcpy(fi, (void *)current_stub_stack(), sizeof(*fi));
}

/*
 * SIGTRAP+0x80 handler — the guest issued a syscall via SYSEMU. The
 * guest IP at this point is back in the guest text; if it lands inside
 * the stub the guest is trying to execute stub code, which is a bug.
 */
static void handle_trap(struct uml_pt_regs *regs)
{
	if ((UPT_IP(regs) >= STUB_START) && (UPT_IP(regs) < STUB_END))
		fatal_sigsegv();

	handle_syscall(regs);
}

/*
 * Defined in arch/um/os-Linux/skas/process.c — shared with seccomp impl
 * and reset by switch_threads().
 */
extern unsigned int unscheduled_userspace_iterations;

/*
 * One ptrace round-trip. See file banner for the contract.
 */
void ptrace_run_userspace(struct uml_pt_regs *regs)
{
	struct mm_id *mm_id = current_mm_id();
	int pid = mm_id->pid;
	siginfo_t si_local;
	siginfo_t *si = NULL;
	int err, status, op, sig;

	/*
	 * Per-iteration shared scaffolding (turnstile, time-travel,
	 * mm_sync). Lives here so the backend owns its lock boundary and
	 * can release it before dispatching kernel handlers that may
	 * sleep.
	 */
	enter_turnstile(mm_id);

	/*
	 * In time-travel mode, userspace can theoretically do a *lot* of
	 * work without being scheduled. Account a jiffie against the
	 * scheduling clock after the configured threshold of unscheduled
	 * iterations to keep RCU and other kernel bookkeeping running.
	 */
	if (time_travel_mode == TT_MODE_INFCPU ||
	    time_travel_mode == TT_MODE_EXTERNAL) {
#ifdef CONFIG_UML_MAX_USERSPACE_ITERATIONS
		if (CONFIG_UML_MAX_USERSPACE_ITERATIONS &&
		    unscheduled_userspace_iterations++ >
		    CONFIG_UML_MAX_USERSPACE_ITERATIONS) {
			tt_extra_sched_jiffies += 1;
			unscheduled_userspace_iterations = 0;
		}
#endif
	}

	time_travel_print_bc_msg();

	current_mm_sync();

	/* Flush out any pending stub-side syscalls (mmap/munmap queue). */
	err = syscall_stub_flush(mm_id);
	if (err) {
		if (err == -ENOMEM)
			report_enomem();
		printk(UM_KERN_ERR "%s - Error flushing stub syscalls: %d",
		       __func__, -err);
		fatal_sigsegv();
	}

	/*
	 * This can legitimately fail if the process loads a bogus value
	 * into a segment register. It will segfault and PTRACE_GETREGS
	 * will read that value out of the process. However,
	 * PTRACE_SETREGS will fail. In this case there is nothing to do
	 * but kill the process.
	 */
	if (ptrace(PTRACE_SETREGS, pid, 0, regs->gp)) {
		printk(UM_KERN_ERR "%s - ptrace set regs failed, errno = %d\n",
		       __func__, errno);
		fatal_sigsegv();
	}

	if (put_fp_registers(pid, regs->fp)) {
		printk(UM_KERN_ERR "%s - ptrace set fp regs failed, errno = %d\n",
		       __func__, errno);
		fatal_sigsegv();
	}

	op = singlestepping() ? PTRACE_SYSEMU_SINGLESTEP : PTRACE_SYSEMU;

	if (ptrace(op, pid, 0, 0)) {
		printk(UM_KERN_ERR "%s - ptrace continue failed, op = %d, errno = %d\n",
		       __func__, op, errno);
		fatal_sigsegv();
	}

	CATCH_EINTR(err = waitpid(pid, &status, WUNTRACED | __WALL));
	if (err < 0) {
		printk(UM_KERN_ERR "%s - wait failed, errno = %d\n",
		       __func__, errno);
		fatal_sigsegv();
	}

	regs->is_user = 1;

	if (ptrace(PTRACE_GETREGS, pid, 0, regs->gp)) {
		printk(UM_KERN_ERR "%s - PTRACE_GETREGS failed, errno = %d\n",
		       __func__, errno);
		fatal_sigsegv();
	}

	if (get_fp_registers(pid, regs->fp)) {
		printk(UM_KERN_ERR "%s - get_fp_registers failed, errno = %d\n",
		       __func__, errno);
		fatal_sigsegv();
	}

	if (WIFSTOPPED(status)) {
		sig = WSTOPSIG(status);

		/*
		 * These signal handlers need the si argument and SIGSEGV
		 * needs the faultinfo. The SIGIO and SIGALARM handlers
		 * which constitute the majority of invocations don't use it.
		 */
		switch (sig) {
		case SIGSEGV:
			get_skas_faultinfo(pid, &regs->faultinfo);
			fallthrough;
		case SIGTRAP:
		case SIGILL:
		case SIGBUS:
		case SIGFPE:
		case SIGWINCH:
			ptrace(PTRACE_GETSIGINFO, pid, 0,
			       (struct siginfo *)&si_local);
			si = &si_local;
			break;
		default:
			si = NULL;
			break;
		}
	} else {
		sig = 0;
	}

	exit_turnstile(mm_id);

	UPT_SYSCALL_NR(regs) = -1; /* Assume: It's not a syscall */

	if (sig) {
		switch (sig) {
		case SIGSEGV:
			if (PTRACE_FULL_FAULTINFO)
				(*sig_info[SIGSEGV])(SIGSEGV,
						     (struct siginfo *)si,
						     regs, NULL);
			else
				segv(regs->faultinfo, 0, 1, NULL, NULL);
			break;
		case SIGSYS:
			handle_syscall(regs);
			break;
		case SIGTRAP + 0x80:
			handle_trap(regs);
			break;
		case SIGTRAP:
			relay_signal(SIGTRAP, (struct siginfo *)si, regs, NULL);
			break;
		case SIGALRM:
			break;
		case SIGIO:
		case SIGILL:
		case SIGBUS:
		case SIGFPE:
		case SIGWINCH:
			block_signals_trace();
			(*sig_info[sig])(sig, (struct siginfo *)si, regs, NULL);
			unblock_signals_trace();
			break;
		default:
			printk(UM_KERN_ERR "%s - child stopped with signal %d\n",
			       __func__, sig);
			fatal_sigsegv();
		}
		interrupt_end();

		/* Avoid -ERESTARTSYS handling in host */
		if (PT_SYSCALL_NR_OFFSET != PT_SYSCALL_RET_OFFSET)
			PT_SYSCALL_NR(regs->gp) = -1;
	}
}
