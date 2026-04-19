// SPDX-License-Identifier: GPL-2.0
/*
 * UML kprobes arch layer — int3 + single-step path (workstream C-04).
 *
 * Commit 1a scope: skeleton only. Every arch hook is stubbed to return
 * a sensible error so the tree builds with CONFIG_KPROBES=y but
 * register_kprobe() fails cleanly. Commits 1b and 1c fill in
 * arch_prepare/arm/disarm, the relay_signal() hook in trap.c, and the
 * single-step completion path.
 *
 * References:
 *   - arch/x86/kernel/kprobes/core.c (primary reference)
 *   - Documentation/virt/uml/redesign/02-workstreams/
 *     C-profiles-and-gaps/04-port-kprobes.md (design)
 *   - Documentation/virt/uml/redesign/04-risks/decisions-log.md §D32
 */

#include <linux/kprobes.h>
#include <linux/percpu.h>
#include <linux/ptrace.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/kprobes.h>
#include <asm/patchable.h>

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
 * kretprobe blacklist. Kept empty on UML for now — the blacklist
 * is a protection for functions whose return addresses cannot be
 * safely probed (e.g. recursive printk helpers on some arches).
 * UML does not need arch-specific blacklist entries for the
 * minimal int3 port; C-04's kretprobes commit populates this if
 * testing surfaces entries that must be excluded.
 */
struct kretprobe_blackpoint kretprobe_blacklist[] = { };
const int kretprobe_blacklist_size = ARRAY_SIZE(kretprobe_blacklist);

/*
 * Allocate an out-of-line instruction slot and copy the original
 * bytes. The generic kprobes core's insn-slot cache (backed by
 * execmem_alloc(EXECMEM_KPROBES)) gives us an RWX page-backed slot
 * guaranteed to be within ±2GB of the kernel image — enough for
 * rip-relative instructions to remain correct when single-stepped
 * out of line.
 *
 * We copy MAX_INSN_SIZE bytes (x86_64 max instruction length = 15,
 * we round up to 16). Commit 1b doesn't decode; commit 1c relies
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
 * use the B-04 C-05-extended mprotect helpers to open the one
 * page containing p->addr RW, write the single byte, and restore
 * RX. Caller (kernel/kprobes.c) already holds text_mutex; UML is
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
 * Called from arch/um/kernel/trap.c's relay_signal() when a kernel-
 * mode SIGTRAP arrives. Commit 1b wires relay_signal()'s kernel-SIGTRAP
 * branch into this function; commit 1c grows the body to dispatch
 * to pre_handler + setup_singlestep.
 *
 * Return: 1 if this was a kprobe we handled, 0 if not (caller should
 * fall through to its normal kernel-SIGTRAP panic path).
 */
int kprobe_int3_handler(struct pt_regs *regs)
{
	return 0;
}
NOKPROBE_SYMBOL(kprobe_int3_handler);

/*
 * Called after a successful single-step of the original instruction.
 * Commit 1c fills this in; currently a no-op.
 */
int kprobe_debug_handler(struct pt_regs *regs)
{
	return 0;
}
NOKPROBE_SYMBOL(kprobe_debug_handler);

/*
 * Called from do_page_fault when a fault occurs inside a kprobe
 * handler. Minimal stub for now — commit 1c may grow it if the
 * single-step path requires fault-fixup.
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
