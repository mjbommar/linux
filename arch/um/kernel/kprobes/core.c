// SPDX-License-Identifier: GPL-2.0
/*
 * UML kprobes arch layer: int3 + single-step path.
 *
 * Every arch hook returns a sensible error until its implementation is
 * wired, so the tree builds with CONFIG_KPROBES=y and unsupported
 * operations fail cleanly.
 *
 * References:
 *   - arch/x86/kernel/kprobes/core.c (primary reference)
 *   - Documentation/virt/uml/kprobes.rst
 */

#include <linux/kprobes.h>
#include <linux/percpu.h>
#include <linux/ptrace.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/kprobes.h>
#include <asm/patchable.h>
#include <asm/processor-flags.h>	/* X86_EFLAGS_TF, X86_EFLAGS_IF */

/* Access guest EFLAGS via UML's uml_pt_regs.gp[HOST_EFLAGS] layout.
 * arch/x86/um/shared/sysdep/ptrace.h already defines UPT_EFLAGS for
 * uml_pt_regs; here we wrap it for the struct-pt_regs-wrapping-
 * uml-pt-regs container that kprobes handlers receive.
 */
#define KPROBE_REGS_FLAGS(r)	UPT_EFLAGS(&(r)->regs)

/*
 * Per-CPU kprobe control block storage. Declared extern in
 * <linux/kprobes.h>; each arch provides the actual definition.
 */
DEFINE_PER_CPU(struct kprobe_ctlblk, kprobe_ctlblk);

/*
 * Per-CPU pointer to the currently-in-flight kprobe. Generic
 * kprobes core reads/writes it via __this_cpu_* accessors; each
 * arch defines the storage.
 */
DEFINE_PER_CPU(struct kprobe *, current_kprobe);

/*
 * kretprobe blacklist. UML does not need arch-specific blacklist entries
 * for the int3 port; the generic blacklist remains available for functions
 * whose return addresses cannot be safely probed.
 */
struct kretprobe_blackpoint kretprobe_blacklist[] = { };
const int kretprobe_blacklist_size = ARRAY_SIZE(kretprobe_blacklist);

#ifdef CONFIG_KRETPROBES
/*
 * Arch hook that tells the kprobes core whether a given kprobe sits
 * inside a trampoline (and so must not be handled via the normal
 * re-entry path). With CONFIG_KRETPROBE_ON_RETHOOK=y the arch
 * trampoline is owned by rethook, not kprobes; matching x86 we
 * always answer "no". When CONFIG_KRETPROBES=n, <linux/kprobes.h>
 * provides a static inline definition, so this one is guarded.
 */
int arch_trampoline_kprobe(struct kprobe *p)
{
	return 0;
}
NOKPROBE_SYMBOL(arch_trampoline_kprobe);
#endif

/*
 * Allocate an out-of-line instruction slot and copy the original
 * bytes. The generic kprobes core's insn-slot cache (backed by
 * execmem_alloc(EXECMEM_KPROBES)) gives us an RWX page-backed slot
 * guaranteed to be within 2GB of the kernel image, enough for
 * rip-relative instructions to remain correct when single-stepped
 * out of line.
 *
 * We copy MAX_INSN_SIZE bytes (x86_64 max instruction length = 15,
 * we round up to 16). This path relies
 * on TF-driven single-step to trap after the first instruction,
 * then uses (regs->ip - insn_slot) as the effective length.
 */
int arch_prepare_kprobe(struct kprobe *p)
{
	p->ainsn.insn = get_insn_slot();
	if (!p->ainsn.insn)
		return -ENOMEM;

	memcpy(p->ainsn.insn, p->addr, MAX_INSN_SIZE);
	p->ainsn.size = MAX_INSN_SIZE;
	p->opcode = *(kprobe_opcode_t *)p->addr;

	return 0;
}

/*
 * Install the int3 (0xcc) breakpoint. UML has no text_poke_bp; we
 * use the section-split mprotect helpers to open the one page containing
 * p->addr RW, write the single byte, and restore RX. Caller
 * (kernel/kprobes.c) already holds text_mutex; UML is
 * effectively UP-serialized for the purposes of this write, so no
 * stop_machine needed for a 1-byte atomic store.
 */
void arch_arm_kprobe(struct kprobe *p)
{
	kprobe_opcode_t int3 = BREAKPOINT_INSTRUCTION;

	if (um_kernel_text_patch_begin(p->addr, sizeof(int3)))
		return;
	*(kprobe_opcode_t *)p->addr = int3;
	um_kernel_text_patch_end(p->addr, sizeof(int3));
}

void arch_disarm_kprobe(struct kprobe *p)
{
	if (um_kernel_text_patch_begin(p->addr, sizeof(p->opcode)))
		return;
	*(kprobe_opcode_t *)p->addr = p->opcode;
	um_kernel_text_patch_end(p->addr, sizeof(p->opcode));
}

