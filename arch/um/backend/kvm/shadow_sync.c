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
 * shadow update at the point of mutation.
 *
 * IMPORTANT — atomic-context contract:
 *
 *   set_ptes(), pte_clear(), pmd_clear(), pud_clear(), p4d_clear()
 *   are all called by generic mm code while holding page-table
 *   spinlocks (pte_lockptr / pmd_lockptr). That means we CANNOT
 *   sleep here — no mutex_lock(), no GFP_KERNEL allocation.
 *
 *   Strategy: do single-u64 atomic writes to existing shadow leaf
 *   slots without taking the per-shadow_mm fill_lock. Reads of
 *   intermediate PUD/PMD/PT pages are safe without locking because
 *   those pages are only freed at mm teardown (after all PTE
 *   mutators are gone). If installation requires allocating a
 *   new intermediate page, we set shadow->needs_full_resync and
 *   defer to kvm_enter_guest's repair path which DOES run in
 *   sleepable context and CAN allocate.
 *
 *   The single-u64 PTE writes are safe without the fill_lock
 *   because (a) UML normally runs in a cooperative single-host-
 *   thread model and (b) the operations are read-modify-write of
 *   an aligned u64, atomic on x86_64. Concurrent fill is the only
 *   real race; fill takes fill_lock so it sees a consistent state
 *   between its iterations.
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/sched.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/kvm_mmu_sync.h>

#include "kvm_backend.h"

/* UML PTE bit definitions (mirroring lifecycle.c). */
#define UM_PTE_PRESENT	0x001

/*
 * Walk the shadow tree to the leaf-PTE slot for `addr`. Returns
 * the slot pointer if all intermediate tables exist, or NULL if
 * any level is missing (which means there is no current shadow
 * leaf for this VA — for clear it's already absent; for install
 * the caller must defer to repair).
 *
 * Read-only walk; no allocation, no lock. Safe in atomic context.
 */
static u64 *kvm_shadow_walk_leaf(struct kvm_shadow_mm *shadow, u64 addr)
{
	u64 *pgd = shadow->pgd;
	unsigned int pgd_i = (addr >> 39) & 0x1ff;
	unsigned int pud_i = (addr >> 30) & 0x1ff;
	unsigned int pmd_i = (addr >> 21) & 0x1ff;
	unsigned int pte_i = (addr >> 12) & 0x1ff;
	u64 *pud, *pmd, *pte;

	if (!(pgd[pgd_i] & 1ULL))
		return NULL;
	pud = (u64 *)__va(pgd[pgd_i] & 0x000ffffffffff000ULL);
	if (!(pud[pud_i] & 1ULL))
		return NULL;
	pmd = (u64 *)__va(pud[pud_i] & 0x000ffffffffff000ULL);
	if (!(pmd[pmd_i] & 1ULL))
		return NULL;
	pte = (u64 *)__va(pmd[pmd_i] & 0x000ffffffffff000ULL);
	return &pte[pte_i];
}

int kvm_shadow_sync_pte(struct mm_struct *mm, unsigned long addr, pte_t pte)
{
	struct kvm_shadow_mm *shadow;
	u64 ume = pte_val(pte);
	u64 x86e;
	u64 *spte;
	bool was_present;

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
	 * Walk to the leaf slot. If any intermediate level is
	 * absent the leaf is by definition absent — no clear
	 * needed. For install we'd need allocation; defer to
	 * repair.
	 */
	spte = kvm_shadow_walk_leaf(shadow, addr);

	if (!(ume & UM_PTE_PRESENT)) {
		/*
		 * Clearing the leaf. If shadow already absent
		 * (no path), nothing to do. Otherwise atomic-
		 * write 0 — single u64 write on aligned address.
		 */
		if (!spte)
			return 0;
		if (READ_ONCE(*spte) & 1ULL) {
			WRITE_ONCE(*spte, 0);
			WRITE_ONCE(shadow->dirty, true);
		}
		return 0;
	}

	/*
	 * Installing. Translate first.
	 */
	x86e = kvm_um_pte_to_x86(ume);
	if (!x86e) {
		/*
		 * Translator says absent (PROT_NONE / !ACCESSED).
		 * Treat like clear: drop existing leaf if any.
		 */
		if (!spte)
			return 0;
		if (READ_ONCE(*spte) & 1ULL) {
			WRITE_ONCE(*spte, 0);
			WRITE_ONCE(shadow->dirty, true);
		}
		return 0;
	}

	/*
	 * Need to install. If the path doesn't exist (intermediate
	 * tables missing), we can't allocate from atomic context.
	 * Set needs_full_resync; kvm_enter_guest's repair will
	 * allocate + fill before KVM_RUN.
	 */
	if (!spte) {
		WRITE_ONCE(shadow->needs_full_resync, true);
		WRITE_ONCE(shadow->dirty, true);
		return 0;
	}

	/*
	 * Path exists — install the leaf atomically.
	 */
	was_present = (READ_ONCE(*spte) & 1ULL) != 0;
	WRITE_ONCE(*spte,
		   (x86e & 0x000ffffffffff000ULL) |
		   (x86e & ~0x000ffffffffff000ULL));
	WRITE_ONCE(shadow->dirty, true);

	/*
	 * Do NOT set shadow->synced = false on success. Per memo 15
	 * #2: a successful direct sync MAINTAINS the synced state —
	 * the shadow now exactly mirrors the new PTE in pgd. Forcing
	 * synced=false would trigger a redundant full fill on the
	 * next entry. Only the failure paths above (deferred via
	 * needs_full_resync) need the next entry to repair.
	 */
	(void)was_present;
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_shadow_sync_pte);

/*
 * Range clear for parent-level clears (pmd_clear, pud_clear,
 * p4d_clear). Walks the shadow user-half over [start, end) and
 * clears any present leaf. Atomic: single-u64 writes only,
 * no allocation.
 */
void kvm_shadow_clear_range_atomic(struct mm_struct *mm,
				   unsigned long start, unsigned long end)
{
	struct kvm_shadow_mm *shadow;
	unsigned long addr;
	bool any_cleared = false;

	if (!mm)
		return;
	shadow = mm->context.id.kvm_shadow;
	if (!shadow || !shadow->pgd)
		return;

	for (addr = start & ~0xfffUL;
	     addr < ((end + 0xfffUL) & ~0xfffUL);
	     addr += PAGE_SIZE) {
		u64 *spte = kvm_shadow_walk_leaf(shadow, addr);

		if (!spte)
			continue;
		if (READ_ONCE(*spte) & 1ULL) {
			WRITE_ONCE(*spte, 0);
			any_cleared = true;
		}
	}
	if (any_cleared)
		WRITE_ONCE(shadow->dirty, true);
}
EXPORT_SYMBOL_GPL(kvm_shadow_clear_range_atomic);
