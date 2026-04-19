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

/*
 * Under -fpatchable-function-entry=5,0 every traced function's entry
 * already contains a 5-byte NOP at compile time. The generic
 * ftrace_init_nop() fallback calls ftrace_make_nop() on every record
 * during ftrace_init(), which on UML means 21000+ mprotect round
 * trips against the host process's one mm — fragmenting the VMA
 * list and, in the research profile, hanging boot.
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