void arch_remove_kprobe(struct kprobe *p)
{
	if (p->ainsn.insn) {
		free_insn_slot(p->ainsn.insn, 0);
		p->ainsn.insn = NULL;
	}
}

/*
 * Save current kprobe state before re-entering.
 */
static nokprobe_inline void save_previous_kprobe(struct kprobe_ctlblk *kcb)
{
	kcb->prev_kprobe.kp = kprobe_running();
	kcb->prev_kprobe.status = kcb->kprobe_status;
	kcb->prev_kprobe.old_flags = kcb->kprobe_old_flags;
	kcb->prev_kprobe.saved_flags = kcb->kprobe_saved_flags;
}

static nokprobe_inline void restore_previous_kprobe(struct kprobe_ctlblk *kcb)
{
	__this_cpu_write(current_kprobe, kcb->prev_kprobe.kp);
	kcb->kprobe_status = kcb->prev_kprobe.status;
	kcb->kprobe_old_flags = kcb->prev_kprobe.old_flags;
	kcb->kprobe_saved_flags = kcb->prev_kprobe.saved_flags;
}

static nokprobe_inline void set_current_kprobe(struct kprobe *p,
					       struct pt_regs *regs,
					       struct kprobe_ctlblk *kcb)
{
	__this_cpu_write(current_kprobe, p);
	kcb->kprobe_old_flags =
		KPROBE_REGS_FLAGS(regs) & (X86_EFLAGS_TF | X86_EFLAGS_IF);
	kcb->kprobe_saved_flags = kcb->kprobe_old_flags;
}

/*
 * Set up out-of-line single-step. We redirect the guest IP to the
 * copied instruction, clear IF so interrupts don't fire mid-step,
 * and set TF so the CPU delivers another SIGTRAP after one
 * instruction. X86_EFLAGS_TF propagates through UML's mcontext/regs
 * roundtrip (arch/x86/um/os-Linux/mcontext.c:81..84) without
 * arch-specific emulation.
 */
static void setup_singlestep(struct kprobe *p, struct pt_regs *regs,
			     struct kprobe_ctlblk *kcb, int reenter)
{
	if (reenter) {
		save_previous_kprobe(kcb);
		set_current_kprobe(p, regs, kcb);
		kcb->kprobe_status = KPROBE_REENTER;
	} else {
		kcb->kprobe_status = KPROBE_HIT_SS;
	}

	/* Clear IF during single-step so interrupts don't re-enter
	 * the trap path while we're in the middle of servicing a
	 * probe. Set TF so the CPU delivers SIGTRAP (TRAP_TRACE) after
	 * the next instruction.
	 */
	KPROBE_REGS_FLAGS(regs) &= ~X86_EFLAGS_IF;
	KPROBE_REGS_FLAGS(regs) |= X86_EFLAGS_TF;

	instruction_pointer(regs) = (unsigned long)p->ainsn.insn;
}
NOKPROBE_SYMBOL(setup_singlestep);

static void kprobe_post_process(struct kprobe *p, struct pt_regs *regs,
				struct kprobe_ctlblk *kcb)
{
	if (kcb->kprobe_status == KPROBE_REENTER) {
		/* Pops the re-entered kprobe back to the outer one. */
		restore_previous_kprobe(kcb);
	} else {
		if (p->post_handler)
			p->post_handler(p, regs, 0);
		reset_current_kprobe();
	}
}
NOKPROBE_SYMBOL(kprobe_post_process);

/*
 * Handle a re-entered kprobe: one handler being probed by another.
 * We silently single-step the inner probe without invoking its
 * user handlers, since the outer probe owns the kcb state.
 */
static int reenter_kprobe(struct kprobe *p, struct pt_regs *regs,
			  struct kprobe_ctlblk *kcb)
{
	switch (kcb->kprobe_status) {
	case KPROBE_HIT_SSDONE:
	case KPROBE_HIT_ACTIVE:
	case KPROBE_HIT_SS:
		kprobes_inc_nmissed_count(p);
		setup_singlestep(p, regs, kcb, 1);
		break;
	case KPROBE_REENTER:
		/* Nested re-entry: the kprobes machinery cannot recover
		 * from a probe inside a probe handler inside a probe
		 * handler. Raise BUG before we stack overflow.
		 */
		pr_err("Unrecoverable kprobe detected.\n");
		dump_kprobe(p);
		BUG();
	default:
		WARN_ON(1);
		return 0;
	}
	return 1;
}
NOKPROBE_SYMBOL(reenter_kprobe);

/*
 * Entry point for int3 (0xcc) traps in kernel mode.
 *
 * Called from arch/um/kernel/trap.c's relay_signal() when SIGTRAP
 * arrives from kernel mode. The int3 instruction advances the guest
 * IP past the 0xcc byte before the trap is delivered, so the real
 * probe address is IP - 1.
 *
 * Return: 1 if we consumed the trap (a kprobe is installed at IP-1),
 * 0 if not (relay_signal should continue to its panic path).
 */
