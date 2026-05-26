// SPDX-License-Identifier: GPL-2.0
/*
 * KVM v2 state-snapshot trace ring buffer.
 *
 * The state-audit identified ~149 state items spanning per-task,
 * per-vCPU, per-mm, and live KVM mmap state (Documentation/virt/uml/
 * redesign/02-workstreams/D-kvm-backend/state-audit/01-state-inventory
 * .md). Spot printk + ratelimit can't carry the volume or the
 * cross-cutting fields needed to cross-correlate state evolution
 * across (pid, op) tuples between PASS and FAIL boots — which is
 * what we need to root-cause cross-task contamination under SMP T>=N
 * stress.
 *
 * Implementation:
 *
 *   Ring        Per-CPU vmalloc; default 2 MB / CPU
 *               (~5400 entries × ~384 B). Resized via debugfs
 *               'ring_bytes' (read at next enable=1 transition).
 *
 *   Capture     One entry point — kvm_v2_state_trace_capture(). Reads
 *               current task arch state, vCPU state, and the live
 *               kvm_run mmap. Writes one entry to the local CPU's
 *               ring with local IRQs off. Per-CPU sequence number;
 *               wrap is allowed (newest N entries always retained).
 *
 *   Hook        KVMV2_TRACE() in state_trace.h. Compiles away when
 *               CONFIG=n; gated by a static_branch when CONFIG=y so
 *               disabled state is one 5-byte NOP.
 *
 *   Dump        Merge all per-CPU rings, sort ascending by ts, emit
 *               one pr_emerg block per entry. Triggers:
 *                 - debugfs:  echo 1 > .../dump
 *                 - userland: write 1 to /sys/kernel/debug/
 *                             um_kvm_v2_trace/dump from inside the
 *                             UML guest (mt-mini does this on FAIL).
 *                 - panic:    callable from panic context (NMI watchdog
 *                             touched between batches).
 *
 * The fields captured come straight from state-audit Layer 1 buckets
 * marked PRIMARY in the matrix (Layer 3). FPU is captured as a 4-byte
 * FNV-1a hash of arch.kvm_v2.iotrap_fpu so cross-task FPU leakage is
 * detectable without dumping 512 bytes.
 */

#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/mm.h>
#include <linux/nmi.h>		/* touch_nmi_watchdog */
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include <asm/page.h>
#include <sysdep/ptrace.h>

#include "kvm_v2_backend.h"
#include "state_trace.h"

DEFINE_STATIC_KEY_FALSE(kvm_v2_state_trace_key);

/*
 * Anomaly auto-freeze: when an anomaly fires (e.g., mmap returning 0)
 * we set this to 1 so subsequent capture()s return early. The
 * static_branch can't be disabled from inside capture() because that
 * path can sleep; this gate is the cheap atomic alternative.
 *
 * Reset to 0 by debugfs 'clear'.
 */
static atomic_t trace_frozen = ATOMIC_INIT(0);

/*
 * Round 2 (Django investigation, 2026-05-17): the default 2 MB / CPU
 * ring captures only ~5400 entries, but Python startup runs orders of
 * magnitude more syscalls. Bumped to 32 MB / CPU so the
 * fatal-signal / fatal-segv auto-freeze (capture()'s POST-segv_handler
 * and HANDLE_SYSCALL_PRE hooks) lands inside the still-live ring with
 * room for the full pre-corruption window. CI configs can override via
 * the new `kvm_v2_trace_bytes=` kernel parameter or the existing
 * debugfs `ring_bytes` file (read at next enable=1 transition).
 *
 * vmalloc accounting: with KVMV2_TRACE_RING_BYTES_DEFAULT = 32 MB and
 * NR_CPUS_DEFAULT = 1 (UP), this consumes 32 MB of vmalloc on a 1 GB
 * UML mem config — well under the vmalloc area's headroom on x86_64.
 * SMP CI configs that need to scale to NR_CPUS > 1 should set
 * kvm_v2_trace_bytes= explicitly to bound total vmalloc cost.
 */
