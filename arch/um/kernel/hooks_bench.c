// SPDX-License-Identifier: GPL-2.0
/*
 * Microbenchmarks for static-key gate cost.
 *
 * Exports /sys/kernel/debug/um/bench (root, 0400) that, when read,
 * runs a tight kernel-side loop over each um_on_*() hook helper in
 * both off and on states and emits per-hook ns-per-call numbers.
 *
 * Why this lives in the kernel: the hook helpers are __always_inline;
 * measuring them from userspace would measure the USER/KERNEL syscall
 * boundary instead of the gate itself. Running the loop in-kernel
 * isolates the gate cost.
 *
 * Why a debugfs-read entry (not a module param, not a /proc file):
 * triggering a benchmark is a meaningful side-effect that takes ~1 s
 * (7 hooks x 2 states x 1M iterations), so treating the fetch as a
 * pull-driven seq_file operation gives clean user-triggered control
 * without a new syscall or sysctl.
 *
 * Methodology: for each hook helper, 10 batches x 100k iterations
 * each in a tight loop, median-of-batches reported as the per-call
 * cost. Wall time via ktime_get_ns(). 100k per batch is large enough
 * to amortize ktime read overhead (~10 ns per read) to <1% of the
 * reported figure.
 *
 * The benchmarks must not change observable behavior: gate state is
 * saved and restored around each measurement so reading the bench
 * file once leaves every gate in its original position.
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/ptrace.h>
#include <linux/seq_file.h>
#include <linux/sort.h>
#include <linux/types.h>

#include <asm/um-hooks.h>

#define BENCH_ITERS    100000u
#define BENCH_BATCHES  10u

/*
 * A scratch pt_regs so __um_trace_* slow paths have something real to
 * chase. The hook helpers take a pointer and the slow paths are
 * marked notrace with argument swallowing (void)regs, so no actual
 * field reads happen; keeping this on-stack is the cleanest way to
 * pass a non-NULL pointer through.
 */

/*
 * Disable inlining of the loop bodies so the compiler can't hoist
 * the static_branch check out of the loop (which would make every
 * measurement read zero).
 */
#define BENCH_LOOP(name, expr)						\
static noinline u64 bench_loop_##name(struct pt_regs *regs)		\
{									\
	u64 start, end;							\
	unsigned int i;							\
									\
	start = ktime_get_ns();						\
	for (i = 0; i < BENCH_ITERS; i++) {				\
		expr;							\
	}								\
	end = ktime_get_ns();						\
	return end - start;						\
}

BENCH_LOOP(syscall_entry,    um_on_syscall_entry(regs))
BENCH_LOOP(syscall_exit,     um_on_syscall_exit(regs))
BENCH_LOOP(context_switch,   um_on_context_switch(NULL, NULL))
BENCH_LOOP(clock_read,       um_on_clock_read(0))

struct bench_site {
	const char *name;
	u64 (*runner)(struct pt_regs *regs);
};

static const struct bench_site bench_sites[] = {
	{ "syscall_entry",   bench_loop_syscall_entry  },
	{ "syscall_exit",    bench_loop_syscall_exit   },
	{ "context_switch",  bench_loop_context_switch },
	{ "clock_read",      bench_loop_clock_read     },
};

static int cmp_u64(const void *a, const void *b)
{
	u64 x = *(const u64 *)a;
	u64 y = *(const u64 *)b;

	return (x > y) - (x < y);
}

static u64 median_ns_per_call(u64 *batch_ns, unsigned int batches,
			      unsigned int iters)
{
	/* divide each batch by iters, then median the per-call
	 * figures. Returns ns * 1000 so one decimal of precision
	 * survives the integer-only path.
	 */
	u64 per_call[BENCH_BATCHES];
	unsigned int i;

	for (i = 0; i < batches; i++)
		per_call[i] = (batch_ns[i] * 1000u) / iters;

	sort(per_call, batches, sizeof(per_call[0]), cmp_u64, NULL);
	return per_call[batches / 2];
}

static u64 measure_one(const struct bench_site *site, struct pt_regs *regs)
{
	u64 batches[BENCH_BATCHES];
	unsigned int i;

	for (i = 0; i < BENCH_BATCHES; i++)
		batches[i] = site->runner(regs);

	return median_ns_per_call(batches, BENCH_BATCHES, BENCH_ITERS);
}

static void save_gates(u8 *saved)
{
	enum um_hook_id id;

	for (id = 0; id < UM_HOOK__COUNT; id++) {
		struct static_key_false *key = um_hook_key(id);

		saved[id] = key && static_key_enabled(&key->key) ? 1 : 0;
	}
}

static void restore_gates(const u8 *saved)
{
	enum um_hook_id id;

	for (id = 0; id < UM_HOOK__COUNT; id++) {
		struct static_key_false *key = um_hook_key(id);

		if (!key)
			continue;
		if (saved[id])
			static_branch_enable(key);
		else
			static_branch_disable(key);
	}
}

static void gates_all_off(void)
{
	enum um_hook_id id;

	for (id = 0; id < UM_HOOK__COUNT; id++) {
		struct static_key_false *key = um_hook_key(id);

		if (key)
			static_branch_disable(key);
	}
}

static void gates_all_on(void)
{
	enum um_hook_id id;

	for (id = 0; id < UM_HOOK__COUNT; id++) {
		struct static_key_false *key = um_hook_key(id);

		if (key)
			static_branch_enable(key);
	}
}

static int um_bench_show(struct seq_file *m, void *v)
{
	u8 saved[UM_HOOK__COUNT];
	struct pt_regs regs = {};
	size_t i;

	/*
	 * Save gate state so the benchmark is idempotent.
	 */
	save_gates(saved);

	seq_puts(m, "# UML static-key gate microbenchmark\n");
	seq_printf(m, "# %u iters/batch x %u batches, median ns x 1000 per call\n",
		   BENCH_ITERS, BENCH_BATCHES);
	seq_puts(m, "# format: <site>  off_ns_x1000  on_ns_x1000\n");

	gates_all_off();
	/* Warm the icache and the branch predictor. */
	for (i = 0; i < ARRAY_SIZE(bench_sites); i++)
		(void)bench_sites[i].runner(&regs);

	for (i = 0; i < ARRAY_SIZE(bench_sites); i++) {
		u64 off_ns, on_ns;

		gates_all_off();
		off_ns = measure_one(&bench_sites[i], &regs);

		gates_all_on();
		on_ns = measure_one(&bench_sites[i], &regs);

		seq_printf(m, "%-20s %10llu %10llu\n",
			   bench_sites[i].name, off_ns, on_ns);
	}

	restore_gates(saved);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(um_bench);

static int __init um_hooks_bench_init(void)
{
	struct dentry *root;

	if (!debugfs_initialized())
		return 0;

	root = debugfs_lookup("um", NULL);
	if (IS_ERR_OR_NULL(root))
		return 0; /* um_debugfs_init didn't run; nothing to hang onto */

	debugfs_create_file("bench", 0400, root, NULL, &um_bench_fops);
	dput(root); /* debugfs_lookup returned an acquired reference */
	return 0;
}
late_initcall_sync(um_hooks_bench_init);
