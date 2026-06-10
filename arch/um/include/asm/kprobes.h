/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_KPROBES_H
#define __ASM_UM_KPROBES_H

/*
 * UML kprobes arch header.
 *
 * UML runs the kernel as a host userspace process; int3 (0xCC)
 * executed by the guest kernel becomes a host SIGTRAP delivered
 * to the UML process. arch/um/kernel/trap.c's relay_signal()
 * routes kernel-mode SIGTRAP into kprobe_int3_handler() below.
 *
 * The port is deliberately minimal: no booster, no instruction
 * emulation, no optprobes. Every probed instruction single-steps
 * through an out-of-line copy using the host's X86_EFLAGS_TF
 * propagation (arch/x86/um/os-Linux/mcontext.c:81..84) to deliver
 * the post-step SIGTRAP.
 */

#include <asm-generic/kprobes.h>

#ifdef CONFIG_KPROBES

#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/percpu.h>

typedef u8 kprobe_opcode_t;

/* x86_64 int3 opcode. */
#define BREAKPOINT_INSTRUCTION	0xcc

/* Maximum instruction bytes we copy for out-of-line single-step.
 * x86_64 instructions are at most 15 bytes.
 */
#define MAX_INSN_SIZE		16

/* Let the generic kprobe insn-slot cache allocate our copy buffers
 * (kernel/kprobes.c's alloc_insn_page via execmem).
 */
#define __ARCH_WANT_KPROBES_INSN_SLOT

#define flush_insn_slot(p)	do { } while (0)

struct pt_regs;
struct kprobe;

/* Per-kprobe arch-specific storage. We keep this small: the
 * out-of-line instruction copy plus its length. UML uses
 * single-step for every probe.
 */
struct arch_specific_insn {
	kprobe_opcode_t	*insn;		/* out-of-line copy, MAX_INSN_SIZE */
	unsigned char	size;		/* bytes of original instruction */
};

/* Saved previous-kprobe state for re-entry (a kprobe handler that
 * itself triggers a kprobe).
 */
struct prev_kprobe {
	struct kprobe		*kp;
	unsigned long		status;
	unsigned long		old_flags;
	unsigned long		saved_flags;
};

/* Per-CPU kprobe control block. */
struct kprobe_ctlblk {
	unsigned long		kprobe_status;
	unsigned long		kprobe_old_flags;
	unsigned long		kprobe_saved_flags;
	struct prev_kprobe	prev_kprobe;
};

/* Entry points called from arch/um/kernel/trap.c's relay_signal(). */
int kprobe_int3_handler(struct pt_regs *regs);
int kprobe_debug_handler(struct pt_regs *regs);
int kprobe_fault_handler(struct pt_regs *regs, int trapnr);

void arch_remove_kprobe(struct kprobe *p);

/* Size of the arch-specific kretprobe blacklist. Defined alongside
 * kretprobe_blacklist[] in arch/um/kernel/kprobes/core.c. Kept as a
 * zero-length array on UML; see core.c for the rationale.
 */
extern const int kretprobe_blacklist_size;

#endif /* CONFIG_KPROBES */

#endif /* __ASM_UM_KPROBES_H */
