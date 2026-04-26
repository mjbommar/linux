// SPDX-License-Identifier: GPL-2.0
/*
 * Direct Shadow Synchronization (memo 15) — KVM backend.
 *
 * Replaces the deferred chain
 *   set_pte_at -> _PAGE_NEEDSYNC -> um_tlb_sync ->
 *   ops.mm_map/unmap -> kvm_shadow_invalidate_va_range ->
 *   kvm_enter_guest full fill
 * with a synchronous shadow update at the source of truth (the
 * PTE mutation). The deferred chain still runs because non-KVM
 * backends use it for host VA mapping; this just adds the
 * shadow update at the point of mutation, so the shadow is
 * never lagging the UML pgd from the moment set_ptes returns.
 */

#include <linux/cleanup.h>		/* guard(mutex) */
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/sched.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/kvm_mmu_sync.h>

#include "kvm_backend.h"

/* UML PTE bit definitions (mirroring lifecycle.c). */
#define UM_PTE_PRESENT	0x001

int kvm_shadow_sync_pte(struct mm_struct *mm, unsigned long addr, pte_t pte)
{
	struct kvm_shadow_mm *shadow;
	u64 ume = pte_val(pte);
	u64 x86e;
	int rc;

	/*
	 * Resolve the target shadow. mm can be NULL during very
	 * early init / kernel-thread paths; treat that as "no
	 * shadow to sync" rather than an error.
	 */
	if (!mm)
		return 0;
	shadow = mm->context.id.kvm_shadow;
	if (!shadow || !shadow->pgd)
		return 0;

	/*
	 * Hold fill_lock for the ENTIRE clear+install transaction
	 * so concurrent invalidate / fill / sync calls on the same
	 * shadow can't interleave between our clear and our
	 * install. The existing kvm_shadow_invalidate_va_range and
	 * kvm_shadow_map_page take the lock individually, so we
	 * use raw walkers here and inline the work under one
	 * lock acquisition.
	 */
	guard(mutex)(&shadow->fill_lock);

	{
		u64 *pgd = shadow->pgd;
		unsigned int pgd_i = (addr >> 39) & 0x1ff;
		unsigned int pud_i = (addr >> 30) & 0x1ff;
		unsigned int pmd_i = (addr >> 21) & 0x1ff;
		unsigned int pte_i = (addr >> 12) & 0x1ff;
		u64 *pud, *pmd, *spte;

		/* Clear existing shadow leaf, if any. */
		if (pgd[pgd_i] & 1ULL) {
			pud = (u64 *)__va(pgd[pgd_i] &
					  0x000ffffffffff000ULL);
			if (pud[pud_i] & 1ULL) {
				pmd = (u64 *)__va(pud[pud_i] &
						  0x000ffffffffff000ULL);
				if (pmd[pmd_i] & 1ULL) {
					spte = (u64 *)__va(pmd[pmd_i] &
							   0x000ffffffffff000ULL);
					if (spte[pte_i] & 1ULL) {
						spte[pte_i] = 0;
						shadow->dirty = true;
					}
				}
			}
		}
	}

	/*
	 * If the new PTE isn't present in UML's pgd at all, the
	 * shadow leaf stays absent. Done.
	 */
	if (!(ume & UM_PTE_PRESENT)) {
		shadow->synced = false;
		return 0;
	}

	/*
	 * Translate. kvm_um_pte_to_x86 returns 0 for PTEs that
	 * shouldn't be installed in shadow:
	 *   - !PRESENT (handled above)
	 *   - !ACCESSED (UML emulates A in software; we want a
	 *     #PF on first access so UML marks it young)
	 *   - PROT_NONE
	 */
	x86e = kvm_um_pte_to_x86(ume);
	if (!x86e) {
		shadow->synced = false;
		return 0;
	}

	/*
	 * Install. kvm_shadow_map_page does NOT take fill_lock
	 * itself — only fill and invalidate do — so calling it
	 * from inside our locked region is safe and keeps the
	 * clear+install transaction atomic w.r.t. other
	 * shadow-mutating callers on this mm.
	 */
	rc = kvm_shadow_map_page(shadow, addr,
				 x86e & 0x000ffffffffff000ULL,
				 x86e & ~0x000ffffffffff000ULL);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm shadow_sync_pte: map_page(va=0x%lx) failed (%d) — marking needs_full_resync\n",
				    addr, rc);
		WRITE_ONCE(shadow->needs_full_resync, true);
		shadow->synced = false;
		return rc;
	}
	shadow->synced = false;
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_shadow_sync_pte);
