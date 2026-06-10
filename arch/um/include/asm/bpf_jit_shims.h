/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML-local declarations for the x86 BPF JIT helper symbols implemented
 * by arch/um/kernel/bpf_jit_stubs.c. The native prototypes live in:
 *
 *   arch/x86/include/asm/nospec-branch.h  (clear_bhb_loop)
 *   arch/x86/include/asm/text-patching.h  (text_poke_set,
 *                                          smp_text_poke_single)
 *
 * UML cannot include those headers from the stub translation unit: their
 * dependency graph pulls in x86-only declarations, including
 * __ro_after_init struct mm_struct * and the retpoline_thunk_t typedef
 * that conflicts with UML's dummy indirect-thunk arrays. Keeping these
 * declarations in a UML-specific header gives the stub implementation and
 * UML callers a shared prototype source without .c-local externs.
 *
 * A signature mismatch with the upstream headers would manifest as a
 * link-time resolution against the wrong-typed symbol.
 */
#ifndef _ASM_UM_BPF_JIT_SHIMS_H
#define _ASM_UM_BPF_JIT_SHIMS_H

#include <linux/types.h>

void clear_bhb_loop(void);
void *text_poke_set(void *addr, int c, size_t len);
void smp_text_poke_single(void *addr, const void *opcode,
			  size_t len, const void *emulate);

#endif /* _ASM_UM_BPF_JIT_SHIMS_H */
