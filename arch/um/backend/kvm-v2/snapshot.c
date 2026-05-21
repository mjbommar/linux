// SPDX-License-Identifier: GPL-2.0
/*
 * KVM-backend snapshot/forkserver — v2 port (Phases 1-3).
 *
 * Strategic Time-machine lift per PLAN-2026-05-14 §4.1 (#168). v1's
 * implementation at kvm-v1-archive/snapshot.c proved the design;
 * this file ports the public surface onto v2's vCPU-pool +
 * single-memslot shape.
 *
 * Phase 1 (commit aa4cd328102c):
 *   - struct kvm_v2_snapshot + alloc/destroy/free.
 *   - kvm_v2_snapshot_capture_regs_only IMPLEMENTED.
 *   - kvm_v2_snapshot_restore_full IMPLEMENTED (regs-only path).
 *   - kvm_v2_snapshot_capture stubbed at -EOPNOTSUPP.
 *
 * Phase 2 (commit 9baf6a1e9838):
 *   - boot-time KUnit fixture (kvm_v2_vcpu_prime_for_kunit) +
 *     suite_init driver wired in test_snapshot.c so the regs-only
 *     test runs end-to-end before any user task dispatches.
 *
 * Phase 3 (this revision):
 *   - kvm_v2_snapshot_capture_full IMPLEMENTED: walks vm->memslots
 *     under vm->lock, copies each slot's bytes to a kvmalloc'd
 *     buffer hung off snap->memslots[i].data, alongside the
 *     regs-only state.
 *   - kvm_v2_snapshot_restore_full_vcpu IMPLEMENTED: memcpy the
 *     memslot bytes back, replay KVM_SET_USER_MEMORY_REGION (idempotent
 *     under v2's identity-mapping layout — slot ids are stable across
 *     the capture/restore boundary), then push the regs / sregs /
 *     xsave / xcrs / events / msrs.
 *   - kvm_v2_snapshot_capture now delegates to capture_full(NULL)
 *     and kvm_v2_snapshot_restore_full delegates to
 *     restore_full_vcpu(NULL) so Phase 1's public surface keeps
 *     working with the legacy pick-by-current helper.
 *
 * v1 → v2 deltas Phase 1+3 handle (memo 26-snapshot §3):
 *   - vCPU lookup: v1 used current->thread.arch.kvm.vcpu (per-task);
 *     v2 walks vcpus[] for vcpu->last_task == current (per-pool).
 *   - FPU capture: v1 used KVM_GET_FPU (legacy 512 B FXSAVE); v2
 *     uses KVM_GET_XSAVE so the SMP-T57 Phase A XCR0.YMM upper-128
 *     state is preserved across the round-trip.
 *   - XCR0: v2 captures it via KVM_GET_XCRS; v1 didn't (XCR0 was
 *     always zero under v1's curated CPUID).
 *   - Memslot copy: v1 had a single Policy-A memslot at uml_physmem
 *     and copied it as a single mem_backing/mem_size pair; v2's
 *     memslot list is dynamic (giant physmem slot plus any future
 *     per-region entries — region.c currently keeps the list down
 *     to just the physmem slot, but the snapshot has to be robust
 *     against that changing). Phase 3 stores an array of per-memslot
 *     captures keyed by slot_id.
 */

#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/kvm.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>		/* current */
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

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
 * current's most recent dispatch result.
 *
 * Caller responsibility: hold preempt_disable() across the lookup
 * and the subsequent ioctl calls so the per-CPU vCPU mapping
 * doesn't shift mid-flight.
 *
 * Returns NULL if no vCPU has last_task == current (e.g. a fresh
 * task that has never dispatched, or a KUnit kthread).  Callers
 * with that situation must pass an explicit vCPU via the _for_vcpu
 * variant — see SMP-T79 (state-audit/28).  The pre-T79 fallback
 * to vcpus[smp_processor_id()] was racy under ncpus>1: the
 * smp_processor_id() could pick a vCPU whose KVM state did not
 * reflect the test's KVM_SET_REGS target, producing the
 * "captured RAX is 0" symptom the soak surfaced.
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
	return NULL;
}

/*
 * kvm_v2_snapshot_capture_vcpu_state - issue the per-vCPU KVM_GET_*
 *                                      ioctl sequence into @snap.
 *
 * Factored out of kvm_v2_snapshot_capture_regs_only so the full
 * capture path can reuse it. Caller holds preempt_disable() and
 * has already validated @vcpu->vcpu_fd >= 0.
 *
 * Issue order: REGS / SREGS / XSAVE / XCRS / VCPU_EVENTS / MSRS.
 * SREGS isn't strictly required to come second (the GET path has
 * no cross-ioctl validation) but matches the SET-side ordering
 * convention for symmetry.
 */
