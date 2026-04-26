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
 * Fail-safe direct shadow update for one PTE in the given mm.
 *
 * Contract:
 *   - Clears any existing shadow leaf for `addr` first, regardless
 *     of whether the new PTE is present.
 *   - Translates the new UML PTE through kvm_um_pte_to_x86() to
 *     decide whether the new shadow leaf should be present.
 *   - Allocates intermediate tables on demand if installing.
 *   - On allocation failure, leaves the leaf absent + marks
 *     shadow->needs_full_resync so kvm_enter_guest's verifier
 *     repairs before KVM_RUN.
 *
 * Returns 0 on success or absent-by-design; negative errno on
 * allocation failure (recoverable via repair on next entry).
 *
 * Safe to call from atomic context only because the mutex it
 * takes is the per-shadow_mm fill_lock — that mutex is process-
 * context only. Callers from pgtable inline paths must therefore
 * be in process context (true for set_ptes / pte_clear from
 * generic mm code).
 */
int kvm_shadow_sync_pte(struct mm_struct *mm, unsigned long addr,
			pte_t pte);

#else /* !CONFIG_UM_BACKEND_KVM_INTEGRATED */

static inline int kvm_shadow_sync_pte(struct mm_struct *mm,
				      unsigned long addr,
				      pte_t pte)
{
	return 0;
}

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */

#endif /* _ASM_UM_KVM_MMU_SYNC_H */
