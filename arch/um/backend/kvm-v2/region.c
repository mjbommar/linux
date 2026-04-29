// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase B.2/B.3: mm_region_added /
 * mm_region_removed wiring onto KVM_SET_USER_MEMORY_REGION
 * (add / delete).
 *
 * Per memo 26 §B.2 + §B.3. B.1 shipped the slot-id allocator +
 * in-memory memslot list; this TU is the first caller.
 *
 * mm_region_added (B.2):
 *   (a) calls seccomp_mm_region_added so the seccomp stub child
 *       (which still owns guest user-mode execution until Phase D
 *       wires KVM_RUN) keeps the mapping in its address space —
 *       without this the guest faults every memory access;
 *   (b) reserves a slot id + list entry via kvm_v2_memslot_add;
 *   (c) issues KVM_SET_USER_MEMORY_REGION add against the per-VM
 *       fd, populating the KVM memslot table for the eventual
 *       Phase D / TDP path.
 * On ioctl failure the list entry is unwound; the seccomp mapping
 * stays in place (the corresponding remove will tear it down).
 *
 * mm_region_removed (B.3):
 *   (a) looks up the slot via kvm_v2_memslot_lookup(gpa);
 *   (b) issues KVM_SET_USER_MEMORY_REGION with memory_size=0
 *       (KVM's "delete this slot" syntax). KVM's mmu_notifier
 *       auto-invalidates EPT;
 *   (c) frees the bitmap bit + list entry via kvm_v2_memslot_del;
 *   (d) calls seccomp_mm_region_removed so the stub child unmaps
 *       its own copy.
 * If the lookup fails (ioctl-side add failed earlier; only the
 * seccomp side was populated) we still call seccomp's remove so
 * the stub child stays consistent.
 *
 * The dual-side population is the "v2 delegates to seccomp until
 * Phase D" reality of memo 26's incremental migration. As Phase D
 * lands KVM_RUN and stops needing the stub child, the
 * seccomp_mm_region_added call drops out and we're left with
 * KVM-only memslots.
 *
 * Single-VM model in this commit:
 *   memo 26 §B.2's prose talks about "the per-mm worker's bitmap" but
 *   memo 26 §B.2 itself defers per-mm VM separation to Phase D's
 *   worker-process wiring ("for now use per-VM until the worker model
 *   lands"). A.2 created exactly one struct kvm_v2_vm (file-scope
 *   static in context.c, returned by kvm_v2_vm_get()), so every
 *   mm_region_added call funnels through that one VM. The mm
 *   parameter is intentionally unused — it's plumbed for forward
 *   compat with Phase D's per-mm VM split, but the single-VM model
 *   is sufficient for B.2's "boot smoke must complete" gate.
 *
 * Identity host_va == guest_phys_addr:
 *   memo 26 §B.2 fixes userspace_addr = host_va = region->va and
 *   guest_phys_addr = same value. The giant-slot identity mapping is
 *   what lets us avoid carrying a separate gpa allocator; v2 may
 *   relax this in a later phase but B.2's invariant is "GPA == VA".
 *
 * Why we don't mmap region->va here:
 *   region->va is already host-mapped — UML's tlb.c reaches us
 *   AFTER the mm-arbiter has made the page present (phys_fd is the
 *   physmem fd, the VA is already host-readable). KVM only needs a
 *   host VA that resolves to a valid userspace_addr; we hand it the
 *   same VA the kernel half is already using.
 *
 * Why backend_data is NOT stashed here:
 *   memo 26 §B.1's note suggested stashing slot_id on
 *   region->backend_data so mm_region_removed could free by id
 *   without a gpa-keyed lookup. The struct um_memory_region is
 *   stack-allocated by arch/um/kernel/tlb.c and does NOT persist
 *   between the add and remove calls — they get distinct stack
 *   structs. B.3 will look up by gpa via kvm_v2_memslot_lookup()
 *   instead; backend_data stays NULL.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/mm_types.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <asm/um_memory.h>
#include <backend.h>
#include <os.h>

#include "kvm_v2_backend.h"

int kvm_v2_mm_region_added(struct mm_struct *mm,
			   const struct um_memory_region *region)
{
	struct kvm_v2_vm *vm = kvm_v2_vm_get();
	struct kvm_v2_memslot *existing;
	struct kvm_userspace_memory_region kr;
	int slot_id, rc;
	u32 flags;

	if (!vm) {
		pr_warn_ratelimited("um: kvm-v2 region_added: VM not initialised\n");
		return -ENODEV;
	}
	if (!region || !region->len)
		return -EINVAL;

	/*
	 * Seccomp stub child still owns guest execution (Phase D
	 * replaces it). Tell it about the region first; if that fails
	 * we propagate without touching KVM state.
	 */
	rc = seccomp_mm_region_added(mm, region);
	if (rc < 0)
		return rc;

	flags = (region->prot & UM_PROT_WRITE) ? 0 : KVM_MEM_READONLY;

	/*
	 * tlb.c surfaces "add" for both new mappings AND in-place
	 * updates of existing ones (prot toggle, partial-range
	 * replace). seccomp's um_stub_mm_map handles either via
	 * MAP_FIXED. KVM rejects a fresh slot id at an overlapping
	 * gpa with -EEXIST; the right semantic is to reuse the slot
	 * id so KVM_SET_USER_MEMORY_REGION becomes an update. Look
	 * up the gpa first; if a slot already exists, reuse it.
	 */
	existing = kvm_v2_memslot_lookup(vm, region->va);
	if (existing) {
		slot_id = (int)existing->slot_id;
		existing->size = region->len;
		existing->flags = flags;
	} else {
		/*
		 * Reserve list entry + slot id atomically
		 * (kvm_v2_memslot_add does both under vm->lock). On
		 * ioctl failure we unwind via kvm_v2_memslot_del so
		 * neither the bitmap nor the list leak.
		 */
		slot_id = kvm_v2_memslot_add(vm, region->va, region->va,
					     region->len, flags);
		if (slot_id < 0) {
			pr_warn_ratelimited("um: kvm-v2 region_added: memslot_add failed (%d) va=%#lx len=%#lx\n",
					    slot_id, region->va, region->len);
			return slot_id;
		}
	}

	kr = (struct kvm_userspace_memory_region){
		.slot		 = (u32)slot_id,
		.flags		 = flags,
		.guest_phys_addr = region->va,
		.memory_size	 = region->len,
		.userspace_addr	 = region->va,
	};

	rc = os_ioctl_generic(vm->vm_fd, KVM_SET_USER_MEMORY_REGION,
			      (unsigned long)&kr);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm-v2 region_added: KVM_SET_USER_MEMORY_REGION(slot=%d va=%#lx len=%#lx flags=%#x) failed (%d)\n",
				    slot_id, region->va, region->len, flags,
				    rc);
		if (!existing)
			kvm_v2_memslot_del(vm, (u32)slot_id);
		return rc;
	}

	return 0;
}

