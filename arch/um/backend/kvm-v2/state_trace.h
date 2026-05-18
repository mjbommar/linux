/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KVM v2 state-snapshot trace ring — public API.
 *
 * Design rationale lives in state_trace.c's leader comment and in
 * Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
 * state-audit/00-overview.md (which enumerated the ~149 state items
 * and ~50 operations this trace ring captures).
 *
 * Three layers shield production from any cost:
 *   1. CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE=n: every hook below
 *      compiles to do{}while(0); no text emitted; no struct sizes
 *      change.
 *   2. CONFIG=y but disabled at runtime: each hook is one
 *      static_branch_unlikely test → 5-byte NOP when disabled.
 *      Toggle via debugfs.
 *   3. Per-CPU ring storage: no locks, no cross-CPU cacheline
 *      bouncing.
 *
 * Operation IDs mirror state-audit Layer 2 (file
 * 02-operations-inventory.md). Names match the call-site comment so
 * grep round-trips between trace dump and the audit doc.
 */
#ifndef _UM_BACKEND_KVM_V2_STATE_TRACE_H
#define _UM_BACKEND_KVM_V2_STATE_TRACE_H

#include <linux/types.h>

struct uml_pt_regs;
struct kvm_run;
struct kvm_v2_vcpu;

enum kvm_v2_trace_op {
	KVMV2_OP_VCPU_RUN_ENTRY       = 1,  /* vcpu.c: top of vcpu_run     */
	KVMV2_OP_POST_TLB_SYNC        = 2,  /* vcpu.c: after um_tlb_sync   */
	KVMV2_OP_POST_LOAD_SREGS      = 3,  /* vcpu.c: after CR4.PGE toggle*/
	KVMV2_OP_POST_FPU_INSTALL     = 4,  /* vcpu.c: after iotrap restore*/
	KVMV2_OP_POST_IST_RESTORE     = 5,  /* vcpu.c: after frame_restore */
	KVMV2_OP_PRE_KVM_RUN          = 6,  /* vcpu.c: before KVM_RUN ioctl*/
	KVMV2_OP_POST_KVM_RUN         = 7,  /* vcpu.c: after KVM_RUN ioctl */
	KVMV2_OP_EINTR_PATH           = 8,  /* vcpu.c: -EINTR taken        */
	KVMV2_OP_EINTR_INLINE_PF      = 9,  /* vcpu.c: PF inline replay    */
	KVMV2_OP_EINTR_RAW_SNAPSHOT   = 10, /* vcpu.c: ist_frame_snapshot  */
	KVMV2_OP_HANDLE_SYSCALL_PRE   = 11, /* syscall_trap: handle_sys in */
	KVMV2_OP_HANDLE_SYSCALL_POST  = 12, /* syscall_trap: handle_sys out*/
	KVMV2_OP_HANDLE_IO_PF_PRE     = 13, /* syscall_trap: handle_io_pf  */
	KVMV2_OP_HANDLE_IO_PF_POST    = 14, /* syscall_trap: handle_io_pf  */
	KVMV2_OP_IST_FRAME_WRITE_PRE  = 15, /* syscall_trap: ist_write in  */
	KVMV2_OP_IST_FRAME_WRITE_POST = 16, /* syscall_trap: ist_write out */
	KVMV2_OP_VCPU_RUN_EXIT        = 17, /* vcpu.c: bottom of vcpu_run  */
	KVMV2_OP_TRACE_TRIGGER        = 18, /* dump trigger marker         */
	KVMV2_OP_EINTR_INLINE_LSTAR   = 19, /* vcpu.c: SMP-T25 LSTAR-EINTR */
	KVMV2_OP_DISPATCH_LOCATION    = 20, /* vcpu.c: Round 6 pool-share
					     * correlation — records the
					     * (host_cpu, vcpu, pid, mm,
					     * dispatch_seq) tuple after the
					     * per-host-CPU vCPU pick in
					     * kvm_v2_vcpu_run so post-process
					     * can correlate flakes with
					     * pool-slot reuse / cross-CPU
					     * task migration windows.
					     */
	KVMV2_OP_MAX
};

