// SPDX-License-Identifier: GPL-2.0
/*
 * UML dynamic ftrace patching.
 *
 * The compiler emits a 5-byte NOP at every traced function's entry
 * via -fpatchable-function-entry=5,0 (see arch/um/Makefile). This
 * file implements the arch hooks that
 * patch those NOPs to call ftrace_caller and back.
 *
 * Patch mechanism:
 *   1. text_mutex is held by the generic ftrace core across the
 *      arch_ftrace_update_code() callback (see
 *      ftrace_arch_code_modify_prepare()).
 *   2. arch_ftrace_update_code() runs inside stop_machine_cpuslocked()
 *      so no peer UML vCPU host thread runs kernel code during the
 *      patch window.
 *   3. The stop_machine callback maps the kernel text image
 *      [_text, _etext) RW once, calls ftrace_modify_all_code()
 *      which iterates every record and invokes the per-site memcpy
 *      helpers, then mprotect back to RX once. Two mprotect
 *      syscalls per enable/disable, regardless of how many records
 *      are patched. Per-site mprotect would turn one enable into
 *      2 * N syscalls (N is about 21000) and cumulative VMA fragmentation
 *      hangs the host process.
 *
 * UML cannot replicate arm/arm64/riscv's fixmap-alias or x86's
 * text_poke_mm pattern because UML has exactly one mm (its host
 * process's). stop_machine + bulk mprotect is the closest analogue:
 * no peer kernel path observes the transient RW window.
 *
 * The function graph trampolines live in arch/um/kernel/mcount.S; this
 * file implements the hooks fgraph needs: prepare_ftrace_return()
 * (called from ftrace_graph_caller on every traced function entry when
 * fgraph is active) and the enable/disable pair that patches the
 * ftrace_graph_call site inside ftrace_caller.
 */

#include <linux/ftrace.h>
#include <linux/memory.h>
#include <linux/stop_machine.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <asm/sections.h>

#include <os.h>

#define CALL_INSN_OPCODE	0xe8
#define CALL_INSN_SIZE		5	/* = MCOUNT_INSN_SIZE */

/* Standard 5-byte x86_64 NOP: nopl 0(%rax,%rax,1). Same encoding
 * emitted by GCC under -fpatchable-function-entry=5,0.
 */
static const u8 um_ftrace_nop5[CALL_INSN_SIZE] = {
	0x0f, 0x1f, 0x44, 0x00, 0x00
};

static void um_ftrace_build_call(u8 buf[CALL_INSN_SIZE],
				 unsigned long site, unsigned long target)
{
	s32 disp = (s32)(target - (site + CALL_INSN_SIZE));

	buf[0] = CALL_INSN_OPCODE;
	memcpy(&buf[1], &disp, sizeof(disp));
}

/*
 * Per-record patchers. Invoked from ftrace_modify_all_code() inside
 * arch_ftrace_update_code()'s stop_machine callback, where [_text, _etext)
 * is already mprotect'd RW. Plain memcpy is sufficient.
 */

int ftrace_init_nop(struct module *mod, struct dyn_ftrace *rec)
{
	/*
	 * -fpatchable-function-entry=5,0 places the 5-byte NOP at
	 * compile time, so init has nothing to write. Skipping this
	 * avoids per-record work during ftrace_init() entirely; any
	 * drift (corrupted build, in-tree poisoning) will be caught
	 * by the generic verification path on first ftrace_make_call.
	 */
	return 0;
}

int ftrace_make_nop(struct module *mod, struct dyn_ftrace *rec,
		    unsigned long addr)
{
	memcpy((void *)rec->ip, um_ftrace_nop5, CALL_INSN_SIZE);
	return 0;
}

int ftrace_make_call(struct dyn_ftrace *rec, unsigned long addr)
{
	u8 new[CALL_INSN_SIZE];

	um_ftrace_build_call(new, rec->ip, addr);
	memcpy((void *)rec->ip, new, CALL_INSN_SIZE);
	return 0;
}

/*
 * ftrace_modify_call() is only required when CONFIG_DYNAMIC_FTRACE_WITH_REGS
 * or *_WITH_CALL_OPS or *_WITH_DIRECT_CALLS is set. UML enables none of
 * those; the generic static-inline stub in <linux/ftrace.h> returns -EINVAL
 * and is never reached.
 */

int ftrace_update_ftrace_func(ftrace_func_t func)
{
	unsigned long site = (unsigned long)&ftrace_call;
	u8 new[CALL_INSN_SIZE];

	um_ftrace_build_call(new, site, (unsigned long)func);
	memcpy((void *)site, new, CALL_INSN_SIZE);
	return 0;
}

static int um_ftrace_update_code_cb(void *data)
{
	int *command = data;
	unsigned long text_len = (unsigned long)_etext - (unsigned long)_text;
	int err;

	/*
	 * stop_machine dispatches to every online CPU in parallel; only
	 * one needs to do the work. cpumask_first() picks a stable CPU
	 * regardless of caller. Other CPUs' callback invocations return
	 * 0 immediately but stay frozen until the chosen CPU returns,
	 * preserving the no-peer-execution property.
	 */
	if (smp_processor_id() != cpumask_first(cpu_online_mask))
		return 0;

	err = os_protect_memory(_text, text_len, 1, 1, 1);
	if (err)
		return err;

	ftrace_modify_all_code(*command);

	return os_protect_memory(_text, text_len, 1, 0, 1);
}

