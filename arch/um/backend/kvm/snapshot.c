// SPDX-License-Identifier: GPL-2.0
/*
 * KVM-backend snapshot/forkserver — capture/restore primitives
 * (task #250 v2 ladder, memo 12).
 *
 * The C-09 v1 forkserver in arch/um/kernel/snapshot.c uses fork()
 * to spawn fuzz workers, which doesn't compose with the KVM
 * backend (per-vCPU fd aliasing + shared kvm_run mmap +
 * memslot host-VA divergence — see commit 7f79b35e1531). This
 * TU is the v2 path: capture vCPU + memslot state via KVM
 * ioctls, then restore in-place per fuzz iteration without
 * forking.
 *
 * Today this provides the building blocks (capture / restore_full
 * / free) but no integration into um_snapshot_ready yet — that's
 * step 3 of the memo-12 ladder, blocked on the KVM-aware path
 * needing a separate routine in arch/um/kernel/snapshot.c.
 *
 * Design memo: Documentation/virt/uml/redesign/02-workstreams/
 *              D-kvm-backend/12-snapshot-forkserver-kvm.md
 */

#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <os.h>
#include <mem.h>		/* uml_physmem */
#include <as-layout.h>		/* physmem_size */

#include "kvm_backend.h"

/*
 * struct kvm_snapshot — opaque container for a captured vCPU +
 * memslot state. Allocated by kvm_snapshot_capture, freed by
 * kvm_snapshot_free. The struct itself is tiny; the bulk of the
 * memory lives in the embedded buffers (memslot copy is the big
 * one, sized by physmem_size).
 *
 * MSRs we care about for UML's KVM backend:
 *   MSR_LSTAR, MSR_STAR, MSR_FMASK, MSR_KERNEL_GS_BASE,
 *   MSR_FS_BASE, MSR_GS_BASE, MSR_EFER
 * Seven entries; allocated as kvm_msrs with nmsrs=7.
 *
 * FPU state captures XSAVE area (AVX/AVX-512). MMIO regions are
 * registered via the same memslot path under Policy A so they
 * round-trip through the memslot copy.
 */
#define KVM_SNAPSHOT_MSR_COUNT	7

struct kvm_snapshot {
	struct kvm_regs		regs;
	struct kvm_sregs	sregs;
	struct kvm_fpu		fpu;
	struct kvm_vcpu_events	events;

	/* MSR list — variable-length but bounded; kept inline. */
	struct {
		__u32 nmsrs;
		__u32 pad;
		struct kvm_msr_entry entries[KVM_SNAPSHOT_MSR_COUNT];
	} msrs;

	/*
	 * Memslot snapshot. Singleton under Policy A: one slot
	 * covers all of [0, physmem_size). v1 captures the full
	 * memslot; v2 (memo-12 step 4) adds dirty-bitmap delta
	 * capture for sub-millisecond iteration cost.
	 */
	void	*mem_backing;
	size_t	mem_size;
};

/* Static MSR index list. Order matches kvm_msrs.entries[]. */
static const u32 kvm_snapshot_msr_indices[KVM_SNAPSHOT_MSR_COUNT] = {
	0xC0000082,	/* MSR_LSTAR */
	0xC0000081,	/* MSR_STAR */
	0xC0000084,	/* MSR_FMASK */
	0xC0000102,	/* MSR_KERNEL_GS_BASE */
	0xC0000100,	/* MSR_FS_BASE */
	0xC0000101,	/* MSR_GS_BASE */
	0xC0000080,	/* MSR_EFER */
};

/**
 * kvm_snapshot_capture - capture full vCPU + memslot state.
 * @snap: caller-allocated snapshot struct (kvm_snapshot_alloc'd
 *        or stack-allocated)
 *
 * Issues KVM_GET_REGS / KVM_GET_SREGS / KVM_GET_FPU /
 * KVM_GET_VCPU_EVENTS / KVM_GET_MSRS against vcpu0_fd to
 * snapshot the architectural state, then memcpy's the entire
 * memslot from the host VA backing store into a kvmalloc'd
 * buffer.
 *
 * Returns 0 on success; -errno on any ioctl/allocation failure.
 * On failure, partial state may have been captured but the
 * caller must call kvm_snapshot_free to release any allocations.
 */
