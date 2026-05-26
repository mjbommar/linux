// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — mm ops.
 *
 * Workstream D-03b: mm_attach / mm_detach — per-UML-mm
 * bookkeeping hooks that refcount the single shared KVM VM
 * created in kvm_init() per decisions-log D57.
 * Workstream D-03d: mm_map / mm_unmap — host-side mmap / munmap
 * against UML's own process VA per the 03b-memslot-policy.md
 * Policy A decision (the KVM memslot registered in kvm_init()
 * already covers the whole UML address space, so these ops do
 * not touch KVM ioctls on the hot path).
 *
 * None of the four ops issue KVM ioctls: memslot state is
 * static after init, guest-vs-host isolation is handled by
 * CR3 switching (D-04).
 */
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <os.h>
#include <mem.h>		/* uml_physmem */
#include <as-layout.h>		/* physmem_size */
#include <skas/mm_id.h>
#include <asm/backend.h>

#include "kvm_backend.h"

int kvm_mm_attach(struct mm_id *id)
{
	struct kvm_um *ctx = kvm_backend_ctx();

	if (ctx->vm_fd < 0) {
		pr_err("um: kvm mm_attach before init — vm_fd unset\n");
		return -EIO;
	}

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	/*
	 * #275: allocate the per-mm shadow PGD here. Each UML mm
	 * gets its own shadow tree; cross-mm switches just point
	 * the vCPU's CR3 at a different tree, no clear/refill
	 * dance needed.
	 */
	if (!id->kvm_shadow) {
		struct kvm_shadow_mm *shadow = kvm_shadow_mm_alloc();

		if (!shadow) {
			pr_err("um: kvm mm_attach: shadow alloc failed\n");
			return -ENOMEM;
		}
		id->kvm_shadow = shadow;
	}
#endif

	return 0;
}

void kvm_mm_detach(struct mm_id *id)
{
	struct kvm_um *ctx = kvm_backend_ctx();

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	if (id->kvm_shadow) {
		kvm_shadow_mm_free(id->kvm_shadow);
		id->kvm_shadow = NULL;
	}
#endif

	(void)ctx;
}

/*
 * Policy A (03b-memslot-policy.md): the KVM memslot registered in
 * kvm_init() covers the UML PHYSICAL backing memory (the physmem
 * region [uml_physmem, uml_physmem + physmem_size)), NOT the whole
 * UML address space. User VAs (e.g. 0x40xxxxxx) live OUTSIDE the
 * memslot — KVM never sees them directly; the guest reaches user
 * pages via the shadow PT walking GPAs that DO lie inside the
 * memslot.
 *
 * Consequence for mm_map / mm_unmap: they need no KVM ioctls for
 * the user-VA range itself (the memslot doesn't cover it). What
 * they DO need is to update the shadow PT (kvm_shadow_invalidate_
 * va_range) so the guest hardware-MMU walk sees the change, AND
 * to update the host VA mapping (os_map_memory / os_unmap_memory)
 * so UML kernel-side accesses (copy_*_user, sigframe setup) reach
 * the correct backing pages. This is exactly what `kern_map` in
 * arch/um/kernel/tlb.c does for init_mm — the user mms reuse the
 * same os_map_memory() / os_unmap_memory() helpers.
 *
 * The "memslot covers whole address space" wording predated the
 * memo 19 audit; it caused at least one bad debugging path that
 * assumed mmu_notifier on user-VA mprotects fired through the
 * memslot. (It does not — user VAs are outside the memslot range.)
 *
 * Note on phys_fd: ptrace / seccomp translate phys_fd into the
 * stub child's fd namespace via get_stub_fd() before queueing
 * the mmap. For the KVM backend there is no stub child — phys_fd
 * is already an fd in UML's own process table, so no translation
 * is needed.
 */
/*
 * BUG.1 (2026-04-27 audit): kvm_mm_map writes to a SHARED host VA
 * via os_map_memory((void *)virt, ...). Two UML processes mapping
 * the same user VA in their own mms collide — last-writer-wins on
 * host VA. Per-mm shadow CR3 (Stage A) gives correct GUEST execution,
 * but kernel-side uaccess (raw_copy_*_user via memcpy on host VA),
 * record/replay payload restore, and signal-frame setup still hit
 * the shared host mapping.
 *
 * Interim observability: track which mm last mapped each (virt, len)
 * key. On cross-mm collision, log a one-shot warning so production
 * use surfaces the bug. The structural fix is Stage B's per-mm host
 * worker / per-mm memslot model — not implementable as a small
 * patch because UML's uaccess (raw_copy_to_user / raw_copy_from_user
 * in arch/um/kernel/skas/uaccess.c) does direct memcpy on the host
 * VA. Either every uaccess walks the shadow PT to find the right
 * host PFN (expensive, per-uaccess), or each mm gets a separate
 * host VA space (per-mm host worker pthread or stub-child process,
 * matching seccomp's model). The latter is the synthesis's Stage B
 * recommendation.
 */
