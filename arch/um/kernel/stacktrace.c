// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2001 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 * Copyright (C) 2013 Richard Weinberger <richard@nod.at>
 * Copyright (C) 2014 Google Inc., Author: Daniel Walter <dwalter@google.com>
 */

#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/stacktrace.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <asm/stacktrace.h>

void dump_trace(struct task_struct *tsk,
		const struct stacktrace_ops *ops,
		void *data)
{
	int reliable = 0;
	unsigned long *sp, bp, addr;
	unsigned long *stack_start, *stack_end;
	struct pt_regs *segv_regs = tsk->thread.segv_regs;
	struct stack_frame *frame;

	bp = get_frame_pointer(tsk, segv_regs);
	sp = get_stack_pointer(tsk, segv_regs);

	/*
	 * Bound the walk by the task's actual thread stack. Without
	 * this, an early-boot panic (where current is init_task and
	 * the saved SP is still in the main() stack, not a thread
	 * stack) walks past the end of mapped memory and segfaults on
	 * the first READ_ONCE_NOCHECK, masking the real panic
	 * message. A SIGSEGV inside panic() is the worst-case UML
	 * failure mode because the printk buffer never flushes, leaving
	 * only a silent crash loop.
	 */
	stack_start = (unsigned long *)task_stack_page(tsk);
	stack_end = stack_start + THREAD_SIZE / sizeof(unsigned long);
	if (sp < stack_start || sp >= stack_end)
		return;

	frame = (struct stack_frame *)bp;
	while (((long) sp & (THREAD_SIZE-1)) != 0) {
		addr = READ_ONCE_NOCHECK(*sp);
		if (__kernel_text_address(addr)) {
			reliable = 0;
			if ((unsigned long) sp == bp + sizeof(long)) {
				frame = frame ? frame->next_frame : NULL;
				bp = (unsigned long)frame;
				reliable = 1;
			}
			ops->address(data, addr, reliable);
		}
		sp++;
	}
}

static void save_addr(void *data, unsigned long address, int reliable)
{
	struct stack_trace *trace = data;

	if (!reliable)
		return;
	if (trace->skip) {
		trace->skip--;
		return;
	}
	if (trace->nr_entries >= trace->max_entries)
		return;

	trace->entries[trace->nr_entries++] = address;
}

static const struct stacktrace_ops dump_ops = {
	.address = save_addr
};

static void __save_stack_trace(struct task_struct *tsk, struct stack_trace *trace)
{
	dump_trace(tsk, &dump_ops, trace);
}

void save_stack_trace(struct stack_trace *trace)
{
	__save_stack_trace(current, trace);
}
EXPORT_SYMBOL_GPL(save_stack_trace);

void save_stack_trace_tsk(struct task_struct *tsk, struct stack_trace *trace)
{
	__save_stack_trace(tsk, trace);
}
EXPORT_SYMBOL_GPL(save_stack_trace_tsk);