#define KVMV2_TRACE_RING_BYTES_DEFAULT  (32UL * 1024 * 1024)
#define KVMV2_TRACE_DUMP_BATCH          32

struct kvm_v2_trace_ring {
	struct kvm_v2_state_snap *entries;
	unsigned int  capacity;       /* # of entries */
	atomic_t      next;           /* sequence; entries[(next-1) % cap] */
};

static DEFINE_PER_CPU(struct kvm_v2_trace_ring, trace_rings);

static unsigned long ring_bytes = KVMV2_TRACE_RING_BYTES_DEFAULT;
static bool          rings_allocated;
static DEFINE_MUTEX(trace_alloc_mutex);

static struct dentry *trace_dir;

/*
 * FNV-1a 32-bit. Collision-resistant enough to detect "FPU contents
 * changed across an op boundary" (we only need ~uniqueness, not
 * cryptographic strength).
 */
static u32 fnv1a32(const void *data, size_t len)
{
	const u8 *p = data;
	u32 h = 2166136261U;

	while (len--) {
		h ^= *p++;
		h *= 16777619U;
	}
	return h;
}

/*
 * Allocate per-CPU rings via vmalloc (size > kmalloc max). Idempotent
 * — second call when already allocated returns 0.
 */
static int trace_rings_alloc_locked(void)
{
	int cpu;
	unsigned int cap;

	if (rings_allocated)
		return 0;

	cap = ring_bytes / sizeof(struct kvm_v2_state_snap);
	if (cap < 64)
		cap = 64;

	for_each_possible_cpu(cpu) {
		struct kvm_v2_trace_ring *r = &per_cpu(trace_rings, cpu);

		r->entries = vzalloc_node(cap * sizeof(*r->entries),
					  cpu_to_node(cpu));
		if (!r->entries)
			goto fail;
		r->capacity = cap;
		atomic_set(&r->next, 0);
	}
	WRITE_ONCE(rings_allocated, true);
	return 0;

fail:
	for_each_possible_cpu(cpu) {
		struct kvm_v2_trace_ring *r = &per_cpu(trace_rings, cpu);

		vfree(r->entries);
		r->entries = NULL;
		r->capacity = 0;
	}
	return -ENOMEM;
}

void kvm_v2_state_trace_clear(void)
{
	int cpu;

	atomic_set(&trace_frozen, 0);

	if (!READ_ONCE(rings_allocated))
		return;

	for_each_possible_cpu(cpu) {
		struct kvm_v2_trace_ring *r = &per_cpu(trace_rings, cpu);

		atomic_set(&r->next, 0);
		if (r->entries)
			memset(r->entries, 0,
			       r->capacity * sizeof(*r->entries));
	}
}

/*
 * Capture one snapshot. Writer-side fast path; called from inside
 * preempt-disabled regions of vcpu_run, from handle_io_pf, etc.
 *
 * Local IRQ save ensures we can't be re-entered on the same CPU by
 * a signal/IRQ handler that also calls KVMV2_TRACE.
 */
