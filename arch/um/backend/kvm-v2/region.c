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
#include <skas/mm_id.h>		/* enter_turnstile / exit_turnstile */

#include "kvm_v2_backend.h"

int kvm_v2_mm_region_added(struct mm_struct *mm,
			   const struct um_memory_region *region)
{
	struct kvm_v2_vm *vm = kvm_v2_vm_get();
	struct mm_id *mm_id;
	int rc;

	if (!vm) {
		pr_warn_ratelimited("um: kvm-v2 region_added: VM not initialised\n");
		return -ENODEV;
	}
	if (!region || !region->len)
		return -EINVAL;

	/*
	 * H.1b residual fix (2026-04-30): serialize per-mm to prevent
	 * concurrent threads of the same mm racing on mm_id->syscall_data.
	 *
	 * seccomp_mm_region_added → um_stub_mm_map appends to
	 * mm_id->syscall_data[] without internal locking — it relies on
	 * the caller to ensure single-threaded access. Under v2, two
	 * pthreads sharing one mm can both be in handle_syscall(mmap)
	 * on different host CPUs simultaneously (preempt_disable in
	 * vcpu_run only prevents per-CPU migration, not cross-CPU
	 * concurrency on different UML kernel kthreads). Without this
	 * lock, two threads' append+update operations interleave and
	 * corrupt the queue, leading to mappings landing on wrong VAs
	 * or shared physical backing.
	 *
	 * Caught by tools/testing/selftests/um/mt-mmap-stress (3
	 * pthreads × 100 iters of mmap+memset+munmap) which detects
	 * "memset corruption" — different threads' private pages
	 * showing identical 16-byte sequences from another thread's
	 * data. 100% PASS under seccomp, 100% FAIL under v2 pre-fix.
	 *
	 * seccomp serializes per-mm via the turnstile mutex acquired
	 * around its entire vcpu_run cycle. v2 can't hold the
	 * turnstile across vcpu_run (it includes handle_syscall which
	 * may exec() and free the mm). Instead serialize at the
	 * narrowest scope where the race exists: mm_region_added /
	 * mm_region_removed.
	 */
	mm_id = &mm->context.id;
	enter_turnstile(mm_id);

	/*
	 * H.1b residual A/B (2026-04-30 cont): also drop the
	 * seccomp_mm_region_added delegation. Under v2, the stub
	 * child does NOT drive vcpu_run (kvm_v2_vcpu_run uses KVM_RUN
	 * directly, never calls worker_drive_vcpu_run). The stub's
	 * mm_id->syscall_data queue accumulates per-mmap entries but
	 * is never executed against the guest's address space —
	 * because v2's address space IS the spawner mm, not the
	 * stub child's mm.
	 *
	 * The queue accumulation is dead state under v2 BUT was a
	 * source of inter-thread state sharing: two pthreads sharing
	 * one mm both append to the same syscall_data array. With
	 * the turnstile around enter/exit, the appends are
	 * serialized, but accumulating dead state still has a
	 * footprint (eventually triggers do_syscall_stub flush when
	 * the queue fills up — which under v2 is wasted work).
	 *
	 * Dropping the seccomp delegation entirely:
	 * - Eliminates the dead-state accumulation
	 * - Removes the last shared mm_id state in v2's hot path
	 * - Keeps the turnstile for future v2-specific serialization
	 *
	 * Original code (commented out for A/B):
	 *   rc = seccomp_mm_region_added(mm, region);
	 *   if (rc < 0) { exit_turnstile(mm_id); return rc; }
	 */
	rc = 0;

	/*
	 * H.1b residual A/B (2026-04-30): try DROPPING the os_map_memory
	 * call. With slot 0 (gpa=0..physmem_size, hva=uml_physmem) KVM's
	 * TDP walk resolves guest VA → guest PT (in physmem) → guest GPA
	 * → user_addr=uml_physmem+GPA (slot 0) → spawner mm walk for
	 * uml_physmem range. Per-region os_map_memory at user-half VAs
	 * is needed only for "in-place update" compatibility with v1
	 * (see comment block) but slot 0 alone should suffice for v2.
	 *
	 * If this fixes the multi-thread mt-mmap-stress race, the bug
	 * was in the spawner-mm parallel mapping path racing somehow
	 * with the per-mm slot-0 EPT walk.
	 *
	 * Original code (commented out for A/B):
	 *   rc = os_map_memory(...region->va...phys_fd...offset...);
	 *   if (rc < 0) { ...; return rc; }
	 */

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

	/*
	 * E.5 (Codex CLAIM C pull-forward, was deferred to Phase H):
	 * skip per-region KVM_SET_USER_MEMORY_REGION. Slot 0 (gpa=0..
	 * physmem_size, hva=uml_physmem) covers every UML physical page
	 * — every UML PTE's PFN field is an offset into physmem_fd
	 * (per phys_mapping at arch/um/kernel/physmem.c:146-156, fd =
	 * physmem_fd, offset = phys for any phys < physmem_size, and
	 * UML's buddy allocator hands out kvas inside the physmem
	 * mmap so __pa(any kpage) < physmem_size by construction).
	 * KVM TDP walks UML's pgd, gets the leaf's GPA from the PFN
	 * field, resolves via slot 0 to HVA = uml_physmem + GPA = the
	 * actual page. Per-region memslots at gpa=region->va add no
	 * GPA→HVA coverage that slot 0 doesn't already provide.
	 *
	 * Empirical motivation: the per-region DELETE+CREATE churn
	 * (every user PTE change → tlb.c drain → here →
	 * KVM_SET_USER_MEMORY_REGION) triggers KVM internal MMU
	 * invalidate_zap that, while scoped per-slot in the TDP MMU
	 * fast path, still racing-cohorts with the IDT-vectoring walk
	 * leaves slot 0's PML4-page EPT entries un-refilled
	 * (pf_taken=1, pf_fixed=0 in the loop). Dropping per-region
	 * eliminates that source of EPT zap churn.
	 *
	 * The seccomp_mm_region_added call above stays (stub child may
	 * still be used for spawning under R4); the os_map_memory
	 * above stays (UML kernel still needs region->va mapped in its
	 * own address space for copy_to_user / copy_from_user). Only
	 * the KVM memslot side is dropped.
	 *
	 * READONLY: KVM_MEM_READONLY-via-per-region-slot is also
	 * dropped, but UML's user PTE encodes R/W in its own bits
	 * (post-912587c605d8 bit alignment with x86 hardware paging).
	 * KVM's TDP walk reads those bits during the GVA→GPA
	 * dimension and combines with slot 0's permissions; slot 0 is
	 * RW (flags=0 at context.c:138), so the effective RW is
	 * determined by UML's PTE bits — same protection as the
	 * per-region path.
	 */
	(void)vm;
	exit_turnstile(mm_id);
	return 0;
}

