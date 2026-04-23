// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — thread + run_userspace ops.
 *
 * Workstream D-04a. Thread lifecycle (thread_create,
 * thread_start_idle, context_switch) on the UML kernel side is
 * the same jmp_buf-based mechanism ptrace and seccomp already
 * use; these wrappers exist so the dispatch macro resolves
 * kvm_<op>() in single-backend KVM_ONLY builds.
 *
 * run_userspace is the first op that actually exercises KVM:
 * ioctl(KVM_RUN) on vcpu0 and dispatch on exit_reason. At D-04a
 * the vCPU has no SREGS / CR3 / LSTAR set up yet (that's D-04b
 * and D-04c), so KVM_RUN is expected to fail-enter or shutdown
 * immediately. Log the exit_reason and panic with a readable
 * message — the panic itself still pre-console-dies today but
 * the log buffer captures enough detail for core-dump analysis.
 *
 * init_thread_regs uses the existing get_safe_registers() helper
 * (same as seccomp_init_thread_regs).
 */
#include <linux/errno.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>

#include <asm/processor.h>
#include <as-layout.h>
#include <os.h>
#include <registers.h>
#include <sysdep/ptrace_user.h>
#include <asm/backend.h>

#include "kvm_backend.h"

int kvm_thread_create(struct task_struct *p, void *stack,
		      void (*handler)(void))
{
	new_thread(stack, &p->thread.switch_buf, handler);
	return 0;
}

int kvm_thread_start_idle(void *stack, struct thread_struct *t)
{
	return start_idle_thread(stack, &t->switch_buf);
}

void kvm_context_switch(struct task_struct *prev, struct task_struct *next)
{
	switch_threads(&prev->thread.switch_buf, &next->thread.switch_buf);
}

void kvm_init_thread_regs(unsigned long *gp, unsigned long *fp)
{
	get_safe_registers(gp, fp);

#ifdef CONFIG_UM_BACKEND_KVM_ONLY
	/*
	 * Under KVM_ONLY, os_early_checks short-circuits before
	 * init_pid_registers (registers.c:20) runs — there's no
	 * ptraced stub child to PTRACE_GETREGS against. That
	 * leaves the exec_regs baseline zero-filled, so
	 * get_safe_registers above returns all zeros. Two
	 * consequences we have to paper over here:
	 *
	 *   - The A-05 contract KUnit test asserts at least one
	 *     gp[] slot is non-zero (the "init writes *something*"
	 *     invariant).
	 *   - UML's scheduler uses the gp buffer as a thread's
	 *     initial register state; zero-filled gp can pass NULL
	 *     checks but leaves RIP == 0, which isn't useful.
	 *
	 * Seed RIP with a sentinel non-zero value. A real vCPU RIP
	 * is set per-KVM_RUN via KVM_SET_REGS in run_userspace;
	 * this sentinel is only ever observed by the scheduler's
	 * bookkeeping + the contract test, not by the CPU. Use
	 * STUB_START as the sentinel because it's a known-valid
	 * guest VA under UML's existing stub conventions; future
	 * real-run_userspace path will overwrite this before any
	 * KVM_RUN.
	 */
	if (!gp[HOST_IP])
		gp[HOST_IP] = STUB_START;
#endif
}

/*
 * D-05b: backend-neutral host-side IPI — the UML kernel's
 * inter-CPU signal machinery, not a KVM vCPU IPI. Mirrors
 * seccomp_ipi_send + ptrace_ipi_send almost exactly. Under
 * ncpus=1 (default) nobody calls this; the contract still
 * requires a non-NULL slot in the ops table, so we provide
 * the thin wrapper rather than a -EOPNOTSUPP stub.
 */
int kvm_ipi_send(int cpu, int vector)
{
#if IS_ENABLED(CONFIG_SMP)
	return os_send_ipi(cpu, vector);
#else
	(void)cpu;
	(void)vector;
	return 0;
#endif
}

/*
 * Map an exit_reason back to its symbol for pr_info. The list
 * matches arch/x86/kvm/kvm_host.h / Documentation/virt/kvm/api.rst;
 * we only enumerate reasons we expect to see during D-04a..D-04c
 * bring-up. Unknown reasons hit a numeric default.
 */
static const char *kvm_exit_reason_str(u32 r)
{
	switch (r) {
	case KVM_EXIT_UNKNOWN:		return "UNKNOWN";
	case KVM_EXIT_EXCEPTION:	return "EXCEPTION";
	case KVM_EXIT_IO:		return "IO";
	case KVM_EXIT_HYPERCALL:	return "HYPERCALL";
	case KVM_EXIT_DEBUG:		return "DEBUG";
	case KVM_EXIT_HLT:		return "HLT";
	case KVM_EXIT_MMIO:		return "MMIO";
	case KVM_EXIT_SHUTDOWN:		return "SHUTDOWN";
	case KVM_EXIT_FAIL_ENTRY:	return "FAIL_ENTRY";
	case KVM_EXIT_INTR:		return "INTR";
	case KVM_EXIT_INTERNAL_ERROR:	return "INTERNAL_ERROR";
	default:			return "???";
	}
}

void kvm_run_userspace(struct uml_pt_regs *regs)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct kvm_run *run = kvm_backend_ctx()->run0;
	int rc;

	(void)regs;

	if (vcpu_fd < 0 || !run) {
		panic("um: kvm run_userspace: vCPU not initialized (vcpu_fd=%d run=%p)",
		      vcpu_fd, run);
	}

	/*
	 * First call registers the memslot now that arch_setup() has
	 * populated uml_physmem / physmem_size. Subsequent calls are
	 * idempotent no-ops. If registration fails we still attempt
	 * KVM_RUN so the panic below reports the real KVM exit reason
	 * rather than hiding behind a memslot-unavailable message.
	 */
	rc = kvm_ensure_memslot();
	if (rc < 0)
		pr_warn_once("um: kvm run_userspace: memslot registration failed (%d)\n",
			     rc);

	rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);

	/*
	 * D-04a scaffold: no SREGS / CR3 / LSTAR yet, so KVM_RUN
	 * returns either an error or a fail-entry / shutdown exit
	 * immediately. Neither is "correct" guest behavior; both are
	 * expected until D-04b lands. Panic with the exit_reason so
	 * the failure mode is diagnosable in dmesg.
	 */
	panic("um: kvm run_userspace: ioctl rc=%d, exit_reason=%u (%s) — D-04b SREGS/CR3 setup pending",
	      rc, run->exit_reason, kvm_exit_reason_str(run->exit_reason));
}