void kvm_v2_state_trace_capture(enum kvm_v2_trace_op op,
				struct uml_pt_regs *regs,
				struct kvm_run *run,
				struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_trace_ring *r;
	struct kvm_v2_state_snap *e;
	struct task_struct *t = current;
	unsigned int slot, seq_after;
	unsigned long flags;

	if (!READ_ONCE(rings_allocated))
		return;
	if (atomic_read(&trace_frozen))
		return;

	local_irq_save(flags);
	r = this_cpu_ptr(&trace_rings);
	if (!r->entries) {
		local_irq_restore(flags);
		return;
	}

	seq_after = (unsigned int)atomic_inc_return(&r->next);
	slot = (seq_after - 1) % r->capacity;
	e = &r->entries[slot];

	memset(e, 0, sizeof(*e));
	e->ts          = sched_clock();
	e->seq         = seq_after - 1;
	e->pid         = t ? t->pid : 0;
	e->cpu         = (u8)smp_processor_id();
	e->op          = (u8)op;

	if (run) {
		e->exit_reason = (u8)run->exit_reason;
		e->io_port     = (u16)run->io.port;
		e->rax = run->s.regs.regs.rax;
		e->rbx = run->s.regs.regs.rbx;
		e->rcx = run->s.regs.regs.rcx;
		e->rdx = run->s.regs.regs.rdx;
		e->rsi = run->s.regs.regs.rsi;
		e->rdi = run->s.regs.regs.rdi;
		e->rbp = run->s.regs.regs.rbp;
		e->rsp = run->s.regs.regs.rsp;
		e->r8  = run->s.regs.regs.r8;
		e->r9  = run->s.regs.regs.r9;
		e->r10 = run->s.regs.regs.r10;
		e->r11 = run->s.regs.regs.r11;
		e->r12 = run->s.regs.regs.r12;
		e->r13 = run->s.regs.regs.r13;
		e->r14 = run->s.regs.regs.r14;
		e->r15 = run->s.regs.regs.r15;
		e->rip    = run->s.regs.regs.rip;
		e->rflags = run->s.regs.regs.rflags;
		e->cr0 = run->s.regs.sregs.cr0;
		e->cr2 = run->s.regs.sregs.cr2;
		e->cr3 = run->s.regs.sregs.cr3;
		e->cr4 = run->s.regs.sregs.cr4;
		e->fs_base = run->s.regs.sregs.fs.base;
		e->gs_base = run->s.regs.sregs.gs.base;
	}

	if (t) {
		struct arch_thread *a = &t->thread.arch;

		e->task_mm_ptr            = (u64)(uintptr_t)t->mm;
		e->task_active_mm_ptr     = (u64)(uintptr_t)t->active_mm;
		e->task_saved_cr2         = a->kvm_v2.saved_cr2_at_eintr;
		e->task_saved_cr2_valid   = a->kvm_v2.saved_cr2_valid;
		e->task_ist_pending       = a->kvm_v2.ist_pending;
		e->task_iotrap_fpu_valid  = a->kvm_v2.iotrap_fpu_valid;
		e->task_fpu_valid         = a->kvm_v2.fpu_valid;
		e->current_regs_ptr       =
			(u64)(uintptr_t)&t->thread.regs.regs;
		if (a->kvm_v2.iotrap_fpu_valid)
			e->task_fpu_hash =
				fnv1a32(&a->kvm_v2.iotrap_fpu,
					sizeof(a->kvm_v2.iotrap_fpu));
		memcpy(e->task_ist_frame, a->kvm_v2.ist_frame,
		       sizeof(e->task_ist_frame));
		if (t->mm)
			e->mm_tlb_gen =
				atomic64_read(&t->mm->context.tlb_gen);
	}

	if (regs) {
		e->task_host_ax       = regs->gp[HOST_AX];
		e->task_host_orig_ax  = regs->gp[HOST_ORIG_AX];
		e->task_host_ip       = regs->gp[HOST_IP];
		e->task_host_sp       = regs->gp[HOST_SP];
		e->regs_ptr           = (u64)(uintptr_t)regs;
		e->regs_current_match =
			(t && regs == &t->thread.regs.regs) ? 1 : 0;
	}

	if (vcpu) {
		e->vcpu_last_seen_tlb_gen =
			atomic64_read(&vcpu->last_seen_tlb_gen);
		e->vcpu_current_mm =
			(u64)(uintptr_t)READ_ONCE(vcpu->current_mm);
		e->vcpu_kick_pending =
			(u32)atomic_read(&vcpu->kick_pending);
		if (vcpu->ist_stack_kva) {
			u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;

			e->ist_live[0] = *(u64 *)(top - 48);
			e->ist_live[1] = *(u64 *)(top - 40 +  0);
			e->ist_live[2] = *(u64 *)(top - 40 +  8);
			e->ist_live[3] = *(u64 *)(top - 40 + 16);
			e->ist_live[4] = *(u64 *)(top - 40 + 24);
			e->ist_live[5] = *(u64 *)(top - 40 + 32);
		}
	}

	local_irq_restore(flags);

	/*
	 * Anomaly auto-freeze: if a HANDLE_SYSCALL_POST records
	 * orig_ax=__NR_mmap and ax=0, mmap returned NULL — that's the
	 * mt-mini MMAP_NULL bug. Freeze the ring right now so
	 * subsequent dispatches don't overwrite the failing context.
	 * mt-mini's MMAP_NULL printf will trigger debugfs dump shortly
	 * and we get the moment-of-failure history.
	 *
	 * One-shot via cmpxchg on trace_frozen — first hit wins.
	 */
	if (op == KVMV2_OP_HANDLE_SYSCALL_POST &&
	    regs && regs->gp[HOST_ORIG_AX] == 9 /* __NR_mmap */ &&
	    regs->gp[HOST_AX] == 0) {
		if (atomic_cmpxchg(&trace_frozen, 0, 1) == 0)
			pr_emerg("KVMV2T_ANOMALY mmap-returns-zero pid=%u cpu=%u "
				 "ts=%llu seq=%u — froze trace ring\n",
				 e->pid, e->cpu, e->ts, e->seq);
	}

	/*
	 * SMP-T21 (2026-05-02): freeze the ring at TRACE_TRIGGER (= Bug B
	 * detection — handle_io_pf saw user_rip in HANDLERS range). The
	 * BUG_B printk + state_trace_dump in handle_io_pf will print the
	 * ring next; freezing here prevents subsequent dispatches from
	 * overwriting the moment-of-failure context. One-shot via cmpxchg.
	 */
	if (op == KVMV2_OP_TRACE_TRIGGER) {
		if (atomic_cmpxchg(&trace_frozen, 0, 1) == 0)
			pr_emerg("KVMV2T_ANOMALY trace-trigger pid=%u cpu=%u "
				 "ts=%llu seq=%u — froze trace ring (Bug B)\n",
				 e->pid, e->cpu, e->ts, e->seq);
	}

	/*
	 * Django-bug investigation 2026-05-17 (memo §44 Investigation Round
	 * 2026-05-17, hypothesis A.2): auto-freeze the ring when a guest
	 * userspace task issues a fatal-signal syscall. The Django flake
	 * surfaces as a CPython "Executing a cache" abort whose moment of
	 * corruption is many thousands of syscalls earlier than the
	 * SERVER_FAIL marker that triggers the debugfs dump; with a 2 MB
	 * default ring the corruption point is gone by dump time. Catching
	 * the SIGABRT delivery — which fires immediately AFTER Py_FatalError
	 * writes its message but BEFORE the abort handler runs — freezes
	 * the ring while the corruption context is still fresh.
	 *
	 * Watched syscalls: tgkill (NR 234), kill (NR 62),
	 * rt_sigqueueinfo (NR 129), tkill (NR 200). All take signo in a
	 * predictable arg slot:
	 *   tgkill(tgid, tid, sig)              → rdx = sig (HOST_DX)
	 *   tkill(tid, sig)                     → rsi = sig (HOST_SI)
	 *   kill(pid, sig)                      → rsi = sig (HOST_SI)
	 *   rt_sigqueueinfo(tgid, sig, *info)   → rsi = sig (HOST_SI)
	 *
	 * Watched signals: SIGABRT (6) is the canonical fatal-abort signal
	 * that CPython, glibc assert(), and most language runtimes use.
	 * Including SIGSEGV (11) catches BSD-style abort-via-segfault and
	 * the rare cases where the runtime mishandles a SIGSEGV and tries
	 * to re-signal itself.
	 *
	 * Hook fires only for in-guest user-mode syscalls (HANDLE_SYSCALL_PRE
	 * runs in handle_io_trap → before handle_syscall executes), so the
	 * captured trace ends one entry before the would-be syscall handler.
	 * One-shot via cmpxchg.
	 */
	if (op == KVMV2_OP_HANDLE_SYSCALL_PRE && regs) {
		unsigned long nr  = regs->gp[HOST_ORIG_AX];
		unsigned long sig = 0;

		switch (nr) {
		case 234: /* __NR_tgkill */
			sig = regs->gp[HOST_DX];
			break;
		case 62:  /* __NR_kill  */
		case 200: /* __NR_tkill */
		case 129: /* __NR_rt_sigqueueinfo */
			sig = regs->gp[HOST_SI];
			break;
		default:
			break;
		}
		if (sig == 6 /* SIGABRT */ || sig == 11 /* SIGSEGV */) {
			if (atomic_cmpxchg(&trace_frozen, 0, 1) == 0)
				pr_emerg("KVMV2T_ANOMALY fatal-signal pid=%u cpu=%u "
					 "ts=%llu seq=%u nr=%lu sig=%lu — froze trace ring\n",
					 e->pid, e->cpu, e->ts, e->seq, nr, sig);
		}
	}
}

