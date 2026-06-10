/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML ftrace trampoline.
 *
 * The compiler emits a 5-byte NOP at every traced function's entry
 * (-fpatchable-function-entry=5,0). At runtime
 * the kernel patches that NOP to call ftrace_caller when the
 * function tracer is enabled for that record, and back to a NOP when
 * disabled.
 *
 * On entry to ftrace_caller:
 *   - %rsp points at the return address pushed by the call (i.e.,
 *     the address just past the patched 5-byte sequence in the
 *     traced function).
 *   - All System V AMD64 caller-clobbered registers belong to the
 *     traced function; they hold its incoming arguments. We must
 *     preserve them.
 *
 * We call ftrace_stub by default at the ftrace_call label; at
 * init time ftrace_update_ftrace_func() patches that call to point
 * at ftrace_ops_list_func (or a per-ops trampoline).
 *
 * When CONFIG_FUNCTION_GRAPH_TRACER is enabled, the tail of
 * ftrace_caller reaches a 5-byte patchable ftrace_graph_call site
 * that starts as jmp ftrace_stub (a no-op return path) and gets
 * rewritten to jmp ftrace_graph_caller by
 * ftrace_enable_ftrace_graph_caller() when the graph tracer is
 * active. ftrace_graph_caller rewrites the traced function's return
 * slot so the traced function returns to return_to_handler, which
 * pops fgraph's shadow-stack entry and jumps to the real caller.
 */

#include <linux/cfi_types.h>
#include <linux/linkage.h>

	.section .text, "ax"
	.code64

/* Default tail for a patched call, used before ftrace_caller is
 * initialized and when no tracer is attached. Just returns.
 */
SYM_TYPED_FUNC_START(ftrace_stub)
	ret
SYM_FUNC_END(ftrace_stub)

/*
 * ftrace_caller: invoked by a patched call at the entry of every
 * traced function. Must not clobber any caller-saved register across
 * the return, because those registers hold the traced function's
 * arguments.
 */
SYM_FUNC_START(ftrace_caller)
	/* Save all SysV AMD64 caller-clobbered regs, in a stable order
	 * so we can reach parent_ip and the return address by offset.
	 *
	 * x86_64 pushq decrements rsp then stores, so after 9 pushes
	 * rsp has dropped by 72 and the last-pushed reg (r11) sits at
	 * the new (%rsp). The two slots sitting above the pushed frame
	 * are the two return addresses the two nested calls pushed
	 * onto the stack before we got here:
	 *
	 *   [entry_rsp+0] = return-to-traced-fn
	 *                   (pushed by the patched call ftrace_caller
	 *                   inside traced_fn's 5-byte prologue slot)
	 *   [entry_rsp+8] = parent_ip
	 *                   (pushed by the outer call traced_fn in
	 *                   traced_fn's caller)
	 *
	 * So after the 9 pushes, 72(%rsp) is return-to-traced-fn and
	 * 80(%rsp) is parent_ip. This matches arch/x86/kernel/
	 * ftrace_64.S's save_mcount_regs: MCOUNT_REG_SIZE+0 = ip-source,
	 * MCOUNT_REG_SIZE+8 = parent_ip.
	 *
	 * Layout after the nine pushes (bottom to top of stack):
	 *   (%rsp)    = r11
	 *   8(%rsp)   = r10
	 *   16(%rsp)  = r9
	 *   24(%rsp)  = r8
	 *   32(%rsp)  = rdi
	 *   40(%rsp)  = rsi
	 *   48(%rsp)  = rdx
	 *   56(%rsp)  = rcx
	 *   64(%rsp)  = rax
	 *   72(%rsp)  = return address to the traced function
	 *               (one past the 5-byte call ftrace_caller in
	 *               traced_fn's prologue; subtract 5 to get the
	 *               "ip" the callback wants)
	 *   80(%rsp)  = return address to the traced function's caller
	 *               (the "parent_ip" the callback wants)
	 */
	pushq	%rax
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11

	/* Arg 1 (%rdi): ip = (return to traced fn) - 5 */
	movq	72(%rsp), %rdi
	subq	$5, %rdi

	/* Arg 2 (%rsi): parent_ip = return to traced fn's caller */
	movq	80(%rsp), %rsi

	/* Arg 3 (%rdx): op = function_trace_op */
	movq	function_trace_op(%rip), %rdx

	/* Arg 4 (%rcx): fregs = NULL (no REGS variant on UML) */
	xorq	%rcx, %rcx