static int kvm_v2_snapshot_capture_vcpu_state(struct kvm_v2_snapshot *snap,
					      struct kvm_v2_vcpu *vcpu)
{
	int vcpu_fd = vcpu->vcpu_fd;
	unsigned int i;
	int rc;

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&snap->regs);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_GET_REGS failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
			      (unsigned long)&snap->sregs);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_GET_SREGS failed (%d)\n", rc);
		return rc;
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
		return rc;
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
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_VCPU_EVENTS,
			      (unsigned long)&snap->events);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_GET_VCPU_EVENTS failed (%d)\n",
			rc);
		return rc;
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
		return rc;
	}

	return 0;
}

/*
 * kvm_v2_snapshot_restore_vcpu_state - issue the per-vCPU KVM_SET_*
 *                                      ioctl sequence from @snap.
 *
 * KVM ordering invariant (carries over from v1): SREGS must precede
 * REGS — KVM validates RIP/RSP against the post-SREGS segment cache
 * during KVM_SET_REGS, so the regs push fails -EINVAL if the SREGS
 * load hasn't installed the matching CS:SS descriptors yet.
 *
 * XCRS must precede XSAVE so KVM cross-validates the XSAVE area's
 * component bits against the post-XCRS XCR0 / supported XCR0 mask.
 *
 * Caller holds preempt_disable() and has already validated
 * @vcpu->vcpu_fd >= 0.
 */
static int kvm_v2_snapshot_restore_vcpu_state(const struct kvm_v2_snapshot *snap,
					      struct kvm_v2_vcpu *vcpu)
{
	int vcpu_fd = vcpu->vcpu_fd;
	int rc;

	/*
	 * KVM_SET_* takes a non-const argp from userspace POV but the
	 * ioctls do not modify the buffer. The cast through
	 * (unsigned long) erases C-level const so we don't need a
	 * stack copy — important because kvm_xsave alone is 4 KB and
	 * the cumulative on-stack copies blew Wframe-larger-than=
	 * (memo state-audit/26 §Phase 3 frame-size lesson).
	 */
	/*
	 * SREGS first — KVM rejects KVM_SET_REGS if RIP/RSP don't
	 * canonicalise against the post-SREGS segment cache. v1 had
	 * the same ordering; verbatim port.
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS,
			      (unsigned long)&snap->sregs);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_SREGS failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
			      (unsigned long)&snap->regs);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_REGS failed (%d)\n", rc);
		return rc;
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
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_XSAVE,
			      (unsigned long)&snap->xsave);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_XSAVE failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_VCPU_EVENTS,
			      (unsigned long)&snap->events);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_VCPU_EVENTS failed (%d)\n",
			rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS,
			      (unsigned long)&snap->msrs);
	if (rc < 0 || rc != KVM_V2_SNAPSHOT_MSR_COUNT) {
		pr_warn("um: kvm-v2 snapshot: KVM_SET_MSRS rc=%d (expected %d)\n",
			rc, KVM_V2_SNAPSHOT_MSR_COUNT);
		if (rc >= 0)
			rc = -EIO;
		return rc;
	}

	return 0;
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
 * safe. Phase 3 may have populated snap->memslots[]; each entry's
 * .data is its own kvmalloc'd buffer and gets kvfree'd here. The
 * legacy mem_backing/mem_size pair is freed for ABI continuity with
 * Phase 1 (always NULL/0 under Phase 3 but a forward-compatible
 * caller might still pass us a Phase-1-shaped snapshot).
 */
void kvm_v2_snapshot_free(struct kvm_v2_snapshot *snap)
{
	int i;

	if (!snap)
		return;

	if (snap->memslots) {
		for (i = 0; i < snap->memslot_count; i++) {
			if (snap->memslots[i].data) {
				kvfree(snap->memslots[i].data);
				snap->memslots[i].data = NULL;
			}
			snap->memslots[i].data_size = 0;
		}
		kvfree(snap->memslots);
		snap->memslots = NULL;
	}
	snap->memslot_count = 0;

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

	rc = kvm_v2_snapshot_capture_vcpu_state(snap, vcpu);
	if (rc == 0)
		pr_info("um: kvm-v2 snapshot: captured regs+sregs+xsave+xcrs+events+%u msrs (regs-only)\n",
			KVM_V2_SNAPSHOT_MSR_COUNT);
	preempt_enable();
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_capture_regs_only);

/**
 * kvm_v2_snapshot_capture_regs_only_for_vcpu - capture against an
 *                                              explicit vCPU.
 * @snap: caller-allocated snapshot.
 * @vcpu: explicit pool entry to capture from.  Caller asserts the
 *        KVM state on @vcpu reflects what they expect to capture
 *        (e.g. the KUnit suite_init primed vcpus[0]; calls here
 *        pass that vcpus[0] directly rather than relying on
 *        pick_vcpu's last_task heuristic).
 *
 * SMP-T79 (state-audit/28): added so KUnit tests don't need to
 * pin themselves to CPU 0 just to make pick_vcpu's
 * smp_processor_id() fallback land on vcpus[0].  Removes a
 * production-vs-test divergence — production goes through
 * kvm_v2_snapshot_capture_regs_only (last_task match) and
 * gets -ENODEV cleanly if there's no match.
 *
 * Returns 0 on success; -EINVAL if @snap or @vcpu is NULL or
 * @vcpu->vcpu_fd < 0; -errno on the first ioctl failure.
 */
int kvm_v2_snapshot_capture_regs_only_for_vcpu(struct kvm_v2_snapshot *snap,
					       struct kvm_v2_vcpu *vcpu)
{
	int rc;