void arch_ftrace_update_code(int command)
{
	stop_machine_cpuslocked(um_ftrace_update_code_cb, &command,
				cpu_online_mask);
}

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
/*
 * ftrace_graph_call / ftrace_graph_caller / return_to_handler are
 * declared in <asm/ftrace.h>. They resolve to the mcount.S
 * trampolines this file patches and hooks via prepare_ftrace_return.
 *
 * ftrace_graph_call is a 5-byte JMP rel32 site inside ftrace_caller
 * that starts as jmp ftrace_stub (a no-op return path) and gets
 * patched to jmp ftrace_graph_caller when the function graph
 * tracer activates. Patching happens from inside
 * ftrace_modify_all_code() (generic kernel/trace/ftrace.c, see
 * FTRACE_START_FUNC_RET/FTRACE_STOP_FUNC_RET), which on UML runs
 * inside arch_ftrace_update_code()'s stop_machine callback, so
 * [_text, _etext) is already mapped RW and no peer CPU is
 * executing kernel code. Plain memcpy is safe; the per-patch mprotect
 * handling provided by native x86's smp_text_poke_single() is not
 * needed here.
 */
#define JMP_INSN_OPCODE		0xe9
#define JMP_INSN_SIZE		5	/* = MCOUNT_INSN_SIZE */

static int um_ftrace_mod_jmp(unsigned long site, void *target)
{
	u8 new[JMP_INSN_SIZE];
	s32 disp = (s32)((unsigned long)target - (site + JMP_INSN_SIZE));

	new[0] = JMP_INSN_OPCODE;
	memcpy(&new[1], &disp, sizeof(disp));
	memcpy((void *)site, new, JMP_INSN_SIZE);
	return 0;
}

int ftrace_enable_ftrace_graph_caller(void)
{
	return um_ftrace_mod_jmp((unsigned long)&ftrace_graph_call,
				 &ftrace_graph_caller);
}

int ftrace_disable_ftrace_graph_caller(void)
{
	return um_ftrace_mod_jmp((unsigned long)&ftrace_graph_call,
				 &ftrace_stub);
}

/*
 * Hook the return address of a traced function via the fgraph
 * shadow stack. Called from ftrace_graph_caller (mcount.S) on
 * every traced-function entry while fgraph is active.
 *
 * - @ip: address of the patched NOP at the traced function's
 *   entry (== traced_fn's real start on UML).
 * - @parent: pointer to the return-address slot on the traced
 *   function's stack. Rewriting *parent sends the traced
 *   function's ret to return_to_handler instead of the real
 *   caller.
 * - @frame_pointer: 0 on UML (no frame pointers through the
 *   ftrace trampoline path).
 *
 * function_graph_enter() pushes a shadow-stack entry keyed by the
 * original parent address; return_to_handler() calls
 * ftrace_return_to_handler() to recover the real caller ip.
 *
 * Safety: the signal-gate family in arch/um/{kernel,os-Linux}/
 * signal.c is marked notrace (kernel/signal.c) or built without
 * patchable NOPs (os-Linux/signal.c), so this path never fires from
 * inside arch_local_irq_* callers. Without that, the activation-path
 * mutex inside function_graph_enter would sleep in atomic context.
 */
void notrace prepare_ftrace_return(unsigned long ip, unsigned long *parent,
				   unsigned long frame_pointer)
{
	unsigned long return_hooker = (unsigned long)&return_to_handler;

	if (unlikely(ftrace_graph_is_dead()))
		return;

	if (unlikely(atomic_read(&current->tracing_graph_pause)))
		return;

	/*
	 * UML-UP kernels use TINY_RCU, where rcu_read_lock() is
	 * implemented as preempt_disable(). Kernel call sites that
	 * legitimately hold rcu_read_lock around kallsyms / BPF /
	 * tracepoint lookup (is_bpf_text_address() is the one most
	 * visibly on the path) therefore run with preempt_count > 0.
	 * Under PROVE_LOCKING + DEBUG_ATOMIC_SLEEP, the body of a
	 * graph-traced function that happens to take a sleeping lock
	 * (free_irq to __mutex_lock, tracer activation to
	 * tracepoints_mutex, etc.) will then trip __might_resched() and
	 * emit a lockdep-flavoured BUG. Native x86 escapes because it
	 * builds with TREE_RCU, where rcu_read_lock() does not touch
	 * preempt_count; UML-UP has no equivalent option.
	 *
	 * Skip the shadow-stack push when preempt_count is already
	 * non-zero. We lose graph events for the traced-function
	 * window that sits inside the rcu_read_lock / atomic
	 * context, which is exactly the window where the generic
	 * fgraph trampoline's push-then-callback-then-pop contract is
	 * unsafe on UML. The traced function still executes normally,
	 * and all fgraph events that happen outside atomic context
	 * are still captured. Read preempt_count() directly rather than
	 * in_atomic() so the checked state is explicit.
	 */
	if (unlikely(preempt_count()))
		return;

	if (!function_graph_enter(*parent, ip, frame_pointer, parent))
		*parent = return_hooker;
}
#endif /* CONFIG_FUNCTION_GRAPH_TRACER */
