// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: debug / introspection ops.
 *
 *   init_thread_regs   - wraps get_safe_registers().
 *   read_guest_regs    - KGDB consumer. Returns -EOPNOTSUPP unless the
 *                        KGDB path wires it to get_stub_state() with proper
 *                        turnstile serialization.
 *   write_guest_regs   - symmetric. Returns -EOPNOTSUPP.
 *
 * This file is built with libc available because get_safe_registers()
 * lives in arch/um/os-Linux/registers.c, and read/write_guest_regs
 * implementations also need the host-side get_stub_state() and
 * set_stub_state() helpers.
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
	 * Match the x86_64 start_thread_common contract: a freshly exec'd
	 * userspace process observes FS/GS and their bases as zero until it
	 * installs TLS with arch_prctl(). The bootstrap snapshot may contain
	 * the launcher's host TLS bases, so clear them before using it as a
	 * guest initial-register template.
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
