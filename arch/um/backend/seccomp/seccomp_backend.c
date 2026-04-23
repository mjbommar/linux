// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: struct um_backend_ops singleton.
 *
 * Workstream A-03. The seccomp backend wraps Benjamin Berg's
 * SIGSYS-trap impl that landed in 6.16. Most ops share the same
 * host-side helpers as the ptrace backend; the per-backend wrappers
 * exist so the dispatch macro resolves to a named symbol per the
 * D11 dispatch convention.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/backend.h>

#include "seccomp_backend.h"

const struct um_backend_ops um_backend_seccomp_ops = {
	.name			= "seccomp",
	.kind			= UM_BACKEND_KIND_SECCOMP,
	.contract_version	= UM_BACKEND_CONTRACT_VERSION,

	/* Capability flags — see backend.h */
	.uses_stub_reaper	= true,
	.has_syscall_stub_fd_map = true,
	.stub_syscall_uses_futex = true,

	/* Lifecycle and trap (4) */
	.probe			= seccomp_probe,
	.init			= seccomp_init,
	.shutdown		= seccomp_shutdown,
	.run_userspace		= seccomp_run_userspace,	/* HOT */

	/* Memory (4) */
	.mm_attach		= seccomp_mm_attach,
	.mm_detach		= seccomp_mm_detach,
	.mm_map			= seccomp_mm_map,	/* HOT */
	.mm_unmap		= seccomp_mm_unmap,	/* HOT */

	/* Scheduling (4) */
	.thread_create		= seccomp_thread_create,
	.thread_start_idle	= seccomp_thread_start_idle,
	.context_switch		= seccomp_context_switch,	/* HOT */
	.ipi_send		= seccomp_ipi_send,

	/* Time (3) */
	.read_clock_ns		= seccomp_read_clock_ns,	/* HOT */
	.set_timer		= seccomp_set_timer,
	.read_persistent_clock_ns = seccomp_read_persistent_clock_ns,

	/* Debug (3) */
	.init_thread_regs	= seccomp_init_thread_regs,
	.read_guest_regs	= seccomp_read_guest_regs,
	.write_guest_regs	= seccomp_write_guest_regs,
};
