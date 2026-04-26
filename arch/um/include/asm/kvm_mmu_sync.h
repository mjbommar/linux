/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Direct Shadow Synchronization API (memo 15).
 *
 * Hooks PTE mutation to update the per-mm KVM shadow tree
 * synchronously, instead of relying on the deferred
 * NEEDSYNC → um_tlb_sync → ops.mm_map → invalidate chain to
 * eventually catch up. The deferred chain has too many edges
 * where the shadow can lag the UML pgd; direct sync collapses
 * the producer (PTE mutation) and consumer (shadow update)
 * into a single transactional event.
 *
 * Inline no-op stubs when the integrated KVM backend is not
 * compiled in — set_ptes / pte_clear in pgtable.h call these
 * unconditionally so the deferred chain stays the only mechanism
 * on builds without integrated KVM.
 */
#ifndef _ASM_UM_KVM_MMU_SYNC_H
#define _ASM_UM_KVM_MMU_SYNC_H

#include <linux/mm_types.h>

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED

/*
 * Direct shadow update for one PTE in the given mm.
 *
 * Contract (post-5d807d8e atomic-safe rewrite — F6 doc fix):
 *   - ATOMIC. Does NOT take any sleeping lock. Does NOT allocate.
 *     Safe to call from page-table-spinlock context (set_ptes /
 *     pte_clear from generic mm code).
 *   - Walks the shadow tree read-only to find the leaf slot. If
 *     any intermediate level is missing, treats as "no existing
 *     leaf" for clear (no-op) and as "deferred install" for new
 *     installs (sets shadow->needs_full_resync; kvm_enter_guest's
 *     repair path will allocate + fill before KVM_RUN).
 *   - Clears existing leaf via single-u64 WRITE_ONCE.
 *   - Translates the new UML PTE through kvm_um_pte_to_x86() to
 *     decide whether the new shadow leaf should be present
 *     (returns 0 for !PRESENT, !ACCESSED, PROT_NONE — those map
 *     to "absent leaf").
 *   - Installs new leaf via single-u64 WRITE_ONCE if path exists.
 *
 * Returns 0 always (no failure mode that the caller can act on
 * synchronously; alloc-needed deferred via needs_full_resync).
 */
int kvm_shadow_sync_pte(struct mm_struct *mm, unsigned long addr,
			pte_t pte);

/*
 * F1 — sync-on-flush helpers.
 *
 * The flush_tlb_* hooks call these instead of clear-only because
 * generic mm code calls flush after set_pte_at (e.g.
 * ptep_set_access_flags → set_pte_at → flush_tlb_fix_spurious_fault
 * → flush_tlb_page). A blind clear there would erase the leaf
 * direct-sync just installed. Sync-on-flush re-derives the leaf
 * from the current UML PTE — preserving recent installs while
 * picking up any concurrent change.
 *
 * Both atomic, no allocation; alloc-needed deferred via
 * needs_full_resync. F9 threshold: ranges >= 512 pages bail to
 * needs_full_resync to avoid pathological page-by-page work.
 */
void kvm_shadow_sync_va_atomic(struct mm_struct *mm, unsigned long addr);
void kvm_shadow_sync_range_atomic(struct mm_struct *mm,
				  unsigned long start, unsigned long end);

/*
 * Legacy clear-only — kept for callers that explicitly want to
 * drop shadow leaves regardless of pgd state. No in-tree caller
 * post-F1; consider removing.
 */
void kvm_shadow_clear_range_atomic(struct mm_struct *mm,
				   unsigned long start, unsigned long end);

#else /* !CONFIG_UM_BACKEND_KVM_INTEGRATED */

static inline int kvm_shadow_sync_pte(struct mm_struct *mm,
				      unsigned long addr,
				      pte_t pte)
{
	return 0;
}

static inline void kvm_shadow_sync_va_atomic(struct mm_struct *mm,
					     unsigned long addr)
{
}

static inline void kvm_shadow_sync_range_atomic(struct mm_struct *mm,
						unsigned long start,
						unsigned long end)
{
}

static inline void kvm_shadow_clear_range_atomic(struct mm_struct *mm,
						 unsigned long start,
						 unsigned long end)
{
}

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */

#endif /* _ASM_UM_KVM_MMU_SYNC_H */