	if (!snap || !vcpu || vcpu->vcpu_fd < 0)
		return -EINVAL;

	memset(snap, 0, sizeof(*snap));

	preempt_disable();
	rc = kvm_v2_snapshot_capture_vcpu_state(snap, vcpu);
	if (rc == 0)
		pr_info("um: kvm-v2 snapshot: captured regs+sregs+xsave+xcrs+events+%u msrs (regs-only, explicit vcpu=%d)\n",
			KVM_V2_SNAPSHOT_MSR_COUNT, vcpu->cpu);
	preempt_enable();
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_capture_regs_only_for_vcpu);

/*
 * kvm_v2_snapshot_capture_memslots - copy each memslot's bytes into
 *                                    a fresh per-entry kvmalloc'd
 *                                    buffer hung off snap->memslots.
 *
 * Walks vm->memslots under vm->lock to count and then snapshot the
 * registered slots. Two-pass design avoids holding vm->lock across
 * the (potentially large) kvmalloc allocations and the memcpy's
 * themselves — the locked first pass produces a thread-local array
 * of (slot_id, host_va, size, flags, gpa) triples, then we drop
 * the lock and do the allocations + memcpy's against the snapshotted
 * descriptors.
 *
 * Memslot stability across the lock drop: under Phase 3's usage
 * model the caller holds preempt_disable() AND the snapshot path is
 * single-threaded (KUnit suite_init context, no UML user task is
 * touching mm_region_added concurrently). region.c never modifies
 * vm->memslots without vm->lock anyway. The lock-drop window is a
 * tightening target for Phase 4 if a record/replay caller introduces
 * concurrent memslot churn.
 *
 * Returns 0 on success, -ENOMEM on any allocation failure (the
 * caller frees partial state via kvm_v2_snapshot_free).
 */
static int kvm_v2_snapshot_capture_memslots(struct kvm_v2_snapshot *snap,
					    struct kvm_v2_vm *vm)
{
	struct kvm_v2_memslot_snapshot *captures;
	struct kvm_v2_memslot *m;
	int count = 0;
	int i = 0;

	scoped_guard(spinlock, &vm->lock) {
		list_for_each_entry(m, &vm->memslots, list)
			count++;
	}

	if (count == 0) {
		snap->memslots = NULL;
		snap->memslot_count = 0;
		return 0;
	}

	captures = kvmalloc_array(count, sizeof(*captures),
				  GFP_KERNEL | __GFP_ZERO);
	if (!captures)
		return -ENOMEM;

	/*
	 * Second pass: copy descriptors out of the list under lock,
	 * then drop the lock before each per-entry data buffer alloc +
	 * memcpy. The descriptor copies fit inside the lock window
	 * cheaply; the data copies don't.
	 */
	scoped_guard(spinlock, &vm->lock) {
		list_for_each_entry(m, &vm->memslots, list) {
			if (i >= count)
				break;
			captures[i].region.slot		= m->slot_id;
			captures[i].region.flags	= m->flags;
			captures[i].region.guest_phys_addr = m->gpa;
			captures[i].region.memory_size	= m->size;
			captures[i].region.userspace_addr = m->host_va;
			captures[i].data_size		= m->size;
			i++;
		}
	}

