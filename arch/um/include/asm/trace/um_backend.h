/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML backend tracepoints (memo 25 R7).
 *
 * Per-event definitions for the `um_backend` ftrace subsystem.
 * Hooks fire at the boundary between UML's mm-arbiter / trap loop
 * and the active backend (today: seccomp; tomorrow: kvm-v2).
 *
 * Enable with `trace-cmd record -e um_backend:*`.
 *
 * Event list:
 *
 *  - um_backend_mm_create(mm)         — per-mm lifecycle start
 *  - um_backend_mm_destroy(mm)        — per-mm lifecycle end
 *  - um_backend_mm_region_added(...)  — backend learned of a new
 *                                       VA range mapping
 *  - um_backend_mm_region_removed     — backend learned of an
 *                                       unmap
 *
 * The seccomp run loop has no kernel-side vcpu_run_enter / exit
 * hook (it lives in a USER TU compiled with USER_CFLAGS, no access
 * to kernel-side tracepoint macros). Phase C.2's
 * kvm_v2_vcpu_run() runs in kernel context, so the
 * um_backend_kvm_v2_vcpu_enter / _exit events below cover the v2
 * dispatcher; seccomp-side coverage is left to the worker-process
 * wrapper as memo 28 lands. Memo 25 R7's longer wishlist
 * (mm_region_protected, syscall_dispatch, signal_received) lands
 * incrementally as each surface gets a kernel-side hook.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM um_backend

#if !defined(_TRACE_UM_BACKEND_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_UM_BACKEND_H

#include <linux/tracepoint.h>

struct mm_struct;
struct uml_pt_regs;

DECLARE_EVENT_CLASS(um_backend_mm,
	TP_PROTO(struct mm_struct *mm),
	TP_ARGS(mm),
	TP_STRUCT__entry(
		__field(void *, mm)
	),
	TP_fast_assign(
		__entry->mm = mm;
	),
	TP_printk("mm=%p", __entry->mm)
);

DEFINE_EVENT(um_backend_mm, um_backend_mm_create,
	TP_PROTO(struct mm_struct *mm),
	TP_ARGS(mm)
);

DEFINE_EVENT(um_backend_mm, um_backend_mm_destroy,
	TP_PROTO(struct mm_struct *mm),
	TP_ARGS(mm)
);

TRACE_EVENT(um_backend_mm_region_added,
	TP_PROTO(struct mm_struct *mm, unsigned long va,
		 unsigned long len, int prot, int phys_fd, u64 offset),
	TP_ARGS(mm, va, len, prot, phys_fd, offset),
	TP_STRUCT__entry(
		__field(void *,        mm)
		__field(unsigned long, va)
		__field(unsigned long, len)
		__field(int,           prot)
		__field(int,           phys_fd)
		__field(u64,           offset)
	),
	TP_fast_assign(
		__entry->mm      = mm;
		__entry->va      = va;
		__entry->len     = len;
		__entry->prot    = prot;
		__entry->phys_fd = phys_fd;
		__entry->offset  = offset;
	),
	TP_printk("mm=%p va=%#lx len=%#lx prot=%#x fd=%d off=%#llx",
		  __entry->mm, __entry->va, __entry->len, __entry->prot,
		  __entry->phys_fd, __entry->offset)
);

TRACE_EVENT(um_backend_mm_region_removed,
	TP_PROTO(struct mm_struct *mm, unsigned long va, unsigned long len),
	TP_ARGS(mm, va, len),
	TP_STRUCT__entry(
		__field(void *,        mm)
		__field(unsigned long, va)
		__field(unsigned long, len)
	),
	TP_fast_assign(
		__entry->mm  = mm;
		__entry->va  = va;
		__entry->len = len;
	),
	TP_printk("mm=%p va=%#lx len=%#lx",
		  __entry->mm, __entry->va, __entry->len)
);

/*
 * v2 vcpu_enter / vcpu_exit live further down (Phase C.2). They
 * are kvm-v2 specific because the seccomp loop has no kernel-side
 * site to fire from.
 */

/*
 * kvm_v2_init — fired once when the v2 backend's init op runs after a
 * successful /dev/kvm probe + capability negotiation. The cap bitmap
 * encodes which optional KVM_CHECK_EXTENSION queries returned >0; layout
 * is private to arch/um/backend/kvm-v2/ (see KVM_V2_CAP_* in init.c).
 *
 * This event is the canonical Phase A.1 observability hook per memo 26
 * §A.1 + memo 27 Part B.9 ("build observability before you need it");
 * later phases extend the v2 tracepoint family rather than logging cap
 * results ad-hoc.
 */
TRACE_EVENT(um_backend_kvm_v2_init,
	TP_PROTO(int kvm_fd, int api_version, u64 caps),
	TP_ARGS(kvm_fd, api_version, caps),
	TP_STRUCT__entry(
		__field(int, kvm_fd)
		__field(int, api_version)
		__field(u64, caps)
	),
	TP_fast_assign(
		__entry->kvm_fd      = kvm_fd;
		__entry->api_version = api_version;
		__entry->caps        = caps;
	),
	TP_printk("kvm_fd=%d api=%d caps=%#llx",
		  __entry->kvm_fd, __entry->api_version, __entry->caps)
);

/*
 * kvm_v2_vcpu_create — fired when A.3's placeholder vCPU is built.
 * Phase C will emit a separate event per pool member; until then this
 * tracepoint marks the single placeholder's birth so a `trace-cmd
 * record -e um_backend:um_backend_kvm_v2_vcpu_create` confirms the v2
 * init path made it past KVM_CREATE_VCPU + kvm_run mmap. Per memo 27
 * Part B.9 every new v2 surface is observable from the day it lands.
 */
TRACE_EVENT(um_backend_kvm_v2_vcpu_create,
	TP_PROTO(int vcpu_fd, u32 kvm_run_size),
	TP_ARGS(vcpu_fd, kvm_run_size),
	TP_STRUCT__entry(
		__field(int, vcpu_fd)
		__field(u32, kvm_run_size)
	),
	TP_fast_assign(
		__entry->vcpu_fd      = vcpu_fd;
		__entry->kvm_run_size = kvm_run_size;
	),
	TP_printk("vcpu_fd=%d kvm_run_size=%u",
		  __entry->vcpu_fd, __entry->kvm_run_size)
);

/*
 * kvm_v2_memslot_add / _del — fired whenever B.1's allocator hands out
 * or frees a memslot id. Phase B.1 lands the events alongside the
 * allocator so B.2's KVM_SET_USER_MEMORY_REGION wiring is observable
 * from the day it lights up (memo 27 Part B.9). Until B.2, no caller
 * invokes kvm_v2_memslot_add() and the events are silent.
 */
TRACE_EVENT(um_backend_kvm_v2_memslot_add,
	TP_PROTO(u32 slot_id, u64 gpa, u64 host_va, u64 size, u32 flags),
	TP_ARGS(slot_id, gpa, host_va, size, flags),
	TP_STRUCT__entry(
		__field(u32, slot_id)
		__field(u64, gpa)
		__field(u64, host_va)
		__field(u64, size)
		__field(u32, flags)
	),
	TP_fast_assign(
		__entry->slot_id = slot_id;
		__entry->gpa     = gpa;
		__entry->host_va = host_va;
		__entry->size    = size;
		__entry->flags   = flags;
	),
	TP_printk("slot=%u gpa=%#llx host_va=%#llx size=%#llx flags=%#x",
		  __entry->slot_id, __entry->gpa, __entry->host_va,
		  __entry->size, __entry->flags)
);

TRACE_EVENT(um_backend_kvm_v2_memslot_del,
	TP_PROTO(u32 slot_id),
	TP_ARGS(slot_id),
	TP_STRUCT__entry(
		__field(u32, slot_id)
	),
	TP_fast_assign(
		__entry->slot_id = slot_id;
	),
	TP_printk("slot=%u", __entry->slot_id)
);

/*
 * kvm_v2_physmem_memslot_install — fired once when D.4b-pre's giant
 * physmem identity-offset memslot lands (memo 26 §D.4 D.4b-pre). One
 * slot at gpa=0 / hva=uml_physmem / size=physmem_size; mirrors v1's
 * kvm_ensure_memslot at kvm-v1-archive/lifecycle.c:613-648. The slot
 * makes KVM's TDP walk resolve __pa(pgd) and __pa(trampoline_kva) —
 * both physmem-offset GPAs in [0, physmem_size) — to host pages.
 *
 * Fires from either the eager vm_create attempt or (more often, since
 * uml_physmem isn't set at init_backend time) the
 * subsys_initcall lazy retry in syscall_trap.c. Idempotent at the
 * helper level so the event fires AT MOST once per VM lifetime.
 *
 * Until D.4b lands the PML4[448] PT chain and D.5 flips .vcpu_run, no
 * guest CR3 walk hits this slot — but the install is observable from
 * D.4b-pre. Per memo 27 §B.9: observability lands with the helper.
 */
TRACE_EVENT(um_backend_kvm_v2_physmem_memslot_install,
	TP_PROTO(int slot_id, unsigned long hva, u64 size),
	TP_ARGS(slot_id, hva, size),
	TP_STRUCT__entry(
		__field(int,           slot_id)
		__field(unsigned long, hva)
		__field(u64,           size)
	),
	TP_fast_assign(
		__entry->slot_id = slot_id;
		__entry->hva     = hva;
		__entry->size    = size;
	),
	TP_printk("slot=%d gpa=0 hva=%#lx size=%#llx",
		  __entry->slot_id, __entry->hva, __entry->size)
);

/*
 * kvm_v2_vcpu_enter / kvm_v2_vcpu_exit — bracket each KVM_RUN ioctl
 * issued by Phase C.2's dispatcher (kvm_v2_vcpu_run). enter fires
 * after the per-iteration vCPU state load (CR3 / fs.base / gs.base /
 * GPRs) and before the ioctl; exit fires immediately after the
 * ioctl returns, before the exit-reason switch and the GET_REGS
 * marshal-back. cpu is the host CPU id this vCPU is pinned to;
 * kvm_run is the mmap'd shared page (lets a tracer correlate
 * userspace and kernel views of the same vCPU). exit_reason matches
 * KVM_EXIT_* in <uapi/linux/kvm.h>.
 *
 * Per memo 27 Part B.9: observability lands with the helper, not
 * after it earns a caller.
 */
TRACE_EVENT(um_backend_kvm_v2_vcpu_enter,
	TP_PROTO(int cpu, void *kvm_run),
	TP_ARGS(cpu, kvm_run),
	TP_STRUCT__entry(
		__field(int,    cpu)
		__field(void *, kvm_run)
	),
	TP_fast_assign(
		__entry->cpu     = cpu;
		__entry->kvm_run = kvm_run;
	),
	TP_printk("cpu=%d kvm_run=%p", __entry->cpu, __entry->kvm_run)
);

TRACE_EVENT(um_backend_kvm_v2_vcpu_exit,
	TP_PROTO(int cpu, u32 exit_reason),
	TP_ARGS(cpu, exit_reason),
	TP_STRUCT__entry(
		__field(int, cpu)
		__field(u32, exit_reason)
	),
	TP_fast_assign(
		__entry->cpu         = cpu;
		__entry->exit_reason = exit_reason;
	),
	TP_printk("cpu=%d exit_reason=%u", __entry->cpu, __entry->exit_reason)
);

/*
 * kvm_v2_fpu_capture / kvm_v2_fpu_install — bracket the fork-time
 * KVM_GET_FPU snapshot (capture) and the per-KVM_RUN restore
 * (install). Phase C.4 wires both. `valid` reports whether the
 * helper produced (capture) / consumed (install) a parent snapshot;
 * a `valid=0` install means the dispatcher fell back to the
 * architectural reset values (fcw=0x037f, mxcsr=0x1f80) instead of
 * inheriting parent FPU state. Useful for diagnosing fork→FPU
 * inheritance regressions when Phase D's pointer flip activates the
 * dispatcher end-to-end.
 *
 * Per memo 27 Part B.9: observability lands with the helper, not
 * after it earns a caller (capture has a real caller via
 * arch_copy_thread today; install fires only when Phase D wires
 * .vcpu_run).
 */
TRACE_EVENT(um_backend_kvm_v2_fpu_capture,
	TP_PROTO(int cpu, int valid),
	TP_ARGS(cpu, valid),
	TP_STRUCT__entry(
		__field(int, cpu)
		__field(int, valid)
	),
	TP_fast_assign(
		__entry->cpu   = cpu;
		__entry->valid = valid;
	),
	TP_printk("cpu=%d valid=%d", __entry->cpu, __entry->valid)
);

TRACE_EVENT(um_backend_kvm_v2_fpu_install,
	TP_PROTO(int cpu, int was_valid),
	TP_ARGS(cpu, was_valid),
	TP_STRUCT__entry(
		__field(int, cpu)
		__field(int, was_valid)
	),
	TP_fast_assign(
		__entry->cpu       = cpu;
		__entry->was_valid = was_valid;
	),
	TP_printk("cpu=%d was_valid=%d", __entry->cpu, __entry->was_valid)
);

/*
 * kvm_v2_cpuid_install — fired when the lazy first-run CPUID install
 * (memo 26 §D.0a) lands the curated KVM_SET_CPUID2 against a pool
 * member. A.3's eager install ran at init_backend before the buddy
 * allocator was up and silently degraded to KVM-default CPUID; D.0a
 * defers the install to first KVM_RUN. Until D.5 flips .vcpu_run
 * this event has no caller and stays silent.
 *
 * vcpu_fd identifies the pool member; nent is the number of
 * kvm_cpuid_entry2 entries the curated mask was applied to (matches
 * what KVM returned from KVM_GET_SUPPORTED_CPUID).
 */
TRACE_EVENT(um_backend_kvm_v2_cpuid_install,
	TP_PROTO(int vcpu_fd, u32 nent),
	TP_ARGS(vcpu_fd, nent),
	TP_STRUCT__entry(
		__field(int, vcpu_fd)
		__field(u32, nent)
	),
	TP_fast_assign(
		__entry->vcpu_fd = vcpu_fd;
		__entry->nent    = nent;
	),
	TP_printk("vcpu_fd=%d nent=%u", __entry->vcpu_fd, __entry->nent)
);

/*
 * kvm_v2_msr_program — fired when D.4a's eager SYSCALL-MSR install
 * (memo 26 §D.4) lands MSR_LSTAR / MSR_STAR / MSR_SYSCALL_MASK +
 * readback against a pool member at vcpu_create_one. Fires once per
 * pool member at backend init; readback mismatch panics inside the
 * helper before this event would fire, so a successful trace event
 * means the three SYSCALL MSRs round-tripped exactly. Until D.5 flips
 * .vcpu_run away from seccomp the MSRs we programmed have no consumer
 * — but the event is observable from D.4a as a "trampoline armed"
 * signal. Per memo 27 §B.9: observability lands with the helper.
 */
TRACE_EVENT(um_backend_kvm_v2_msr_program,
	TP_PROTO(int vcpu_fd),
	TP_ARGS(vcpu_fd),
	TP_STRUCT__entry(
		__field(int, vcpu_fd)
	),
	TP_fast_assign(
		__entry->vcpu_fd = vcpu_fd;
	),
	TP_printk("vcpu_fd=%d", __entry->vcpu_fd)
);

