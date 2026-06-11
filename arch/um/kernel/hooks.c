// SPDX-License-Identifier: GPL-2.0
/*
 * UML hook gate definitions, slow-path stubs, and per-gate hit counters.
 *
 * The gates are declared in arch/um/include/asm/um-hooks.h and
 * inserted at hot paths by the um_on_*() helpers there. When a gate
 * is off the call site short-circuits via the jump-label machinery;
 * when on it becomes a call into one of the __um_* slow-path
 * functions defined below. Off-state cost uses the jump-label C fallback
 * because UML does not select HAVE_ARCH_JUMP_LABEL.
 *
 * Each slow-path stub bumps a per-CPU counter; totals are summed in
 * um_hook_stats_read. Per-CPU avoids cross-CPU cacheline contention
 * under high hit rates so "gate on" benchmarks measure the hook
 * itself, not atomic fencing. Real consumers can replace individual
 * stubs without modifying the gate call sites or this file's public
 * surface.
 */

#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/ptrace.h>
#include <linux/smp.h>
#include <linux/types.h>

#include <asm/um-hooks.h>
#include <sysdep/ptrace.h>

#if IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL)
#include <asm/um-kvm-v2-record.h>
#endif

/* --- Gate definitions ------------------------------------------------ */

DEFINE_STATIC_KEY_FALSE(um_hook_trace_syscalls);
EXPORT_SYMBOL_GPL(um_hook_trace_syscalls);
DEFINE_STATIC_KEY_FALSE(um_hook_kcov_enabled);
EXPORT_SYMBOL_GPL(um_hook_kcov_enabled);
DEFINE_STATIC_KEY_FALSE(um_hook_time_travel_active);
EXPORT_SYMBOL_GPL(um_hook_time_travel_active);
DEFINE_STATIC_KEY_FALSE(um_hook_kfence_sample);
EXPORT_SYMBOL_GPL(um_hook_kfence_sample);
DEFINE_STATIC_KEY_FALSE(um_hook_record_replay);
EXPORT_SYMBOL_GPL(um_hook_record_replay);
DEFINE_STATIC_KEY_FALSE(um_hook_perf_dispatch);
EXPORT_SYMBOL_GPL(um_hook_perf_dispatch);

/* --- Hit counters (per-CPU) ----------------------------------------- *
 *
 * Each CPU owns its own u64 array; increments are unlocked native
 * writes on the writing CPU, and the reader sums across CPUs. Reads
 * are inherently racy against concurrent writers; acceptable for
 * observability telemetry where the exact count matters less than
 * rate of change. A global atomic distorts "gate on" benchmarks
 * because every hit site collides on one cacheline.
 */

struct um_hook_percpu_counters {
	u64 hits[UM_HOOK__COUNT];
};

static DEFINE_PER_CPU_ALIGNED(struct um_hook_percpu_counters,
			      um_hook_hits_pc);

static const char * const um_hook_names[UM_HOOK__COUNT] = {
	[UM_HOOK_TRACE_SYSCALLS]     = "trace_syscalls",
	[UM_HOOK_KCOV_ENABLED]       = "kcov_enabled",
	[UM_HOOK_TIME_TRAVEL_ACTIVE] = "time_travel_active",
	[UM_HOOK_KFENCE_SAMPLE]      = "kfence_sample",
	[UM_HOOK_RECORD_REPLAY]      = "record_replay",
	[UM_HOOK_PERF_DISPATCH]      = "perf_dispatch",
};

static struct static_key_false * const um_hook_keys[UM_HOOK__COUNT] = {
	[UM_HOOK_TRACE_SYSCALLS]     = &um_hook_trace_syscalls,
	[UM_HOOK_KCOV_ENABLED]       = &um_hook_kcov_enabled,
	[UM_HOOK_TIME_TRAVEL_ACTIVE] = &um_hook_time_travel_active,
	[UM_HOOK_KFENCE_SAMPLE]      = &um_hook_kfence_sample,
	[UM_HOOK_RECORD_REPLAY]      = &um_hook_record_replay,
	[UM_HOOK_PERF_DISPATCH]      = &um_hook_perf_dispatch,
};

void um_hook_stats_inc(enum um_hook_id id)
{
	struct um_hook_percpu_counters *pc;

	if (unlikely((unsigned int)id >= UM_HOOK__COUNT))
		return;

	/*
	 * this_cpu_inc variant: atomic on the local CPU, no cross-CPU
	 * synchronization. Safe inside IRQ/softirq context.
	 */
	pc = this_cpu_ptr(&um_hook_hits_pc);
	pc->hits[id]++;
}
EXPORT_SYMBOL_GPL(um_hook_stats_inc);

u64 um_hook_stats_read(enum um_hook_id id)
{
	u64 sum = 0;
	int cpu;

	if ((unsigned int)id >= UM_HOOK__COUNT)
		return 0;

	for_each_possible_cpu(cpu)
		sum += per_cpu(um_hook_hits_pc, cpu).hits[id];

	return sum;
}
EXPORT_SYMBOL_GPL(um_hook_stats_read);

