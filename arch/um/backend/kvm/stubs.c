// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — stubbed ops pending D-05..D-06.
 *
 * Workstream D-02 wired these up as pr_warn_once + -EOPNOTSUPP
 * canaries; dispatching any of them panics per the A-01 contract.
 * Every stub below will be replaced as the corresponding op
 * migrates across D-05..D-06:
 *
 *   D-05 (partial-landed in this commit set — see below):
 *     ipi_send  — still a stub, will land with SMP timer work
 *   D-06: read_guest_regs, write_guest_regs (KGDB integration)
 *
 * Lifecycle (probe, init, shutdown, vcpu bring-up) in
 * lifecycle.c (D-03a + D-03c + D-04a). All four mm ops live in
 * mm.c (D-03b + D-03d). Thread lifecycle + run_userspace live
 * in thread.c (D-04a). Time ops (read_clock_ns,
 * read_persistent_clock_ns, set_timer) live in time.c (D-05a).
 */
#include <linux/err.h>
#include <linux/printk.h>
#include <asm/backend.h>

#include "kvm_backend.h"

/*
 * Every hot + cold op returning -EOPNOTSUPP below is a load-
 * bearing canary: if anyone reaches one, we want a loud +
 * diagnosable failure. They can't be NULL (the arbiter's
 * validate_hot_ops() would panic) so the pattern is "populate
 * the slot, return -EOPNOTSUPP, log which op tripped."
 *
 * The stubs are written inline (rather than via a control-flow
 * macro) so each can be replaced independently across D-05..D-06
 * without a macro-rename churn commit in the middle.
 */

int kvm_ipi_send(int cpu, int vector)
{
	pr_warn_once("um: kvm ipi_send not implemented yet (see design-memo.md)\n");
	return -EOPNOTSUPP;
}

int kvm_read_guest_regs(struct task_struct *t, struct pt_regs *regs)
{
	pr_warn_once("um: kvm read_guest_regs not implemented yet (see design-memo.md)\n");
	return -EOPNOTSUPP;
}

int kvm_write_guest_regs(struct task_struct *t, const struct pt_regs *regs)
{
	pr_warn_once("um: kvm write_guest_regs not implemented yet (see design-memo.md)\n");
	return -EOPNOTSUPP;
}
