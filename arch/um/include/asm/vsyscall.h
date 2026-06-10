/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML has no vsyscall page. Define VSYSCALL_ADDR to zero so
 * code that does "addr < VSYSCALL_ADDR" bounds checks (notably
 * arch/x86/net/bpf_jit_comp.c's BPF arena-access sanity clamp)
 * compiles against UML and behaves correctly: VSYSCALL_ADDR=0
 * collapses the upper bound into a no-op past-TASK_SIZE check,
 * which is the semantically correct answer on UML.
 */
#ifndef _ASM_UM_VSYSCALL_H
#define _ASM_UM_VSYSCALL_H

#define VSYSCALL_ADDR	0UL

#endif /* _ASM_UM_VSYSCALL_H */
