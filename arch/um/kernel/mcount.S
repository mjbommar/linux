/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML ftrace trampoline (workstream C-05).
 *
 * The compiler emits a 5-byte NOP at every traced function's entry
 * (-fpatchable-function-entry=5,0, per decisions-log D29). At runtime
 * the kernel patches that NOP to `call ftrace_caller` when the
 * function tracer is enabled for that record, and back to a NOP when
 * disabled.
 *
 * On entry to ftrace_caller:
 *   - %rsp points at the return address pushed by the `call` (i.e.,
 *     the address just past the patched 5-byte sequence in the
 *     traced function).
 *   - All System V AMD64 caller-clobbered registers belong to the
 *     traced function; they hold its incoming arguments. We must
 *     preserve them.
 *
 * We call `ftrace_stub` by default at the ftrace_call label; at
 * init time ftrace_update_ftrace_func() patches that call to point
 * at ftrace_ops_list_func (or a per-ops trampoline).
 *
 * Function graph is out of scope for C-05 (decisions-log D27); no
 * return trampoline here.
 */

#include <linux/cfi_types.h>
#include <linux/linkage.h>

	.section .text, "ax"
	.code64

/* Default tail for a patched `call`, used before ftrace_caller is
 * initialized and when no tracer is attached. Just returns.
 */
SYM_TYPED_FUNC_START(ftrace_stub)
	ret
SYM_FUNC_END(ftrace_stub)

/*
 * ftrace_caller: invoked by a patched `call` at the entry of every
 * traced function. Must not clobber any caller-saved register across
 * the return, because those registers hold the traced function's
 * arguments.
 */
SYM_FUNC_START(ftrace_caller)
	/* Save all SysV AMD64 caller-clobbered regs, in a stable order
	 * so we can reach parent_ip and the return address by offset.
	 *
	 * Layout after the nine pushes (bottom → top of stack):
	 *   (%rsp)    = r11
	 *   8(%rsp)   = r10
	 *   16(%rsp)  = r9
	 *   24(%rsp)  = r8
	 *   32(%rsp)  = rdi
	 *   40(%rsp)  = rsi
	 *   48(%rsp)  = rdx
	 *   56(%rsp)  = rcx
	 *   64(%rsp)  = rax
	 *   72(%rsp)  = return address to the traced function's caller
	 *               (this is the "parent_ip" the callback wants)
	 *   80(%rsp)  = return address to the traced function
	 *               (this is one past the patched 5 bytes; the
	 *               "ip" the callback wants is 5 bytes before it)
	 *
	 * Note: the "return address to the traced function" here refers
	 * to the address the `call` in the traced function's prologue
	 * will return to, which is the instruction immediately following
	 * the 5-byte `call ftrace_caller` we were reached through.
	 * ftrace wants the IP of the call itself, so we subtract 5
	 * (MCOUNT_INSN_SIZE) below.
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
	movq	80(%rsp), %rdi
	subq	$5, %rdi

	/* Arg 2 (%rsi): parent_ip = return to traced fn's caller */
	movq	72(%rsp), %rsi

	/* Arg 3 (%rdx): op = function_trace_op */
	movq	function_trace_op(%rip), %rdx

	/* Arg 4 (%rcx): fregs = NULL (no REGS variant on UML) */
	xorq	%rcx, %rcx

SYM_INNER_LABEL(ftrace_call, SYM_L_GLOBAL)
	/* Patched at runtime by ftrace_update_ftrace_func(). The default
	 * target is ftrace_stub (a plain `ret`); when tracing is active
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

	ret
SYM_FUNC_END(ftrace_caller)
