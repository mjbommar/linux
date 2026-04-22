// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/ftrace.h>
#include <asm/siginfo.h>
#include <asm/signal.h>
#include <asm/unistd.h>
#include <frame_kern.h>
#include <kern_util.h>
#include <os.h>

EXPORT_SYMBOL(block_signals);
EXPORT_SYMBOL(unblock_signals);

/*
 * These four helpers gate arch-level IRQ state on UML: they wrap
 * block_signals()/unblock_signals() (which implement
 * arch_local_irq_disable/enable) with the lockdep hardirqs-on/off
 * tracepoints. They run from arch_local_irq_restore paths (via
 * unblock_signals_trace() in the trap return path and friends) and
 * from host-signal handlers; both contexts can be atomic
 * (preempt_count > 0, or hardirqs already off inside a
 * local_irq_save/restore window).
 *
 * The generic function_graph tracer's first activation takes
 * sched_register_mutex / tracepoints_mutex; if it pushes a shadow-
 * stack entry from inside one of these atomic-context callers the
 * prepare_ftrace_return → function_graph_enter → __mutex_lock chain
 * sleeps with preempt_count > 0 and the kernel panics (observed in
 * decisions-log D34 addendum-3). On native x86 the analogous path
 * cannot even exist because arch_local_irq_* is inline asm and has
 * no function-entry NOP to patch; on UML these are real C functions
 * so we need the notrace annotation to match.
 */
notrace void block_signals_trace(void)
{
	block_signals();
	if (current_thread_info())
		trace_hardirqs_off();
}

notrace void unblock_signals_trace(void)
{
	if (current_thread_info())
		trace_hardirqs_on();
	unblock_signals();
}

notrace void um_trace_signals_on(void)
{
	if (current_thread_info())
		trace_hardirqs_on();
}

notrace void um_trace_signals_off(void)
{
	if (current_thread_info())
		trace_hardirqs_off();
}

/*
 * OK, we're invoking a handler
 */
static void handle_signal(struct ksignal *ksig, struct pt_regs *regs)
{
	sigset_t *oldset = sigmask_to_save();
	int singlestep = 0;
	unsigned long sp;
	int err;

	if (test_thread_flag(TIF_SINGLESTEP) && (current->ptrace & PT_PTRACED))
		singlestep = 1;

	/* Did we come from a system call? */
	if (PT_REGS_SYSCALL_NR(regs) >= 0) {
		/* If so, check system call restarting.. */
		switch (PT_REGS_SYSCALL_RET(regs)) {
		case -ERESTART_RESTARTBLOCK:
		case -ERESTARTNOHAND:
			PT_REGS_SYSCALL_RET(regs) = -EINTR;
			break;

		case -ERESTARTSYS:
			if (!(ksig->ka.sa.sa_flags & SA_RESTART)) {
				PT_REGS_SYSCALL_RET(regs) = -EINTR;
				break;
			}
			fallthrough;
		case -ERESTARTNOINTR:
			PT_REGS_RESTART_SYSCALL(regs);
			PT_REGS_ORIG_SYSCALL(regs) = PT_REGS_SYSCALL_NR(regs);
			break;
		}
	}

	sp = PT_REGS_SP(regs);
	if ((ksig->ka.sa.sa_flags & SA_ONSTACK) && (sas_ss_flags(sp) == 0))
		sp = current->sas_ss_sp + current->sas_ss_size;

#ifdef CONFIG_ARCH_HAS_SC_SIGNALS
	if (!(ksig->ka.sa.sa_flags & SA_SIGINFO))
		err = setup_signal_stack_sc(sp, ksig, regs, oldset);
	else
#endif
		err = setup_signal_stack_si(sp, ksig, regs, oldset);

	signal_setup_done(err, ksig, singlestep);
}

void do_signal(struct pt_regs *regs)
{
	struct ksignal ksig;
	int handled_sig = 0;

	while (get_signal(&ksig)) {
		handled_sig = 1;
		/* Whee!  Actually deliver the signal.  */
		handle_signal(&ksig, regs);
	}

	/* Did we come from a system call? */
	if (!handled_sig && (PT_REGS_SYSCALL_NR(regs) >= 0)) {
		/* Restart the system call - no handlers present */
		switch (PT_REGS_SYSCALL_RET(regs)) {
		case -ERESTARTNOHAND:
		case -ERESTARTSYS:
		case -ERESTARTNOINTR:
			PT_REGS_ORIG_SYSCALL(regs) = PT_REGS_SYSCALL_NR(regs);
			PT_REGS_RESTART_SYSCALL(regs);
			break;
		case -ERESTART_RESTARTBLOCK:
			PT_REGS_ORIG_SYSCALL(regs) = __NR_restart_syscall;
			PT_REGS_RESTART_SYSCALL(regs);
			break;
		}
	}

	/*
	 * if there's no signal to deliver, we just put the saved sigmask
	 * back
	 */
	if (!handled_sig)
		restore_saved_sigmask();
}
