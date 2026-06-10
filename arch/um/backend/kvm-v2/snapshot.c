// SPDX-License-Identifier: GPL-2.0
/*
 * KVM v2 snapshot support.
 *
 * A snapshot contains one pool vCPU's architectural state plus an
 * optional copy of each registered memslot.  The vCPU side captures the
 * full XSAVE image and XCR0 so AVX state can round-trip without falling
 * back to legacy FPU state.  The memory side snapshots the memslot list
 * as independent descriptors so future slots do not change the container
 * ABI.
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

/* Order matches snap->msrs.entries[]. */
static const u32 kvm_v2_snapshot_msr_indices[KVM_V2_SNAPSHOT_MSR_COUNT] = {
	0xC0000082,	/* MSR_LSTAR */
	0xC0000081,	/* MSR_STAR */
	0xC0000084,	/* MSR_SYSCALL_MASK */
	0xC0000102,	/* MSR_KERNEL_GS_BASE */
	0xC0000100,	/* MSR_FS_BASE */
	0xC0000101,	/* MSR_GS_BASE */
	0xC0000080,	/* MSR_EFER */
};

/*
 * kvm_v2_snapshot_pick_vcpu - resolve the pool entry the snapshot
 *                             should target for the calling task.
 *
 * The vCPU whose last_task == current is the pool entry whose KVM-owned
 * state holds current's most recent dispatch result. If current has not
 * dispatched through KVM yet, fall back to the vCPU pinned to the current
 * host CPU; that is where the next dispatch would run.
 *
 * Caller responsibility: hold preempt_disable() across the lookup
 * and the subsequent ioctl calls so the per-CPU vCPU mapping
 * doesn't shift mid-flight.
 *
 * Returns NULL if the pool is not initialised yet.
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
	 * KVM_GET_XSAVE returns the fixed 4 KB struct kvm_xsave shape.  The
	 * variable-size KVM_GET_XSAVE2 ioctl is only needed for dynamic XCR0
	 * features that v2 does not currently expose.
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_GET_XSAVE,
			      (unsigned long)&snap->xsave);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot: KVM_GET_XSAVE failed (%d)\n", rc);
		return rc;
	}

	/*
	 * Capture XCR0 so restore interprets the XSAVE payload with the same
	 * component mask that was active at capture time.
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
 * KVM ordering invariant: SREGS must precede REGS because KVM validates
 * RIP/RSP against the post-SREGS segment cache during KVM_SET_REGS.
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
	 * KVM_SET_* takes a non-const argp from userspace POV but these ioctls
	 * do not modify the buffers. Avoid stack copies: kvm_xsave alone is
	 * 4 KB.
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
	 * XCRS before XSAVE - KVM cross-validates the XSAVE area's
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
 * safe. Each memslot entry owns its data buffer.
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
 * KVM_GET_XSAVE is used instead of KVM_GET_FPU so extended components
 * such as YMM upper halves are preserved.
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

/*
 * kvm_v2_snapshot_capture_memslots - copy each memslot's bytes into
 *                                    a fresh per-entry kvmalloc'd
 *                                    buffer hung off snap->memslots.
 *
 * Walks vm->memslots under vm->lock to count and then snapshot the
 * registered slots. Two-pass design avoids holding vm->lock across
 * the (potentially large) kvmalloc allocations and the memcpy's
 * themselves - the locked first pass produces a thread-local array
 * of (slot_id, host_va, size, flags, gpa) triples, then we drop
 * the lock and do the allocations + memcpy's against the snapshotted
 * descriptors.
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
		 * The physmem slot can be too large to duplicate on a small UML
		 * instance. Keep the slot descriptor and skip its byte payload
		 * rather than failing snapshots that can still provide vCPU
		 * state and any smaller data-bearing slots.
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
 * Restore writes captured bytes back to the host VA recorded in each
 * descriptor. The KVM memslot itself is already registered for the
 * current VM, so restore does not replay KVM_SET_USER_MEMORY_REGION.
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
 * @vcpu: explicit pool entry to snapshot; NULL picks current's pool vCPU.
 *
 * Captures regs / sregs / xsave / xcrs / events / msrs (same set
 * as kvm_v2_snapshot_capture_regs_only) AND the per-VM memslot
 * contents. The IDT/GDT/IST/TSS/gadget-state pages all live in
 * physmem (allocated from buddy -> covered by the giant
 * gpa=0..physmem_size slot installed by
 * kvm_v2_physmem_memslot_install), so the memslot pass also captures
 * those pages.
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
 * kvm_v2_snapshot_capture - capture full vCPU + memslot state.
 * @snap: caller-allocated snapshot.
 *
 * Forwards to kvm_v2_snapshot_capture_full(snap, NULL), which picks the
 * current task's pool vCPU.
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
 * @vcpu: explicit pool entry to restore against; NULL picks current's
 *        pool vCPU.
 *
 * Restore order:
 *   1. memslot memcpy back (so descriptor-table pages are valid
 *      bytes before the SET_SREGS load reads sregs.idt/.gdt/.tr).
 *   2. KVM_SET_SREGS / SET_REGS / SET_XCRS / SET_XSAVE / SET_VCPU_
 *      EVENTS / SET_MSRS.
 *
 * Post-restore hygiene: clear the per-vCPU task ownership cache so the
 * next dispatch reloads architectural state instead of trusting stale
 * lazy-path ownership.
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
	 * Restore memslot bytes FIRST, outside preempt_disable() -
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

	/* Force the next dispatch to reload state after the snapshot write. */
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
 * kvm_v2_snapshot_restore_full - restore vCPU + memslot state.
 * @snap: previously-captured snapshot.
 *
 * Forwards to kvm_v2_snapshot_restore_full_vcpu(snap, NULL), which picks
 * the current task's pool vCPU.
 */
int kvm_v2_snapshot_restore_full(struct kvm_v2_snapshot *snap)
{
	return kvm_v2_snapshot_restore_full_vcpu(snap, NULL);
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_restore_full);

/*
 * Copy current's saved KVM-trap task state into or out of the snapshot.
 * External callers use kvm_v2_snapshot_capture_task() and
 * kvm_v2_snapshot_restore_task() so task state is sequenced with the vCPU
 * and memslot state.
 *
 * Why "current" rather than an explicit task pointer: callers
 * Callers snapshot/restore from their own context. The arch_thread fields
 * are per-task storage, and only current may write its own saved state.
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
 * can re-install them on the replaying task - even if that task
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
 *        kvm_v2_snapshot_capture_task - task_state_captured must be true.
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
 * writes happen on the dispatch boundary). Record/replay callers restore
 * from the task that consumes the captured state.
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

	pr_info("um: kvm-v2 snapshot: restored task state (source_pid=%d -> current pid=%d)\n",
		snap->task_source_pid, current->pid);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_restore_task);

/*
 * kvm_v2_snapshot_bench - simple capture/restore benchmark.
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
 * Bounded by KVM_V2_SNAPSHOT_BENCH_N_MAX so a runaway echo or cmdline
 * cannot pin the CPU forever.
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
		pr_warn("um: kvm-v2 snapshot bench: capture failed (%d) - vCPU not yet KVM_RUN'd?\n",
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