int kvm_v2_mm_region_removed(struct mm_struct *mm,
			     const struct um_memory_region *region)
{
	struct kvm_v2_vm *vm = kvm_v2_vm_get();
	struct kvm_v2_memslot *slot;
	struct kvm_userspace_memory_region kr;
	u32 slot_id;
	int rc;

	if (!vm) {
		pr_warn_ratelimited("um: kvm-v2 region_removed: VM not initialised\n");
		return -ENODEV;
	}
	if (!region || !region->len)
		return -EINVAL;

	/*
	 * Look up our matching memslot by gpa (B.1's helper). On
	 * miss the add either failed mid-way or never ran; either
	 * way we still need to drop the seccomp side, but skip the
	 * KVM-side teardown.
	 */
	slot = kvm_v2_memslot_lookup(vm, region->va);
	if (slot) {
		slot_id = slot->slot_id;

		kr = (struct kvm_userspace_memory_region){
			.slot		 = slot_id,
			.flags		 = 0,
			.guest_phys_addr = region->va,
			.memory_size	 = 0,	/* KVM's delete syntax */
			.userspace_addr	 = region->va,
		};

		rc = os_ioctl_generic(vm->vm_fd, KVM_SET_USER_MEMORY_REGION,
				      (unsigned long)&kr);
		if (rc < 0)
			pr_warn_ratelimited("um: kvm-v2 region_removed: KVM_SET_USER_MEMORY_REGION(slot=%u memory_size=0) failed (%d)\n",
					    slot_id, rc);

		kvm_v2_memslot_del(vm, slot_id);
	}

	return seccomp_mm_region_removed(mm, region);
}