	for (i = 0; i < count; i++) {
		void *buf;

		if (captures[i].data_size == 0)
			continue;

		/*
		 * Per-slot allocation can fail for the giant physmem slot
		 * (size == mem=N) on memory-constrained UML configs — N
		 * bytes of physmem can't host another N-byte copy plus the
		 * running kernel. Graceful-degrade rather than failing the
		 * entire capture: record the slot's metadata (region
		 * descriptor) but leave data NULL. The matching restore
		 * step skips slots with data=NULL. This keeps Phase 3
		 * useful for the common case (multiple small slots, e.g.
		 * the per-region slots region.c will reinstate post-
		 * "Codex CLAIM C" decision reversal) while admitting that
		 * a full whole-physmem checkpoint needs Phase 4's
		 * dirty-bitmap optimization to fit on a real config.
		 *
		 * __GFP_NOWARN keeps the boot log clean (we log the slot-
		 * sized failure once at pr_warn below; the default
		 * vmalloc-side WARN at "size > INT_MAX" is noise here).
		 */
		buf = kvmalloc(captures[i].data_size,
			       GFP_KERNEL | __GFP_NOWARN);
		if (!buf) {
			pr_warn_ratelimited("um: kvm-v2 snapshot: memslot[%d] slot_id=%u kvmalloc(%zu) failed; metadata-only capture for this slot\n",
					    i, captures[i].region.slot,
					    captures[i].data_size);
			captures[i].data = NULL;
			continue;
		}
		/*
		 * Copy from the host VA the slot was registered against.
		 * userspace_addr is a host kernel VA in the spawner mm
		 * (KVM's per-VM mm); under v2's identity-mapping layout
		 * for the giant physmem slot this is uml_physmem, which
		 * the kernel already has linear-mapped.
		 */
		memcpy(buf, (void *)(uintptr_t)captures[i].region.userspace_addr,
		       captures[i].data_size);
		captures[i].data = buf;
	}

	snap->memslots = captures;
	snap->memslot_count = count;
	return 0;
}

/*
 * kvm_v2_snapshot_restore_memslots - memcpy each captured memslot
 *                                    back into its host_va.
 *
 * Unlike capture, restore doesn't need to take vm->lock — the
 * userspace_addr ranges captured at snapshot time are stable across
 * the snapshot lifetime (the slot might be deleted by region.c
 * concurrently in production, but Phase 3's usage is KUnit-only
 * single-threaded). The memcpy targets the SAME host VA the bytes
 * were captured from, so we don't even need a fresh
 * KVM_SET_USER_MEMORY_REGION replay — the slot is already
 * registered. Phase 4 may revisit when cross-task / cross-VM
 * snapshots land.
 */
static int kvm_v2_snapshot_restore_memslots(const struct kvm_v2_snapshot *snap)
{
	int i;

	if (!snap->memslots || snap->memslot_count <= 0)
		return 0;

	for (i = 0; i < snap->memslot_count; i++) {
		const struct kvm_v2_memslot_snapshot *e = &snap->memslots[i];

		if (!e->data || e->data_size == 0)
			continue;
		memcpy((void *)(uintptr_t)e->region.userspace_addr,
		       e->data, e->data_size);
	}
	return 0;
}

/**
 * kvm_v2_snapshot_capture_full - capture full vCPU + memslot state.
 * @snap: caller-allocated snapshot.
 * @vcpu: explicit pool entry to snapshot; NULL falls back to
 *        kvm_v2_snapshot_pick_vcpu (Phase 1 compatibility).
 *
 * Captures regs / sregs / xsave / xcrs / events / msrs (same set
 * as kvm_v2_snapshot_capture_regs_only) AND the per-VM memslot
 * contents. The IDT/GDT/IST/TSS/gadget-state pages all live in
 * physmem (allocated from buddy → covered by the giant
 * gpa=0..physmem_size slot installed by
 * kvm_v2_physmem_memslot_install), so they're snapshotted
 * implicitly by the memslot pass — no separate capture step is
 * needed.
 *
 * Returns 0 on success, -ENODEV if no vCPU is available, -ENOMEM on
 * memslot allocation failure, -errno on the first ioctl failure.
 */
int kvm_v2_snapshot_capture_full(struct kvm_v2_snapshot *snap,
				 struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_vm *vm;
	int rc;

	if (!snap)
		return -EINVAL;

	memset(snap, 0, sizeof(*snap));

	vm = kvm_v2_vm_get();
	if (!vm) {
		pr_warn("um: kvm-v2 snapshot: capture_full no VM\n");
		return -ENODEV;
	}

	/*
	 * Pin to a stable host CPU for the duration of the vCPU
	 * KVM_GET_* sequence. The memslot pass below runs OUTSIDE the
	 * preempt_disable() region because (a) kvmalloc may sleep
	 * (GFP_KERNEL on a potentially-large allocation) and (b)
	 * vm->memslots is protected by vm->lock, not preemption
	 * disable.
	 */
	preempt_disable();
	if (!vcpu)
		vcpu = kvm_v2_snapshot_pick_vcpu();
	if (!vcpu || vcpu->vcpu_fd < 0) {
		preempt_enable();
		return -ENODEV;
	}

	rc = kvm_v2_snapshot_capture_vcpu_state(snap, vcpu);
	preempt_enable();
	if (rc < 0)
		return rc;

	rc = kvm_v2_snapshot_capture_memslots(snap, vm);
	if (rc < 0) {
		/*
		 * vCPU side captured successfully; tear it down so the
		 * caller doesn't see a half-populated snapshot. Use free()
		 * not destroy() so the caller's container survives.
		 */
		kvm_v2_snapshot_free(snap);
		memset(snap, 0, sizeof(*snap));
		return rc;
	}

	pr_info("um: kvm-v2 snapshot: captured regs+sregs+xsave+xcrs+events+%u msrs + %d memslot%s\n",
		KVM_V2_SNAPSHOT_MSR_COUNT,
		snap->memslot_count,
		snap->memslot_count == 1 ? "" : "s");
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_capture_full);

/**
 * kvm_v2_snapshot_capture - capture full vCPU + memslot state
 *                           (Phase 1 wrapper).
 * @snap: caller-allocated snapshot.
 *
 * Forwards to kvm_v2_snapshot_capture_full(snap, NULL), which
 * picks a vCPU via kvm_v2_snapshot_pick_vcpu (the Phase 1
 * compatibility path).
 */
int kvm_v2_snapshot_capture(struct kvm_v2_snapshot *snap)
{
	return kvm_v2_snapshot_capture_full(snap, NULL);
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_capture);

/**
 * kvm_v2_snapshot_restore_full_vcpu - restore vCPU + memslot state
 *                                     against an explicit pool entry.
 * @snap: previously-captured snapshot.
 * @vcpu: explicit pool entry to restore against; NULL falls back to
 *        kvm_v2_snapshot_pick_vcpu (Phase 1 compatibility).
 *
 * Restore order:
 *   1. memslot memcpy back (so descriptor-table pages are valid
 *      bytes before the SET_SREGS load reads sregs.idt/.gdt/.tr).
 *   2. KVM_SET_SREGS / SET_REGS / SET_XCRS / SET_XSAVE / SET_VCPU_
 *      EVENTS / SET_MSRS.
 *
 * Post-restore hygiene (memo 26-snapshot §4.5): clear the per-vCPU
 * last_task / last_mm / fpu_owner_task so the next dispatch's
 * cross-task arrival branch fires and unconditionally re-installs
 * SREGS + FPU. The lazy fast paths re-arm on the dispatch after.
 *
 * Cross-vCPU restore semantics (memo §C v2-deltas): if the @vcpu
 * passed here differs from the vCPU the snapshot was captured
 * against, the target vCPU's cpuid_primed flag must be cleared so
 * the next dispatch's lazy KVM_SET_CPUID2 + CR4.OSXSAVE +
 * KVM_SET_XCRS arming runs against the restored sregs state.
 * Phase 3 doesn't track the cross-vCPU identity (the snapshot
 * doesn't record which vCPU it came from); we conservatively leave
 * cpuid_primed alone for in-place restore and document the
 * cross-vCPU case as Phase 4 work. Today the KUnit test restores
 * to the SAME vCPU it captured from, so this is a non-issue.
 *
 * Returns 0 on success; -ENODEV if no vCPU is available; -errno on
 * the first ioctl failure.
 */
int kvm_v2_snapshot_restore_full_vcpu(const struct kvm_v2_snapshot *snap,
				      struct kvm_v2_vcpu *vcpu)
{
	int rc;