int kvm_v2_mm_region_removed(struct mm_struct *mm,
			     const struct um_memory_region *region)
{
	struct kvm_v2_vm *vm = kvm_v2_vm_get();
	struct mm_id *mm_id;
	int rc;

	if (!vm) {
		pr_warn_ratelimited("um: kvm-v2 region_removed: VM not initialised\n");
		return -ENODEV;
	}
	if (!region || !region->len)
		return -EINVAL;

	/*
	 * H.1b residual fix: same per-mm serialization as
	 * mm_region_added — un-mapping path also touches mm_id->
	 * syscall_data via seccomp_mm_region_removed → um_stub_mm_unmap.
	 */
	mm_id = &mm->context.id;
	enter_turnstile(mm_id);

	/*
	 * E.5 (Codex CLAIM C pull-forward): per-region memslot
	 * teardown removed in lockstep with mm_region_added's drop.
	 * Slot 0 (gpa=0..physmem_size) covers all UML physmem; no
	 * per-region KVM memslot exists to remove. The os_unmap_memory
	 * + seccomp_mm_region_removed steps below are still required:
	 * UML kernel needs region->va dropped from its own address
	 * space (no leak under map/unmap cycling), and the stub child
	 * (still alive under R4 for spawning) needs to drop its copy
	 * symmetrically.
	 */

	(void)vm;

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
	/*
	 * H.1b residual A/B: drop the os_unmap_memory call too. If
	 * mm_region_added no longer maps user-half VAs in the spawner
	 * mm (slot 0 covers physmem), there's nothing to unmap on
	 * region removal. The init_new_context bulk-clear sentinel
	 * (va==0 && len >= 1<<46) is a no-op pair-wise.
	 *
	 * Also drop the seccomp_mm_region_removed delegation —
	 * symmetrical with mm_region_added's drop above. The stub
	 * child's mm tracking is dead state under v2.
	 */
	rc = 0;
	exit_turnstile(mm_id);
	return rc;
}