int kprobe_int3_handler(struct pt_regs *regs)
{
	struct kprobe *p;
	struct kprobe_ctlblk *kcb;
	kprobe_opcode_t *addr;

	addr = (kprobe_opcode_t *)(instruction_pointer(regs) -
				   sizeof(kprobe_opcode_t));

	p = get_kprobe(addr);

	if (!p)
		return 0;

	/*
	 * Generic kprobes contract: pre/post handlers run with preemption
	 * disabled. On architectural int3 this is
	 * provided by the exception entry path; UML's SIGTRAP handler
	 * is just a signal handler, so preempt state is whatever the
	 * interrupted guest-kernel context had. Disable explicitly
	 * before touching per-CPU kprobe state or running user handlers.
	 *
	 * Matched by preempt_enable_notrace() on every return path.
	 */
	preempt_disable_notrace();

	kcb = get_kprobe_ctlblk();

	if (kprobe_running()) {
		/*
		 * Re-entry: reenter_kprobe() sets up the inner probe's
		 * single-step. The matching preempt_enable_notrace()
		 * happens in kprobe_debug_handler() when the inner
		 * single-step completes (status == KPROBE_REENTER
		 * restores the outer state via restore_previous_kprobe).
		 * Do NOT re-enable here; a double-enable would
		 * underflow preempt_count across the re-entry pair.
		 */
		return reenter_kprobe(p, regs, kcb);
	}

	set_current_kprobe(p, regs, kcb);
	kcb->kprobe_status = KPROBE_HIT_ACTIVE;

	/* Rewind IP back to the probe address; pre_handler and
	 * single-step should see it pointing at the probed
	 * instruction, not one byte past.
	 */
	instruction_pointer(regs) = (unsigned long)addr;

	/* Pre-handler may ask us to skip the single-step (for example
	 * a jprobe-style emulation that already adjusted regs). Returns
	 * non-zero to skip.
	 */
	if (!p->pre_handler || !p->pre_handler(p, regs)) {
		setup_singlestep(p, regs, kcb, 0);
		/*
		 * The probed instruction will single-step out-of-line and
		 * trap back into kprobe_debug_handler(), which does the
		 * matching preempt_enable_notrace() for this path. Do NOT
		 * re-enable here.
		 */
	} else {
		reset_current_kprobe();
		preempt_enable_notrace();
	}

	return 1;
}
NOKPROBE_SYMBOL(kprobe_int3_handler);

/*
 * Entry point for SIGTRAP delivered after a TF-driven single-step
 * of the out-of-line instruction copy. Called from relay_signal()
 * before its kernel-mode panic path.
 *
 * Fix up the IP so control resumes at the instruction following
 * the original probed instruction, clear our TF bit, restore the
 * saved IF, and run the post-handler.
 */
int kprobe_debug_handler(struct pt_regs *regs)
{
	struct kprobe *p;
	struct kprobe_ctlblk *kcb;
	unsigned long insn_length;

	kcb = get_kprobe_ctlblk();
	p = kprobe_running();

	if (!p)
		return 0;

	if (kcb->kprobe_status != KPROBE_HIT_SS &&
	    kcb->kprobe_status != KPROBE_REENTER)
		return 0;

	/* How many bytes the single-stepped instruction consumed.
	 * With TF, the CPU trapped after executing exactly one
	 * instruction; regs->ip points past that instruction in the
	 * out-of-line copy.
	 */
	insn_length = instruction_pointer(regs) - (unsigned long)p->ainsn.insn;
	instruction_pointer(regs) = (unsigned long)p->addr + insn_length;

	/* Clear our TF; restore the IF the traced function had. */
	KPROBE_REGS_FLAGS(regs) &= ~X86_EFLAGS_TF;
	KPROBE_REGS_FLAGS(regs) |= (kcb->kprobe_saved_flags & X86_EFLAGS_IF);

	kprobe_post_process(p, regs, kcb);
	/*
	 * Matched pair to the preempt_disable_notrace() in
	 * kprobe_int3_handler(). Preemption was disabled when the int3
	 * fired; this is the single-step-completion path and the
	 * natural place to re-enable it.
	 */
	preempt_enable_notrace();
	return 1;
}
NOKPROBE_SYMBOL(kprobe_debug_handler);

/*
 * Called from do_page_fault when a fault occurs inside a kprobe
 * handler. UML has no arch-specific fault fixups for the int3 path.
 */
int kprobe_fault_handler(struct pt_regs *regs, int trapnr)
{
	return 0;
}
NOKPROBE_SYMBOL(kprobe_fault_handler);

int __init arch_init_kprobes(void)
{
	return 0;
}
