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
 * The vcpu_run_enter / vcpu_run_exit events are deferred until v2
 * lands a kernel-side dispatcher around the trap loop. Today's
 * userspace() loop in arch/um/os-Linux/skas/process.c is a USER
 * TU (compiled with USER_CFLAGS, no access to kernel-side
 * tracepoint macros); v2's per-mm worker process wrapper will
 * give us a kernel-side site to emit them. Memo 25 R7's longer
 * wishlist (mm_region_protected, syscall_dispatch, signal_received)
 * lands incrementally as each surface gets a kernel-side hook.
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
 * vcpu_run_enter / vcpu_run_exit deferred — see header comment.
 * They will go here once v2's kernel-side dispatcher exists.
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

#endif /* _TRACE_UM_BACKEND_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH asm/trace
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE um_backend
#include <trace/define_trace.h>
