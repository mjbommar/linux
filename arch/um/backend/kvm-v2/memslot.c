// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend memslot allocator and lookup helpers.
 *
 * This file owns the per-VM list of registered memslots and the bitmap
 * that hands out KVM_SET_USER_MEMORY_REGION slot ids. It does not issue
 * KVM ioctls; callers own the ioctl sequencing.
 *
 * Why a bitmap and not e.g. an idr / xarray:
 *   The slot-id space is small (KVM_V2_MAX_USER_MEM_SLOTS == 32767),
 *   bounded, and dense in practice (a long-running UML guest will fill
 *   it in well under a minute of fork/exec churn). A flat bitmap fits
 *   in 4 KiB and gives O(slots) allocation with cache-friendly scans.
 *   idr would allocate per-id, xarray would over-engineer for the
 *   workload.
 *
 * Why list_head, not a hash/tree:
 *   Lookup-by-GPA traffic is dominated by exit-handler paths that
 *   already do per-region bookkeeping (mm_region_added/removed
 *   are "build a region object, hand it to the backend" calls; one
 *   per drained TLB entry). At expected densities (hundreds of regions
 *   per running mm, not tens of thousands) a
 *   linear list under spinlock is simpler than a tree and avoids forcing
 *   GPA-overlap policy into tree keys.
 *   The alloc/free API keeps the backing data structure private to
 *   this file.
 *
 * Locking: every public function in this file acquires vm->lock.
 *   Callers must not hold vm->lock when calling in. This file uses
 *   scoped_guard(spinlock, &vm->lock); spin_lock_irqsave is
 *   unnecessary because the only contention is between the kernel
 *   thread TLB drain path and v2's exit handler, neither of which runs
 *   from hard-irq context.
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

	m = kzalloc_obj(*m, GFP_KERNEL);
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
	 * Returning the pointer after dropping the lock is safe for the
	 * current callers: deletion also takes vm->lock, and lookups run from
	 * the vCPU exit path that owns the active memslot walk. If a caller
	 * needs lock-free lookup, convert the list to RCU without
	 * changing this API.
	 */
	scoped_guard(spinlock, &vm->lock) {
		list_for_each_entry(m, &vm->memslots, list) {
			if (gpa >= m->gpa && gpa < m->gpa + m->size)
				return m;
		}
	}
	return NULL;
}
