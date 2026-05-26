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
#include <sysdep/ptrace_user.h>

#include "seccomp_backend.h"

void seccomp_init_thread_regs(unsigned long *gp, unsigned long *fp)
{
	get_safe_registers(gp, fp);

#if defined(CONFIG_X86_64) || defined(HOST_FS_BASE)
	/*
	 * Match the upstream x86_64 start_thread_common contract: a
	 * freshly-exec'd userspace process must observe FS = 0,
	 * GS = 0, FS_BASE = 0, GS_BASE = 0 — no TLS until the program
	 * sets one up via arch_prctl(SET_FS).
	 *
	 * get_safe_registers() above hands back a boot-time snapshot of
	 * the launcher's host registers, including whatever FS/GS_BASE
	 * the launcher had at PTRACE_GETREGS time.  When the same
	 * snapshot is reused for every guest exec, the new program runs
	 * with the launcher's stale TLS pointer — Python's _Py_Dealloc
	 * doing `mov %fs:-0x18, %r12` then dereferences garbage and
	 * SIGSEGVs at NULL+0x350 deterministically (CPython 3.14 free-
	 * threaded uses TLS for the per-thread interpreter pointer).
	 *
	 * Repro deterministic under parallel multiprocessing.spawn():
	 *   python3 -m test test_multiprocessing_spawn.test_processes
	 * — segfault in _Py_Dealloc on every worker pre-fix; clean
	 * post-fix.
	 */
	gp[HOST_FS_BASE] = 0;
	gp[HOST_GS_BASE] = 0;
#endif
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