static const char *op_name(u8 op)
{
	static const char * const names[KVMV2_OP_MAX] = {
		[KVMV2_OP_VCPU_RUN_ENTRY]       = "VCPU_RUN_ENTRY",
		[KVMV2_OP_POST_TLB_SYNC]        = "POST_TLB_SYNC",
		[KVMV2_OP_POST_LOAD_SREGS]      = "POST_LOAD_SREGS",
		[KVMV2_OP_POST_FPU_INSTALL]     = "POST_FPU_INSTALL",
		[KVMV2_OP_POST_IST_RESTORE]     = "POST_IST_RESTORE",
		[KVMV2_OP_PRE_KVM_RUN]          = "PRE_KVM_RUN",
		[KVMV2_OP_POST_KVM_RUN]         = "POST_KVM_RUN",
		[KVMV2_OP_EINTR_PATH]           = "EINTR_PATH",
		[KVMV2_OP_EINTR_INLINE_PF]      = "EINTR_INLINE_PF",
		[KVMV2_OP_EINTR_RAW_SNAPSHOT]   = "EINTR_RAW_SNAPSHOT",
		[KVMV2_OP_HANDLE_SYSCALL_PRE]   = "HANDLE_SYSCALL_PRE",
		[KVMV2_OP_HANDLE_SYSCALL_POST]  = "HANDLE_SYSCALL_POST",
		[KVMV2_OP_HANDLE_IO_PF_PRE]     = "HANDLE_IO_PF_PRE",
		[KVMV2_OP_HANDLE_IO_PF_POST]    = "HANDLE_IO_PF_POST",
		[KVMV2_OP_IST_FRAME_WRITE_PRE]  = "IST_FRAME_WRITE_PRE",
		[KVMV2_OP_IST_FRAME_WRITE_POST] = "IST_FRAME_WRITE_POST",
		[KVMV2_OP_VCPU_RUN_EXIT]        = "VCPU_RUN_EXIT",
		[KVMV2_OP_TRACE_TRIGGER]        = "TRACE_TRIGGER",
		[KVMV2_OP_EINTR_INLINE_LSTAR]   = "EINTR_INLINE_LSTAR",
	};

	if (op < KVMV2_OP_MAX && names[op])
		return names[op];
	return "UNKNOWN";
}

