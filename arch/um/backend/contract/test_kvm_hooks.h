/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private test hooks into the KVM backend's integrated path
 * (memo 08). Only consumed by arch/um/backend/contract/test_ops.c.
 *
 * Prototypes duplicate the public ones in
 * arch/um/backend/kvm/kvm_backend.h rather than including that
 * header cross-subdirectory — the private KVM header is not on
 * the contract-test translation unit's include path and adding
 * one relative include to this one TU is easier to review than
 * a cross-subdir header plumb.
 *
 * If either side drifts, the kvm_wire_ports_test / NULL-probe
 * test assertions fail at runtime.
 */
#ifndef __ARCH_UM_BACKEND_CONTRACT_TEST_KVM_HOOKS_H
#define __ARCH_UM_BACKEND_CONTRACT_TEST_KVM_HOOKS_H

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED

#include <linux/kvm.h>
#include <linux/types.h>
#include <sysdep/ptrace.h>

int kvm_enter_guest_probe(struct kvm_sregs *sregs, struct kvm_regs *regs,
			  const struct uml_pt_regs *src,
			  u64 cr3_gpa, u64 gdt_gpa);
int kvm_exit_guest_probe(struct uml_pt_regs *dst, const struct kvm_regs *src);
int kvm_bootstrap_copy_lstar(u8 *dst, size_t len);
int kvm_bootstrap_force_init(void);

/* Memo 09 step 1 lifecycle probes. */
u64 kvm_shadow_pgd_gpa(void);
int kvm_shadow_pgd_alloc(void);
void kvm_shadow_pgd_free(void);

#define _TEST_UM_KVM_SYSCALL_PORT	0xf4
#define _TEST_UM_KVM_SYSRETQ_PORT	0xf5

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */
#endif /* __ARCH_UM_BACKEND_CONTRACT_TEST_KVM_HOOKS_H */
