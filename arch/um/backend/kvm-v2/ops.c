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
 * Capability flags reflect the ACTUAL stub model v2 uses, not v1's
 * archived "no stub child" semantics. R4 (per-mm host worker
 * process) put a real seccomp stub child inside each per-mm worker
 * process, so the four host-side flags must be TRUE — they describe
 * the worker-internal stub-child machinery that handles_syscall
 * delegation through seccomp_* ops actually exercises.
 *
 * The original A.1 commit set these to false (copied from v1
 * archive). That deadlocked `backend=force=kvm-v2 init=/bin/true`:
 * stub_child_runs_seccomp=false → stub binary did PTRACE_TRACEME +
 * kill(SIGSTOP) instead of installing a seccomp filter; nobody
 * attached as ptracer; spawner hung waiting on STUB_ALLOC_REP that
 * never came. Fixed in the followup that ships with B.2.
 *
 * As Phase D's vmcall syscall path migrates ops off seccomp, the
 * stub-child model fades; at that point these flags can flip back
 * to false and the seccomp delegation in the table below shrinks
 * to nothing.
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
	 * Capability flags — true for as long as v2 delegates to
	 * seccomp_* ops. Each flag describes the worker-internal
	 * stub-child machinery that those delegated ops drive:
	 *   uses_stub_reaper       — worker SIGCHLD-reaps stub
	 *   has_syscall_stub_fd_map — SCM_RIGHTS fd-passing per region
	 *   stub_syscall_uses_futex — futex round-trip on stub_data
	 *   stub_child_runs_seccomp — stub installs seccomp filter
	 *                             (vs. PTRACE_TRACEME path)
	 * Each flag flips to false as Phase D migrates the
	 * corresponding op off the seccomp delegation.
	 */
	/*
	 * D.5 INVESTIGATION (2026-04-29): the §D.5 spec said to flip
	 * these four flags to false. That broke boot — the stub child
	 * is STILL spawned because seccomp_mm_create / mm_attach are
	 * the active backend ops (v2 only owns .vcpu_run + memslot ops
	 * + context_switch right now). With stub_child_runs_seccomp
	 * = false, skas/process.c:220 takes the PTRACE_TRACEME branch
	 * instead of installing the seccomp filter, and the stub child
	 * waits forever for a ptracer that never attaches (this is the
	 * exact boot-deadlock A.1 followup `40a97b5f3b72` first fixed).
	 *
	 * The flags describe stub-child behavior, not "is v2 in
	 * charge of syscalls." They should flip to false in a future
	 * phase that REPLACES seccomp_mm_create / mm_attach /
	 * thread_create with v2-native equivalents (Phase D follow-up
	 * or Phase G — needs a memo update). Today the stub child
	 * still exists and still installs its seccomp filter, so
	 * keep the flags true.
	 *
	 * The .vcpu_run flip below DOES move the syscall hot path off
	 * the stub child — guest user code runs in KVM, not in the
	 * stub. The stub child is still spawned (seccomp_mm_create)
	 * but its SIGSYS handler never fires under v2 because no
	 * syscall traps to it.
	 */
	.uses_stub_reaper	= true,
	.has_syscall_stub_fd_map = true,
	.stub_syscall_uses_futex = true,
	.stub_child_runs_seccomp = true,

	/* Lifecycle (4) — v2 owns these */
	.probe			= kvm_v2_probe,
	.init			= kvm_v2_init,
	.shutdown		= kvm_v2_shutdown,
	/*
	 * D.5: flip the pointer. C.2-C.4 built kvm_v2_vcpu_run on the
	 * side; D.0-D.4b populated CPUID, MSR_LSTAR/STAR/FMASK, EFER.SCE,
	 * the trampoline page, the physmem memslot, and the PML4[448]
	 * kernel-half PT chain. With this flip, every UML guest task's
	 * SYSCALL goes through KVM_RUN → KVM_EXIT_IO → handle_syscall.
	 */
	.vcpu_run		= kvm_v2_vcpu_run,	/* HOT — D.5 */

	/* Memory (5) — A.2 + Phase B */
	.mm_create		= seccomp_mm_create,
	.mm_destroy		= seccomp_mm_destroy,
	.mm_region_added	= kvm_v2_mm_region_added,	/* HOT */
	.mm_region_removed	= kvm_v2_mm_region_removed,	/* HOT */
	.mm_region_protected	= NULL,	/*
						 * memo 26 §B.4: mm-arbiter falls
						 * back to remove+add when this
						 * is NULL. Under B.2+B.3 the
						 * fallback already issues
						 * KVM_SET_USER_MEMORY_REGION
						 * delete then add (with new
						 * flags) — exactly what §B.4
						 * specs. A direct
						 * mm_region_protected op would
						 * collapse it to a single
						 * KVM_SET_USER_MEMORY_REGION
						 * with new flags but is a
						 * Phase H optimisation.
						 */

	/* Scheduling (4) — Phase C */
	.thread_create		= seccomp_thread_create,
	.thread_start_idle	= seccomp_thread_start_idle,
	.context_switch		= kvm_v2_context_switch,	/* HOT — D.3 */
	.ipi_send		= seccomp_ipi_send,
	/*
	 * Cross-vCPU TLB kick (Phase G.2): pthread_sigqueue IPI_SIGNAL
	 * to all OTHER UML CPUs after um_tlb_sync so they exit KVM_RUN
	 * with -EINTR and the next dispatch's CR4.PGE toggle flushes
	 * their guest TLBs.
	 */
	.tlb_kick_others	= kvm_v2_tlb_kick_others,

	/* Time (3) — Phase F */
	.read_clock_ns		= seccomp_read_clock_ns,	/* HOT */
	.set_timer		= seccomp_set_timer,
	.read_persistent_clock_ns = seccomp_read_persistent_clock_ns,

	/* Debug (3) — Phase G */
	.init_thread_regs	= seccomp_init_thread_regs,
	.read_guest_regs	= seccomp_read_guest_regs,
	.write_guest_regs	= seccomp_write_guest_regs,
};
