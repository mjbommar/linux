// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: vcpu_run (one trap-loop iteration).
 *
 * Workstream A-03.S1.3 (renamed from seccomp_run_userspace by memo 25
 * R2 ops cleanup). Lifted from arch/um/os-Linux/skas/process.c::
 * seccomp_userspace_iter(). Uses Berg's SIGSYS+futex+stub-state
 * mechanism (the ptrace backend that mirrored this shape was removed
 * in memo 25 R11; archived at the kvm-v1-archive-20260428 tag).
 *
 * Per Documentation/virt/uml/backend-contract.rst, run_userspace is
 * one round-trip: resume the guest, wait for the next trap, fetch
 * register state and (for fault-bearing exits) faultinfo, then
 * dispatch the kernel-side handler before returning. The function
 * owns its own per-mm turnstile and per-iteration scaffolding so the
 * caller's loop body is empty.
 *
 * USER TU (`_user.c` suffix → USER_CFLAGS via Makefile.rules) so it
 * can call libc syscall() and libc memcpy directly, mirroring the
 * existing arch/um/os-Linux/skas/ pattern.
 */

#include <signal.h>
#include <string.h>
#include <as-layout.h>
#include <kern_util.h>
#include <os.h>
#include <skas.h>
#include <sysdep/mcontext.h>
#include <sysdep/ptrace.h>
#include <sysdep/stub.h>
#include <timetravel.h>
#include <backend.h>
#include <worker_api.h>
#include "../../os-Linux/internal.h"

extern unsigned long tt_extra_sched_jiffies;
/*
 * Defined in arch/um/os-Linux/skas/process.c — shared with ptrace impl
 * and reset by switch_threads().
 */
extern unsigned int unscheduled_userspace_iterations;

void seccomp_vcpu_run(struct uml_pt_regs *regs)
{
	struct mm_id *mm_id = current_mm_id();
	struct stub_data *proc_data = (void *)mm_id->stack;
	siginfo_t si_local;
	siginfo_t *si;
	int err, sig;
	int worker_rc;

	enter_turnstile(mm_id);

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

	err = set_stub_state(regs, proc_data, singlestepping());
	if (err) {
		printk(UM_KERN_ERR "%s - failed to set regs: %d",
		       __func__, err);
		fatal_sigsegv();
	}

	/* Must have been reset by the syscall caller */
	if (proc_data->restart_wait != 0)
		panic("Programming error: Flag to only run syscalls in child was not cleared!");

	/* Mark pending syscalls for flushing */
	proc_data->syscall_data_len = mm_id->syscall_data_len;

	/*
	 * memo 28 E.3d.2: under WORKER_PROCESS=y the per-mm worker owns
	 * the stub child (cloned CLONE_VM into the worker, not the
	 * spawner). worker_drive_vcpu_run ships VCPU_RUN over the per-mm
	 * IPC socket; the worker runs the futex round-trip against the
	 * shared (memfd, MAP_SHARED) stub_data page and replies VCPU_DONE
	 * once the stub re-traps. Inside that window the spawner-side
	 * guest task answers any SYSCALL_REQ the stub trapped on,
	 * running handle_syscall under its real `current` (matches Part
	 * C.E "wait-queue bounce" without the kthread hop). The spawner
	 * still owns set_stub_state / get_stub_state and any
	 * sendmsg-with-SCM_RIGHTS prelude — see worker_drive_vcpu_run.
	 *
	 * Returns -ENODEV when current->mm has no worker; falls back to
	 * the legacy in-spawner wait_stub_done_seccomp.
	 */
	worker_rc = worker_drive_vcpu_run(regs, singlestepping(),
					  mm_id->syscall_data_len);
	if (worker_rc == -ENODEV) {
		wait_stub_done_seccomp(mm_id, 0, 0);
	} else if (worker_rc < 0) {
		printk(UM_KERN_ERR "%s - worker drive failed: %d",
		       __func__, worker_rc);
		fatal_sigsegv();
	}

	err = get_stub_state(regs, proc_data, NULL);
	if (err) {
		printk(UM_KERN_ERR "%s - failed to get regs: %d",
		       __func__, err);
		fatal_sigsegv();
	}

	sig = proc_data->signal;

	if (sig == SIGTRAP && proc_data->err != 0) {
		printk(UM_KERN_ERR "%s - Error flushing stub syscalls",
		       __func__);
		syscall_stub_dump_error(mm_id);
		mm_id->syscall_data_len = proc_data->err;
		fatal_sigsegv();
	}

	mm_id->syscall_data_len = 0;
	mm_id->syscall_fd_num = 0;

	if (proc_data->si_offset > sizeof(proc_data->sigstack) - sizeof(*si))
		panic("%s - Invalid siginfo offset from child", __func__);

	si = &si_local;
	memcpy(si, &proc_data->sigstack[proc_data->si_offset], sizeof(*si));

	regs->is_user = 1;

	/* Fill in ORIG_RAX and extract fault information */
	PT_SYSCALL_NR(regs->gp) = si->si_syscall;
	if (sig == SIGSEGV) {
		mcontext_t *mcontext =
			(void *)&proc_data->sigstack[proc_data->mctx_offset];

		GET_FAULTINFO_FROM_MC(regs->faultinfo, mcontext);
	}

	exit_turnstile(mm_id);

	UPT_SYSCALL_NR(regs) = -1; /* Assume: It's not a syscall */

	if (sig) {
		switch (sig) {
		case SIGSEGV:
			(*sig_info[SIGSEGV])(SIGSEGV,
					     (struct siginfo *)si,
					     regs, NULL);
			break;
		case SIGSYS:
			handle_syscall(regs);
			break;
		case SIGTRAP + 0x80:
			/* SYSEMU stop — only ptrace produces this. */
			fatal_sigsegv();
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