static int snap_cmp(const void *a, const void *b)
{
	const struct kvm_v2_state_snap *p = a, *q = b;

	/*
	 * sched_clock() in UML has HZ-tick (10ms) resolution, so many
	 * snapshots share a ts. Within a tick, fall back to (cpu, seq)
	 * so each per-CPU stream is at least monotonic.
	 */
	if (p->ts < q->ts)
		return -1;
	if (p->ts > q->ts)
		return  1;
	if (p->cpu < q->cpu)
		return -1;
	if (p->cpu > q->cpu)
		return  1;
	if (p->seq < q->seq)
		return -1;
	if (p->seq > q->seq)
		return  1;
	return 0;
}

/*
 * Print one record across 8 tagged lines, each carrying cpu+seq so
 * the parser can reassemble entries even when printk interleaves
 * lines from different CPUs. Section letters: H=header, R=GPR,
 * S=sregs, T=task, F=tflags+tist, M=mmvcpu, I=ist_live.
 *
 * Field names match the struct member names so downstream parsing
 * scripts ride the symbol table.
 */
static void dump_one(const struct kvm_v2_state_snap *e)
{
	pr_emerg("KVMV2T-H cpu=%u seq=%u ts=%llu pid=%u op=%s exit=%u port=%#x\n",
		 e->cpu, e->seq, e->ts, e->pid, op_name(e->op),
		 e->exit_reason, e->io_port);
	pr_emerg("KVMV2T-R cpu=%u seq=%u rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx rsp=%llx r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx rip=%llx rfl=%llx\n",
		 e->cpu, e->seq,
		 e->rax, e->rbx, e->rcx, e->rdx, e->rsi, e->rdi,
		 e->rbp, e->rsp, e->r8, e->r9, e->r10, e->r11,
		 e->r12, e->r13, e->r14, e->r15, e->rip, e->rflags);
	pr_emerg("KVMV2T-S cpu=%u seq=%u cr0=%llx cr2=%llx cr3=%llx cr4=%llx fsb=%llx gsb=%llx\n",
		 e->cpu, e->seq,
		 e->cr0, e->cr2, e->cr3, e->cr4, e->fs_base, e->gs_base);
	pr_emerg("KVMV2T-T cpu=%u seq=%u tmm=%llx tamm=%llx tscr2=%llx hax=%llx horax=%llx hip=%llx hsp=%llx rmatch=%u rptr=%llx crptr=%llx\n",
		 e->cpu, e->seq,
		 e->task_mm_ptr, e->task_active_mm_ptr, e->task_saved_cr2,
		 e->task_host_ax, e->task_host_orig_ax,
		 e->task_host_ip, e->task_host_sp, e->regs_current_match,
		 e->regs_ptr, e->current_regs_ptr);
	pr_emerg("KVMV2T-F cpu=%u seq=%u tfpuh=%x tscv=%u tistp=%u tiofv=%u tfpuv=%u tist=[%llx,%llx,%llx,%llx,%llx,%llx]\n",
		 e->cpu, e->seq,
		 e->task_fpu_hash, e->task_saved_cr2_valid,
		 e->task_ist_pending, e->task_iotrap_fpu_valid,
		 e->task_fpu_valid,
		 e->task_ist_frame[0], e->task_ist_frame[1],
		 e->task_ist_frame[2], e->task_ist_frame[3],
		 e->task_ist_frame[4], e->task_ist_frame[5]);
	pr_emerg("KVMV2T-M cpu=%u seq=%u mmgen=%llu vlast=%llu vmm=%llx vkick=%u\n",
		 e->cpu, e->seq,
		 e->mm_tlb_gen, e->vcpu_last_seen_tlb_gen,
		 e->vcpu_current_mm, e->vcpu_kick_pending);
	pr_emerg("KVMV2T-I cpu=%u seq=%u list=[%llx,%llx,%llx,%llx,%llx,%llx]\n",
		 e->cpu, e->seq,
		 e->ist_live[0], e->ist_live[1], e->ist_live[2],
		 e->ist_live[3], e->ist_live[4], e->ist_live[5]);
}

