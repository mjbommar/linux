// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace backend: debug / introspection ops.
 *
 * Workstream A-02.COLD-2.
 *
 *   init_thread_regs   — wraps get_safe_registers() (USER side).
 *   read_guest_regs    — KGDB consumer; not yet wired (no in-tree caller
 *                        as of A-02; the full impl needs a USER helper
 *                        to issue PTRACE_GETREGS for an arbitrary PID).
 *                        Returns -EOPNOTSUPP until C-11 lands KGDB.
 *   write_guest_regs   — symmetric. Returns -EOPNOTSUPP.
 *
 * The stub return-codes mean callers in DYNAMIC builds get a clean
 * error rather than a NULL-deref; in single-backend builds the
 * wrapper still resolves to a real symbol so the dispatch macro
 * works.
 */
#include <linux/errno.h>
#include <linux/types.h>
#include <registers.h>

#include "ptrace_backend.h"

void ptrace_init_thread_regs(unsigned long *gp, unsigned long *fp)
{
	get_safe_registers(gp, fp);
}

int ptrace_read_guest_regs(struct task_struct *t, struct pt_regs *regs)
{
	(void)t;
	(void)regs;
	return -EOPNOTSUPP;
}

int ptrace_write_guest_regs(struct task_struct *t, const struct pt_regs *regs)
{
	(void)t;
	(void)regs;
	return -EOPNOTSUPP;
}
