// SPDX-License-Identifier: GPL-2.0
/*
 * KVM-backend snapshot/forkserver — v2 port (Phase 1).
 *
 * Strategic Time-machine lift per PLAN-2026-05-14 §4.1 (#168). v1's
 * implementation at kvm-v1-archive/snapshot.c proved the design;
 * this file ports the public surface onto v2's vCPU-pool +
 * single-memslot shape.
 *
 * Phase 1 (this commit):
 *   - struct kvm_v2_snapshot + alloc/destroy/free skeleton.
 *   - kvm_v2_snapshot_capture_regs_only IMPLEMENTED.
 *   - kvm_v2_snapshot_restore_full IMPLEMENTED.
 *   - kvm_v2_snapshot_capture stubbed at -ENOSYS.
 *
 * Phase 2 will add the memslot + IDT/GDT/IST capture in
 * kvm_v2_snapshot_capture; Phase 3+ adds cross-task semantics, a
 * bench harness, and selftest re-plumbing. Sub-sequencing memo:
 * Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
 * 26-snapshot-v2-port.md.
 *
 * v1 → v2 deltas Phase 1 handles:
 *   - vCPU lookup: v1 used current->thread.arch.kvm.vcpu (per-task);
 *     v2 walks vcpus[] for vcpu->last_task == current (per-pool).
 *   - FPU capture: v1 used KVM_GET_FPU (legacy 512 B FXSAVE); v2
 *     uses KVM_GET_XSAVE so the SMP-T57 Phase A XCR0.YMM upper-128
 *     state is preserved across the round-trip.
 *   - XCR0: v2 captures it via KVM_GET_XCRS; v1 didn't (XCR0 was
 *     always zero under v1's curated CPUID).
 */

#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kvm.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>		/* current */
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/string.h>

#include <os.h>

#include "kvm_v2_backend.h"

/*
 * Static MSR index list. Order matches snap->msrs.entries[]. Same
 * seven MSRs v1 captured (kvm-v1-archive/snapshot.c:84-92); the
 * upstream header name for FMASK is MSR_SYSCALL_MASK but the numeric
 * encoding is unchanged.
 */
static const u32 kvm_v2_snapshot_msr_indices[KVM_V2_SNAPSHOT_MSR_COUNT] = {
	0xC0000082,	/* MSR_LSTAR */
	0xC0000081,	/* MSR_STAR */
	0xC0000084,	/* MSR_SYSCALL_MASK (v1 named it MSR_FMASK) */
	0xC0000102,	/* MSR_KERNEL_GS_BASE */
	0xC0000100,	/* MSR_FS_BASE */
	0xC0000101,	/* MSR_GS_BASE */
	0xC0000080,	/* MSR_EFER */
};

/*
 * kvm_v2_snapshot_pick_vcpu - resolve the pool entry the snapshot
 *                             should target for the calling task.
 *
 * Phase 1 invariant (memo 26-snapshot §4.3): the vCPU whose
 * last_task == current is the one whose KVM-owned state holds
 * current's most recent dispatch result. If no entry matches yet
 * (fresh task, never dispatched), fall back to the vCPU pinned to
 * the current host CPU index — that's where the next dispatch would
 * land, and an "empty" capture there is at least well-defined.
 *
 * Caller responsibility: hold preempt_disable() across the lookup
 * and the subsequent ioctl calls so the per-CPU vCPU mapping
 * doesn't shift mid-flight.
 *
 * Returns NULL if the pool isn't initialised yet (early-boot
 * harness builds where init_backend() has not run).
 */
static struct kvm_v2_vcpu *kvm_v2_snapshot_pick_vcpu(void)
{
	struct kvm_v2_vcpu *v;
	int cpu;

	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		v = kvm_v2_vcpu_get(cpu);
		if (v && v->last_task == current)
			return v;
	}
	return kvm_v2_vcpu_get(smp_processor_id());
}

/**
 * kvm_v2_snapshot_alloc - allocate a fresh snapshot container.
 *
 * Returns a zero-initialised snapshot struct on the heap, or NULL
 * on allocation failure. Caller pairs with kvm_v2_snapshot_destroy
 * (or kvm_v2_snapshot_free + kfree).
 */
struct kvm_v2_snapshot *kvm_v2_snapshot_alloc(void)
{
	return kzalloc(sizeof(struct kvm_v2_snapshot), GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_alloc);

/**
 * kvm_v2_snapshot_free - release snapshot-owned allocations.
 * @snap: snapshot whose allocations should be freed; the struct
 *        itself is not freed (caller owns it).
 *
 * Idempotent: passing a never-captured or already-freed snapshot is
 * safe. Phase 1 has no external allocations (the XSAVE area is an
 * inline struct kvm_xsave field) — the function is wired now so
 * Phase 3's memslot allocation slots in without churning the public
 * surface.
 */
void kvm_v2_snapshot_free(struct kvm_v2_snapshot *snap)
{
	if (!snap)
		return;
	if (snap->mem_backing) {
		kvfree(snap->mem_backing);
		snap->mem_backing = NULL;
	}
	snap->mem_size = 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_free);

/**
 * kvm_v2_snapshot_destroy - free both the snapshot's allocations
 *                           and the container itself.
 * @snap: snapshot to release; can be NULL.
 */
void kvm_v2_snapshot_destroy(struct kvm_v2_snapshot *snap)
{
	if (!snap)
		return;
	kvm_v2_snapshot_free(snap);
	kfree(snap);
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_destroy);

/**
 * kvm_v2_snapshot_capture_regs_only - capture vCPU regs only,
 *                                     skipping the memslot copy.
 * @snap: caller-allocated snapshot (typically via
 *        kvm_v2_snapshot_alloc).
 *
 * Captures the per-vCPU scalar state for the vCPU current most
 * recently dispatched on (see kvm_v2_snapshot_pick_vcpu). Issues
 * KVM_GET_REGS, KVM_GET_SREGS, KVM_GET_XSAVE, KVM_GET_XCRS,
 * KVM_GET_VCPU_EVENTS and a 7-entry KVM_GET_MSRS.
 *
 * v1 → v2 delta: KVM_GET_FPU replaced by KVM_GET_XSAVE so the
 * SMP-T57 Phase A XCR0.YMM upper-128 state is captured (the legacy
 * 512 B FXSAVE area only covers X87+SSE; YMM upper lives in the
 * XSAVE extended area).
 *
 * Returns 0 on success; -ENODEV if no vCPU is available; -errno on
 * the first ioctl failure.
 */
int kvm_v2_snapshot_capture_regs_only(struct kvm_v2_snapshot *snap)
{
	struct kvm_v2_vcpu *vcpu;
	int vcpu_fd;
	unsigned int i;
	int rc;

	if (!snap)
		return -EINVAL;

	memset(snap, 0, sizeof(*snap));

	/*
	 * Pin to a stable host CPU for the duration of the ioctl
	 * sequence. The lookup walks the pool by last_task; without
	 * preempt_disable the scheduler could migrate us mid-walk and
	 * the per-host-CPU vCPU index would shift.
	 */
	preempt_disable();
	vcpu = kvm_v2_snapshot_pick_vcpu();
	if (!vcpu || vcpu->vcpu_fd < 0) {
		preempt_enable();
		return -ENODEV;
	}
	vcpu_fd = vcpu->vcpu_fd;

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&snap->regs);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_GET_REGS failed (%d)\n", rc);
		goto out;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
			      (unsigned long)&snap->sregs);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_GET_SREGS failed (%d)\n", rc);
		goto out;
	}

	/*
	 * KVM_GET_XSAVE returns the legacy 4 KB struct kvm_xsave shape.
	 * The variable-size KVM_GET_XSAVE2 ioctl is only needed for
	 * dynamic XCR0 features (e.g. AMX); v2's curated CPUID leaves
	 * those off so the static shape suffices. See memo 26-snapshot
	 * §3.2 for the v1 (KVM_GET_FPU) → v2 (KVM_GET_XSAVE) delta.
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_GET_XSAVE,
			      (unsigned long)&snap->xsave);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_GET_XSAVE failed (%d)\n", rc);
		goto out;
	}

	/*
	 * Capture XCR0 so restore lands on a vCPU whose XSAVE-area
	 * interpretation matches the captured bytes. v1 didn't do this
	 * (XCR0 was always 0 under v1's curated mask); v2's SMP-T57
	 * Phase A sets XCR0 = 0x7 (FP|SSE|YMM) on first dispatch.
	 */
	snap->xcrs.nr_xcrs = 1;
	rc = os_ioctl_generic(vcpu_fd, KVM_GET_XCRS,
			      (unsigned long)&snap->xcrs);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_GET_XCRS failed (%d)\n", rc);
		goto out;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_VCPU_EVENTS,
			      (unsigned long)&snap->events);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_GET_VCPU_EVENTS failed (%d)\n",
			rc);
		goto out;
	}

	/* MSRs: prepare the index list, KVM_GET_MSRS fills values. */
	snap->msrs.nmsrs = KVM_V2_SNAPSHOT_MSR_COUNT;
	for (i = 0; i < KVM_V2_SNAPSHOT_MSR_COUNT; i++) {
		snap->msrs.entries[i].index = kvm_v2_snapshot_msr_indices[i];
		snap->msrs.entries[i].reserved = 0;
		snap->msrs.entries[i].data = 0;
	}
	rc = os_ioctl_generic(vcpu_fd, KVM_GET_MSRS,
			      (unsigned long)&snap->msrs);
	if (rc < 0 || rc != KVM_V2_SNAPSHOT_MSR_COUNT) {
		pr_warn("um: kvm-v2 snapshot: KVM_GET_MSRS rc=%d (expected %d)\n",
			rc, KVM_V2_SNAPSHOT_MSR_COUNT);
		if (rc >= 0)
			rc = -EIO;
		goto out;
	}

	pr_info("um: kvm-v2 snapshot: captured regs+sregs+xsave+xcrs+events+%u msrs (regs-only)\n",
		KVM_V2_SNAPSHOT_MSR_COUNT);
	rc = 0;