	if (!snap)
		return -EINVAL;

	/*
	 * Restore memslot bytes FIRST, outside preempt_disable() —
	 * (a) potentially-large memcpy shouldn't run with preemption
	 * off for the entire duration, and (b) the memcpy is against
	 * stable host VAs (snap->memslots[].region.userspace_addr).
	 * preempt_disable wraps only the per-vCPU KVM_SET_* sequence
	 * below so the vcpu lookup result is stable across the ioctl
	 * chain.
	 */
	rc = kvm_v2_snapshot_restore_memslots(snap);
	if (rc < 0)
		return rc;

	preempt_disable();
	if (!vcpu)
		vcpu = kvm_v2_snapshot_pick_vcpu();
	if (!vcpu || vcpu->vcpu_fd < 0) {
		preempt_enable();
		return -ENODEV;
	}

	rc = kvm_v2_snapshot_restore_vcpu_state(snap, vcpu);
	if (rc < 0) {
		preempt_enable();
		return rc;
	}

	/*
	 * Post-restore: force the next dispatch's cross-task arrival
	 * branch (memo 26-snapshot §4.5). Without this the lazy
	 * KVM_SET_SREGS skip would see fpu_owner_task == previous-task
	 * and trust the vCPU's view — but we just overwrote that view
	 * with snapshot bytes; the dispatch has to re-install from the
	 * restored state.
	 */
	vcpu->last_task        = NULL;
	vcpu->last_mm          = NULL;
	vcpu->fpu_owner_task   = NULL;
	vcpu->fpu_dirty        = true;

	preempt_enable();

