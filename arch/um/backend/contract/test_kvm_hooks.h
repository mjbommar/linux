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

/* Memo 11 G3 gadget state channel probes. */
int kvm_gadget_state_alloc(void);
void kvm_gadget_state_free(void);
void kvm_gadget_state_refresh(void);
u64 kvm_gadget_state_va(void);
u64 kvm_gadget_state_gpa(void);

/*
 * Mirror of struct kvm_gadget_state + offsets from
 * arch/um/backend/kvm/kvm_backend.h. The contract TU
 * doesn't include the private header, so drifting the
 * two copies would fail the _TEST_KVM_GADGET_OFF_*
 * equivalence checks below.
 */
struct _test_kvm_gadget_state {
	u32 seq;
	u32 cpu_id;
	u32 pid;
	u32 tgid;
	u32 ppid;
	u32 uid;
	u32 euid;
	u32 gid;
	u32 egid;
	u32 _pad;
};
#define _TEST_KVM_GADGET_OFF_TGID	0x0c
#define _TEST_KVM_GADGET_OFF_UID	0x14

/*
 * Memo 10 syscall classification. Mirror of the enum in
 * arch/um/backend/kvm/kvm_backend.h; duplicated here for the
 * same reason as the kvm_enter_guest_probe prototype above
 * (private header not on the contract TU's include path).
 */
enum kvm_syscall_class {
	_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH = 0,
	_TEST_KVM_SYSCALL_CLASS_VCPU_STATE,
	_TEST_KVM_SYSCALL_CLASS_SIGFRAME,
	_TEST_KVM_SYSCALL_CLASS_TRAP,
};
enum kvm_syscall_class kvm_classify_syscall(unsigned long nr);

#define _TEST_UM_KVM_SYSCALL_PORT	0xf4
#define _TEST_UM_KVM_SYSRETQ_PORT	0xf5

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */
#endif /* __ARCH_UM_BACKEND_CONTRACT_TEST_KVM_HOOKS_H */