static DEFINE_SPINLOCK(kvm_mm_collision_lock);
static struct {
	unsigned long virt;
	struct mm_id *id;
} kvm_mm_collision_last;	/* last writer per VA — sample, not exhaustive */
static bool kvm_mm_collision_warned;

static void kvm_mm_collision_check(struct mm_id *id, unsigned long virt)
{
	bool is_collision = false;
	unsigned long flags;

	if (READ_ONCE(kvm_mm_collision_warned))
		return;

	spin_lock_irqsave(&kvm_mm_collision_lock, flags);
	if (kvm_mm_collision_last.virt == virt &&
	    kvm_mm_collision_last.id != NULL &&
	    kvm_mm_collision_last.id != id)
		is_collision = true;
	kvm_mm_collision_last.virt = virt;
	kvm_mm_collision_last.id   = id;
	spin_unlock_irqrestore(&kvm_mm_collision_lock, flags);

	if (is_collision && !xchg(&kvm_mm_collision_warned, true)) {
		pr_warn("um: kvm BUG.1: cross-mm host-VA collision at virt=0x%lx (mm_id changed across mappings); per-mm shadow CR3 keeps GUEST execution correct but kernel-side uaccess still uses shared host VA. Stage B (per-mm host worker / per-mm memslot) is the structural fix. This warning fires once.\n",
			virt);
	}
}

/*
 * BUG.1 / shadow_stale_repro root cause (2026-04-27): user mmap at
 * a host VA that overlaps UML's kernel-direct-map range
 * (uml_physmem) clobbers UML kernel memory — including the KVM
 * bootstrap pages (IDT/GDT/LSTAR/IST) at kvm_bootstrap_va. The
 * guest CPU running with that corrupted IDT jumps to garbage
 * (RIP=0x11111111 if user filled the file with 0x11). 100%
 * deterministic via /tmp/shadow_stale_repro.c which mmaps 16 MiB
 * of pattern-filled file at MAP_FIXED virt=0x60000000.
 *
 * Defensive fix: refuse os_map_memory in the kvm_bootstrap_va range
 * (4 pages: bootstrap_page, gadget_state, gadget_vvar, IST stack)
 * AND in any uml_physmem range that overlaps. Returns -EFAULT so
 * UML's mmap arbiter sees a clean failure and propagates -EFAULT
 * to the user task. The user's program gets MAP_FAILED instead of
 * crashing the host UML kernel.
 *
 * Note: this is a defense-in-depth shield. The proper structural
 * fix is to either move uml_physmem out of user-VA reach
 * (TASK_SIZE shrink + uml_physmem at PML4[256+]) or use a per-mm
 * host worker process. Both are larger UML-core changes.
 */
extern u64 kvm_bootstrap_va;
extern u64 kvm_bootstrap_page;	/* Forward — actually a void *, used here for non-zero check */

static bool kvm_mm_map_collides_kernel(unsigned long virt, unsigned long len)
{
	unsigned long uml_lo = (unsigned long)uml_physmem;
	unsigned long uml_hi = uml_lo + (unsigned long)physmem_size;

	if (!uml_lo || !physmem_size)
		return false;
	/* Refuse any mapping that overlaps the kernel-direct-map range. */
	if (virt + len > uml_lo && virt < uml_hi) {
		pr_warn_ratelimited("um: kvm mm_map: REFUSING user mmap at virt=0x%lx len=0x%lx — overlaps UML kernel direct-map range [0x%lx, 0x%lx). User code cannot map kernel memory.\n",
				    virt, len, uml_lo, uml_hi);
		return true;
	}
	return false;
}