/*
 * kvm_v2_sregs_install — fired when long-mode SREGS (CS/DS/SS/CR0/
 * CR4/EFER) are programmed at vcpu_create_one. D.5-fix landed this
 * after diagnosing that the sync-regs mmap is zero on first dispatch
 * and KVM_RUN returns -EINVAL because kvm_is_valid_sregs rejects
 * EFER.LMA=1 with CR0.PG=0. One event per pool member at create.
 */
TRACE_EVENT(um_backend_kvm_v2_sregs_install,
	TP_PROTO(int vcpu_fd),
	TP_ARGS(vcpu_fd),
	TP_STRUCT__entry(
		__field(int, vcpu_fd)
	),
	TP_fast_assign(
		__entry->vcpu_fd = vcpu_fd;
	),
	TP_printk("vcpu_fd=%d", __entry->vcpu_fd)
);

/*
 * kvm_v2_sigmask_install — fired when KVM_SET_SIGNAL_MASK
 * (sigfillset minus SIGALRM) is programmed at vcpu_create_one.
 * D.5-fix-2 landed this after the .vcpu_run flip + sregs install
 * (D.5-fix-1) surfaced a SIGALRM EINTR loop: UML's HZ=100 timer
 * fires before the guest can make progress, and without an installed
 * sigmask + post-KVM_RUN unblock_signals() the deferred-signal queue
 * stays armed and re-EINTRs on every dispatch. Mirrors v1's pattern
 * at kvm-v1-archive/thread.c:71-124. One event per pool member at
 * create. Per memo 27 §B.9: observability lands with the helper.
 */
TRACE_EVENT(um_backend_kvm_v2_sigmask_install,
	TP_PROTO(int vcpu_fd),
	TP_ARGS(vcpu_fd),
	TP_STRUCT__entry(
		__field(int, vcpu_fd)
	),
	TP_fast_assign(
		__entry->vcpu_fd = vcpu_fd;
	),
	TP_printk("vcpu_fd=%d", __entry->vcpu_fd)
);

/*
 * kvm_v2_trampoline_install — fired when the per-VM LSTAR trampoline
 * page (memo 26 §D.1) is allocated and the 5 SYSCALL-trap bytes
 * (out %al, $0xf4 / sysretq) are written. gpa is __pa(host page); gva
 * is KVM_V2_LSTAR_GVA (= KVM_V2_TRAMPOLINE_GVA + 0x40 = the address
 * D.4 will program into MSR_LSTAR). Fires at most once per VM
 * lifetime; idempotent re-invocations from the lazy retry path
 * short-circuit before reaching the trace site. Until D.4 / D.5 wire
 * MSR_LSTAR + ops.vcpu_run, no guest code reaches the trampoline,
 * but the bytes + storage are observable from this event.
 *
 * Per memo 27 §B.9: observability lands with the helper, not after
 * it earns a caller.
 */
TRACE_EVENT(um_backend_kvm_v2_trampoline_install,
	TP_PROTO(u64 gpa, u64 gva),
	TP_ARGS(gpa, gva),
	TP_STRUCT__entry(
		__field(u64, gpa)
		__field(u64, gva)
	),
	TP_fast_assign(
		__entry->gpa = gpa;
		__entry->gva = gva;
	),
	TP_printk("gpa=%#llx gva=%#llx", __entry->gpa, __entry->gva)
);

/*
 * kvm_v2_pml4_install — fired once per VM when D.4b's kernel-half PT
 * chain lands (memo 26 §D.4 D.4b). pud_gpa is __pa(pud_kva) — what gets
 * written to swapper_pg_dir[448] and init_mm.pgd[448] (OR'd with
 * _KERNPG_TABLE). gva is KVM_V2_LSTAR_GVA — the trampoline VA the chain
 * walks down to. The chain is per-VM, VM-lifetime; freed only at
 * vm_destroy. Until D.5 flips .vcpu_run no guest CR3 walk hits the
 * chain — but the install is observable from D.4b. Per memo 27 §B.9
 * observability lands with the helper.
 */
