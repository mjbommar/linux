/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML backend tracepoints.
 *
 * Per-event definitions for the um_backend ftrace subsystem.
 * Hooks fire at the boundary between UML's mm-arbiter / trap loop
 * and the active backend.
 *
 * Enable with trace-cmd record -e um_backend:*.
 *
 * Event list:
 *
 *  - um_backend_mm_create(mm)         - per-mm lifecycle start
 *  - um_backend_mm_destroy(mm)        - per-mm lifecycle end
 *  - um_backend_mm_region_added(...)  - backend learned of a new
 *                                       VA range mapping
 *  - um_backend_mm_region_removed     - backend learned of an
 *                                       unmap
 *
 * The seccomp run loop has no kernel-side vcpu_run_enter / exit hook:
 * it is built with libc support and cannot use kernel tracepoint
 * macros. kvm_v2_vcpu_run() runs in kernel context, so
 * um_backend_kvm_v2_vcpu_enter / _exit cover the v2 dispatcher.
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
 * v2 vcpu_enter / vcpu_exit are kvm-v2 specific because the seccomp
 * loop has no kernel-side site to fire from.
 */

/*
 * kvm_v2_init - fired once when the v2 backend's init op runs after a
 * successful /dev/kvm probe + capability negotiation. The cap bitmap
 * encodes which optional KVM_CHECK_EXTENSION queries returned >0; layout
 * is private to arch/um/backend/kvm-v2/ (see KVM_V2_CAP_* in init.c).
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
 * kvm_v2_vcpu_create - fired when a vCPU is built and its kvm_run
 * shared page is mapped.
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
 * kvm_v2_memslot_add / _del - fired whenever the allocator hands out
 * or frees a memslot id.
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
 * kvm_v2_physmem_memslot_install - fired once when the physmem
 * identity-offset memslot is installed. The slot maps
 * gpa=0 / hva=uml_physmem / size=physmem_size and lets KVM's TDP walk
 * resolve __pa(pgd) and __pa(trampoline_kva), both physmem-offset GPAs
 * in [0, physmem_size), to host pages.
 *
 * The helper is idempotent and the event fires at most once per VM
 * lifetime.
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
 * kvm_v2_vcpu_enter / kvm_v2_vcpu_exit - bracket each KVM_RUN ioctl
 * issued by kvm_v2_vcpu_run(). enter fires
 * after the per-iteration vCPU state load (CR3 / fs.base / gs.base /
 * GPRs) and before the ioctl; exit fires immediately after the
 * ioctl returns, before the exit-reason switch and the GET_REGS
 * marshal-back. cpu is the host CPU id this vCPU is pinned to;
 * kvm_run is the mmap'd shared page (lets a tracer correlate
 * userspace and kernel views of the same vCPU). exit_reason matches
 * KVM_EXIT_* in <uapi/linux/kvm.h>.
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
 * kvm_v2_fpu_capture / kvm_v2_fpu_install - bracket the fork-time
 * KVM_GET_FPU snapshot (capture) and the per-KVM_RUN restore
 * (install). valid reports whether the
 * helper produced (capture) / consumed (install) a parent snapshot;
 * a valid=0 install means the dispatcher fell back to the
 * architectural reset values (fcw=0x037f, mxcsr=0x1f80) instead of
 * inheriting parent FPU state. Useful for diagnosing fork-to-FPU
 * inheritance regressions.
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
 * kvm_v2_cpuid_install - fired when lazy CPUID setup applies the curated
 * KVM_SET_CPUID2 against a pool member.
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
 * kvm_v2_msr_program - fired when SYSCALL MSR install lands
 * MSR_LSTAR / MSR_STAR / MSR_SYSCALL_MASK + readback against a
 * pool member at vcpu_create_one. Fires once per
 * pool member at backend init; readback mismatch panics inside the
 * helper before this event would fire, so a successful trace event
 * means the three SYSCALL MSRs round-tripped exactly.
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
 * kvm_v2_sregs_install - fired when long-mode SREGS (CS/DS/SS/CR0/
 * CR4/EFER) are programmed at vcpu_create_one. One event per pool
 * member at create.
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
 * kvm_v2_sigmask_install - fired when KVM_SET_SIGNAL_MASK
 * (sigfillset minus SIGALRM) is programmed at vcpu_create_one.
 * UML's HZ=100 timer can otherwise interrupt KVM_RUN before the
 * guest can make progress. One event fires per pool member at create.
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
 * kvm_v2_trampoline_install - fired when the per-VM LSTAR trampoline
 * page is allocated and the 5 SYSCALL-trap bytes
 * (out %al, $0xf4 / sysretq) are written. gpa is __pa(host page); gva
 * is KVM_V2_LSTAR_GVA (= KVM_V2_TRAMPOLINE_GVA + 0x40 = the address
 * programmed into MSR_LSTAR). Fires at most once per VM
 * lifetime; idempotent re-invocations from the lazy retry path
 * short-circuit before reaching the trace site.
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
 * kvm_v2_pml4_install - fired once per VM when the kernel-half PT
 * chain is installed. pud_gpa is __pa(pud_kva), which is written to
 * swapper_pg_dir[448] and init_mm.pgd[448] (OR'd with _KERNPG_TABLE).
 * gva is KVM_V2_LSTAR_GVA, the trampoline VA the chain
 * walks down to. The chain is per-VM, VM-lifetime; freed only at
 * vm_destroy.
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
 * kvm_v2_iotrap_syscall_enter / kvm_v2_iotrap_syscall_exit - bracket
 * the KVM_EXIT_IO to handle_syscall path. enter fires immediately
 * before handle_syscall(), after the syscall NR has been extracted
 * from regs->gp[HOST_AX] and the user RIP/RFLAGS have been propagated
 * from RCX/R11. exit fires immediately after handle_syscall() returns,
 * with the syscall return value still in regs->gp[HOST_AX]. port is
 * run->io.port.
 */
/*
 * kvm_v2_vcpu_eintr - fired when KVM_RUN returned -EINTR (SIGALRM or
 * other unmasked host signal interrupted the ioctl before the guest
 * produced a meaningful exit_reason). The dispatcher re-enables
 * preemption and returns so the UML scheduler can re-enter it on the
 * next slice.
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
 * kvm_v2_exception_install - fired once per VM when the
 * IDT + handler stubs + GDT pages and patches PTE[1..3] of the
 * trampoline PT chain are installed. The three GPAs are __pa() of
 * the buddy-allocated pages (each in physmem, so they sit inside
 * the identity-offset memslot).
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
 * kvm_v2_descriptors_sregs_install - fired once per pool member when
 * exception_install iterates the pool and re-issues
 * KVM_SET_SREGS to install the IDT/GDT bases. This event is observable
 * per pool member separately from the original sregs_install event so
 * a tracer can distinguish "vCPU created" from "descriptor tables
 * installed".
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
 * kvm_v2_per_vcpu_ist_tss_install - fired once per pool member when
 * the per-vCPU IST stack + TSS pages are installed. The IST stack top is
 * the GVA the TSS body's IST1 field points at (CPU pushes iretq
 * frame here on exception delivery); the TSS GVA is what SREGS.tr.base
 * cached on this vCPU points at.
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
 * Per-class exception dispatch tracepoints.
 *
 * Fired once per KVM_EXIT_IO that lands on an exception-class port.
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