void kvm_v2_state_trace_dump(const char *reason)
{
	struct kvm_v2_state_snap *all = NULL;
	size_t total = 0, written = 0, i;
	int cpu;

	if (!READ_ONCE(rings_allocated)) {
		pr_emerg("KVMV2T_DUMP_BEGIN reason=%s rings=NULL\n", reason);
		pr_emerg("KVMV2T_DUMP_END entries=0\n");
		return;
	}

	for_each_possible_cpu(cpu) {
		struct kvm_v2_trace_ring *r = &per_cpu(trace_rings, cpu);
		unsigned int n = (unsigned int)atomic_read(&r->next);

		if (n > r->capacity)
			n = r->capacity;
		total += n;
	}
	if (!total) {
		pr_emerg("KVMV2T_DUMP_BEGIN reason=%s entries=0\n", reason);
		pr_emerg("KVMV2T_DUMP_END entries=0\n");
		return;
	}

	all = vmalloc(total * sizeof(*all));
	if (!all) {
		pr_emerg("KVMV2T_DUMP_BEGIN reason=%s vmalloc-OOM total=%zu\n",
			 reason, total);
		return;
	}

	for_each_possible_cpu(cpu) {
		struct kvm_v2_trace_ring *r = &per_cpu(trace_rings, cpu);
		unsigned int total_seq, n, start_seq, j;

		total_seq = (unsigned int)atomic_read(&r->next);
		n = total_seq;
		if (n > r->capacity) {
			start_seq = total_seq - r->capacity;
			n = r->capacity;
		} else {
			start_seq = 0;
		}
		for (j = 0; j < n && written < total; j++) {
			unsigned int slot = (start_seq + j) % r->capacity;

			all[written++] = r->entries[slot];
		}
	}

	sort(all, written, sizeof(*all), snap_cmp, NULL);

	pr_emerg("KVMV2T_DUMP_BEGIN reason=%s entries=%zu\n", reason, written);
	for (i = 0; i < written; i++) {
		dump_one(&all[i]);
		if ((i & (KVMV2_TRACE_DUMP_BATCH - 1)) == 0)
			touch_nmi_watchdog();
	}
	pr_emerg("KVMV2T_DUMP_END entries=%zu\n", written);

	vfree(all);
}

