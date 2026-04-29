// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase B.2/B.3 + D.0b: mm_region_added /
 * mm_region_removed wiring onto KVM_SET_USER_MEMORY_REGION
 * (add / delete) with dual-mm mapping (stub child + spawner).
 *
 * Per memo 26 §B.2, §B.3, §D.0b. B.1 shipped the slot-id allocator +
 * in-memory memslot list; B.2/B.3 added the seccomp + KVM wiring;
 * D.0b extends mm_region_added to also map region->va in the spawner
 * mm so KVM's fault-in path resolves the userspace_addr against a
 * VMA that actually exists.
 *
 * mm_region_added (B.2 + D.0b):
 *   (a) calls seccomp_mm_region_added so the seccomp stub child
 *       (which still owns guest user-mode execution until Phase D.5
 *       flips .vcpu_run) keeps the mapping in its address space —
 *       without this the guest faults every memory access;
 *   (b) D.0b: calls os_map_memory(region->va, phys_fd, offset, len,
 *       r,w,x) so the SPAWNER mm gets the same identity mapping. KVM
 *       binds the VM to the creator's current->mm at vm_create
 *       (virt/kvm/kvm_main.c:1107-1109) and resolves userspace_addr
 *       against that mm at fault-in (virt/kvm/kvm_main.c:2999-3027).
 *       Without this map, KVM_SET_USER_MEMORY_REGION returns -EINVAL
 *       — the access_ok() check at virt/kvm/kvm_main.c:2014-2025 only
 *       validates that the VA is in user-numeric range, not that a
 *       VMA exists; the failure surfaces at registration time on
 *       some VAs and at fault-in on others. Boot log under v2 pre-
 *       D.0b shows -EINVAL on user VAs (0x550000007000, 0x40265000);
 *       D.0b eliminates them.
 *   (c) D.0b: looks up the slot by gpa. If a slot exists for this
 *       VA, issue an in-band DELETE (memory_size=0) first — the
 *       modify-existing-slot path at virt/kvm/kvm_main.c:2079-2082
 *       rejects with -EINVAL on userspace_addr / npages / READONLY-
 *       toggle changes, all of which fire under normal UML boot
 *       (partial-range replace, prot WRITE↔READ). After the delete
 *       the slot id is reusable for a clean CREATE.
 *   (d) reserves a slot id + list entry via kvm_v2_memslot_add (or
 *       reuses the existing slot id post-DELETE);
 *   (e) issues KVM_SET_USER_MEMORY_REGION add against the per-VM
 *       fd, populating the KVM memslot table for the Phase D / TDP
 *       path.
 * On ioctl failure the list entry is unwound for fresh slots; the
 * spawner-side os_map_memory mapping stays in place because the
 * corresponding remove will tear it down via os_unmap_memory.
 *
 * mm_region_removed (B.3 + D.0b):
 *   (a) looks up the slot via kvm_v2_memslot_lookup(gpa);
 *   (b) issues KVM_SET_USER_MEMORY_REGION with memory_size=0
 *       (KVM's "delete this slot" syntax). KVM's mmu_notifier
 *       auto-invalidates EPT;
 *   (c) frees the bitmap bit + list entry via kvm_v2_memslot_del;
 *   (d) D.0b: calls os_unmap_memory(region->va, region->len) to
 *       drop the spawner-side mapping symmetrically with the add
 *       path's os_map_memory;
 *   (e) calls seccomp_mm_region_removed so the stub child unmaps
 *       its own copy.
 * If the lookup fails (ioctl-side add failed earlier; only the
 * seccomp side was populated) we still call the spawner-side
 * unmap + seccomp's remove so both keep moving.
 *
 * Dual-mm mapping invariant (D.0b):
 *   After mm_region_added returns 0, region->va is mapped to
 *   phys_fd:offset in BOTH (i) the seccomp stub child's mm (via
 *   um_stub_mm_map MAP_FIXED|MAP_SHARED) and (ii) the spawner's mm
 *   (via os_map_memory MAP_FIXED|MAP_SHARED). KVM's per-VM mm is the
 *   spawner's mm (set at KVM_CREATE_VM); the spawner-side mapping is
 *   what KVM walks at fault-in. The dual mapping is the cost of
 *   "v2 delegates to seccomp until D.5"; once D.5 lands and the
 *   stub child stops running guest user mode, the seccomp side
 *   drops out and only the spawner side survives.
 *
 * Update-vs-create semantics:
 *   tlb.c surfaces "add" for both new mappings AND in-place updates
 *   (prot toggle, partial-range replace). Both seccomp's
 *   um_stub_mm_map and os_map_memory use MAP_FIXED, so re-mapping
 *   the same VA simply overlays — no -EEXIST handling required.
 *   The KVM side reuses the existing slot id when the gpa matches.
 *
 * Single-VM model in this commit:
 *   memo 26 §B.2's prose talks about "the per-mm worker's bitmap" but
 *   §B.2 itself defers per-mm VM separation to Phase D's worker-
 *   process wiring ("for now use per-VM until the worker model
 *   lands"). A.2 created exactly one struct kvm_v2_vm (file-scope
 *   static in context.c, returned by kvm_v2_vm_get()), so every
 *   mm_region_added call funnels through that one VM. The mm
 *   parameter is intentionally unused — plumbed for forward compat
 *   with Phase D's per-mm VM split.
 *
 * Identity host_va == guest_phys_addr:
 *   memo 26 §B.2 fixes userspace_addr = host_va = region->va and
 *   guest_phys_addr = same value. The giant-slot identity mapping
 *   sidesteps a separate gpa allocator; v2 may relax later but the
 *   B.2 invariant is "GPA == VA".
 *
 * Why backend_data is NOT stashed here:
 *   memo 26 §B.1's note suggested stashing slot_id on
 *   region->backend_data so mm_region_removed could free by id
 *   without a gpa-keyed lookup. The struct um_memory_region is
 *   stack-allocated by arch/um/kernel/tlb.c and does NOT persist
 *   between the add and remove calls — they get distinct stack
 *   structs. B.3 looks up by gpa via kvm_v2_memslot_lookup();
 *   backend_data stays NULL.
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
	 * Seccomp stub child still owns guest execution (Phase D.5
	 * replaces it). Tell it about the region first; if that fails
	 * we propagate without touching KVM state.
	 */
	rc = seccomp_mm_region_added(mm, region);
	if (rc < 0)
		return rc;

	/*
	 * D.0b: map region->va in the SPAWNER mm too. KVM binds the VM
	 * to the creator's current->mm (virt/kvm/kvm_main.c:1107-1109)
	 * and resolves userspace_addr against that mm at fault-in
	 * (virt/kvm/kvm_main.c:2999-3027); the stub child's mm is
	 * irrelevant to KVM's view. os_map_memory uses MAP_FIXED|
	 * MAP_SHARED so overlaying an existing mapping is fine (the
	 * tlb.c "add for in-place update" case relies on this — see
	 * v1 archive's kvm_mm_map at kvm-v1-archive/mm.c:231-235 for
	 * the same MAP_FIXED rationale).
	 */
	rc = os_map_memory((void *)region->va, region->phys_fd, region->offset,
			   region->len,
			   !!(region->prot & UM_PROT_READ),
			   !!(region->prot & UM_PROT_WRITE),
			   !!(region->prot & UM_PROT_EXEC));
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm-v2 region_added: os_map_memory(va=%#lx len=%#lx fd=%d off=%#llx prot=%#x) failed (%d)\n",
				    region->va, region->len, region->phys_fd,
				    region->offset, region->prot, rc);
		return rc;
	}

	/*
	 * D.0b note on the spec's defensive access_ok call: an explicit
	 * UML-side access_ok((void __user *)region->va, region->len)
	 * here would check against UML's TASK_SIZE — which on UML x86_64
	 * is host_top - STUB_SIZE, capped at PTRS_PER_PGD * PGDIR_SIZE.
	 * KVM's own access_ok (virt/kvm/kvm_main.c:2014-2025) runs in
	 * the SPAWNER host context and checks against the HOST's
	 * TASK_SIZE_MAX (which is wider — e.g. ~128 TB on x86_64
	 * 4-level paging). So a UML-side pre-check is more restrictive
	 * than KVM's actual gate and would reject VAs near the
	 * UML-task-size boundary that KVM would otherwise accept (boot
	 * smoke surfaced exactly this on a stub-adjacent VA at
	 * 0x7f7ffffff000). The os_map_memory above is the actual
	 * VMA-presence guarantee KVM needs; an extra UML-side
	 * access_ok would be sugar that introduces false negatives.
	 */

	flags = (region->prot & UM_PROT_WRITE) ? 0 : KVM_MEM_READONLY;

	/*
	 * tlb.c surfaces "add" for both new mappings AND in-place
	 * updates of existing ones (prot toggle, partial-range replace,
	 * size grow / shrink). The seccomp stub-side helper handles
	 * either via MAP_FIXED, but KVM's modify-existing-slot path is
	 * strict: per virt/kvm/kvm_main.c:2079-2082, a same-slot-id
	 * follow-up KVM_SET_USER_MEMORY_REGION rejects with -EINVAL if
	 *   (a) userspace_addr changed,
	 *   (b) npages changed (partial-range replace), or
	 *   (c) the READONLY flag toggled (prot WRITE↔READ).
	 *
	 * Both (b) and (c) fire under normal UML boot — boot smoke logs
	 * showed -EINVAL on update paths from B.3-followup until D.0b.
	 *
	 * Strategy: if a slot already exists for this gpa, treat the
	 * incoming "add" as a delete-then-add. KVM's "memory_size = 0"
	 * is the in-band delete syntax (line 2053-2061); after the
	 * delete the same slot_id is reusable for a fresh CREATE with
	 * the new len + flags. This sidesteps every modify-existing
	 * invariant and matches what seccomp's MAP_FIXED already does
	 * on its side. Symmetric on the spawner mm too: os_map_memory
	 * with MAP_FIXED overlays cleanly.
	 */
	existing = kvm_v2_memslot_lookup(vm, region->va);
	if (existing) {
		struct kvm_userspace_memory_region del = {
			.slot		 = existing->slot_id,
			.guest_phys_addr = region->va,
			.memory_size	 = 0,	/* KVM's delete syntax */
		};

		rc = os_ioctl_generic(vm->vm_fd, KVM_SET_USER_MEMORY_REGION,
				      (unsigned long)&del);
		if (rc < 0)
			pr_warn_ratelimited("um: kvm-v2 region_added: pre-update DELETE(slot=%u va=%#lx) failed (%d)\n",
					    existing->slot_id, region->va, rc);

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
	 * way we still need to drop the spawner-side mapping + the
	 * seccomp side, but skip the KVM-side teardown.
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

	/*
	 * D.0b: drop the spawner-side mapping that mm_region_added
	 * installed via os_map_memory. Symmetric with the add path so
	 * the spawner mm doesn't accumulate stale identity mappings as
	 * UML cycles VAs through map / unmap. os_unmap_memory is
	 * munmap(2); failure here is non-fatal (warn-ratelimited) —
	 * the seccomp side still needs to be told to drop its copy
	 * regardless.
	 *
	 * Skip the init_new_context bulk-clear: arch/um/kernel/skas/
	 * mmu.c:82-89 issues a single "remove [0, STUB_START)" call
	 * per fresh mm to clear the new stub child's address space
	 * below the stub. The seccomp backend handles this via
	 * um_stub_mm_unmap (per-stub-child mm); the spawner mm has
	 * never had a single mapping spanning that whole range —
	 * mm_region_added always sees small per-page ranges — so a
	 * blanket munmap of [0, STUB_START) would (a) overflow
	 * os_unmap_memory's int len arg and silently truncate, and
	 * (b) potentially destroy unrelated host-side mappings if the
	 * truncated len happened to land on something. v1 archive's
	 * kvm_mm_unmap (kvm-v1-archive/mm.c:307-311) used the same
	 * heuristic. virt==0 && len >= 1<<46 (64 TB; far beyond any
	 * legitimate user munmap) is the unambiguous init_new_context
	 * signature.
	 */
	if (region->va == 0 && region->len >= (1UL << 46)) {
		rc = 0;
	} else {
		rc = os_unmap_memory((void *)region->va, (int)region->len);
		if (rc < 0)
			pr_warn_ratelimited("um: kvm-v2 region_removed: os_unmap_memory(va=%#lx len=%#lx) failed (%d)\n",
					    region->va, region->len, rc);
	}

	return seccomp_mm_region_removed(mm, region);
}
