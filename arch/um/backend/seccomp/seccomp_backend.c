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
	.stub_child_runs_seccomp = true,

	/* Lifecycle and trap (4) */
	.probe			= seccomp_probe,
	.init			= seccomp_init,
	.shutdown		= seccomp_shutdown,
	.vcpu_run		= seccomp_vcpu_run,	/* HOT */

	/* Memory (5) — memo 25 R2 ops cleanup */
	.mm_create		= seccomp_mm_create,
	.mm_destroy		= seccomp_mm_destroy,
	.mm_region_added	= seccomp_mm_region_added,	/* HOT */
	.mm_region_removed	= seccomp_mm_region_removed,	/* HOT */
	.mm_region_protected	= NULL,	/* mm-arbiter falls back to remove+add */

	/* Scheduling (4) */
	.thread_create		= seccomp_thread_create,
	.thread_start_idle	= seccomp_thread_start_idle,
	.context_switch		= seccomp_context_switch,	/* HOT */
	.ipi_send		= seccomp_ipi_send,
	/*
	 * Cross-vCPU TLB kick: NULL for seccomp. The seccomp stub-child
	 * model already cross-CPU-flushes via host mmu_notifier on the
	 * real munmap that mm_region_removed issues — no extra kick
	 * needed. um_tlb_sync NULL-checks before calling.
	 */
	.tlb_kick_others	= NULL,

	/* Time (3) */
	.read_clock_ns		= seccomp_read_clock_ns,	/* HOT */
	.set_timer		= seccomp_set_timer,
	.read_persistent_clock_ns = seccomp_read_persistent_clock_ns,

	/* Debug (3) */
	.init_thread_regs	= seccomp_init_thread_regs,
	.read_guest_regs	= seccomp_read_guest_regs,
	.write_guest_regs	= seccomp_write_guest_regs,
};
