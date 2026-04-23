// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend: struct um_backend_ops singleton.
 *
 * Workstream D-02. First-phase (naive) KVM-platform backend per the
 * design memo at
 * Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
 * design-memo.md. This commit is scaffold-only: `probe` and `init`
 * do the real work of opening /dev/kvm; every other op is a stub
 * that returns -EOPNOTSUPP pending the D-03..D-06 bring-up work
 * (memslots + CR3 + vCPU loop + signal/interrupt delivery).
 *
 * Selection: CONFIG_UM_BACKEND_KVM + either UM_BACKEND_KVM_ONLY
 * (inline dispatch) or UM_BACKEND_DYNAMIC + `backend=kvm` boot
 * arg. Until the data path lands the backend will fail to boot
 * past init — intentional; don't set kvm as default anywhere.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/backend.h>

#include "kvm_backend.h"

const struct um_backend_ops um_backend_kvm_ops = {
	.name			= "kvm",
	.kind			= UM_BACKEND_KIND_KVM,
	.contract_version	= UM_BACKEND_CONTRACT_VERSION,

	/*
	 * Capability flags — see backend.h.
	 *
	 * uses_stub_reaper=false: KVM has no host stub child; the
	 * guest runs under KVM_RUN, not as a ptraced/seccomp'd
	 * peer process.
	 *
	 * has_syscall_stub_fd_map=false: KVM doesn't use the
	 * stub-syscall ABI at all; it dispatches through VMEXIT
	 * + VMCALL, not through a ring-3 stub child.
	 *
	 * stub_syscall_uses_futex=false: same reason —
	 * do_syscall_stub() is never reached on the KVM path.
	 */
	.uses_stub_reaper	= false,
	.has_syscall_stub_fd_map = false,
	.stub_syscall_uses_futex = false,

	/* Lifecycle and trap (4) */
	.probe			= kvm_probe,
	.init			= kvm_init,
	.shutdown		= kvm_shutdown,
	.run_userspace		= kvm_run_userspace,	/* HOT */

	/* Memory (4) */
	.mm_attach		= kvm_mm_attach,
	.mm_detach		= kvm_mm_detach,
	.mm_map			= kvm_mm_map,		/* HOT */
	.mm_unmap		= kvm_mm_unmap,		/* HOT */

	/* Scheduling (4) */
	.thread_create		= kvm_thread_create,
	.thread_start_idle	= kvm_thread_start_idle,
	.context_switch		= kvm_context_switch,	/* HOT */
	.ipi_send		= kvm_ipi_send,

	/* Time (3) */
	.read_clock_ns		= kvm_read_clock_ns,	/* HOT */
	.set_timer		= kvm_set_timer,
	.read_persistent_clock_ns = kvm_read_persistent_clock_ns,

	/* Debug (3) */
	.init_thread_regs	= kvm_init_thread_regs,
	.read_guest_regs	= kvm_read_guest_regs,
	.write_guest_regs	= kvm_write_guest_regs,
};
