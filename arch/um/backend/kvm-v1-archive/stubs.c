// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — read/write_guest_regs (BUG.5 implementation).
 *
 * Per-task vCPU (Stage A landed 2026-04-27) makes these natural:
 * each task's struct kvm_vcpu_handle has its own vcpu_fd. Reading
 * the task's guest GPRs is just KVM_GET_REGS on that fd; writing
 * is KVM_SET_REGS. Pre-Stage-A this couldn't work because the
 * singleton vcpu0_fd's regs reflected whichever task last ran.
 *
 * Returns -ENODEV if the task hasn't run yet (no vcpu allocated).
 * Generic UML's ptrace path treats -ENODEV as "no register state
 * to inspect", which is the correct answer for a never-ran task.
 */
#include <linux/err.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <asm/backend.h>
#include <asm/ptrace.h>
#include <os.h>
#include <sysdep/ptrace.h>

#include "kvm_backend.h"

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED

int kvm_read_guest_regs(struct task_struct *t, struct pt_regs *regs)
{
	struct kvm_vcpu_handle *vcpu;
	struct kvm_regs kregs;
	unsigned long *gp;
	int rc;

	if (!t || !regs)
		return -EINVAL;

	vcpu = t->thread.arch.kvm.vcpu;
	if (!vcpu || vcpu->fd < 0)
		return -ENODEV;

	rc = os_ioctl_generic(vcpu->fd, KVM_GET_REGS, (unsigned long)&kregs);
	if (rc < 0)
		return rc;

	gp = regs->regs.gp;
	gp[HOST_AX]     = kregs.rax;
	gp[HOST_BX]     = kregs.rbx;
	gp[HOST_CX]     = kregs.rcx;
	gp[HOST_DX]     = kregs.rdx;
	gp[HOST_SI]     = kregs.rsi;
	gp[HOST_DI]     = kregs.rdi;
	gp[HOST_BP]     = kregs.rbp;
	gp[HOST_SP]     = kregs.rsp;
	gp[HOST_R8]     = kregs.r8;
	gp[HOST_R9]     = kregs.r9;
	gp[HOST_R10]    = kregs.r10;
	gp[HOST_R11]    = kregs.r11;
	gp[HOST_R12]    = kregs.r12;
	gp[HOST_R13]    = kregs.r13;
	gp[HOST_R14]    = kregs.r14;
	gp[HOST_R15]    = kregs.r15;
	gp[HOST_IP]     = kregs.rip;
	gp[HOST_EFLAGS] = kregs.rflags;
	return 0;
}

int kvm_write_guest_regs(struct task_struct *t, const struct pt_regs *regs)
{
	struct kvm_vcpu_handle *vcpu;
	struct kvm_regs kregs;
	const unsigned long *gp;
	int rc;

	if (!t || !regs)
		return -EINVAL;

	vcpu = t->thread.arch.kvm.vcpu;
	if (!vcpu || vcpu->fd < 0)
		return -ENODEV;

	/*
	 * Read current SREGS (segment selectors etc.) so KVM_SET_REGS
	 * doesn't need them. Then overlay the user's GP regs and write
	 * back. We don't need a fresh GET first because KVM_SET_REGS
	 * only writes the GP fields.
	 */
	memset(&kregs, 0, sizeof(kregs));
	gp = regs->regs.gp;
	kregs.rax    = gp[HOST_AX];
	kregs.rbx    = gp[HOST_BX];
	kregs.rcx    = gp[HOST_CX];
	kregs.rdx    = gp[HOST_DX];
	kregs.rsi    = gp[HOST_SI];
	kregs.rdi    = gp[HOST_DI];
	kregs.rbp    = gp[HOST_BP];
	kregs.rsp    = gp[HOST_SP];
	kregs.r8     = gp[HOST_R8];
	kregs.r9     = gp[HOST_R9];
	kregs.r10    = gp[HOST_R10];
	kregs.r11    = gp[HOST_R11];
	kregs.r12    = gp[HOST_R12];
	kregs.r13    = gp[HOST_R13];
	kregs.r14    = gp[HOST_R14];
	kregs.r15    = gp[HOST_R15];
	kregs.rip    = gp[HOST_IP];
	kregs.rflags = gp[HOST_EFLAGS];

	rc = os_ioctl_generic(vcpu->fd, KVM_SET_REGS, (unsigned long)&kregs);
	if (rc < 0)
		return rc;
	return 0;
}

#else /* !CONFIG_UM_BACKEND_KVM_INTEGRATED */

int kvm_read_guest_regs(struct task_struct *t, struct pt_regs *regs)
{
	(void)t; (void)regs;
	return -EOPNOTSUPP;
}

int kvm_write_guest_regs(struct task_struct *t, const struct pt_regs *regs)
{
	(void)t; (void)regs;
	return -EOPNOTSUPP;
}

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */
