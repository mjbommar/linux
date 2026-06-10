// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend memslot record helpers.
 *
 * This file owns the per-VM list of registered memslots and the bitmap
 * that hands out KVM_SET_USER_MEMORY_REGION slot ids. Callers own KVM
 * ioctl sequencing; these helpers only allocate records, publish them on
 * @vm->memslots, and free them during VM teardown. All exported helpers
 * take @vm->lock internally.
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

static int kvm_v2_memslot_alloc_id(struct kvm_v2_vm *vm)
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

static void kvm_v2_memslot_free_id(struct kvm_v2_vm *vm, u32 slot_id)
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
