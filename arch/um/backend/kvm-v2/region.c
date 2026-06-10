// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend memory-region hooks.
 *
 * KVM v2 uses the giant physmem memslot installed by context.c for the
 * actual TDP coverage. The region add/remove hooks remain synchronization
 * points for per-mm activity, but they do not create per-region KVM
 * memslots. UML PTEs carry physmem-offset PFNs, so KVM walks guest
 * page tables to GPAs inside the giant slot.
 *
 * The hooks serialize per-mm with the turnstile because multiple UML
 * threads sharing an mm can issue mmap/munmap concurrently on different
 * host CPUs. The KVM side is quiet here; slot 0 covers the pages KVM
 * needs.
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
#include <skas/mm_id.h>		/* enter_turnstile / exit_turnstile */

#include "kvm_v2_backend.h"

int kvm_v2_mm_region_added(struct mm_struct *mm,
			   const struct um_memory_region *region)
{
	struct kvm_v2_vm *vm = kvm_v2_vm_get();
	struct mm_id *mm_id;

	if (!vm) {
		pr_warn_ratelimited("um: kvm-v2 region_added: VM not initialised\n");
		return -ENODEV;
	}
	if (!region || !region->len)
		return -EINVAL;

	/* Serialize mmap/munmap updates for threads sharing the same mm. */
	mm_id = &mm->context.id;
	enter_turnstile(mm_id);

	/*
	 * Do not delegate to seccomp's region mapper. KVM v2 runs guest
	 * userspace directly with KVM_RUN and does not drive the seccomp
	 * stub child's syscall queue for these mappings.
	 */

	/*
	 * Skip per-region KVM_SET_USER_MEMORY_REGION. Slot 0 (gpa=0..
	 * physmem_size, hva=uml_physmem) covers every UML physical page
	 * because every UML PTE's PFN field is an offset into physmem_fd.
	 * UML's buddy allocator hands out kvas inside the physmem mmap, so
	 * __pa(any kpage) < physmem_size by construction.
	 * KVM TDP walks UML's pgd, gets the leaf's GPA from the PFN
	 * field, resolves via slot 0 to HVA = uml_physmem + GPA = the
	 * actual page. Per-region memslots at gpa=region->va add no
	 * GPA->HVA coverage that slot 0 doesn't already provide.
	 *
	 * READONLY: KVM_MEM_READONLY-via-per-region-slot is also
	 * dropped, but UML's user PTE encodes R/W in its own bits, aligned
	 * with x86 hardware paging.
	 * KVM's TDP walk reads those bits during the GVA->GPA
	 * dimension and combines with slot 0's permissions. Slot 0 is
	 * writable, so the effective access is determined by UML's PTE bits:
	 * the same protection as the per-region path.
	 */
	exit_turnstile(mm_id);
	return 0;
}

int kvm_v2_mm_region_removed(struct mm_struct *mm,
			     const struct um_memory_region *region)
{
	struct kvm_v2_vm *vm = kvm_v2_vm_get();
	struct mm_id *mm_id;

	if (!vm) {
		pr_warn_ratelimited("um: kvm-v2 region_removed: VM not initialised\n");
		return -ENODEV;
	}
	if (!region || !region->len)
		return -EINVAL;

	/* Same per-mm serialization as mm_region_added. */
	mm_id = &mm->context.id;
	enter_turnstile(mm_id);

	/*
	 * No per-region KVM memslot, spawner-side mapping, or seccomp stub
	 * mapping is created by mm_region_added, so removal is a synchronized
	 * no-op for the KVM backend.
	 */
	exit_turnstile(mm_id);
	return 0;
}