const char *um_hook_name(enum um_hook_id id)
{
	if ((unsigned int)id >= UM_HOOK__COUNT)
		return NULL;
	return um_hook_names[id];
}
EXPORT_SYMBOL_GPL(um_hook_name);

struct static_key_false *um_hook_key(enum um_hook_id id)
{
	if ((unsigned int)id >= UM_HOOK__COUNT)
		return NULL;
	return um_hook_keys[id];
}
EXPORT_SYMBOL_GPL(um_hook_key);

/* --- Slow-path stubs ------------------------------------------------ *
 *
 * One per gate and hook site combination. The default implementations
 * bump counters; consumers can replace them one by one.
 *
 * The implementations are marked notrace to avoid recursion if ftrace
 * is enabled on the syscall tracepoint the trace_syscalls gate feeds.
 */

notrace void __um_trace_syscall_entry(struct pt_regs *regs)
{
	(void)regs;
	um_hook_stats_inc(UM_HOOK_TRACE_SYSCALLS);
}
EXPORT_SYMBOL_GPL(__um_trace_syscall_entry);

notrace void __um_trace_syscall_exit(struct pt_regs *regs)
{
	(void)regs;
	um_hook_stats_inc(UM_HOOK_TRACE_SYSCALLS);
}
EXPORT_SYMBOL_GPL(__um_trace_syscall_exit);

notrace void __um_trace_page_fault(struct faultinfo *fi, unsigned long ip,
				   int is_user)
{
	(void)fi; (void)ip; (void)is_user;
	um_hook_stats_inc(UM_HOOK_TRACE_SYSCALLS);
}
EXPORT_SYMBOL_GPL(__um_trace_page_fault);

notrace void __um_trace_context_switch(struct task_struct *from,
				       struct task_struct *to)
{
	(void)from; (void)to;
	um_hook_stats_inc(UM_HOOK_TRACE_SYSCALLS);
}
EXPORT_SYMBOL_GPL(__um_trace_context_switch);

notrace void __um_trace_irq_entry(int irq, struct uml_pt_regs *regs)
{
	(void)irq; (void)regs;
	um_hook_stats_inc(UM_HOOK_TRACE_SYSCALLS);
}
EXPORT_SYMBOL_GPL(__um_trace_irq_entry);

notrace void __um_trace_clock_read(u64 ns)
{
	(void)ns;
	um_hook_stats_inc(UM_HOOK_TRACE_SYSCALLS);
}
EXPORT_SYMBOL_GPL(__um_trace_clock_read);

notrace void __um_kcov_record_syscall(struct pt_regs *regs)
{
	(void)regs;
	um_hook_stats_inc(UM_HOOK_KCOV_ENABLED);
}
EXPORT_SYMBOL_GPL(__um_kcov_record_syscall);

notrace void __um_perf_syscall(struct pt_regs *regs)
{
	(void)regs;
	um_hook_stats_inc(UM_HOOK_PERF_DISPATCH);
}
EXPORT_SYMBOL_GPL(__um_perf_syscall);

notrace void __um_perf_context_switch(struct task_struct *from,
				      struct task_struct *to)
{
	(void)from; (void)to;
	um_hook_stats_inc(UM_HOOK_PERF_DISPATCH);
}
EXPORT_SYMBOL_GPL(__um_perf_context_switch);

notrace void __um_record_event_clock(u64 ns)
{
#if IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL)
	struct kvm_v2_record *rec = kvm_v2_record_active();

	if (rec)
		kvm_v2_record_observe_time_travel(rec, ns);
#else
	(void)ns;
#endif
	um_hook_stats_inc(UM_HOOK_RECORD_REPLAY);
}
EXPORT_SYMBOL_GPL(__um_record_event_clock);

notrace bool um_time_travel_consume_replay(u64 *ns)
{
#if IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL)
	struct kvm_v2_record *rec = kvm_v2_record_active();
	u64 recorded;

	if (!rec || !ns)
		return false;

	if (kvm_v2_record_consume_time_travel(rec, &recorded, NULL) != 1)
		return false;

	*ns = recorded;
	um_hook_stats_inc(UM_HOOK_RECORD_REPLAY);
	return true;
#else
	(void)ns;
	return false;
#endif
}
EXPORT_SYMBOL_GPL(um_time_travel_consume_replay);

notrace void __um_time_travel_clock(u64 ns)
{
	(void)ns;
	um_hook_stats_inc(UM_HOOK_TIME_TRAVEL_ACTIVE);
}
EXPORT_SYMBOL_GPL(__um_time_travel_clock);

notrace void __um_kfence_clock_tick(u64 ns)
{
	(void)ns;
	um_hook_stats_inc(UM_HOOK_KFENCE_SAMPLE);
}
EXPORT_SYMBOL_GPL(__um_kfence_clock_tick);