out:
	preempt_enable();
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_capture_regs_only);

/**
 * kvm_v2_snapshot_capture - capture full vCPU + memslot state.
 * @snap: caller-allocated snapshot.
 *
 * Phase 1 STUB — returns -EOPNOTSUPP. Phase 3 (memo 26-snapshot §6,
 * sub-sequencing step 3) will add the memslot memcpy + the per-VM
 * IDT/GDT + per-vCPU IST/TSS/gadget-state capture. For now callers
 * that need only the vCPU register state should use
 * kvm_v2_snapshot_capture_regs_only.
 */
int kvm_v2_snapshot_capture(struct kvm_v2_snapshot *snap)
{
	if (!snap)
		return -EINVAL;
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_capture);

/**
 * kvm_v2_snapshot_restore_full - restore vCPU state from snapshot.
 * @snap: previously-captured snapshot.
 *
 * Phase 1 path: restores vCPU registers only (no memslot rollback).
 * The memslot restore lands with Phase 3 alongside the matching
 * kvm_v2_snapshot_capture implementation.
 *
 * KVM ordering invariant (carries over from v1): SREGS must precede
 * REGS — KVM validates RIP/RSP against the post-SREGS segment cache
 * during KVM_SET_REGS, so the regs push fails -EINVAL if the SREGS
 * load hasn't installed the matching CS:SS descriptors yet.
 *
 * Post-restore hygiene (memo 26-snapshot §4.5): clear the per-vCPU
 * last_task / last_mm / fpu_owner_task so the next dispatch's
 * cross-task arrival branch fires and unconditionally re-installs
 * SREGS + FPU. The lazy fast paths re-arm on the dispatch after.
 *
 * Returns 0 on success; -ENODEV if no vCPU is available; -errno on
 * the first ioctl failure.
 */
int kvm_v2_snapshot_restore_full(struct kvm_v2_snapshot *snap)
{
	struct kvm_v2_vcpu *vcpu;
	int vcpu_fd;
	int rc;

	if (!snap)
		return -EINVAL;

	preempt_disable();
	vcpu = kvm_v2_snapshot_pick_vcpu();
	if (!vcpu || vcpu->vcpu_fd < 0) {
		preempt_enable();
		return -ENODEV;
	}
	vcpu_fd = vcpu->vcpu_fd;

	/*
	 * SREGS first — KVM rejects KVM_SET_REGS if RIP/RSP don't
	 * canonicalise against the post-SREGS segment cache. v1 had
	 * the same ordering; verbatim port.
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS,
			      (unsigned long)&snap->sregs);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_SREGS failed (%d)\n", rc);
		goto out;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
			      (unsigned long)&snap->regs);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_REGS failed (%d)\n", rc);
		goto out;
	}

	/*
	 * XCRS before XSAVE — KVM cross-validates the XSAVE area's
	 * component bits against the vCPU's current XCR0 / supported
	 * XCR0 mask. Setting XCR0 first ensures the subsequent
	 * KVM_SET_XSAVE accepts the YMM upper bytes.
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_SET_XCRS,
			      (unsigned long)&snap->xcrs);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_XCRS failed (%d)\n", rc);
		goto out;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_XSAVE,
			      (unsigned long)&snap->xsave);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_XSAVE failed (%d)\n", rc);
		goto out;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_VCPU_EVENTS,
			      (unsigned long)&snap->events);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_VCPU_EVENTS failed (%d)\n",
			rc);
		goto out;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS,
			      (unsigned long)&snap->msrs);
	if (rc < 0 || rc != KVM_V2_SNAPSHOT_MSR_COUNT) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_MSRS rc=%d (expected %d)\n",
			rc, KVM_V2_SNAPSHOT_MSR_COUNT);
		if (rc >= 0)
			rc = -EIO;
		goto out;
	}

	/*
	 * Post-restore: force the next dispatch's cross-task arrival
	 * branch (memo 26-snapshot §4.5). Without this the lazy
	 * KVM_SET_SREGS skip would see fpu_owner_task == previous-task
	 * and trust the vCPU's view — but we just overwrote that view
	 * with snapshot bytes; the dispatch has to re-install from the
	 * restored state. Clearing last_task / last_mm /
	 * fpu_owner_task makes the load_user_sregs cross-task gate
	 * fire and re-install unconditionally.
	 */
	vcpu->last_task        = NULL;
	vcpu->last_mm          = NULL;
	vcpu->fpu_owner_task   = NULL;
	vcpu->fpu_dirty        = true;

	pr_info("um: kvm-v2 snapshot: restored vCPU (regs-only path)\n");
	rc = 0;
out:
	preempt_enable();
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_restore_full);
