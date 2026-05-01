/*
 * Copyright 2003 PathScale, Inc.
 *
 * Licensed under the GPL
 */

#ifndef __UM_PROCESSOR_X86_64_H
#define __UM_PROCESSOR_X86_64_H

#ifdef CONFIG_UM_BACKEND_KVM_V2
/*
 * Phase C.4.0: per-task FPU snapshot for v2's KVM_GET_FPU /
 * KVM_SET_FPU dance (memo 26 §C.4). Embedding the legacy 512 B
 * kvm_fpu directly in arch_thread mirrors v1's solution to the
 * B-FPU-HASH-UAF class — task lifetime owns the storage, no
 * separate allocation, no slab-recycle hazard. AVX/AVX-512 are
 * masked at CPUID by kvm_v2_curate_cpuid (vcpu.c), so the
 * legacy 512 B FXSAVE area is sufficient; KVM_GET/SET_XSAVE
 * is moot under our curated guest.
 */
#include <asm/kvm.h>
#endif

struct arch_thread {
        unsigned long debugregs[8];
        int debugregs_seq;
        struct faultinfo faultinfo;
#ifdef CONFIG_UM_BACKEND_KVM_V2
	struct {
		struct kvm_fpu fpu;
		bool fpu_valid;
		/*
		 * Per-task IST frame snapshot. The CPU pushes the
		 * exception delivery frame (error_code/RIP/CS/RFLAGS/
		 * RSP/SS = 6 × 8 bytes) onto vCPU's IST stack when an
		 * IDT-IST gate fires. Under v2's per-host-CPU vCPU
		 * pool, multiple UML tasks sharing one vCPU each push
		 * their own frame to the SAME physical IST stack —
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
		 * Memo §H.1b SMOKING-GUN fix (2026-04-30): per-task
		 * snapshot of FPU state captured IMMEDIATELY after
		 * KVM_RUN exits, restored IMMEDIATELY before next
		 * KVM_RUN entry. Bypasses KVM's broken auto-save on
		 * fast-path KVM_EXIT_IO that lets host kernel-mode
		 * code clobber XMM/x87 between exit and re-entry.
		 *
		 * mt-xmmprobe.c proved: without this, XMM0 changes
		 * from `02020202...02` (user's splat) to `16000000ff
		 * ffffff0000000000000000` (kernel struct data) across
		 * a #PF cycle — exact same pattern as the residual
		 * mt-mmap-stress page corruption.
		 */
		struct kvm_fpu iotrap_fpu;
		bool iotrap_fpu_valid;

		/*
		 * #121-D15 fix (2026-05-01): preserve sregs.cr2 across
		 * an EINTR that caught the vCPU mid-IDT-delivery. When
		 * SIGALRM interrupts KVM_RUN AFTER hardware pushed the
		 * #PF IDT frame to the IST stack and set RIP=stub-start
		 * (KVM_V2_HANDLERS_GVA + 0x140), but BEFORE the in-guest
		 * stub's first `push %rax` ran, KVM exits with -EINTR
		 * and VMCB.save.cr2 still holds the real fault address.
		 * Without preservation, the next dispatch's
		 * load_user_sregs() writes sregs.cr2 = 0 → vcpu->arch.cr2
		 * = 0 → VMCB.save.cr2 = 0 on the next VMRUN entry. The
		 * stub then resumes from 0x2140, executes `mov %cr2,
		 * %rax` reading 0, captures 0 into the IST page slot,
		 * `out %al, $0xf6` vmexits — and handle_io_pf sees
		 * cr2=0 with no IDT frame freshly pushed (the captured
		 * IST data is from the prior real fault), so the fault
		 * address is lost. With this preservation, sregs.cr2 is
		 * re-installed on the next dispatch so the stub reads
		 * the correct CR2.
		 *
		 * Set true at EINTR-with-stub-RIP; cleared on consumption
		 * by load_user_sregs.
		 */
		u64  saved_cr2_at_eintr;
		bool saved_cr2_valid;
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
#endif
}

static inline void arch_copy_thread(struct arch_thread *from,
                                    struct arch_thread *to)
{
#ifdef CONFIG_UM_BACKEND_KVM_V2
	(void)kvm_v2_fpu_capture_for_fork(from, to);
#endif
}

#define current_sp() ({ void *sp; __asm__("movq %%rsp, %0" : "=r" (sp) : ); sp; })
#define current_bp() ({ unsigned long bp; __asm__("movq %%rbp, %0" : "=r" (bp) : ); bp; })

#endif