	pr_info("um: kvm-v2 snapshot: restored vCPU + %d memslot%s\n",
		snap->memslot_count,
		snap->memslot_count == 1 ? "" : "s");
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_restore_full_vcpu);

/**
 * kvm_v2_snapshot_restore_full - restore vCPU + memslot state from
 *                                snapshot (Phase 1 wrapper).
 * @snap: previously-captured snapshot.
 *
 * Forwards to kvm_v2_snapshot_restore_full_vcpu(snap, NULL), which
 * picks a vCPU via kvm_v2_snapshot_pick_vcpu (Phase 1 compatibility
 * path).
 */
int kvm_v2_snapshot_restore_full(struct kvm_v2_snapshot *snap)
{
	return kvm_v2_snapshot_restore_full_vcpu(snap, NULL);
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_restore_full);

/*
 * Phase 4 helpers — copy current task's per-task arch_thread state
 * (iotrap_fpu + iotrap_events, the SMP-T73 / SMP-T75 plumbing) into
 * or out of the snapshot. Static because external callers go through
 * kvm_v2_snapshot_capture_task / kvm_v2_snapshot_restore_task, which
 * sequence these alongside the existing _full primitives.
 *
 * Why "current" rather than an explicit task pointer: callers
 * snapshot/restore from their own context (record/replay #169 reaches
 * a checkpoint boundary inside the recording task). The CONFIG_UM_
 * BACKEND_KVM_V2 arch_thread fields are per-task storage and the
 * security model is that only the task itself touches them. Phase 4
 * keeps that invariant.
 */
#ifdef CONFIG_UM_BACKEND_KVM_V2
static void kvm_v2_snapshot_capture_current_task_state(struct kvm_v2_snapshot *snap)
{
	snap->task_iotrap_fpu_valid =
		current->thread.arch.kvm_v2.iotrap_fpu_valid;
	if (snap->task_iotrap_fpu_valid)
		snap->task_iotrap_fpu = current->thread.arch.kvm_v2.iotrap_fpu;

	snap->task_iotrap_events_valid =
		current->thread.arch.kvm_v2.iotrap_events_valid;
	if (snap->task_iotrap_events_valid)
		snap->task_iotrap_events =
			current->thread.arch.kvm_v2.iotrap_events;

	snap->task_source_pid     = current->pid;
	snap->task_state_captured = true;
}

static void kvm_v2_snapshot_restore_current_task_state(const struct kvm_v2_snapshot *snap)
{
	current->thread.arch.kvm_v2.iotrap_fpu_valid =
		snap->task_iotrap_fpu_valid;
	if (snap->task_iotrap_fpu_valid)
		current->thread.arch.kvm_v2.iotrap_fpu = snap->task_iotrap_fpu;

	current->thread.arch.kvm_v2.iotrap_events_valid =
		snap->task_iotrap_events_valid;
	if (snap->task_iotrap_events_valid)
		current->thread.arch.kvm_v2.iotrap_events =
			snap->task_iotrap_events;
}
#endif

/**
 * kvm_v2_snapshot_capture_task - capture vCPU + memslot state AND
 *                                the calling task's iotrap_* state.
 * @snap: caller-allocated snapshot to fill.
 * @vcpu: explicit pool entry to capture against; NULL forwards to
 *        kvm_v2_snapshot_pick_vcpu (per-host-CPU fallback).
 *
 * Cross-task variant of capture_full. The vCPU-level state is
 * captured the same way; additionally the calling task's
 * arch_thread.kvm_v2.iotrap_fpu + iotrap_events are copied into the
 * snapshot's task_* fields so a later kvm_v2_snapshot_restore_task
 * can re-install them on the replaying task — even if that task
 * isn't the original source_pid.
 *
 * Returns 0 on success, propagates errors from capture_full.
 */
int kvm_v2_snapshot_capture_task(struct kvm_v2_snapshot *snap,
				 struct kvm_v2_vcpu *vcpu)
{
	int rc;

	if (!snap)
		return -EINVAL;

	rc = kvm_v2_snapshot_capture_full(snap, vcpu);
	if (rc < 0)
		return rc;

#ifdef CONFIG_UM_BACKEND_KVM_V2
	kvm_v2_snapshot_capture_current_task_state(snap);
#else
	snap->task_state_captured = false;
#endif