SYM_INNER_LABEL(ftrace_call, SYM_L_GLOBAL)
	/* Patched at runtime by ftrace_update_ftrace_func(). The default
	 * target is ftrace_stub (a plain ret); when tracing is active
	 * this becomes a direct call to ftrace_ops_list_func or a per-ops
	 * trampoline.
	 */
	call	ftrace_stub

	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rax

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
SYM_INNER_LABEL(ftrace_graph_call, SYM_L_GLOBAL)
	/*
	 * 5-byte JMP rel32 to ftrace_stub by default (functionally
	 * equivalent to ret since ftrace_stub is just ret). When
	 * the graph tracer activates,
	 * ftrace_enable_ftrace_graph_caller() patches this to JMP
	 * rel32 ftrace_graph_caller. Forcing the rel32 encoding via
		 * raw bytes keeps the patch the same size as the default encoding
	 * regardless of how close the jump target is; a plain jmp
	 * mnemonic at this distance would be assembled as a 2-byte
	 * JMP rel8 by gas.
	 */
	.byte	0xe9
	.long	ftrace_stub - (. + 4)
#else
	ret
#endif
SYM_FUNC_END(ftrace_caller)

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
/*
 * ftrace_graph_caller: branched to from ftrace_caller's
 * ftrace_graph_call site after the function-tracer callback has
 * run. Rewrites the traced function's return slot in place so the
 * traced function returns to return_to_handler, which pops the
 * shadow-stack entry and jumps to the real caller.
 *
 * Stack on entry (via jmp, so no push):
 *
 *   (%rsp)    = return-to-traced-fn (pushed by the patched
 *               call ftrace_caller inside traced_fn's 5-byte
 *               prologue slot)
 *   8(%rsp)   = parent_ip (pushed by the outer call traced_fn
 *               from the traced function's caller; this is the
 *               slot we rewrite)
 *
 * We must preserve every SysV AMD64 caller-clobbered reg because
 * those hold the traced function's arguments. Nine pushes matches
 * ftrace_caller's save set; the offsets to return-to-traced-fn and
 * parent_ip shift by 72 bytes.
 */
SYM_FUNC_START(ftrace_graph_caller)
	pushq	%rax
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11

	/* Arg 1 (%rdi): ip of the traced function entry.
	 * 72(%rsp) holds return-to-traced-fn (five bytes past the
	 * patchable NOP); subtract MCOUNT_INSN_SIZE (5) to land on
	 * the NOP's address itself.
	 */
	movq	72(%rsp), %rdi
	subq	$5, %rdi

	/* Arg 2 (%rsi): address of the parent-ip slot. Writing to
	 * this slot rewrites the return address the traced function
	 * will pop with its own ret. */
	leaq	80(%rsp), %rsi

	/* Arg 3 (%rdx): frame_pointer. UML doesn't build with frame
	 * pointers on the ftrace trampoline path, so pass 0; the
	 * generic fgraph code tolerates this and skips the
	 * frame-pointer-based unwind verification when it's 0.
	 */
	xorq	%rdx, %rdx

	call	prepare_ftrace_return

	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rax

	/* Returns into the traced function (which eventually rets
	 * to the rewritten parent slot and then return_to_handler).
	 */
	ret
SYM_FUNC_END(ftrace_graph_caller)

/*
 * return_to_handler: the trampoline the traced function's ret
 * lands on (via the parent slot prepare_ftrace_return rewrote).
 * Recover the real return address from the fgraph shadow stack
 * and jump there.
 *
 * Stack on entry (just popped by traced_fn's ret, which took
 * the rewritten parent slot):
 *
 *   (%rsp) = whatever was below the parent slot; the previous
 *            ret already popped our trampoline's address off
 *            the top.
 *
 * ftrace_return_to_handler(NULL) returns the real caller's ip in
 * %rax. We must preserve the traced function's return values
 * (%rax, %rdx per SysV AMD64) across the call, because the
 * caller expects to receive them.
 */
SYM_CODE_START(return_to_handler)
	/*
	 * Save the traced function's return values (SysV AMD64
	 * uses %rax for integer/pointer, %rdx for the high half of
	 * 128-bit returns). %rdi is clobbered by our call-out.
	 */
	subq	$24, %rsp
	movq	%rax, (%rsp)
	movq	%rdx, 8(%rsp)

	/* Arg 1 (%rdi): frame_pointer, 0 since we don't maintain
	 * frame pointers through the trampoline. The generic
	 * ftrace_return_to_handler() consumes the current task's
	 * ret_stack to recover the real return address.
	 */
	xorq	%rdi, %rdi
	call	ftrace_return_to_handler

	/* %rax now holds the real caller ip. Stash it below our
	 * saved retvals, then restore retvals. */
	movq	%rax, 16(%rsp)
	movq	(%rsp), %rax
	movq	8(%rsp), %rdx
	addq	$16, %rsp

	/* Top of stack now holds the real caller ip. A ret here
	 * pops it into %rip. This is cleaner than jmp *(%rsp)/add
	 * %rsp; it keeps the return-stack-buffer balanced. */
	ret
SYM_CODE_END(return_to_handler)
#endif /* CONFIG_FUNCTION_GRAPH_TRACER */
