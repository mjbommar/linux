// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <linux/seccomp.h>
#include <kern_util.h>
#include <sysdep/ptrace.h>
#include <sysdep/ptrace_user.h>
#include <linux/time-internal.h>
#include <asm/syscall.h>
#include <asm/um-hooks.h>
#include <asm/unistd.h>
#include <asm/delay.h>
#include <linux/timekeeping.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <os.h>
#include <skas/skas.h>
#include <stub-data.h>
#include <backend.h>

void handle_syscall(struct uml_pt_regs *r)
{
	struct pt_regs *regs = container_of(r, struct pt_regs, regs);
	int syscall;

	/* Initialize the syscall number and default return value. */
	UPT_SYSCALL_NR(r) = PT_SYSCALL_NR(r->gp);
	PT_REGS_SET_SYSCALL_RETURN(regs, -ENOSYS);

	um_on_syscall_entry(regs);

	if (syscall_trace_enter(regs))
		goto out;

	/* Do the seccomp check after ptrace; failures should be fast. */
	if (secure_computing() == -1)
		goto out;

	syscall = UPT_SYSCALL_NR(r);

	/*
	 * Tell the stub whether this mm is single-threaded, so it can skip
	 * the per-trap FS/GS resync. A clone() that shares the mm bumps
	 * mm_users and crosses here, re-stamping 0 before the new thread runs.
	 */
	if (um_backend->stub_syscall_uses_futex && current->mm) {
		struct mm_id *mm = current_mm_id();

		if (mm && mm->stack)
			((struct stub_data *)mm->stack)->mm_single_threaded =
				(atomic_read(&current->mm->mm_users) == 1);
	}

	/*
	 * If no time passes, then sched_yield may not actually yield, causing
	 * broken spinlock implementations in userspace (ASAN) to hang for long
	 * periods of time.
	 */
	if ((time_travel_mode == TT_MODE_INFCPU ||
	     time_travel_mode == TT_MODE_EXTERNAL) &&
	    syscall == __NR_sched_yield)
		tt_extra_sched_jiffies += 1;

	if (syscall >= 0 && syscall < __NR_syscalls) {
		unsigned long ret;

		ret = (*sys_call_table[syscall])(UPT_SYSCALL_ARG1(&regs->regs),
						 UPT_SYSCALL_ARG2(&regs->regs),
						 UPT_SYSCALL_ARG3(&regs->regs),
						 UPT_SYSCALL_ARG4(&regs->regs),
						 UPT_SYSCALL_ARG5(&regs->regs),
						 UPT_SYSCALL_ARG6(&regs->regs));

		PT_REGS_SET_SYSCALL_RETURN(regs, ret);

		/*
		 * Stamp the clock-gadget offset so the stub can answer
		 * clock_gettime(CLOCK_MONOTONIC) without a handoff. The guest
		 * clocksource is host CLOCK_MONOTONIC, so the offset is constant.
		 * Only when NOT in time-travel mode (the gadget must not bypass
		 * virtual time), and only for the futex (seccomp) backend.
		 */
		if (syscall == __NR_clock_gettime &&
		    time_travel_mode == TT_MODE_OFF &&
		    um_backend->stub_syscall_uses_futex) {
			struct mm_id *mm = current_mm_id();

			if (mm && mm->stack) {
				struct stub_data *sd = (struct stub_data *)mm->stack;

				sd->clock_mono_offset =
					(long long)os_nsecs() - (long long)ktime_get_ns();
				sd->clock_real_offset =
					(long long)os_persistent_clock_emulation() -
					(long long)ktime_get_real_ns();
			}
		}

		/*
		 * An error value here can be some form of -ERESTARTSYS
		 * and then we'd just loop. Make any error syscalls take
		 * some time, so that it won't just loop if something is
		 * not ready, and hopefully other things will make some
		 * progress.
		 */
		if (IS_ERR_VALUE(ret) &&
		    (time_travel_mode == TT_MODE_INFCPU ||
		     time_travel_mode == TT_MODE_EXTERNAL)) {
			um_udelay(1);
			schedule();
		}
	}

out:
	um_on_syscall_exit(regs);
	syscall_trace_leave(regs);
}
