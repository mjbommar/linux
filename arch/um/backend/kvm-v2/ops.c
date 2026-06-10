// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) ops table singleton.
 *
 * KVM v2 owns VM lifecycle, vCPU dispatch, context switching, TLB
 * kicks, and region add/remove. Operations that still rely on existing
 * seccomp machinery delegate to the seccomp backend through this table.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/backend.h>

#include "kvm_v2_backend.h"

const struct um_backend_ops um_backend_kvm_v2_ops = {
	.name			= "kvm-v2",
	.kind			= UM_BACKEND_KIND_KVM,
	.contract_version	= UM_BACKEND_CONTRACT_VERSION,

	/*
	 * KVM-v2 reuses the seccomp stub-child lifecycle helpers. Each
	 * flag describes the worker-internal stub-child machinery those
	 * helpers drive:
	 *   uses_stub_reaper        - worker SIGCHLD-reaps stub
	 *   has_syscall_stub_fd_map - SCM_RIGHTS fd-passing per region
	 *   stub_syscall_uses_futex - futex round-trip on stub_data
	 *   stub_child_runs_seccomp - stub installs seccomp filter
	 *
	 * These flags describe stub-child behavior, not whether KVM owns
	 * guest syscall execution. Guest user code runs in KVM, not in the
	 * stub. The stub child is still spawned (seccomp_mm_create)
	 * but its SIGSYS handler never fires under v2 because no
	 * syscall traps to it.
	 */
	.uses_stub_reaper	= true,
	.has_syscall_stub_fd_map = true,
	.stub_syscall_uses_futex = true,
	.stub_child_runs_seccomp = true,

	/* Lifecycle */
	.probe			= kvm_v2_probe,
	.init			= kvm_v2_init,
	.shutdown		= kvm_v2_shutdown,
	/*
	 * Every UML guest task's SYSCALL goes through KVM_RUN ->
	 * KVM_EXIT_IO -> handle_syscall.
	 */
	.vcpu_run		= kvm_v2_vcpu_run,	/* HOT */

	/* Memory */
	.mm_create		= seccomp_mm_create,
	.mm_destroy		= seccomp_mm_destroy,
	.mm_region_added	= kvm_v2_mm_region_added,	/* HOT */
	.mm_region_removed	= kvm_v2_mm_region_removed,	/* HOT */
	/*
	 * The mm arbiter falls back to remove+add when this is NULL. KVM v2's
	 * region hooks are synchronized no-ops behind the giant physmem slot,
	 * so the fallback is sufficient until v2 needs separate protection
	 * bookkeeping.
	 */
	.mm_region_protected	= NULL,

	/* Scheduling */
	.thread_create		= seccomp_thread_create,
	.thread_start_idle	= seccomp_thread_start_idle,
	.context_switch		= kvm_v2_context_switch,	/* HOT */
	.ipi_send		= seccomp_ipi_send,
	/*
	 * Cross-vCPU TLB kick: pthread_sigqueue IPI_SIGNAL to all other
	 * UML CPUs after um_tlb_sync so they exit KVM_RUN
	 * with -EINTR and the next dispatch's CR4.PGE toggle flushes
	 * their guest TLBs.
	 */
	.tlb_kick_others	= kvm_v2_tlb_kick_others,

	/* Time */
	.read_clock_ns		= seccomp_read_clock_ns,	/* HOT */
	.set_timer		= seccomp_set_timer,
	.read_persistent_clock_ns = seccomp_read_persistent_clock_ns,

	/* Debug */
	.init_thread_regs	= seccomp_init_thread_regs,
	.read_guest_regs	= seccomp_read_guest_regs,
	.write_guest_regs	= seccomp_write_guest_regs,
};
