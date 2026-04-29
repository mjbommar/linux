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

#endif /* _TRACE_UM_BACKEND_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH asm/trace
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE um_backend
#include <trace/define_trace.h>