/* === debugfs control surface ========================================= */

static int trace_enabled_get(void *data, u64 *val)
{
	*val = static_key_enabled(&kvm_v2_state_trace_key.key) ? 1 : 0;
	return 0;
}

static int trace_enabled_set(void *data, u64 val)
{
	int rc;

	if (val) {
		mutex_lock(&trace_alloc_mutex);
		rc = trace_rings_alloc_locked();
		mutex_unlock(&trace_alloc_mutex);
		if (rc)
			return rc;
		if (!static_key_enabled(&kvm_v2_state_trace_key.key))
			static_branch_enable(&kvm_v2_state_trace_key);
	} else {
		if (static_key_enabled(&kvm_v2_state_trace_key.key))
			static_branch_disable(&kvm_v2_state_trace_key);
	}
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(trace_enabled_fops,
			 trace_enabled_get, trace_enabled_set, "%llu\n");

static int trace_dump_set(void *data, u64 val)
{
	kvm_v2_state_trace_dump(val ? "debugfs" : "debugfs0");
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(trace_dump_fops, NULL, trace_dump_set, "%llu\n");

static int trace_clear_set(void *data, u64 val)
{
	kvm_v2_state_trace_clear();
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(trace_clear_fops, NULL, trace_clear_set, "%llu\n");

static bool trace_enable_at_boot;

static int __init kvm_v2_state_trace_enable_at_boot(char *unused)
{
	trace_enable_at_boot = true;
	return 0;
}
__setup("kvm_v2_trace_enable", kvm_v2_state_trace_enable_at_boot);

/*
 * Round 2 (2026-05-17): override the per-CPU ring size from the kernel
 * command line. Useful for CI configs that need a smaller default than
 * the now-32 MB compile-time default, or for investigations that want
 * an even bigger ring (e.g., 64 MB) without recompiling. Format:
 * `kvm_v2_trace_bytes=<unsigned-long>`. Accepts bare integers (bytes)
 * or 'k'/'m'/'g' suffix via memparse(). Read once at boot — must
 * appear BEFORE `kvm_v2_trace_enable` or be combined in the same
 * cmdline so the boot-arm initcall picks up the override.
 */
static int __init kvm_v2_state_trace_set_bytes(char *str)
{
	unsigned long sz;

	if (!str || !*str)
		return 0;
	sz = memparse(str, NULL);
	if (sz < 64UL * sizeof(struct kvm_v2_state_snap))
		sz = 64UL * sizeof(struct kvm_v2_state_snap);
	ring_bytes = sz;
	return 0;
}
__setup("kvm_v2_trace_bytes=", kvm_v2_state_trace_set_bytes);

/*
 * Round 2 (2026-05-17, Django silent-crash hook): freeze the trace ring
 * from any kvm-v2 call site that has just identified an
 * about-to-terminate event whose root cause is upstream in the trace
 * window. One-shot via cmpxchg — first hit wins. `reason` is logged for
 * post-mortem grep ("KVMV2T_ANOMALY <reason>").
 *
 * Distinct from kvm_v2_state_trace_capture()'s built-in HANDLE_SYSCALL_PRE
 * fatal-signal trigger, which only catches USER-initiated signal
 * delivery (tgkill/tkill/kill/rt_sigqueueinfo). This helper is for
 * KERNEL-initiated signal delivery (force_sig_fault SIGSEGV/SIGBUS from
 * segv_handler) where there is no syscall hook to ride.
 */
void kvm_v2_state_trace_freeze(const char *reason)
{
	if (atomic_cmpxchg(&trace_frozen, 0, 1) == 0)
		pr_emerg("KVMV2T_ANOMALY %s pid=%u — froze trace ring\n",
			 reason, current ? current->pid : 0);
}

/*
 * Defer ring alloc + static-key enable to subsys_initcall — vmalloc and
 * static_branch are available by then but late enough that we still
 * capture the very first KVM_RUN of pid=1.
 */
static int __init kvm_v2_state_trace_boot_arm(void)
{
	int rc;

	if (!trace_enable_at_boot)
		return 0;
	mutex_lock(&trace_alloc_mutex);
	rc = trace_rings_alloc_locked();
	mutex_unlock(&trace_alloc_mutex);
	if (rc) {
		pr_warn("um: kvm-v2 state-trace: boot enable failed alloc rc=%d\n", rc);
		return 0;
	}
	if (!static_key_enabled(&kvm_v2_state_trace_key.key))
		static_branch_enable(&kvm_v2_state_trace_key);
	pr_info("um: kvm-v2 state-trace: ENABLED at boot via kvm_v2_trace_enable\n");
	return 0;
}
subsys_initcall(kvm_v2_state_trace_boot_arm);

static int __init kvm_v2_state_trace_init(void)
{
	trace_dir = debugfs_create_dir("um_kvm_v2_trace", NULL);
	if (IS_ERR_OR_NULL(trace_dir)) {
		trace_dir = NULL;
		return 0; /* debugfs disabled — silent no-op */
	}
	debugfs_create_file_unsafe("enabled", 0600, trace_dir, NULL,
				   &trace_enabled_fops);
	debugfs_create_file_unsafe("dump",    0200, trace_dir, NULL,
				   &trace_dump_fops);
	debugfs_create_file_unsafe("clear",   0200, trace_dir, NULL,
				   &trace_clear_fops);
	debugfs_create_ulong("ring_bytes", 0644, trace_dir, &ring_bytes);
	pr_info("um: kvm-v2 state-trace debugfs at /sys/kernel/debug/um_kvm_v2_trace/\n");
	return 0;
}
late_initcall(kvm_v2_state_trace_init);
