// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: thread + ipi ops.
 *
 * Workstream A-03.S1.5. The jmp_buf machinery (switch_threads,
 * new_thread, start_idle_thread) is host-process-based and shared
 * verbatim with the ptrace backend; these wrappers exist so the
 * dispatch macro resolves to seccomp_<op>() in single-backend builds.
 */
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <asm/processor.h>
#include <os.h>

#include "seccomp_backend.h"

void seccomp_context_switch(struct task_struct *prev, struct task_struct *next)
{
	switch_threads(&prev->thread.switch_buf, &next->thread.switch_buf);
}

int seccomp_thread_create(struct task_struct *p, void *stack,
			  void (*handler)(void))
{
	new_thread(stack, &p->thread.switch_buf, handler);
	return 0;
}

int seccomp_thread_start_idle(void *stack, struct thread_struct *t)
{
	return start_idle_thread(stack, &t->switch_buf);
}

int seccomp_ipi_send(int cpu, int vector)
{
#if IS_ENABLED(CONFIG_SMP)
	return os_send_ipi(cpu, vector);
#else
	(void)cpu;
	(void)vector;
	return 0;
#endif
}
