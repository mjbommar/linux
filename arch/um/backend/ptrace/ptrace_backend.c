// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace backend: struct um_backend_ops singleton.
 *
 * All 18 ops are populated; the contract requires non-NULL for every
 * field and the boot arbiter (init_backend) additionally refuses the
 * backend if a HOT op is NULL. The per-slice annotations below record
 * which A-02 slice migrated each op (archaeology, for reviewers
 * tracing how the table was built up).
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

	/*
	 * Capability flags — see backend.h.
	 *
	 * uses_stub_reaper=false: ptrace reaps the stub child
	 * inline via waitpid inside the trap loop, not via a
	 * SIGCHLD-registered reaper IRQ.
	 *
	 * has_syscall_stub_fd_map=false: ptrace stub syscalls
	 * use plain FDs (no per-mm SCM_RIGHTS indirection table).
	 *
	 * stub_syscall_uses_futex=false: ptrace dispatch walks
	 * PTRACE_SETREGS + PTRACE_CONT + wait_stub_done, not the
	 * futex + wait_stub_done_seccomp path.
	 */
	.uses_stub_reaper	= false,
	.has_syscall_stub_fd_map = false,
	.stub_syscall_uses_futex = false,

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
