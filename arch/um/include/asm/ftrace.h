/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_FTRACE_H
#define __ASM_UM_FTRACE_H

/*
 * UML function tracer arch bits (workstream C-05).
 *
 * UML uses -fpatchable-function-entry=5,0 (decisions-log D29) rather
 * than -pg -mfentry + recordmcount, because UML's -mcmodel=large
 * breaks the -mfentry emission. The compiler places a 5-byte NOP at
 * every traced function's entry and records its address in the
 * __patchable_function_entries section. At runtime the kernel patches
 * that NOP to `call ftrace_caller` (and back to a NOP when disabling).
 */

#ifdef CONFIG_FUNCTION_TRACER

#define MCOUNT_ADDR		((unsigned long)(ftrace_caller))
#define MCOUNT_INSN_SIZE	5	/* 5-byte NOP or `call rel32` */

#ifndef __ASSEMBLER__

/* ftrace_caller and ftrace_stub are declared by <linux/ftrace.h>;
 * MCOUNT_ADDR above takes ftrace_caller's address via that decl.
 *
 * ftrace_call is an inner label inside ftrace_caller (see
 * arch/um/kernel/mcount.S). ftrace_update_ftrace_func() patches the
 * `call` at this address to point at the active dispatcher.
 */
void ftrace_call(void);

static inline unsigned long ftrace_call_adjust(unsigned long addr)
{
	/*
	 * With -fpatchable-function-entry=5,0 the compiler records the
	 * address of the first (and only) NOP region, which is already
	 * the address we want to patch. No adjustment needed.
	 */
	return addr;
}

#ifdef CONFIG_DYNAMIC_FTRACE

struct dyn_arch_ftrace {
	/* No extra per-record data needed on UML. */
};

#endif /* CONFIG_DYNAMIC_FTRACE */

#endif /* !__ASSEMBLER__ */

#endif /* CONFIG_FUNCTION_TRACER */

#endif /* __ASM_UM_FTRACE_H */
