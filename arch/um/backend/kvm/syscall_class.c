// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend syscall classification table. Implements the
 * static class_map step 2 (class-D denylist) + step 6
 * (kvm_classify_syscall accessor) from memo 10:
 * Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
 * 10-syscall-classification.md.
 *
 * The generated truth table lives alongside the memo at
 * `02-workstreams/D-kvm-backend/syscall-inventory.tsv`; this
 * C map mirrors the 12 non-A rows. Any disagreement between
 * the two is caught by the KUnit cross-check in
 * arch/um/backend/contract/test_ops.c.
 *
 * The map is sized by NR_syscalls (x86_64 native table size)
 * and is SPARSE: unspecified slots default to CLASS_PASSTHROUGH
 * (enum value 0), which is the identity dispatcher. New
 * upstream syscalls therefore inherit PASSTHROUGH automatically
 * — promoting a new arrival to B/C/D is a deliberate edit here
 * AND the corresponding row update in syscall-inventory.tsv.
 */

#include <linux/bug.h>
#include <linux/export.h>

#include <asm/unistd.h>

#include "kvm_backend.h"

static const enum kvm_syscall_class kvm_syscall_class_map[NR_syscalls] = {
	/* Class B — vCPU-state propagate (memo 10 §B). */
	[__NR_arch_prctl]	= KVM_SYSCALL_CLASS_VCPU_STATE,
	[__NR_modify_ldt]	= KVM_SYSCALL_CLASS_VCPU_STATE,
	[__NR_set_thread_area]	= KVM_SYSCALL_CLASS_VCPU_STATE,

	/* Class C — signal-frame (memo 10 §C). */
	[__NR_rt_sigreturn]	= KVM_SYSCALL_CLASS_SIGFRAME,

	/* Class D — deny (memo 10 §D). */
	[__NR_ptrace]		= KVM_SYSCALL_CLASS_TRAP,
	[__NR_reboot]		= KVM_SYSCALL_CLASS_TRAP,
	[__NR_init_module]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_finit_module]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_delete_module]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_kexec_load]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_kexec_file_load]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_bpf]		= KVM_SYSCALL_CLASS_TRAP,

	/* Everything else defaults to 0 = CLASS_PASSTHROUGH. */
};

enum kvm_syscall_class kvm_classify_syscall(unsigned long nr)
{
	/*
	 * Out-of-range numbers (including the -1 sentinel UML uses
	 * for "not a syscall frame") fall into PASSTHROUGH. The
	 * caller's regular handle_syscall path will then surface
	 * -ENOSYS via sys_call_table's trailing sys_ni_syscall
	 * slot; no need to synthesize a dispatcher-layer trap for
	 * unknown numbers since UML's kernel is the authoritative
	 * validator.
	 */
	if (nr >= NR_syscalls)
		return KVM_SYSCALL_CLASS_PASSTHROUGH;
	return kvm_syscall_class_map[nr];
}
EXPORT_SYMBOL_GPL(kvm_classify_syscall);