TRACE_EVENT(um_backend_kvm_v2_pml4_install,
	TP_PROTO(u64 pud_gpa, u64 gva),
	TP_ARGS(pud_gpa, gva),
	TP_STRUCT__entry(
		__field(u64, pud_gpa)
		__field(u64, gva)
	),
	TP_fast_assign(
		__entry->pud_gpa = pud_gpa;
		__entry->gva     = gva;
	),
	TP_printk("pud_gpa=%#llx gva=%#llx", __entry->pud_gpa, __entry->gva)
);

/*
 * kvm_v2_iotrap_syscall_enter / kvm_v2_iotrap_syscall_exit — bracket
 * the KVM_EXIT_IO → handle_syscall path that Phase D.2 wires (memo 26
 * §D.2). enter fires immediately before the handle_syscall() call,
 * after the syscall NR has been extracted from regs->gp[HOST_AX] and
 * the user RIP/RFLAGS have been propagated from RCX/R11; exit fires
 * immediately after handle_syscall returns, with the syscall return
 * value (still in regs->gp[HOST_AX] at this point — D.3 will marshal
 * it back to kvm_run->s.regs.regs.rax). port is run->io.port (today
 * UM_KVM_TRAP_SYSCALL = 0xf4); future Phase E.3 exception classes
 * will push other ports through the same enter/exit pair so the
 * tracer can distinguish them by port.
 *
 * Until D.5 flips ops.vcpu_run away from seccomp these events have no
 * caller and stay silent — D.2's helper is unreferenced from any
 * production .vcpu_run path. Per memo 27 §B.9 the observability lands
 * with the helper, not after it earns a caller.
 */
/*
 * kvm_v2_vcpu_eintr — fired when KVM_RUN returned -EINTR (SIGALRM or
 * other unmasked host signal interrupted the ioctl before the guest
 * produced a meaningful exit_reason). D.3 replaces the historical panic
 * on rc<0 with a fall-through that re-enables preempt and returns; the
 * UML scheduler will re-enter the dispatcher on the next slice. Phase
 * F's full signal handling adds restart-via-RAX-rewrite; D.3's minimum
 * is "don't panic." Reference: v1 archive's EINTR path at
 * kvm-v1-archive/thread.c:5121-5127.
 *
 * Until D.5 flips ops.vcpu_run away from seccomp this event has no
 * caller and stays silent — the v2 dispatcher is unreferenced from any
 * production .vcpu_run path. Per memo 27 §B.9 observability lands with
 * the helper, not after it earns a caller.
 */
TRACE_EVENT(um_backend_kvm_v2_vcpu_eintr,
	TP_PROTO(int cpu),
	TP_ARGS(cpu),
	TP_STRUCT__entry(
		__field(int, cpu)
	),
	TP_fast_assign(
		__entry->cpu = cpu;
	),
	TP_printk("cpu=%d", __entry->cpu)
);

TRACE_EVENT(um_backend_kvm_v2_iotrap_syscall_enter,
	TP_PROTO(u16 port, unsigned long nr),
	TP_ARGS(port, nr),
	TP_STRUCT__entry(
		__field(u16,           port)
		__field(unsigned long, nr)
	),
	TP_fast_assign(
		__entry->port = port;
		__entry->nr   = nr;
	),
	TP_printk("port=%#x nr=%lu", __entry->port, __entry->nr)
);

TRACE_EVENT(um_backend_kvm_v2_iotrap_syscall_exit,
	TP_PROTO(u16 port, unsigned long ret),
	TP_ARGS(port, ret),
	TP_STRUCT__entry(
		__field(u16,           port)
		__field(unsigned long, ret)
	),
	TP_fast_assign(
		__entry->port = port;
		__entry->ret  = ret;
	),
	TP_printk("port=%#x ret=%lu", __entry->port, __entry->ret)
);

