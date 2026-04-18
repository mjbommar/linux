// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace backend: struct um_backend_ops singleton.
 *
 * Workstream A-02 first slice. Op fields are NULL except for the ones
 * migrated so far; the boot arbiter (init_backend, A-02.5) refuses
 * the backend if a HOT op is NULL, so partial migrations can't ship
 * silently.
 *
 * Op fill-in plan (per Documentation/virt/uml/redesign/02-workstreams/
 * A-backend-abstraction/02-ptrace-refactor.md priority order):
 *
 *   A-02.6:        read_persistent_clock_ns  (cold proof-of-concept)
 *   A-02.HOT-1:    run_userspace
 *   A-02.HOT-2:    mm_map, mm_unmap
 *   A-02.HOT-3:    context_switch
 *   A-02.COLD-1:   read_clock_ns, set_timer
 *   A-02.COLD-2:   ipi_send, init_thread_regs, read/write_guest_regs
 *   A-02.COLD-3:   mm_attach, mm_detach, thread_create,
 *                  thread_start_idle, probe, init, shutdown
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/backend.h>

#include "ptrace_backend.h"

const struct um_backend_ops um_backend_ptrace_ops = {
	.name			= "ptrace",
	.kind			= UM_BACKEND_KIND_PTRACE,
	.contract_version	= UM_BACKEND_CONTRACT_VERSION,

	/* Lifecycle and trap (4) */
	.probe			= ptrace_probe,
	.init			= ptrace_init,
	.shutdown		= ptrace_shutdown,
	.run_userspace		= ptrace_run_userspace,	/* HOT — A-02.HOT-1 */

	/* Memory (4) */
	.mm_attach		= ptrace_mm_attach,
	.mm_detach		= ptrace_mm_detach,
	.mm_map			= ptrace_mm_map,	/* HOT — A-02.HOT-2 */
	.mm_unmap		= ptrace_mm_unmap,	/* HOT — A-02.HOT-2 */

	/* Scheduling (4) — ipi_send migrates in A-02.COLD-2. */
	.thread_create		= ptrace_thread_create,
	.thread_start_idle	= ptrace_thread_start_idle,
	.context_switch		= ptrace_context_switch,	/* HOT — A-02.HOT-3 */
	.ipi_send		= ptrace_ipi_send,

	/* Time (3) */
	.read_clock_ns		= ptrace_read_clock_ns,	/* HOT */
	.set_timer		= ptrace_set_timer,
	.read_persistent_clock_ns = ptrace_read_persistent_clock_ns,

	/* Debug / introspection (3). */
	.init_thread_regs	= ptrace_init_thread_regs,
	.read_guest_regs	= ptrace_read_guest_regs,
	.write_guest_regs	= ptrace_write_guest_regs,
};
