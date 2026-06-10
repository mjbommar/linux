/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_FTRACE_H
#define __ASM_UM_FTRACE_H

/*
 * UML function tracer arch bits.
 *
 * UML uses -fpatchable-function-entry=5,0 rather than -pg -mfentry +
 * recordmcount, because UML's -mcmodel=large
 * breaks the -mfentry emission. The compiler places a 5-byte NOP at
 * every traced function's entry and records its address in the
 * __patchable_function_entries section. At runtime the kernel patches
 * that NOP to call ftrace_caller (and back to a NOP when disabling).
 */

#ifdef CONFIG_FUNCTION_TRACER

#define MCOUNT_ADDR		((unsigned long)(ftrace_caller))
#define MCOUNT_INSN_SIZE	5	/* 5-byte NOP or call rel32 */

#ifndef __ASSEMBLER__

/* ftrace_caller and ftrace_stub are declared by <linux/ftrace.h>;
 * MCOUNT_ADDR above takes ftrace_caller's address via that decl.
 *
 * ftrace_call is an inner label inside ftrace_caller (see
 * arch/um/kernel/mcount.S). ftrace_update_ftrace_func() patches the
 * call at this address to point at the active dispatcher.
 */
void ftrace_call(void);

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
/*
 * ftrace_graph_call is an inner label inside ftrace_caller: the
 * 5-byte patchable JMP site that ftrace_enable_ftrace_graph_caller
 * rewrites from jmp ftrace_stub (the no-op return path) to
 * jmp ftrace_graph_caller when the graph tracer activates.
 *
 * ftrace_graph_caller and return_to_handler are the trampolines
 * defined in arch/um/kernel/mcount.S; arch/um/kernel/ftrace.c's
 * enable/disable hooks need their addresses for patching and
 * fgraph parent-slot rewriting.
 */
void ftrace_graph_call(void);
void ftrace_graph_caller(void);
void return_to_handler(void);

/*
 * prepare_ftrace_return is the C-side fgraph hook that
 * ftrace_graph_caller (mcount.S) tail-calls on every traced
 * function entry. Mirrors arch/x86/include/asm/ftrace.h's
 * declaration so generic trace code and the UML implementation see
 * the same declaration.
 */
void prepare_ftrace_return(unsigned long ip, unsigned long *parent,
			   unsigned long frame_pointer);
#endif

static inline unsigned long ftrace_call_adjust(unsigned long addr)
{
	/*
	 * With -fpatchable-function-entry=5,0 the compiler records the
	 * address of the first (and only) NOP region, which is already
	 * the address to patch. No adjustment needed.
	 */
	return addr;
}

#ifdef CONFIG_DYNAMIC_FTRACE

struct dyn_arch_ftrace {
	/* No extra per-record data needed on UML. */
};

/*
 * Under -fpatchable-function-entry=5,0 every traced function's entry
 * already contains a 5-byte NOP at compile time. The generic
 * ftrace_init_nop() fallback calls ftrace_make_nop() on every record
 * during ftrace_init(), which on UML means 21000+ mprotect round
 * trips against the host process's one mm and can fragment the VMA
 * list badly enough to stall boot.
 *
 * Override with a no-op (see arch/um/kernel/ftrace.c): if a site is
 * ever observed to hold anything other than the expected NOP5 at
 * init (corrupted build, in-tree poisoning), ftrace_make_call on
 * first enable will notice the mismatch via the generic ftrace
 * core's own verification path.
 */
#define ftrace_init_nop ftrace_init_nop
struct module;
struct dyn_ftrace;
int ftrace_init_nop(struct module *mod, struct dyn_ftrace *rec);

#endif /* CONFIG_DYNAMIC_FTRACE */

#endif /* !__ASSEMBLER__ */

#endif /* CONFIG_FUNCTION_TRACER */

#endif /* __ASM_UM_FTRACE_H */
