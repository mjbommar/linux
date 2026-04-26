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
	 * Memo 17 Phase J (finding 6): fork inherits parent's FPU
	 * (POSIX semantics; matches arch/x86 native). dup_task_struct
	 * already memcpy'd from->kvm.fpu into to->kvm.fpu — preserve
	 * that by copying fpu_valid through. The parent's saved FPU
	 * may be stale (from before its last switch-out) but is the
	 * best snapshot available; the parent's switch-out path will
	 * KVM_GET_FPU into to->kvm.fpu's matching slot anyway, so the
	 * race window is bounded.
	 *
	 * Pending vCPU events are NOT inherited — fork() doesn't
	 * propagate in-flight exceptions. events_valid stays at
	 * whatever was memcpy'd from parent; explicitly invalidate
	 * to force a known-clean state on first switch-in (the fresh-
	 * task path in kvm_fpu_restore_for_task writes a zeroed
	 * kvm_vcpu_events with all VALID flags set).
	 *
	 * For exec, arch_flush_thread is called separately and resets
	 * fpu_valid=false → architectural-init FPU on first switch-in.
	 */
	(void)from;
	to->kvm.events_valid = false;
#endif
}

#define current_sp() ({ void *sp; __asm__("movq %%rsp, %0" : "=r" (sp) : ); sp; })
#define current_bp() ({ unsigned long bp; __asm__("movq %%rbp, %0" : "=r" (bp) : ); bp; })

#endif
