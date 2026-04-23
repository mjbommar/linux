// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — stubbed ops pending D-03..D-06.
 *
 * Workstream D-02 intentionally ships a scaffold: the struct
 * um_backend_ops table in kvm_backend.c points at the functions
 * here, but every op except probe + init + shutdown returns
 * -EOPNOTSUPP. Dispatching to any of them panics per the
 * backend-contract (dispatch macro calls the fn directly without
 * synthesizing errno; the contract says cold-op NULL is a bug in
 * the arbiter and hot-op NULL is a bug in the backend).
 *
 * The effect is: `backend=kvm` boots successfully through
 * init_backend() (probe opens /dev/kvm, init stashes the fd),
 * then the first dispatch to any hot op (e.g. mm_map during arch
 * setup) will panic. That's the signal that the data path isn't
 * ready yet. Users who select kvm today are explicitly testing
 * the scaffold.
 *
 * Every stub below will be replaced as the corresponding op
 * migrates across D-03..D-06:
 *
 *   D-03: mm_attach, mm_detach, mm_map, mm_unmap
 *   D-04: run_userspace, context_switch, thread_create,
 *         thread_start_idle, init_thread_regs
 *   D-05: ipi_send, set_timer, read_clock_ns,
 *         read_persistent_clock_ns
 *   D-06: read_guest_regs, write_guest_regs (KGDB integration)
 */
#include <linux/err.h>
#include <linux/printk.h>
#include <asm/backend.h>

#include "kvm_backend.h"

/*
 * Lifecycle for the D-02 scaffold. Deliberately light-touch: we
 * don't open /dev/kvm here because the USER-side host-fd work
 * belongs with D-03 when there's actual host state to manage
 * (memslots, vCPUs). Doing it at D-02-probe time would add a
 * cross-TU extern dance for no scaffold-stage value and has
 * already bit us on the first try (early-probe segfault in the
 * USER-side helper's link context).
 *
 * What this scaffold's probe + init DO provide:
 *   - A non-NULL ops-table entry so validate_hot_ops() accepts
 *     the table.
 *   - A `um: backend = kvm (contract v1)` pr_info on successful
 *     init, so `backend=kvm` boot arg has a visible effect.
 *   - A hard panic on any hot-op dispatch via the stubs below,
 *     which tells the user they've selected an incomplete
 *     backend (intentional signal per design-memo.md).
 *
 * Real /dev/kvm bring-up lands in D-03 with memslot plumbing.
 */
int kvm_probe(void)
{
	pr_info("um: kvm backend probe: scaffold (/dev/kvm open deferred to D-03)\n");
	return 0;
}

int kvm_init(const struct um_backend_args *args)
{
	(void)args;
	pr_info("um: kvm backend init: scaffold complete, data path TBD (see design-memo.md)\n");
	return 0;
}

void kvm_shutdown(void)
{
	/* Nothing to release — the scaffold holds no host state. */
}

/*
 * Every hot + cold op returning -EOPNOTSUPP below is a load-
 * bearing canary: if anyone reaches one, we want a loud +
 * diagnosable failure. They can't be NULL (the arbiter's
 * validate_hot_ops() would panic) so the pattern is "populate
 * the slot, return -EOPNOTSUPP, log which op tripped."
 *
 * The stubs are written inline (rather than via a control-flow
 * macro) so each can be replaced independently across D-03..D-06
 * without a macro-rename churn commit in the middle.
 */

void kvm_run_userspace(struct uml_pt_regs *regs)
{
	pr_warn_once("um: kvm run_userspace not implemented yet (see design-memo.md)\n");
}

int kvm_mm_attach(struct mm_id *id)
{
	pr_warn_once("um: kvm mm_attach not implemented yet (see design-memo.md)\n");
	return -EOPNOTSUPP;
}

void kvm_mm_detach(struct mm_id *id)
{
	pr_warn_once("um: kvm mm_detach not implemented yet (see design-memo.md)\n");
}

int kvm_mm_map(struct mm_id *id, unsigned long va, unsigned long len,
	       int prot, int phys_fd, u64 offset)
{
	pr_warn_once("um: kvm mm_map not implemented yet (see design-memo.md)\n");
	return -EOPNOTSUPP;
}

int kvm_mm_unmap(struct mm_id *id, unsigned long va, unsigned long len)
{
	pr_warn_once("um: kvm mm_unmap not implemented yet (see design-memo.md)\n");
	return -EOPNOTSUPP;
}

int kvm_thread_create(struct task_struct *p, void *stack,
		      void (*handler)(void))
{
	pr_warn_once("um: kvm thread_create not implemented yet (see design-memo.md)\n");
	return -EOPNOTSUPP;
}

int kvm_thread_start_idle(void *stack, struct thread_struct *t)
{
	pr_warn_once("um: kvm thread_start_idle not implemented yet (see design-memo.md)\n");
	return -EOPNOTSUPP;
}

void kvm_context_switch(struct task_struct *prev, struct task_struct *next)
{
	pr_warn_once("um: kvm context_switch not implemented yet (see design-memo.md)\n");
}

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

void kvm_init_thread_regs(unsigned long *gp, unsigned long *fp)
{
	pr_warn_once("um: kvm init_thread_regs not implemented yet (see design-memo.md)\n");
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