/*
 * kvm_v2_exception_install — fired once per VM when E.1 lands the
 * IDT + handler stubs + GDT pages and patches PTE[1..3] of the
 * trampoline PT chain (memo 26 §E.1). The three GPAs are __pa() of
 * the buddy-allocated pages (each in physmem, so they sit inside
 * D.4b-pre's identity-offset memslot). Until E.3.5 flips .vcpu_run
 * away from seccomp the IDT/GDT pages have no consumer; per memo 27
 * §B.9 observability lands with the helper anyway.
 */
TRACE_EVENT(um_backend_kvm_v2_exception_install,
	TP_PROTO(u64 idt_gpa, u64 handlers_gpa, u64 gdt_gpa),
	TP_ARGS(idt_gpa, handlers_gpa, gdt_gpa),
	TP_STRUCT__entry(
		__field(u64, idt_gpa)
		__field(u64, handlers_gpa)
		__field(u64, gdt_gpa)
	),
	TP_fast_assign(
		__entry->idt_gpa      = idt_gpa;
		__entry->handlers_gpa = handlers_gpa;
		__entry->gdt_gpa      = gdt_gpa;
	),
	TP_printk("idt_gpa=%#llx handlers_gpa=%#llx gdt_gpa=%#llx",
		  __entry->idt_gpa, __entry->handlers_gpa, __entry->gdt_gpa)
);

/*
 * kvm_v2_descriptors_sregs_install — fired once per pool member when
 * E.1's exception_install iterates the pool and re-issues
 * KVM_SET_SREGS to install the IDT/GDT bases. Codex --search audit
 * finding #5: the eager kvm_v2_install_production_sregs at
 * vcpu_create_one ran before E.1's pages existed, so its sregs.idt /
 * sregs.gdt fields stayed at the KVM_GET_SREGS defaults. This event
 * is the post-E.1 patch — observable per pool member separately from
 * the original sregs_install event so a tracer can distinguish "vCPU
 * created" from "descriptor tables installed".
 */
TRACE_EVENT(um_backend_kvm_v2_descriptors_sregs_install,
	TP_PROTO(int vcpu_fd, u64 idt_base, u64 gdt_base),
	TP_ARGS(vcpu_fd, idt_base, gdt_base),
	TP_STRUCT__entry(
		__field(int, vcpu_fd)
		__field(u64, idt_base)
		__field(u64, gdt_base)
	),
	TP_fast_assign(
		__entry->vcpu_fd  = vcpu_fd;
		__entry->idt_base = idt_base;
		__entry->gdt_base = gdt_base;
	),
	TP_printk("vcpu_fd=%d idt_base=%#llx gdt_base=%#llx",
		  __entry->vcpu_fd, __entry->idt_base, __entry->gdt_base)
);

/*
 * kvm_v2_per_vcpu_ist_tss_install — fired once per pool member when
 * E.2 lands the per-vCPU IST stack + TSS pages. The IST stack top is
 * the GVA the TSS body's IST1 field points at (CPU pushes iretq
 * frame here on exception delivery); the TSS GVA is what SREGS.tr.base
 * cached on this vCPU points at.
 *
 * Per memo 26 §E.2 + the codex audit independent finding (per-vCPU
 * TSS is REQUIRED — multiple vCPUs cannot share a TSS without IST1
 * collision risk). Until E.3.5 flips .vcpu_run away from seccomp the
 * IST stack + TSS pages have no consumer; per memo 27 §B.9
 * observability lands with the helper anyway.
 */
TRACE_EVENT(um_backend_kvm_v2_per_vcpu_ist_tss_install,
	TP_PROTO(int vcpu_fd, u64 ist_stack_top_gva, u64 tss_gva),
	TP_ARGS(vcpu_fd, ist_stack_top_gva, tss_gva),
	TP_STRUCT__entry(
		__field(int, vcpu_fd)
		__field(u64, ist_stack_top_gva)
		__field(u64, tss_gva)
	),
	TP_fast_assign(
		__entry->vcpu_fd           = vcpu_fd;
		__entry->ist_stack_top_gva = ist_stack_top_gva;
		__entry->tss_gva           = tss_gva;
	),
	TP_printk("vcpu_fd=%d ist_stack_top_gva=%#llx tss_gva=%#llx",
		  __entry->vcpu_fd, __entry->ist_stack_top_gva,
		  __entry->tss_gva)
);

