// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: debug / introspection ops.
 *
 * Workstream A-03.S1.5.
 *
 *   init_thread_regs   — wraps get_safe_registers(), shared with ptrace.
 *   read_guest_regs    — KGDB consumer; not yet wired (no in-tree caller
 *                        as of A-03; full impl will use get_stub_state()
 *                        with proper turnstile serialization). Returns
 *                        -EOPNOTSUPP until C-11 lands KGDB.
 *   write_guest_regs   — symmetric. Returns -EOPNOTSUPP.
 *
 * USER TU because get_safe_registers() is in arch/um/os-Linux/registers.c
 * (USER); the future real read/write_guest_regs impls will need to call
 * USER-side get_stub_state()/set_stub_state() too.
 */
#include <errno.h>
#include <registers.h>

#include "seccomp_backend.h"

void seccomp_init_thread_regs(unsigned long *gp, unsigned long *fp)
{
	get_safe_registers(gp, fp);
}

int seccomp_read_guest_regs(struct task_struct *t, struct pt_regs *regs)
{
	(void)t;
	(void)regs;
	return -EOPNOTSUPP;
}

int seccomp_write_guest_regs(struct task_struct *t, const struct pt_regs *regs)
{
	(void)t;
	(void)regs;
	return -EOPNOTSUPP;
}
