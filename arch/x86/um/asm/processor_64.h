/*
 * Copyright 2003 PathScale, Inc.
 *
 * Licensed under the GPL
 */

#ifndef __UM_PROCESSOR_X86_64_H
#define __UM_PROCESSOR_X86_64_H

#ifdef CONFIG_UM_BACKEND_KVM_V2
/*
 * Phase C.4.0 (rev SMP-T73, 2026-05-18): per-task FPU snapshot for
 * v2's KVM_GET_XSAVE / KVM_SET_XSAVE dance. Embedding the
 * struct kvm_xsave directly in arch_thread mirrors v1's solution
 * to the B-FPU-HASH-UAF class — task lifetime owns the storage,
 * no separate allocation, no slab-recycle hazard.
 *
 * Originally used struct kvm_fpu (legacy 512 B FXSAVE area)
 * because Phase C.4.0 predates SMP-T57 Phase A. Comment then
 * read "AVX/AVX-512 are masked at CPUID; KVM_GET/SET_XSAVE is
 * moot." SMP-T57 Phase A (commit ab68bf077de3, 2026-05-04)
 * un-masked AVX/AVX2/FMA/F16C/OSXSAVE/XSAVE in CPUID and set
 * CR4.OSXSAVE + XCR0 = FP|SSE|YMM, so glibc now uses AVX-256
 * memcpy / memset variants. The legacy 512 B FXSAVE area only
 * covers x87 + XMM low 128; YMM upper 128 lives in the extended
 * XSAVE state (CPUID leaf 0xD layout) and is lost per-dispatch
 * if we keep using KVM_GET_FPU.
 *
 * That YMM-upper loss across the per-host-CPU vCPU pool drove
 * the Django cache-abort flake — 14 rounds of investigation
 * before correlation with prior T26/T27 + T57 Phase A reasoning
 * landed the fix. See R14 addendum in
 * Documentation/virt/uml/redesign/08-future-phases/
 * 50-kvm-v2-django-flake-investigation-summary.md.
 *
 * Cost: 4 KB - 512 B = 3.5 KB extra per task_struct. snapshot.c
 * already pays this for the snapshot ioctls. Acceptable.
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
		 *
		 * SMP-T73 (2026-05-18): switched from struct kvm_fpu
		 * (legacy 512 B FXSAVE — x87 + XMM low 128) to
		 * struct kvm_xsave (4 KB extended XSAVE — includes
		 * YMM upper 128). See header comment above.
		 */
		struct kvm_xsave iotrap_fpu;
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

		/*
		 * SMP-T75 (Round 14 HW-audit Q2): per-task snapshot of
		 * the in-kernel pending-exception / interrupt-shadow /
		 * NMI / SMI queue, captured via KVM_GET_VCPU_EVENTS
		 * after KVM_RUN exits and restored via
		 * KVM_SET_VCPU_EVENTS before the next KVM_RUN entry.
		 *
		 * Same architectural shape as iotrap_fpu (SMP-T73 /
		 * T26/T27). Without this, the per-host-CPU vCPU pool
		 * leaks a task's queued-but-not-yet-injected exception
		 * (events.exception.pending=1 at vmexit time) into the
		 * next dispatching task. The IST-frame restore at
		 * vcpu.c:2390 handles the hardware-pushed IST stack but
		 * NOT KVM's in-kernel pending-event queue.
		 *
		 * Round 14 HW audit identified this as a latent bug —
		 * dormant only because the UML guest's normal exception
		 * mix has the bulk of pending exceptions land before
		 * KVM_RUN returns (page faults injected and consumed in
		 * the same dispatch). A workload that triggers
		 * exception-pending-at-exit (debug traps, async NMI from
		 * #MC, certain SVM intercepts) would cross-task leak.
		 *
		 * Capacity: struct kvm_vcpu_events is ~184 B. Cheap.
		 */
		struct kvm_vcpu_events iotrap_events;
		bool iotrap_events_valid;

		/*
		 * SMP-T22 (2026-05-02): one-shot bypass of CR0.TS arming
		 * in kvm_v2_load_user_sregs. Set true by the host-side
		 * #NM handlers (kvm_v2_handle_io_nm and the EINTR variant
		 * kvm_v2_handle_nm_eintr_inline) after they clear sregs.
		 * cr0.TS for the user's FP-instruction retry. The next
		 * dispatch's load_user_sregs reads this flag, skips the
		 * unconditional `sregs->cr0 |= TS` and clears the flag,
		 * so the user actually re-enters at user_rip with TS=0.
		 *
		 * Without this, my host-side TS clear is undone by the
		 * very next dispatch's lazy-FPU TS-arming, the user FP
		 * faults again, vmexit, handler clears TS, dispatch arms
		 * TS, ad infinitum (boot hangs at first FP instruction).
		 *
		 * Lazy-FPU semantics preserved: after the user's FP burst
		 * completes and the user vmexits via syscall (or any non-
		 * #NM exit), the next dispatch's load_user_sregs sees
		 * nm_ts_bypass=false and arms TS=1 normally — so the
		 * NEXT FP burst still trips one #NM and the lazy-FPU
		 * "FPU-not-used" detection still works for non-FP
		 * dispatches.
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
	 * SMP-T19 (2026-05-02): the child task's arch_thread was just
	 * memcpy'd from the parent by dup_task_struct(). For the kvm-v2
	 * fields specifically, the parent may carry ist_pending=true with
	 * an ist_frame[] holding a snapshot from the parent's last
	 * mid-stub EINTR (or other code path that called ist_frame_write).
	 * Without explicit reset here, the child's first KVM_RUN would call
	 * kvm_v2_ist_frame_restore_pending and inject the parent's saved
	 * frame into the new vCPU's IST top-40..top-8. If the parent's
	 * ist_frame[1] was a kernel-half stub address (e.g., from a
	 * stub-internal #GP whose IDT push left RIP=stub_addr in the IST
	 * which got snapshotted before any user-mode hardware push could
	 * overwrite it), the iretq off the next exception delivery for
	 * the child would pop kernel-half RIP into user-mode CPL=3 →
	 * Bug B-class fault: `segfault at 0 ip ffffe000000021c2 ...`.
	 *
	 * Mirror what arch_flush_thread (execve path) already does. fork
	 * + execve is the dominant path used by subprocess.Popen, regrtest
	 * worker spawning, etc., and the missing reset here is what made
	 * the threaded-subprocess-wait reproducer fail at ~40% per boot.
	 *
	 * fpu_valid is updated by kvm_v2_fpu_capture_for_fork below
	 * (KVM_GET_FPU into to->kvm_v2.fpu); leave that field alone.
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