int kvm_mm_map(struct mm_id *id, unsigned long virt, unsigned long len,
	       int prot, int phys_fd, u64 offset)
{
	int rc;

	if (kvm_mm_map_collides_kernel(virt, len)) {
		/*
		 * Returning -EFAULT crashes um_tlb_sync. Return 0 (success
		 * from the mm-arbiter's PoV) and skip os_map_memory + the
		 * shadow invalidate. The user's mapping exists in mm->pgd
		 * but not in shadow PT or host VA, so guest reads will #PF
		 * — far better than corrupting kernel memory.
		 *
		 * Proper fix is at UML's mm-arbiter level (refuse the
		 * mmap syscall), but that's a UML-core change.
		 */
		return 0;
	}

	kvm_mm_collision_check(id, virt);

	/*
	 * #276 reverted: hypothesis was that os_map_memory was
	 * not load-bearing under integrated KVM. WRONG —
	 * cumulative-imports under kvm crashes inside libc when
	 * os_map_memory is skipped, even though guest CPU goes
	 * through shadow PT for user VAs. Some path (probably
	 * copy_to_user fallback or io_uring fixed-buffer setup)
	 * relies on the host VA mapping being live. Restore
	 * os_map_memory; the cross-mm collision case is detected
	 * by kvm_mm_collision_check above (BUG.1 / Stage B).
	 */
	rc = os_map_memory((void *)virt, phys_fd, offset, len,
			   prot & UM_PROT_READ, prot & UM_PROT_WRITE,
			   prot & UM_PROT_EXEC);
	if (rc)
		return rc;

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	/*
	 * #274 keystone fix: invalidate the SHADOW belonging to the
	 * mm_id we were called for — NOT current's shadow. The earlier
	 * implementation discarded `id` and used kvm_shadow_mm_current()
	 * which silently routed every invalidate to the active task's
	 * shadow, even when um_tlb_sync was draining a different mm's
	 * pending PTE updates (e.g. the kvm_context_switch pre-switch
	 * sync of prev->active_mm runs AFTER set_current(to) in
	 * arch/um/kernel/process.c — so current is already next, but the
	 * sync target is prev, and prev's pgd writes were being dropped
	 * into next's shadow tree).
	 *
	 * id->kvm_shadow is set by kvm_mm_attach (mm.c:46). If it's NULL
	 * we treat that as the "shadow not attached yet" early-init case
	 * and fall back to current — for genuinely-pre-attach calls this
	 * happens at init when there's no shadow corruption to worry
	 * about anyway.
	 */
	{
		struct kvm_shadow_mm *shadow = id->kvm_shadow;

		if (!shadow)
			shadow = kvm_shadow_mm_current();
		rc = kvm_shadow_invalidate_va_range(shadow,
						    (u64)virt, (u64)len);
		/*
		 * #274 / #13 audit: -ENODEV after mm_attach is a real
		 * bug, not "still initialising". After id->kvm_shadow
		 * is non-NULL the only way invalidate returns -ENODEV
		 * is if shadow->pgd was freed underneath us. Surface
		 * it; for the legitimate pre-attach early-init case the
		 * id->kvm_shadow == NULL fallback above already gave us
		 * current's shadow which is non-NULL during normal boot.
		 */
		if (rc < 0)
			return rc;
	}
#endif
	return 0;
}

int kvm_mm_unmap(struct mm_id *id, unsigned long virt, unsigned long len)
{
	int rc;

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	/*
	 * #274 / T13: short-circuit the init_new_context "clear
	 * STUB area" call. arch/um/kernel/skas/mmu.c:86 calls
	 *   um_backend_dispatch(mm_unmap, new_id, 0, STUB_START);
	 * for every freshly-attached mm. STUB_START is host_task_size
	 * - STUB_SIZE — typically ~128 TB on x86_64.
	 *
	 * Under seccomp this clears the new stub-child's address
	 * space below the stub. Under integrated KVM there is no
	 * stub-child; the call would (a) munmap a 128 TB chunk of
	 * the UML host process — silently truncated to int, so
	 * usually -EINVAL but technically destructive — and
	 * (b) walk kvm_shadow_invalidate_va_range over 32 billion
	 * pages of the WRONG shadow (kvm_shadow_mm_current returns
	 * the parent's shadow, not new_id's).
	 *
	 * Heuristic: virt==0 AND len reaches into the kernel-half VA
	 * region (>= TASK_SIZE/2 == 64 TB) is the init_new_context
	 * signature; legitimate user munmap calls never span that
	 * far. Skip the entire op for the new mm — its shadow tree
	 * was just allocated by mm_attach and has no leaves to
	 * clear.
	 */
	if (virt == 0 && len >= (1UL << 46)) {
		pr_info_once("um: kvm mm_unmap: skipping init_new_context bulk-clear (virt=0, len=0x%lx)\n",
			     len);
		return 0;
	}
#endif

	rc = os_unmap_memory((void *)virt, len);
	if (rc)
		return rc;

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	/* #274 keystone fix: see kvm_mm_map's matching block. */
	{
		struct kvm_shadow_mm *shadow = id->kvm_shadow;

		if (!shadow)
			shadow = kvm_shadow_mm_current();
		rc = kvm_shadow_invalidate_va_range(shadow,
						    (u64)virt, (u64)len);
		if (rc < 0)
			return rc;
	}
#endif
	return 0;
}