int kvm_snapshot_capture(struct kvm_snapshot *snap)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	unsigned int i;
	int rc;

	if (!snap)
		return -EINVAL;
	if (vcpu_fd < 0)
		return -ENODEV;
	if (!uml_physmem || !physmem_size)
		return -EAGAIN;

	memset(snap, 0, sizeof(*snap));

	/*
	 * vCPU registers: GP regs first (cheapest, also what's most
	 * likely to fail under DYNAMIC backend if the vCPU was
	 * never actually KVM_RUN'd).
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&snap->regs);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_GET_REGS failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
			      (unsigned long)&snap->sregs);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_GET_SREGS failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_FPU,
			      (unsigned long)&snap->fpu);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_GET_FPU failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_VCPU_EVENTS,
			      (unsigned long)&snap->events);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_GET_VCPU_EVENTS failed (%d)\n",
			rc);
		return rc;
	}

	/* MSRs: prepare index list, KVM_GET_MSRS fills values. */
	snap->msrs.nmsrs = KVM_SNAPSHOT_MSR_COUNT;
	for (i = 0; i < KVM_SNAPSHOT_MSR_COUNT; i++) {
		snap->msrs.entries[i].index = kvm_snapshot_msr_indices[i];
		snap->msrs.entries[i].reserved = 0;
		snap->msrs.entries[i].data = 0;
	}
	rc = os_ioctl_generic(vcpu_fd, KVM_GET_MSRS,
			      (unsigned long)&snap->msrs);
	if (rc < 0 || rc != KVM_SNAPSHOT_MSR_COUNT) {
		pr_warn("um: kvm snapshot: KVM_GET_MSRS rc=%d (expected %d)\n",
			rc, KVM_SNAPSHOT_MSR_COUNT);
		if (rc >= 0)
			rc = -EIO;
		return rc;
	}

	/*
	 * Memslot copy. Under Policy A there's exactly one memslot
	 * covering [0, physmem_size) at host VA uml_physmem. Use
	 * vmalloc for the backing buffer because physmem_size can
	 * exceed kmalloc's MAX_ORDER-bounded ceiling on tiny-pages
	 * configs; vmalloc handles up to vmalloc_total without
	 * needing physical contiguity.
	 */
	snap->mem_size = physmem_size;
	snap->mem_backing = vmalloc(snap->mem_size);
	if (!snap->mem_backing) {
		pr_warn("um: kvm snapshot: vmalloc(%zu) failed\n",
			snap->mem_size);
		return -ENOMEM;
	}
	memcpy(snap->mem_backing, (void *)uml_physmem, snap->mem_size);

	pr_info("um: kvm snapshot: captured regs+sregs+fpu+events+%u msrs + %zu MiB physmem\n",
		KVM_SNAPSHOT_MSR_COUNT,
		snap->mem_size >> 20);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_snapshot_capture);

/**
 * kvm_snapshot_restore_full - restore vCPU + memslot from snapshot.
 * @snap: previously-captured snapshot
 *
 * v1 full-restore path: memcpy the entire memslot back from the
 * snapshot buffer, then push vCPU state via KVM_SET_*. Cost is
 * O(physmem_size) bound by memory bandwidth (~5-50 ms for typical
 * 64-256 MiB UML configs).
 *
 * v2 dirty-bitmap fast path (memo 12 step 4) will skip the
 * full memcpy in favor of restoring only pages KVM marked dirty
 * since the snapshot, dropping per-iteration cost to <1 ms.
 *
 * Returns 0 on success; -errno on the first ioctl/copy failure.
 */
int kvm_snapshot_restore_full(struct kvm_snapshot *snap)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	int rc;

	if (!snap || !snap->mem_backing)
		return -EINVAL;
	if (vcpu_fd < 0)
		return -ENODEV;
	if (snap->mem_size != physmem_size) {
		pr_warn("um: kvm snapshot: mem_size mismatch (snap=%zu cur=%llu)\n",
			snap->mem_size, physmem_size);
		return -EINVAL;
	}

	memcpy((void *)uml_physmem, snap->mem_backing, snap->mem_size);

	/*
	 * SREGS first because KVM validates RIP/RSP against the
	 * post-SREGS segment cache; setting REGS before SREGS can
	 * cause KVM to reject the load.
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS,
			      (unsigned long)&snap->sregs);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_SET_SREGS failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
			      (unsigned long)&snap->regs);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_SET_REGS failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_FPU,
			      (unsigned long)&snap->fpu);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_SET_FPU failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_VCPU_EVENTS,
			      (unsigned long)&snap->events);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_SET_VCPU_EVENTS failed (%d)\n",
			rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS,
			      (unsigned long)&snap->msrs);
	if (rc < 0 || rc != KVM_SNAPSHOT_MSR_COUNT) {
		pr_warn("um: kvm snapshot: KVM_SET_MSRS rc=%d (expected %d)\n",
			rc, KVM_SNAPSHOT_MSR_COUNT);
		if (rc >= 0)
			rc = -EIO;
		return rc;
	}

	pr_info("um: kvm snapshot: restored vCPU + %zu MiB physmem\n",
		snap->mem_size >> 20);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_snapshot_restore_full);

/**
 * kvm_snapshot_free - release snapshot-owned allocations.
 * @snap: snapshot whose allocations should be freed; the
 *        struct itself is not freed (caller owns it).
 *
 * Idempotent: passing a never-captured or already-freed snapshot
 * is safe (no allocations to release).
 */
void kvm_snapshot_free(struct kvm_snapshot *snap)
{
	if (!snap)
		return;
	if (snap->mem_backing) {
		vfree(snap->mem_backing);
		snap->mem_backing = NULL;
	}
	snap->mem_size = 0;
}
EXPORT_SYMBOL_GPL(kvm_snapshot_free);

/**
 * kvm_snapshot_alloc - allocate a fresh snapshot container.
 *
 * Returns a zero-initialized snapshot struct on the heap, or
 * NULL on allocation failure. Caller pairs with
 * kvm_snapshot_free + kfree of the returned pointer.
 */
struct kvm_snapshot *kvm_snapshot_alloc(void)
{
	return kzalloc(sizeof(struct kvm_snapshot), GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(kvm_snapshot_alloc);

/**
 * kvm_snapshot_destroy - free both the snapshot's allocations
 *                        and the container itself.
 * @snap: snapshot to release; can be NULL.
 */
void kvm_snapshot_destroy(struct kvm_snapshot *snap)
{
	if (!snap)
		return;
	kvm_snapshot_free(snap);
	kfree(snap);
}
EXPORT_SYMBOL_GPL(kvm_snapshot_destroy);