/*
 * Phase E.3 (memo 26 §E.3): per-class exception dispatch tracepoints.
 *
 * Fired once per KVM_EXIT_IO that lands on an exception-class port.
 * Until E.3.5 flips ops.vcpu_run away from seccomp these events have
 * no caller — kvm_v2_vcpu_run is unreferenced from production paths.
 * Per memo 27 §B.9 observability lands with the helper, not after it
 * earns a caller.
 *
 * Each event captures the user-state-at-fault from the IST iretq
 * frame; #PF and #GP additionally carry the CPU-pushed error_code
 * (and #PF carries CR2 from sync-regs sregs.cr2). The panic event
 * carries the run->io.port so a tracer can correlate the unhandled
 * vector at panic time.
 */
TRACE_EVENT(um_backend_kvm_v2_iotrap_pf,
	TP_PROTO(u64 cr2, u64 error_code, u64 user_rip),
	TP_ARGS(cr2, error_code, user_rip),
	TP_STRUCT__entry(
		__field(u64, cr2)
		__field(u64, error_code)
		__field(u64, user_rip)
	),
	TP_fast_assign(
		__entry->cr2        = cr2;
		__entry->error_code = error_code;
		__entry->user_rip   = user_rip;
	),
	TP_printk("cr2=%#llx ec=%#llx user_rip=%#llx",
		  __entry->cr2, __entry->error_code, __entry->user_rip)
);

TRACE_EVENT(um_backend_kvm_v2_iotrap_gp,
	TP_PROTO(u64 error_code, u64 user_rip),
	TP_ARGS(error_code, user_rip),
	TP_STRUCT__entry(
		__field(u64, error_code)
		__field(u64, user_rip)
	),
	TP_fast_assign(
		__entry->error_code = error_code;
		__entry->user_rip   = user_rip;
	),
	TP_printk("ec=%#llx user_rip=%#llx",
		  __entry->error_code, __entry->user_rip)
);

DECLARE_EVENT_CLASS(um_backend_kvm_v2_iotrap_no_ec,
	TP_PROTO(u64 user_rip),
	TP_ARGS(user_rip),
	TP_STRUCT__entry(
		__field(u64, user_rip)
	),
	TP_fast_assign(
		__entry->user_rip = user_rip;
	),
	TP_printk("user_rip=%#llx", __entry->user_rip)
);

DEFINE_EVENT(um_backend_kvm_v2_iotrap_no_ec, um_backend_kvm_v2_iotrap_ud,
	TP_PROTO(u64 user_rip),
	TP_ARGS(user_rip)
);

DEFINE_EVENT(um_backend_kvm_v2_iotrap_no_ec, um_backend_kvm_v2_iotrap_de,
	TP_PROTO(u64 user_rip),
	TP_ARGS(user_rip)
);

DEFINE_EVENT(um_backend_kvm_v2_iotrap_no_ec, um_backend_kvm_v2_iotrap_of,
	TP_PROTO(u64 user_rip),
	TP_ARGS(user_rip)
);

TRACE_EVENT(um_backend_kvm_v2_iotrap_panic,
	TP_PROTO(u16 port, u64 user_rip),
	TP_ARGS(port, user_rip),
	TP_STRUCT__entry(
		__field(u16, port)
		__field(u64, user_rip)
	),
	TP_fast_assign(
		__entry->port     = port;
		__entry->user_rip = user_rip;
	),
	TP_printk("port=%#x user_rip=%#llx",
		  __entry->port, __entry->user_rip)
);

#endif /* _TRACE_UM_BACKEND_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH asm/trace
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE um_backend
#include <trace/define_trace.h>
