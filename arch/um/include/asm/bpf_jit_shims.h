/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML-local forward declarations for the three x86 symbols that
 * arch/um/kernel/bpf_jit_stubs.c provides shim implementations
 * of. The upstream prototypes live in:
 *
 *   arch/x86/include/asm/nospec-branch.h  (clear_bhb_loop)
 *   arch/x86/include/asm/text-patching.h  (text_poke_set,
 *                                          smp_text_poke_single)
 *
 * We cannot include those headers from the stub TU because they
 * transitively pull declarations UML's include path doesn't
 * fully assemble (``__ro_after_init`` ``struct mm_struct *``,
 * the ``retpoline_thunk_t`` typedef that conflicts with our
 * dummy indirect-thunk arrays, …). Extracting the three
 * prototypes into a UML-specific header keeps both the definer
 * (arch/um/kernel/bpf_jit_stubs.c) and any future caller-side
 * UML TU in sync, while avoiding a checkpatch
 * "externs should be avoided in .c files" warning on the stub
 * TU.
 *
 * A signature mismatch with the upstream headers would manifest
 * as a link-time resolution against the wrong-typed symbol;
 * verified by visual diff against the upstream headers as of
 * 2026-04-21.
 *
 * Workstream C-06 per Documentation/virt/uml/redesign/
 * 04-risks/decisions-log.md D43 fifth-view.
 */
#ifndef _ASM_UM_BPF_JIT_SHIMS_H
#define _ASM_UM_BPF_JIT_SHIMS_H

#include <linux/types.h>

void clear_bhb_loop(void);
void *text_poke_set(void *addr, int c, size_t len);
void smp_text_poke_single(void *addr, const void *opcode,
			  size_t len, const void *emulate);

#endif /* _ASM_UM_BPF_JIT_SHIMS_H */
