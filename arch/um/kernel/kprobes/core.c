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
#include <linux/types.h>

#include <asm/kprobes.h>

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
 * Arch hooks required by kernel/kprobes.c. Commit 1a stubs them with
 * WARN_ONCE + -EOPNOTSUPP so a premature register_kprobe() fails
 * loudly rather than silently.
 */

int arch_prepare_kprobe(struct kprobe *p)
{
	WARN_ONCE(1, "um: kprobes: arch_prepare_kprobe stub (commit 1b wires this up)\n");
	return -EOPNOTSUPP;
}

void arch_arm_kprobe(struct kprobe *p)
{
	WARN_ONCE(1, "um: kprobes: arch_arm_kprobe stub\n");
}

void arch_disarm_kprobe(struct kprobe *p)
{
	WARN_ONCE(1, "um: kprobes: arch_disarm_kprobe stub\n");
}

void arch_remove_kprobe(struct kprobe *p)
{
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