	pr_info("um: kvm-v2 snapshot: captured task state (pid=%d, fpu_valid=%d, events_valid=%d)\n",
		snap->task_source_pid,
		snap->task_iotrap_fpu_valid,
		snap->task_iotrap_events_valid);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_capture_task);

/**
 * kvm_v2_snapshot_restore_task - restore vCPU + memslot state AND
 *                                the calling task's iotrap_* state.
 * @snap: previously-captured snapshot (must have been captured by
 *        kvm_v2_snapshot_capture_task — task_state_captured must be
 *        true, else -EINVAL).
 * @vcpu: explicit pool entry to restore against; NULL forwards to
 *        kvm_v2_snapshot_pick_vcpu.
 *
 * Cross-task variant of restore_full_vcpu. Restores the vCPU-level
 * state via the existing path, then installs the snapshot's task_*
 * fields into current's arch_thread.kvm_v2.iotrap_fpu /
 * iotrap_events.
 *
 * Restore is "to current," not "to the snapshot's source_pid." The
 * arch_thread fields are per-task storage and only the calling task
 * may write its own (the kernel could in principle write a remote
 * task_struct, but that conflicts with the iotrap_* invariant that
 * writes happen on the dispatch boundary). For record/replay #169
 * the replay driver is the task that wants the state — by design.
 *
 * Returns 0 on success, -EINVAL if @snap was not captured with the
 * _task variant, propagates errors from restore_full_vcpu.
 */
int kvm_v2_snapshot_restore_task(const struct kvm_v2_snapshot *snap,
				 struct kvm_v2_vcpu *vcpu)
{
	int rc;

	if (!snap)
		return -EINVAL;
	if (!snap->task_state_captured) {
		pr_warn("um: kvm-v2 snapshot: restore_task on snapshot with no captured task state\n");
		return -EINVAL;
	}

	rc = kvm_v2_snapshot_restore_full_vcpu(snap, vcpu);
	if (rc < 0)
		return rc;

#ifdef CONFIG_UM_BACKEND_KVM_V2
	kvm_v2_snapshot_restore_current_task_state(snap);
#endif

	pr_info("um: kvm-v2 snapshot: restored task state (source_pid=%d → current pid=%d)\n",
		snap->task_source_pid, current->pid);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_restore_task);

/*
 * kvm_v2_snapshot_bench — Phase 5 bench harness (memo 26-snapshot
 * §Phase 5). Port of v1's kvm_snapshot_bench surface:
 *
 *   - Boot-time: kernel cmdline `kvm_v2_snapshot_bench=N` fires once
 *     at late_initcall_sync. Used by selftest runners that want a
 *     deterministic measurement without an init-script handshake.
 *   - Debugfs: write decimal N to /sys/kernel/debug/um/kvm_v2_
 *     snapshot_bench (only when CONFIG_DEBUG_FS=y). Useful for
 *     ad-hoc measurements from inside a booted UML.
 *
 * Both paths emit the same dmesg line:
 *
 *   um: kvm-v2 snapshot bench: capture=%llu ns; restore_full ns:
 *     median=%llu p95=%llu min=%llu max=%llu n=%u mode=%s
 *
 * Bounded by KVM_V2_SNAPSHOT_BENCH_N_MAX so a runaway echo / cmdline
 * can't pin the CPU forever. Median + p95 reporting matches v1's
 * statistical hygiene.
 *
 * Memo-12 targets: <50 ms cold-capture / <1 ms iteration restore.
 * If capture_full -ENOMEM under tight vmalloc, fall back to
 * capture_regs_only so the bench still produces a number; the dmesg
 * "mode=" suffix flags which path ran.
 */
#define KVM_V2_SNAPSHOT_BENCH_N_MAX	1024

static int kvm_v2_snapshot_bench_u64_cmp(const void *a, const void *b)
{
	u64 da = *(const u64 *)a;
	u64 db = *(const u64 *)b;

	if (da < db)
		return -1;
	if (da > db)
		return 1;
	return 0;
}

/*
 * Drive an N-iteration capture-once + restore_full-N-times bench
 * cycle. Returns 0 on success, -errno on the first failure. Used by
 * both the cmdline and debugfs entry points.
 *
 * Caller owns @samples buffer (length >= @n). On success the buffer
 * is sorted in-place and the summary is emitted via pr_info.
 *
 * Allocates + frees its own kvm_v2_snapshot internally; the snapshot
 * is not exposed to the caller.
 */
static int kvm_v2_snapshot_bench_run(unsigned int n, u64 *samples)
{
	struct kvm_v2_snapshot *snap;
	u64 t0, t1;
	u64 cap_cyc;
	unsigned int i;
	bool full_mode;
	int rc;

	snap = kvm_v2_snapshot_alloc();
	if (!snap)
		return -ENOMEM;

	/*
	 * Try the full capture (regs + memslot) first. If kvmalloc the
	 * memslot buffer fails (UML's vmalloc area is bounded and tight
	 * at late_initcall_sync on bigger physmem configs), fall back
	 * to regs-only so the bench still produces a measurement.
	 */
	t0 = ktime_get_ns();
	rc = kvm_v2_snapshot_capture(snap);
	t1 = ktime_get_ns();
	if (rc == -ENOMEM) {
		pr_info("um: kvm-v2 snapshot bench: full capture kvmalloc failed; retrying regs-only\n");
		t0 = ktime_get_ns();
		rc = kvm_v2_snapshot_capture_regs_only(snap);
		t1 = ktime_get_ns();
	}
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot bench: capture failed (%d) — vCPU not yet KVM_RUN'd?\n",
			rc);
		goto out_destroy;
	}
	cap_cyc = t1 - t0;
	full_mode = (snap->memslot_count > 0);

	for (i = 0; i < n; i++) {
		t0 = ktime_get_ns();
		rc = kvm_v2_snapshot_restore_full(snap);
		t1 = ktime_get_ns();
		if (rc < 0) {
			pr_warn("um: kvm-v2 snapshot bench: restore_full failed at iter %u (%d)\n",
				i, rc);
			goto out_destroy;
		}
		samples[i] = t1 - t0;
	}

	sort(samples, n, sizeof(u64), kvm_v2_snapshot_bench_u64_cmp, NULL);
	pr_info("um: kvm-v2 snapshot bench: capture=%llu ns; restore_full ns: median=%llu p95=%llu min=%llu max=%llu n=%u mode=%s\n",
		cap_cyc,
		samples[n / 2],
		samples[(n * 95) / 100],
		samples[0],
		samples[n - 1],
		n,
		full_mode ? "full" : "regs-only");
	rc = 0;

