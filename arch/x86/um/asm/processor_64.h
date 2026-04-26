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
        } kvm;
#endif
};

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
#define INIT_ARCH_THREAD { .debugregs  		= { [ 0 ... 7 ] = 0 }, \
			   .debugregs_seq	= 0, \
			   .faultinfo		= { 0, 0, 0 }, \
			   .kvm			= { .fpu_valid = false, \
						    .events_valid = false } }
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
#endif
}

static inline void arch_copy_thread(struct arch_thread *from,
                                    struct arch_thread *to)
{
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	/*
	 * Per-task vCPU state is NOT inherited from the parent: a freshly-
	 * forked child starts with an empty FPU and no pending vCPU
	 * exceptions. Restore-on-first-switch will see fpu_valid=false and
	 * leave the vCPU's current FPU alone (matches the pre-#mod-17
	 * "no slot found → don't touch vCPU FPU" policy in
	 * kvm_fpu_restore_for_task).
	 */
	to->kvm.fpu_valid = false;
	to->kvm.events_valid = false;
#endif
}

#define current_sp() ({ void *sp; __asm__("movq %%rsp, %0" : "=r" (sp) : ); sp; })
#define current_bp() ({ unsigned long bp; __asm__("movq %%rbp, %0" : "=r" (bp) : ); bp; })

#endif