/*
 * Per-snapshot record. Sized for ~384 bytes packed (kept in source
 * order for readability — see kvm_v2_state_trace_capture for the
 * fill order). Default ring size is 2 MB / CPU ⇒ ~5400 entries.
 */
struct kvm_v2_state_snap {
	u64 ts;                     /* sched_clock() */
	u32 seq;                    /* per-cpu sequence */
	u32 pid;                    /* current->pid */
	u8  cpu;                    /* smp_processor_id */
	u8  op;                     /* enum kvm_v2_trace_op */
	u8  exit_reason;            /* run->exit_reason; 0 N/A */
	u8  flags;                  /* unused — reserved */
	u16 io_port;                /* run->io.port; 0 N/A */
	u16 _pad0;

	/* Live KVM regs from run->s.regs.regs (16 GPR + RIP + RFLAGS) */
	u64 rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
	u64 r8,  r9,  r10, r11, r12, r13, r14, r15;
	u64 rip, rflags;

	/* Selected sregs */
	u64 cr0, cr2, cr3, cr4;
	u64 fs_base, gs_base;

	/* Per-task state */
	u64 task_mm_ptr;
	u64 task_active_mm_ptr;
	u64 task_saved_cr2;
	u64 task_host_ax;
	u64 task_host_orig_ax;	/* PT_SYSCALL_NR — syscall number in flight */
	u64 task_host_ip;
	u64 task_host_sp;
	u64 regs_ptr;
	u64 current_regs_ptr;
	u32 task_fpu_hash;
	u8  regs_current_match;
	u8  task_saved_cr2_valid;
	u8  task_ist_pending;
	u8  task_iotrap_fpu_valid;
	u8  task_fpu_valid;

	/* Per-task IST frame snapshot (error_code, RIP, CS, RFL, RSP, SS) */
	u64 task_ist_frame[6];

	/* Per-mm + per-vCPU */
	u64 mm_tlb_gen;
	u64 vcpu_last_seen_tlb_gen;
	u64 vcpu_current_mm;
	u32 vcpu_kick_pending;
	u32 _pad1;

	/* Live IST page top-48 (the iretq frame) */
	u64 ist_live[6];
};

#ifdef CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE

#include <linux/jump_label.h>

DECLARE_STATIC_KEY_FALSE(kvm_v2_state_trace_key);

void kvm_v2_state_trace_capture(enum kvm_v2_trace_op op,
				struct uml_pt_regs *regs,
				struct kvm_run *run,
				struct kvm_v2_vcpu *vcpu);

void kvm_v2_state_trace_dump(const char *reason);
void kvm_v2_state_trace_clear(void);
void kvm_v2_state_trace_freeze(const char *reason);

#define KVMV2_TRACE(op_, regs_, run_, vcpu_) do {			\
	if (static_branch_unlikely(&kvm_v2_state_trace_key))		\
		kvm_v2_state_trace_capture((op_), (regs_), (run_),	\
					   (vcpu_));			\
} while (0)

#else /* !CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE */

static inline void kvm_v2_state_trace_capture(enum kvm_v2_trace_op op,
					      struct uml_pt_regs *regs,
					      struct kvm_run *run,
					      struct kvm_v2_vcpu *vcpu) { }
static inline void kvm_v2_state_trace_dump(const char *reason) { }
static inline void kvm_v2_state_trace_clear(void) { }
static inline void kvm_v2_state_trace_freeze(const char *reason) { }

#define KVMV2_TRACE(op_, regs_, run_, vcpu_) do { } while (0)

#endif /* CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE */

#endif /* _UM_BACKEND_KVM_V2_STATE_TRACE_H */
