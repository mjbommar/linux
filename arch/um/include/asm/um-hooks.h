/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_HOOKS_H
#define __ASM_UM_HOOKS_H

/*
 * Static-key gates for UML kernel-side hot paths. Gate state is toggled
 * at runtime via /sys/kernel/debug/um/hooks/<name>.
 *
 * Off-state cost is about 1-2 ns per gate on UML's static-branch
 * fallback path: a load, compare, and predicted-not-taken branch.
 *
 * Kernel-only header. Host-built files under arch/um/backend/ and
 * arch/um/os-Linux/ must not include this file; they cannot link
 * against <linux/static_key.h>. The hook sites live in kernel files
 * that the backend trap loops hand off to, such as handle_syscall(),
 * segv(), __switch_to(), do_IRQ(), and timer_read().
 *
 * Adding a new gate:
 *   1. DECLARE_STATIC_KEY_FALSE(um_hook_<name>) here;
 *   2. DEFINE_STATIC_KEY_FALSE(um_hook_<name>) in kernel/hooks.c;
 *   3. register it in the debugfs table in kernel/debugfs.c;
 *   4. dispatch it from the relevant um_on_*() helper below;
 *   5. add an entry to the slow-path counter array.
 */

#include <linux/compiler.h>
#include <linux/jump_label.h>
#include <linux/types.h>

struct pt_regs;
struct task_struct;
struct faultinfo;
struct uml_pt_regs;

/* --- Gate declarations ------------------------------------------------
 *
 * New gates ship together with their first real call site. Do not
 * add a gate here that isn't dispatched from one of the um_on_*()
 * helpers below; a named-but-dead gate is dead infrastructure.
 */

DECLARE_STATIC_KEY_FALSE(um_hook_trace_syscalls);
DECLARE_STATIC_KEY_FALSE(um_hook_kcov_enabled);
DECLARE_STATIC_KEY_FALSE(um_hook_time_travel_active);
DECLARE_STATIC_KEY_FALSE(um_hook_kfence_sample);
DECLARE_STATIC_KEY_FALSE(um_hook_record_replay);
DECLARE_STATIC_KEY_FALSE(um_hook_perf_dispatch);

/* --- Stats: per-gate hit counters the slow paths bump --------------- */

enum um_hook_id {
	UM_HOOK_TRACE_SYSCALLS = 0,
	UM_HOOK_KCOV_ENABLED,
	UM_HOOK_TIME_TRAVEL_ACTIVE,
	UM_HOOK_KFENCE_SAMPLE,
	UM_HOOK_RECORD_REPLAY,
	UM_HOOK_PERF_DISPATCH,
	UM_HOOK__COUNT,
};

void um_hook_stats_inc(enum um_hook_id id);
u64  um_hook_stats_read(enum um_hook_id id);
const char *um_hook_name(enum um_hook_id id);
struct static_key_false *um_hook_key(enum um_hook_id id);

/* --- Slow-path stubs (kernel/hooks.c) ------------------------------- */
/*
 * These are the "on-state" work. The default implementations bump a
 * per-gate counter. Real consumers replace them one gate at a time,
 * without touching the gate call sites below.
 */
void __um_trace_syscall_entry(struct pt_regs *regs);
void __um_trace_syscall_exit(struct pt_regs *regs);
void __um_trace_page_fault(struct faultinfo *fi, unsigned long ip,
			   int is_user);
void __um_trace_context_switch(struct task_struct *from,
			       struct task_struct *to);
void __um_trace_irq_entry(int irq, struct uml_pt_regs *regs);
void __um_trace_clock_read(u64 ns);

void __um_kcov_record_syscall(struct pt_regs *regs);

void __um_perf_syscall(struct pt_regs *regs);
void __um_perf_context_switch(struct task_struct *from,
			      struct task_struct *to);

void __um_record_event_clock(u64 ns);
bool um_time_travel_consume_replay(u64 *ns);

void __um_time_travel_clock(u64 ns);
void __um_kfence_clock_tick(u64 ns);

/* --- Hook helpers (inlined at call site) ---------------------------- */
/*
 * Each helper is __always_inline so the static-branch checks expand at
 * the call site, not inside a function prologue.
 */

static __always_inline void um_on_syscall_entry(struct pt_regs *regs)
{
	if (static_branch_unlikely(&um_hook_trace_syscalls))
		__um_trace_syscall_entry(regs);
	if (static_branch_unlikely(&um_hook_kcov_enabled))
		__um_kcov_record_syscall(regs);
	if (static_branch_unlikely(&um_hook_perf_dispatch))
		__um_perf_syscall(regs);
}

static __always_inline void um_on_syscall_exit(struct pt_regs *regs)
{
	if (static_branch_unlikely(&um_hook_trace_syscalls))
		__um_trace_syscall_exit(regs);
}

static __always_inline void um_on_page_fault(struct faultinfo *fi,
					     unsigned long ip,
					     int is_user)
{
	if (static_branch_unlikely(&um_hook_trace_syscalls))
		__um_trace_page_fault(fi, ip, is_user);
}

static __always_inline void um_on_context_switch(struct task_struct *from,
						 struct task_struct *to)
{
	if (static_branch_unlikely(&um_hook_trace_syscalls))
		__um_trace_context_switch(from, to);
	if (static_branch_unlikely(&um_hook_perf_dispatch))
		__um_perf_context_switch(from, to);
}

static __always_inline void um_on_irq_entry(int irq, struct uml_pt_regs *regs)
{
	if (static_branch_unlikely(&um_hook_trace_syscalls))
		__um_trace_irq_entry(irq, regs);
}

static __always_inline void um_on_clock_read(u64 ns)
{
	if (static_branch_unlikely(&um_hook_time_travel_active))
		__um_time_travel_clock(ns);
	if (static_branch_unlikely(&um_hook_kfence_sample))
		__um_kfence_clock_tick(ns);
}

static __always_inline void um_on_time_travel_advance(u64 ns)
{
	if (static_branch_unlikely(&um_hook_record_replay))
		__um_record_event_clock(ns);
}

#endif /* __ASM_UM_HOOKS_H */
