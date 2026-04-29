// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase B.1: memslot allocator + lookup.
 *
 * Per memo 26 §B.1. This TU owns the per-VM list of registered memslots
 * and the bitmap that hands out KVM_SET_USER_MEMORY_REGION slot ids.
 * It does NOT issue any KVM ioctl — the wiring of mm_region_added /
 * mm_region_removed onto KVM_SET_USER_MEMORY_REGION lands in B.2/B.3,
 * and CR3 / mmu_notifier work in B.5/B.6. Until then the allocator is
 * reachable but unused; B.2 turns it on.
 *
 * Why a bitmap and not e.g. an idr / xarray:
 *   The slot-id space is small (KVM_V2_MAX_USER_MEM_SLOTS == 32767),
 *   bounded, and dense in practice (a long-running UML guest will fill
 *   it in well under a minute of fork/exec churn). A flat bitmap fits
 *   in 4 KiB and gives O(slots) allocation with cache-friendly scans.
 *   idr would allocate per-id, xarray would over-engineer for the
 *   workload. v1 archive's policy-A "one giant slot" model used no
 *   allocator at all; B.1 is the first commit where we need one.
 *
 * Why list_head, not a hash/tree:
 *   Phase B's lookup-by-GPA traffic is dominated by exit-handler paths
 *   that already do per-region bookkeeping (mm_region_added/removed
 *   are "build a region object, hand it to the backend" calls — one
 *   per drained TLB entry). At the densities memo 26 anticipates
 *   (hundreds of regions per running mm, not tens of thousands) a
 *   linear list under spinlock is simpler than a tree and avoids the
 *   GPA-overlap edge cases a tree would force us to encode in keys.
 *   If profiling later shows the list is hot, B.1's struct-shape +
 *   alloc/free API are stable enough to swap in a maple tree without
 *   touching B.2-B.6.
 *
 * Locking: every public function in this TU acquires vm->lock. Callers
 *   (B.2's mm_region_added wiring, B.6's mmu_notifier validator) must
 *   NOT hold vm->lock when calling in. Inside this TU we use
 *   scoped_guard(spinlock, &vm->lock) per memo 27 Part E ("kernel
 *   idioms first") — the spin_lock_irqsave dance is unnecessary
 *   because the only contention is between the (kernel-thread) tlb
 *   drain path and v2's eventual exit handler; neither runs from
 *   hard-irq context.
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <asm/trace/um_backend.h>

#include "kvm_v2_backend.h"

int kvm_v2_memslot_alloc_id(struct kvm_v2_vm *vm)
{
	unsigned int id;

	if (!vm)
		return -EINVAL;

	scoped_guard(spinlock, &vm->lock) {
		id = find_first_zero_bit(vm->memslot_bitmap,
					 KVM_V2_MAX_USER_MEM_SLOTS);
		if (id >= KVM_V2_MAX_USER_MEM_SLOTS)
			return -ENOSPC;
		__set_bit(id, vm->memslot_bitmap);
	}
	return (int)id;
}

void kvm_v2_memslot_free_id(struct kvm_v2_vm *vm, u32 slot_id)
{
	if (!vm)
		return;
	if (slot_id >= KVM_V2_MAX_USER_MEM_SLOTS) {
		WARN_ONCE(1, "um: kvm-v2 memslot_free_id: out-of-range id %u\n",
			  slot_id);
		return;
	}

	scoped_guard(spinlock, &vm->lock) {
		if (!__test_and_clear_bit(slot_id, vm->memslot_bitmap))
			WARN_ONCE(1, "um: kvm-v2 memslot_free_id: id %u already free\n",
				  slot_id);
	}
}

int kvm_v2_memslot_add(struct kvm_v2_vm *vm, u64 gpa, u64 host_va,
		       u64 size, u32 flags)
{
	struct kvm_v2_memslot *m;
	int id;

	if (!vm)
		return -EINVAL;
	if (!size)
		return -EINVAL;

	id = kvm_v2_memslot_alloc_id(vm);
	if (id < 0)
		return id;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m) {
		kvm_v2_memslot_free_id(vm, (u32)id);
		return -ENOMEM;
	}

	m->slot_id = (u32)id;
	m->gpa     = gpa;
	m->host_va = host_va;
	m->size    = size;
	m->flags   = flags;
	INIT_LIST_HEAD(&m->list);

	scoped_guard(spinlock, &vm->lock)
		list_add_tail(&m->list, &vm->memslots);

	trace_um_backend_kvm_v2_memslot_add(m->slot_id, m->gpa, m->host_va,
					    m->size, m->flags);
	return id;
}

void kvm_v2_memslot_del(struct kvm_v2_vm *vm, u32 slot_id)
{
	struct kvm_v2_memslot *m, *found = NULL;

	if (!vm)
		return;

	scoped_guard(spinlock, &vm->lock) {
		list_for_each_entry(m, &vm->memslots, list) {
			if (m->slot_id == slot_id) {
				list_del(&m->list);
				found = m;
				break;
			}
		}
	}

	if (!found) {
		WARN_ONCE(1, "um: kvm-v2 memslot_del: slot_id %u not on list\n",
			  slot_id);
		return;
	}

	/*
	 * Free the bitmap bit AFTER unlinking so a concurrent allocator
	 * scan can't hand the same id out while the entry is still
	 * traversable. Bitmap-free takes the same lock, but that's fine
	 * once the entry is unlinked.
	 */
	kvm_v2_memslot_free_id(vm, slot_id);

	trace_um_backend_kvm_v2_memslot_del(slot_id);
	kfree(found);
}

struct kvm_v2_memslot *kvm_v2_memslot_lookup(struct kvm_v2_vm *vm, u64 gpa)
{
	struct kvm_v2_memslot *m;

	if (!vm)
		return NULL;

	/*
	 * Returning the pointer out from under the lock is safe in B.1
	 * because the only deletion path (kvm_v2_memslot_del) is also
	 * under vm->lock and the lookup's caller in B.2+ will be the
	 * exit handler running on the vCPU that triggered the lookup —
	 * the slot can't go away mid-walk. If lock-free lookup is
	 * needed later (e.g., from mmu_notifier callback context),
	 * convert to RCU; the API doesn't change.
	 */
	scoped_guard(spinlock, &vm->lock) {
		list_for_each_entry(m, &vm->memslots, list) {
			if (gpa >= m->gpa && gpa < m->gpa + m->size)
				return m;
		}
	}
	return NULL;
}
