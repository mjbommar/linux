/*
 * Copyright 2003 PathScale, Inc.
 *
 * Licensed under the GPL
 */

#ifndef __UM_PROCESSOR_X86_64_H
#define __UM_PROCESSOR_X86_64_H

#ifdef CONFIG_UM_BACKEND_KVM_V2
/*
 * KVM v2 stores per-task XSAVE state in arch_thread so task lifetime
 * owns the storage.  The full struct kvm_xsave is required once the
 * guest can see OSXSAVE and YMM-capable CPUID leaves; the legacy FPU
 * ioctl shape does not preserve the upper halves of YMM registers.
 */
#include <asm/kvm.h>
#endif

struct arch_thread {
        unsigned long debugregs[8];
        int debugregs_seq;
        struct faultinfo faultinfo;
#ifdef CONFIG_UM_BACKEND_KVM_V2
	struct {
		struct kvm_xsave fpu;
		bool fpu_valid;
		/*
		 * Per-task IST frame snapshot. The CPU pushes the
		 * exception delivery frame (error_code/RIP/CS/RFLAGS/
		 * RSP/SS = 6 * 8 bytes) onto vCPU's IST stack when an
		 * IDT-IST gate fires. Under v2's per-host-CPU vCPU
		 * pool, multiple UML tasks sharing one vCPU each push
		 * their own frame to the same physical IST stack;
		 * later pushes overwrite earlier frames. When this
		 * task's vcpu_run resumes after another task ran on
		 * the same vCPU, the trampoline's iretq would pop the
		 * other task's frame, jumping to the wrong CS:RIP.
		 *
		 * Fix: snapshot the frame contents at handle_io_pf
		 * time into per-task arch_thread storage; restore
		 * into vCPU's IST stack right before this task's
		 * next KVM_RUN re-entry. ist_pending=true means a
		 * frame is queued for restore; cleared on consumption.
		 *
		 * Contents are stored in iretq-pop order
		 * (low-address-first per SDM Vol.3 §6.14.5):
		 *   ist_frame[0] = error_code
		 *   ist_frame[1] = RIP
		 *   ist_frame[2] = CS
		 *   ist_frame[3] = RFLAGS
		 *   ist_frame[4] = RSP
		 *   ist_frame[5] = SS
		 */
		u64 ist_frame[6];
		bool ist_pending;

		/*
		 * Per-task FPU state captured after KVM_RUN exits and
		 * restored before the next KVM_RUN entry. The vCPU pool
		 * is shared by host CPU, not by UML task, so relying on
		 * KVM's vCPU-local FPU state would leak register contents
		 * across UML tasks. Use the XSAVE ioctl shape so extended
		 * state, including YMM upper halves, is preserved.
		 */
		struct kvm_xsave iotrap_fpu;
		bool iotrap_fpu_valid;

		/*
		 * Preserve CR2 when a host signal interrupts KVM_RUN in
		 * the middle of in-guest page-fault delivery. In that
		 * window KVM has already recorded the fault address, but
		 * the UML handler stub has not read it yet. Reinstall CR2
		 * before re-entry so the stub observes the original fault.
		 */
		u64  saved_cr2_at_eintr;
		bool saved_cr2_valid;

		/*
		 * Per-task snapshot of KVM's pending-event state. The
		 * IST-frame snapshot protects the hardware-pushed stack
		 * frame, but not KVM's in-kernel exception, interrupt
		 * shadow, NMI, or SMI queues. Save and restore those
		 * queues so a shared vCPU never delivers one UML task's
		 * pending event to another task.
		 */
		struct kvm_vcpu_events iotrap_events;
		bool iotrap_events_valid;

		/*
		 * One-shot bypass of CR0.TS arming. A #NM handler clears
		 * CR0.TS so the faulting FP instruction can retry. The
		 * next KVM entry must honor that clear once; subsequent
		 * entries arm CR0.TS normally so lazy-FPU detection still
		 * works for later FP bursts.
		 */
		bool nm_ts_bypass;
	} kvm_v2;
#endif
};

#ifdef CONFIG_UM_BACKEND_KVM_V2
#define INIT_ARCH_THREAD { .debugregs		= { [ 0 ... 7 ] = 0 }, \
			   .debugregs_seq	= 0, \
			   .faultinfo		= { 0, 0, 0 }, \
			   .kvm_v2		= { .fpu_valid = false, \
						    .ist_pending = false } }
#else
#define INIT_ARCH_THREAD { .debugregs		= { [ 0 ... 7 ] = 0 }, \
			   .debugregs_seq	= 0, \
			   .faultinfo		= { 0, 0, 0 } }
#endif

#define STACKSLOTS_PER_LINE 4

#ifdef CONFIG_UM_BACKEND_KVM_V2
extern int kvm_v2_fpu_capture_for_fork(struct arch_thread *from,
				       struct arch_thread *to);
#endif

static inline void arch_flush_thread(struct arch_thread *thread)
{
#ifdef CONFIG_UM_BACKEND_KVM_V2
	thread->kvm_v2.fpu_valid = false;
	thread->kvm_v2.iotrap_fpu_valid = false;
	thread->kvm_v2.iotrap_events_valid = false;
	thread->kvm_v2.ist_pending = false;
	thread->kvm_v2.saved_cr2_valid = false;
	thread->kvm_v2.saved_cr2_at_eintr = 0;
	thread->kvm_v2.nm_ts_bypass = false;
#endif
}

static inline void arch_copy_thread(struct arch_thread *from,
                                    struct arch_thread *to)
{
#ifdef CONFIG_UM_BACKEND_KVM_V2
	/*
	 * dup_task_struct() copied the parent's arch_thread. Clear
	 * one-shot KVM v2 state that belongs only to the parent's current
	 * vCPU dispatch before the child can enter KVM. Fork-time FPU
	 * state is captured below and must be left to that helper.
	 */
	to->kvm_v2.iotrap_fpu_valid = false;
	to->kvm_v2.iotrap_events_valid = false;
	to->kvm_v2.ist_pending = false;
	to->kvm_v2.saved_cr2_valid = false;
	to->kvm_v2.saved_cr2_at_eintr = 0;
	to->kvm_v2.nm_ts_bypass = false;
	(void)kvm_v2_fpu_capture_for_fork(from, to);
#endif
}

#define current_sp() ({ void *sp; __asm__("movq %%rsp, %0" : "=r" (sp) : ); sp; })
#define current_bp() ({ unsigned long bp; __asm__("movq %%rbp, %0" : "=r" (bp) : ); bp; })

#endif
