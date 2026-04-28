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
	/*
	 * Class B — vCPU-state propagate (memo 10 §B).
	 *
	 * Audit round-5 F10: modify_ldt and set_thread_area were
	 * previously classified B but had NO dispatcher branch.
	 * modify_ldt is legacy x86 LDT programming that UML
	 * doesn't virtualize through KVM (no LDT SREGS refresh
	 * after sys_modify_ldt), and set_thread_area is a 32-bit
	 * compat syscall that 64-bit glibc never calls. Demoted
	 * to D so the dispatcher returns -EPERM, which is the
	 * accurate "UML KVM doesn't support this" answer —
	 * previously the syscalls passed silently through to
	 * handle_syscall with no vCPU propagation.
	 *
	 * arch_prctl remains the only live class-B entry: the
	 * kvm_decode_syscall dispatcher has a specific branch
	 * for it that calls kvm_propagate_fs_gs_base (see
	 * thread.c's post-syscall block).
	 */
	[__NR_arch_prctl]	= KVM_SYSCALL_CLASS_VCPU_STATE,

	/*
	 * Class C — signal-frame (memo 10 §C).
	 *
	 * Audit round-5 F10: rt_sigreturn needs no dedicated
	 * dispatcher branch because the common post-syscall
	 * KVM_SET_REGS in run_userspace (thread.c:1747) already
	 * pushes the sigreturn-restored regs into the vCPU. The
	 * C classification stays as a documentation tag so the
	 * memo inventory matches the live behaviour.
	 */
	[__NR_rt_sigreturn]	= KVM_SYSCALL_CLASS_SIGFRAME,

	/*
	 * Class D — deny (memo 10 §D). Includes modify_ldt +
	 * set_thread_area post-F10.
	 */
	[__NR_ptrace]		= KVM_SYSCALL_CLASS_TRAP,
	[__NR_reboot]		= KVM_SYSCALL_CLASS_TRAP,
	[__NR_init_module]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_finit_module]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_delete_module]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_kexec_load]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_kexec_file_load]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_bpf]		= KVM_SYSCALL_CLASS_TRAP,
	[__NR_modify_ldt]	= KVM_SYSCALL_CLASS_TRAP,
	[__NR_set_thread_area]	= KVM_SYSCALL_CLASS_TRAP,

	/*
	 * Class E — gadget-handled (memo 11 G4-G6 + G7). The fast
	 * path is the in-guest LSTAR gadget; a CLASS_GADGET VMEXIT
	 * means the gadget chose the fallback, in which case the
	 * dispatcher routes through CLASS_PASSTHROUGH semantics.
	 * See enum kvm_syscall_class definition in kvm_backend.h.
	 * The 10 entries below mirror the live LSTAR dispatch
	 * table in arch/um/backend/kvm/thread.c; any drift is
	 * caught by the KUnit cross-check in arch/um/backend/
	 * contract/test_ops.c (kvm_syscall_class_count_test).
	 * Count = 7 pid-family + clock_gettime + time + getcpu;
	 * sched_yield was demoted in audit round-6 G5 (D94).
	 */
	[__NR_getpid]		= KVM_SYSCALL_CLASS_GADGET,
	[__NR_gettid]		= KVM_SYSCALL_CLASS_GADGET,
	[__NR_getppid]		= KVM_SYSCALL_CLASS_GADGET,
	[__NR_getuid]		= KVM_SYSCALL_CLASS_GADGET,
	[__NR_geteuid]		= KVM_SYSCALL_CLASS_GADGET,
	[__NR_getgid]		= KVM_SYSCALL_CLASS_GADGET,
	[__NR_getegid]		= KVM_SYSCALL_CLASS_GADGET,
	[__NR_clock_gettime]	= KVM_SYSCALL_CLASS_GADGET,
	/*
	 * Audit round-6 G5: __NR_sched_yield demoted from
	 * CLASS_GADGET back to CLASS_PASSTHROUGH (default class A).
	 * In-gadget short-circuit returned 0 without consulting
	 * UML's scheduler; demoting routes sched_yield through
	 * handle_syscall → sys_sched_yield → schedule(). LSTAR
	 * dispatch entry kept but redirected to fallback (D94).
	 */
	/* G6-follow-on (memo 11 G6f): time + getcpu handlers. */
	[__NR_time]		= KVM_SYSCALL_CLASS_GADGET,
	[__NR_getcpu]		= KVM_SYSCALL_CLASS_GADGET,

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
