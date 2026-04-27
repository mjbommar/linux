/*
 * Copyright 2003 PathScale, Inc.
 *
 * Licensed under the GPL
 */

#ifndef __UM_PROCESSOR_X86_64_H
#define __UM_PROCESSOR_X86_64_H

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
/*
 * Memo 17 Phase D: per-task vCPU state. Embed kvm_fpu / kvm_vcpu_events
 * directly in arch_thread so the hash UAF (B-FPU-HASH-UAF in memo
 * 16-architecture-review/00-synthesis.md) is structurally impossible:
 * task lifetime owns the storage, no separate allocation, no exit hook
 * needed, no slab-recycle hazard. AVX is currently masked at CPUID
 * (lifecycle.c:426) so the legacy 512 B kvm_fpu suffices; switch to
 * KVM_GET_XSAVE2 if/when AVX is exposed.
 */
#include <asm/kvm.h>
#endif

/*
 * Forward declaration so arch_thread can hold a per-task vCPU handle
 * without dragging the full struct kvm_vcpu_handle definition (which
 * lives in arch/um/backend/kvm/kvm_backend.h, behind a different
 * include set) into every TU that includes processor_64.h.
 *
 * Per-task vCPU is the Stage A redesign of the KVM backend
 * (Documentation/virt/uml/redesign/03-architecture-review-2026-04-27).
 * The pointer is NULL until the task's first kvm_run_userspace,
 * which lazy-allocates via kvm_vcpu_for_current().
 */
struct kvm_vcpu_handle;

struct arch_thread {
        unsigned long debugregs[8];
        int debugregs_seq;
        struct faultinfo faultinfo;
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
        struct {
                struct kvm_fpu fpu;
                struct kvm_vcpu_events events;
                bool fpu_valid;
                bool events_valid;
                struct kvm_vcpu_handle *vcpu;	/* per-task vCPU; NULL = not yet used */
        } kvm;
#endif
};

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
#define INIT_ARCH_THREAD { .debugregs  		= { [ 0 ... 7 ] = 0 }, \
			   .debugregs_seq	= 0, \
			   .faultinfo		= { 0, 0, 0 }, \
			   .kvm			= { .fpu_valid = false, \
						    .events_valid = false, \
						    .vcpu = NULL } }
#else
#define INIT_ARCH_THREAD { .debugregs  		= { [ 0 ... 7 ] = 0 }, \
			   .debugregs_seq	= 0, \
			   .faultinfo		= { 0, 0, 0 } }
#endif

#define STACKSLOTS_PER_LINE 4

static inline void arch_flush_thread(struct arch_thread *thread)
{
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	thread->kvm.fpu_valid = false;
	thread->kvm.events_valid = false;
	/*
	 * exec() flushes the address space; the per-task vCPU survives
	 * the exec because the same UML task continues running. fpu/events
	 * are invalidated so the next switch-in starts from architectural
	 * defaults rather than carrying registers from the pre-exec image.
	 * Do NOT free the vcpu handle here — that's exit_thread's job.
	 */
#endif
}

extern int kvm_fpu_capture_for_fork(struct arch_thread *from,
				    struct arch_thread *to);

static inline void arch_copy_thread(struct arch_thread *from,
                                    struct arch_thread *to)
{
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	/*
	 * Stage A.5: capture the parent's REAL vCPU FPU state into the
	 * child's arch_thread.kvm.fpu via KVM_GET_FPU on the parent's
	 * per-task vcpu_fd. Pre-Stage-A, this snapshot was via the
	 * singleton vcpu0_fd which never ran a guest, so the child got
	 * KVM-default FPU instead of the parent's — violating POSIX
	 * fork() FPU-inheritance semantics.
	 *
	 * kvm_fpu_capture_for_fork also:
	 *   - Clears to->kvm.vcpu (per-task handle, must not alias).
	 *   - Clears to->kvm.events_valid (fork doesn't inherit pending
	 *     exceptions per POSIX).
	 *   - Sets to->kvm.fpu_valid=true on success; the child's first
	 *     kvm_run_userspace restores via kvm_fpu_install_on_first_run.
	 *
	 * For exec, arch_flush_thread runs separately and resets
	 * fpu_valid=false so the post-exec image gets architectural-
	 * default FPU on first run.
	 */
	(void)kvm_fpu_capture_for_fork(from, to);
#endif
}

#define current_sp() ({ void *sp; __asm__("movq %%rsp, %0" : "=r" (sp) : ); sp; })
#define current_bp() ({ unsigned long bp; __asm__("movq %%rbp, %0" : "=r" (bp) : ); bp; })

#endif
