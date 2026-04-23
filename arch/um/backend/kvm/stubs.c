// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — stubbed ops pending D-05..D-06.
 *
 * Workstream D-02 wired these up as pr_warn_once + -EOPNOTSUPP
 * canaries; dispatching any of them panics per the A-01 contract.
 * Every stub below will be replaced as the corresponding op
 * migrates across D-05..D-06:
 *
 *   D-05: ipi_send, set_timer, read_clock_ns,
 *         read_persistent_clock_ns
 *   D-06: read_guest_regs, write_guest_regs (KGDB integration)
 *
 * Lifecycle (probe, init, shutdown, vcpu bring-up) in
 * lifecycle.c (D-03a + D-03c + D-04a). All four mm ops live in
 * mm.c (D-03b + D-03d). Thread lifecycle + run_userspace live
 * in thread.c (D-04a).
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

u64 kvm_read_clock_ns(void)
{
	pr_warn_once("um: kvm read_clock_ns not implemented yet (see design-memo.md)\n");
	return 0;
}

int kvm_set_timer(int cpu, u64 deadline_ns, enum um_timer_mode mode)
{
	pr_warn_once("um: kvm set_timer not implemented yet (see design-memo.md)\n");
	return -EOPNOTSUPP;
}

u64 kvm_read_persistent_clock_ns(void)
{
	pr_warn_once("um: kvm read_persistent_clock_ns not implemented yet (see design-memo.md)\n");
	return 0;
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
