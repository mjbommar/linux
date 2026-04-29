// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — ops table singleton.
 *
 * Phase A.1 strategy (per memo 26 §A.1 and the kvm_v2_backend.h header
 * comment): every op except probe/init/shutdown delegates to the
 * seccomp backend. This is what the spec means by "system uses
 * fallback for ops not implemented" — the dispatch macro itself has
 * no NULL check or -ENOSYS synthesis, so the fallback has to live in
 * the ops table.
 *
 * As Phase A.2+ lands, individual ops here get rewritten to use the
 * v2 per-VM context / vCPU / memslot infrastructure. Each migration
 * is one bullet on the memo 26 timeline; this file is the central
 * progress board.
 *
 * Capability flags follow v1's archived choices (memo 21 D-02): no
 * stub child, so all four host-side flags are false. This means
 * arch/um/os-Linux/skas code that branches on `um_backend->uses_*`
 * picks the KVM path even though most ops still delegate to seccomp.
 * That's a known A.1 wart — the seccomp ops we delegate to assume
 * the seccomp stub-child model. The wart resolves naturally as ops
 * migrate off seccomp; until then `force=kvm-v2` is for development
 * only (see the Kconfig help text and memo 26's exit criteria).
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
	 * Capability flags — KVM has no host stub child, so all four
	 * stub-related flags are false (matches v1 archive). See file
	 * header comment for the A.1 wart this creates.
	 */
	.uses_stub_reaper	= false,
	.has_syscall_stub_fd_map = false,
	.stub_syscall_uses_futex = false,
	.stub_child_runs_seccomp = false,

	/* Lifecycle (4) — v2 owns these */
	.probe			= kvm_v2_probe,
	.init			= kvm_v2_init,
	.shutdown		= kvm_v2_shutdown,
	.vcpu_run		= seccomp_vcpu_run,	/* HOT — A.3+B+C */

	/* Memory (5) — A.2 + Phase B */
	.mm_create		= seccomp_mm_create,
	.mm_destroy		= seccomp_mm_destroy,
	.mm_region_added	= seccomp_mm_region_added,	/* HOT */
	.mm_region_removed	= seccomp_mm_region_removed,	/* HOT */
	.mm_region_protected	= NULL,	/* mm-arbiter falls back to remove+add */

	/* Scheduling (4) — Phase C */
	.thread_create		= seccomp_thread_create,
	.thread_start_idle	= seccomp_thread_start_idle,
	.context_switch		= seccomp_context_switch,	/* HOT */
	.ipi_send		= seccomp_ipi_send,

	/* Time (3) — Phase F */
	.read_clock_ns		= seccomp_read_clock_ns,	/* HOT */
	.set_timer		= seccomp_set_timer,
	.read_persistent_clock_ns = seccomp_read_persistent_clock_ns,

	/* Debug (3) — Phase G */
	.init_thread_regs	= seccomp_init_thread_regs,
	.read_guest_regs	= seccomp_read_guest_regs,
	.write_guest_regs	= seccomp_write_guest_regs,
};
