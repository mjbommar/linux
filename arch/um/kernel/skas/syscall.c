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
#include <linux/ktime.h>

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
		unsigned long mp_addr = 0, mp_len = 0, mp_prot = 0;
		bool log_mm = (syscall == 9 /* mmap */ ||
			       syscall == 10 /* mprotect */ ||
			       syscall == 11 /* munmap */);
		u64 t0 = 0;

		if (log_mm) {
			mp_addr = UPT_SYSCALL_ARG1(&regs->regs);
			mp_len  = UPT_SYSCALL_ARG2(&regs->regs);
			mp_prot = UPT_SYSCALL_ARG3(&regs->regs);
			t0 = ktime_get_ns();
		}

		ret = (*sys_call_table[syscall])(UPT_SYSCALL_ARG1(&regs->regs),
						 UPT_SYSCALL_ARG2(&regs->regs),
						 UPT_SYSCALL_ARG3(&regs->regs),
						 UPT_SYSCALL_ARG4(&regs->regs),
						 UPT_SYSCALL_ARG5(&regs->regs),
						 UPT_SYSCALL_ARG6(&regs->regs));

		if (log_mm) {
			u64 t1 = ktime_get_ns();
			extern void um_diag_record(int nr, unsigned long a,
						   unsigned long l,
						   unsigned long p, long r,
						   u64 t);
			um_diag_record(syscall, mp_addr, mp_len, mp_prot,
				       (long)ret, t1);
			(void)t0;

			/*
			 * SMP-T12 diagnostic: bounded pr_emerg whenever a
			 * mmap with MAP_PRIVATE|MAP_ANONYMOUS, addr=NULL,
			 * len>0 returns either 0 or some non-userspace
			 * address. Used to ground-truth the MMAP_NULL
			 * symptom — kernel's view of mmap retval at the
			 * exact moment of return.
			 */
			if (syscall == 9 && mp_addr == 0 && mp_len > 0) {
				static atomic_t ump_hits = ATOMIC_INIT(0);
				bool errno_range =
					((unsigned long)ret >= -4096UL);
				bool zero = (ret == 0);

				if (zero) {
					/* CRITICAL: kernel returned 0 for a
					 * NULL+len mmap. This is NEVER valid.
					 * Always log, never rate-limit. */
					pr_emerg("UM_MMAP_ZERO pid=%d cpu=%d "
						 "addr=%#lx len=%#lx prot=%#lx ret=%#lx\n",
						 current->pid,
						 raw_smp_processor_id(),
						 mp_addr, mp_len, mp_prot,
						 (unsigned long)ret);
				} else if (errno_range &&
					   atomic_inc_return(&ump_hits) <= 30) {
					pr_emerg("UM_MMAP_DIAG pid=%d cpu=%d "
						 "addr=%#lx len=%#lx prot=%#lx "
						 "ret=%#lx\n",
						 current->pid,
						 raw_smp_processor_id(),
						 mp_addr, mp_len, mp_prot,
						 (unsigned long)ret);
				}
			}
		}

		PT_REGS_SET_SYSCALL_RETURN(regs, ret);

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
