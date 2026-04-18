// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace backend: context_switch / thread_create / thread_start_idle.
 *
 * Workstream A-02.HOT-3. Thin kernel-side wrappers around the
 * USER-side jmp_buf primitives in arch/um/os-Linux/skas/process.c
 * (switch_threads, new_thread, start_idle_thread). The wrappers
 * extract the per-thread `switch_buf` jmp_buf out of `task_struct`
 * (or `thread_struct`) so that the public ops table contract can
 * be expressed at the task level, hiding the jmp_buf detail.
 *
 * Both the ptrace and seccomp backends use the same shared impls
 * (the jmp_buf machinery is host-process-based and agnostic to the
 * trap mechanism). The seccomp backend (workstream A-03) will add
 * its own thin wrappers around the same shared functions; the
 * shared impls themselves should eventually move to
 * arch/um/backend/common/ for clarity (deferred to A-02.COLD-3).
 *
 * KVM (workstream D) provides its own implementations of these ops
 * — the jmp_buf machinery does not apply to KVM vCPUs.
 */
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <asm/processor.h>
#include <os.h>

#include "ptrace_backend.h"

void ptrace_context_switch(struct task_struct *prev, struct task_struct *next)
{
	switch_threads(&prev->thread.switch_buf, &next->thread.switch_buf);
}

int ptrace_thread_create(struct task_struct *p, void *stack,
			 void (*handler)(void))
{
	new_thread(stack, &p->thread.switch_buf, handler);
	return 0;
}

int ptrace_thread_start_idle(void *stack, struct thread_struct *t)
{
	return start_idle_thread(stack, &t->switch_buf);
}

/*
 * A-02.COLD-2: ipi_send. Backend-shared (the seccomp backend will
 * point at the same impl); the per-backend wrapper exists so the
 * dispatch macro resolves to a named symbol.
 */
int ptrace_ipi_send(int cpu, int vector)
{
#if IS_ENABLED(CONFIG_SMP)
	return os_send_ipi(cpu, vector);
#else
	(void)cpu;
	(void)vector;
	return 0;
#endif
}
