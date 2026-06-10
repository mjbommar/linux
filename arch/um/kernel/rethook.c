// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * UML rethook arch layer; mirrors arch/x86/kernel/rethook.c almost
 * verbatim. When a probed function returns, control reaches the
 * arch_rethook_trampoline below (the probed callsite's return slot
 * was overwritten by arch_rethook_prepare). The trampoline saves
 * regs into a pt_regs-shaped frame and calls the generic
 * rethook_trampoline_handler(), which walks the current task's
 * shadow stack, invokes each rethook handler (including the
 * kretprobe wrapper when CONFIG_KRETPROBE_ON_RETHOOK=y), then
 * returns the real return address via arch_rethook_fixup_return().
 *
 * References:
 *   - arch/x86/kernel/rethook.c (primary reference)
 *   - arch/s390/kernel/rethook.c (minimal port; callback sanity check)
 *   - arch/x86/kernel/kprobes/common.h (SAVE_REGS_STRING / RESTORE_REGS_STRING)
 *   - x86 rethook's complete-pt_regs trampoline shape: the trampoline
 *     must push a non-garbage ss slot; the callback overwrites it with
 *     flags for the popfq+ret sequence.
 *
 * UML-specific differences vs the x86 file:
 *
 *   1. pt_regs access uses the UPT_* accessor macros because UML's
 *      struct pt_regs wraps struct uml_pt_regs; direct member
 *      access like regs->ip is not valid. The memory image that
 *      SAVE_REGS_STRING builds on the stack is byte-compatible with
 *      UML's gp[] layout (both derive from the host user_regs_struct
 *      ordering), so no layout gymnastics are needed.
 *
 *   2. frame_pointer is computed as (char *)regs + 21*sizeof(long)
 *      instead of (regs + 1). UML's struct pt_regs is larger than
 *      the x86 kernel struct (it includes faultinfo/syscall/is_user
 *      tail), so pointer arithmetic via sizeof(*regs) would skip
 *      past the frame-pointer slot. 21*8 = 168 bytes matches the
 *      SAVE_REGS_STRING-built frame exactly.
 *
 *   3. __KERNEL_DS is not defined under UML (arch/x86/um/asm/segment.h
 *      doesn't provide it). We push UM_RETHOOK_KERNEL_DS = 0x18
 *      (x86's conventional value) purely to keep the trampoline's
 *      push layout identical and the regs->ss slot non-garbage
 *      until the callback overwrites it.
 */

#include <linux/bug.h>
#include <linux/kprobes.h>
#include <linux/objtool.h>
#include <linux/rethook.h>
#include <linux/stringify.h>

#include <asm/ptrace.h>
#include <asm/unwind_hints.h>

/* kprobes/common.h brings in <asm/insn.h> which sets MAX_INSN_SIZE=15,
 * but UML's asm/kprobes.h (pulled in by <linux/kprobes.h> above) sets
 * it to 16. Drop the UML definition so the two headers agree; rethook
 * only needs SAVE/RESTORE_REGS_STRING from common.h, not MAX_INSN_SIZE.
 */
#undef MAX_INSN_SIZE
#include "../../x86/kernel/kprobes/common.h"

/* Objtool macros are no-ops on UML (no IBT, no ORC unwinder), but
 * keep the names so the inline asm stays textually identical to the
 * x86 original. The headers above provide UNWIND_HINT_FUNC; the
 * ENDBR annotation is IBT-only.
 */
#ifndef ANNOTATE_NOENDBR
# define ANNOTATE_NOENDBR
#endif

/* See comment at top of file. */
#define UM_RETHOOK_KERNEL_DS 0x18

__visible void arch_rethook_trampoline_callback(struct pt_regs *regs);

/*
 * When a target function returns, this code saves registers and calls
 * arch_rethook_trampoline_callback(), which calls the rethook handler.
 */
asm(
	".text\n"
	".global arch_rethook_trampoline\n"
	".type arch_rethook_trampoline, @function\n"
	"arch_rethook_trampoline:\n"
	ANNOTATE_NOENDBR "\n"	/* Only reached via ret, never indirect call. */
	/* Push a fake return address so unwinders see the trampoline. */
	"	pushq $arch_rethook_trampoline\n"
	UNWIND_HINT_FUNC
	"	pushq $" __stringify(UM_RETHOOK_KERNEL_DS) "\n"
	/* Save 'sp - 16'; fixed up in the callback. */
	"	pushq %rsp\n"
	"	pushfq\n"
	SAVE_REGS_STRING
	"	movq %rsp, %rdi\n"
	"	call arch_rethook_trampoline_callback\n"
	RESTORE_REGS_STRING
	/* Callback set regs->ss = regs->flags, so popfq restores flags. */
	"	addq $16, %rsp\n"
	"	popfq\n"
	ASM_RET
	".size arch_rethook_trampoline, .-arch_rethook_trampoline\n"
);
NOKPROBE_SYMBOL(arch_rethook_trampoline);

/*
 * Called from arch_rethook_trampoline. The caller passes %rsp (start
 * of the SAVE_REGS_STRING frame) as the pt_regs pointer. See the file
 * header comment for why we can't use (regs + 1) to locate
 * frame_pointer on UML.
 */
__used __visible void arch_rethook_trampoline_callback(struct pt_regs *regs)
{
	unsigned long *frame_pointer;

	/* Fix up the register slots SAVE_REGS_STRING left uninitialized.
	 * UPT_CS / UPT_IP / UPT_ORIG_AX are gp[] indices that match the
	 * subq-reserved slots in SAVE_REGS_STRING; writing them here
	 * completes the pt_regs image.
	 */
	UPT_CS(&regs->regs) = 0;		/* UML has no kernel CS; any value works */
	UPT_IP(&regs->regs) = (unsigned long)&arch_rethook_trampoline;
	UPT_ORIG_AX(&regs->regs) = ~0UL;
	/* Undo the two pushq that preceded SAVE_REGS_STRING so the saved
	 * sp reflects %rsp at the moment of the probed function's ret.
	 */
	UPT_SP(&regs->regs) += 2 * sizeof(long);

	/* Frame pointer (where arch_rethook_fixup_return writes the real
	 * return address) sits immediately above the SAVE_REGS_STRING
	 * frame: offset 21 * sizeof(long) from the top of pt_regs.
	 */
	frame_pointer = (unsigned long *)((char *)regs + 21 * sizeof(unsigned long));

	/* Walk the task's shadow stack, invoke handlers, patch the fake
	 * return address to the real one via arch_rethook_fixup_return.
	 */
	rethook_trampoline_handler(regs, (unsigned long)frame_pointer);

	/* Copy FLAGS to the SS slot so the trampoline's popfq restores
	 * flags.
	 */
	UPT_SS(&regs->regs) = UPT_EFLAGS(&regs->regs);
}
NOKPROBE_SYMBOL(arch_rethook_trampoline_callback);

/*
 * arch_rethook_trampoline() skips updating frame pointer. The frame
 * pointer saved by arch_rethook_trampoline_callback points to the
 * real caller function's frame pointer. So the trampoline doesn't
 * have a standard stack frame with CONFIG_FRAME_POINTER=y. Mark it
 * non-standard; the FP unwinder can still walk correctly via the
 * saved rbp.
 */
STACK_FRAME_NON_STANDARD_FP(arch_rethook_trampoline);

/* Called from rethook_trampoline_handler() to write the real return
 * address into the stack slot that currently holds the fake
 * trampoline address.
 */
void arch_rethook_fixup_return(struct pt_regs *regs,
			       unsigned long correct_ret_addr)
{
	unsigned long *frame_pointer =
		(unsigned long *)((char *)regs + 21 * sizeof(unsigned long));

	/* Replace fake return address with real one. */
	*frame_pointer = correct_ret_addr;
}
NOKPROBE_SYMBOL(arch_rethook_fixup_return);

/* Hijack the return address on probed function entry. regs->sp
 * points at the slot the CPU will ret into; save the original and
 * overwrite with the trampoline address.
 */
void arch_rethook_prepare(struct rethook_node *rh, struct pt_regs *regs,
			  bool mcount)
{
	unsigned long *stack = (unsigned long *)UPT_SP(&regs->regs);

	rh->ret_addr = stack[0];
	rh->frame = UPT_SP(&regs->regs);

	/* Replace the return addr with trampoline addr */
	stack[0] = (unsigned long)arch_rethook_trampoline;
}
NOKPROBE_SYMBOL(arch_rethook_prepare);