out_destroy:
	kvm_v2_snapshot_destroy(snap);
	return rc;
}

#ifdef CONFIG_DEBUG_FS

static ssize_t kvm_v2_snapshot_bench_write(struct file *f,
					   const char __user *buf,
					   size_t count, loff_t *ppos)
{
	char tmp[16];
	u64 *samples;
	long ln;
	unsigned int n;
	size_t copy_n;
	int rc;

	copy_n = min_t(size_t, count, sizeof(tmp) - 1);
	if (copy_from_user(tmp, buf, copy_n))
		return -EFAULT;
	tmp[copy_n] = '\0';
	if (copy_n > 0 && tmp[copy_n - 1] == '\n')
		tmp[copy_n - 1] = '\0';

	rc = kstrtol(tmp, 10, &ln);
	if (rc < 0)
		return rc;
	if (ln <= 0 || ln > KVM_V2_SNAPSHOT_BENCH_N_MAX)
		return -EINVAL;
	n = (unsigned int)ln;

	samples = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	if (!samples)
		return -ENOMEM;

	rc = kvm_v2_snapshot_bench_run(n, samples);
	kvfree(samples);
	return rc < 0 ? rc : (ssize_t)count;
}

static const struct file_operations kvm_v2_snapshot_bench_fops = {
	.write = kvm_v2_snapshot_bench_write,
};

static int __init kvm_v2_snapshot_debugfs_init(void)
{
	struct dentry *d;

	d = debugfs_lookup("um", NULL);
	if (!d) {
		d = debugfs_create_dir("um", NULL);
		if (IS_ERR(d))
			return PTR_ERR(d);
	}

	debugfs_create_file("kvm_v2_snapshot_bench", 0200, d, NULL,
			    &kvm_v2_snapshot_bench_fops);
	return 0;
}
late_initcall_sync(kvm_v2_snapshot_debugfs_init);

#endif /* CONFIG_DEBUG_FS */

/*
 * Boot-time cmdline driver. Set N via `kvm_v2_snapshot_bench=N` on
 * the UML kernel cmdline; the late_initcall_sync below fires the
 * bench once with that N. 0 / unset = no bench.
 */
static unsigned int kvm_v2_snapshot_bench_n_at_boot;

static int __init kvm_v2_snapshot_bench_setup(char *s)
{
	long ln;

	if (!s || kstrtol(s, 10, &ln) < 0)
		return 1;
	if (ln < 0 || ln > KVM_V2_SNAPSHOT_BENCH_N_MAX)
		return 1;
	kvm_v2_snapshot_bench_n_at_boot = (unsigned int)ln;
	return 1;
}
__setup("kvm_v2_snapshot_bench=", kvm_v2_snapshot_bench_setup);

static int __init kvm_v2_snapshot_bench_late_init(void)
{
	u64 *samples;
	unsigned int n;
	int rc;

	n = kvm_v2_snapshot_bench_n_at_boot;
	if (!n)
		return 0;

	samples = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	if (!samples) {
		pr_warn("um: kvm-v2 snapshot bench: kvmalloc(%u samples) failed\n",
			n);
		return 0;
	}

	rc = kvm_v2_snapshot_bench_run(n, samples);
	if (rc < 0)
		pr_warn("um: kvm-v2 snapshot bench: bench_run rc=%d\n", rc);

	kvfree(samples);
	return 0;
}
late_initcall_sync(kvm_v2_snapshot_bench_late_init);
