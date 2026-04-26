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
 * F12 mutation ring: record one entry per shadow PTE event so a
 * fatal-fault dump can reconstruct the recent history for cr2.
 * Single-writer per mm (we're inside guard or the atomic
 * single-host-thread invariant); head_seq is incremented after
 * the entry is written. Reader scans the ring backwards from
 * the most recent entry.
 */
static void kvm_shadow_record_mut(struct kvm_shadow_mm *shadow,
				  u64 addr, u64 ume,
				  u64 old_spte, u64 new_spte,
				  u8 action)
{
	u64 seq = READ_ONCE(shadow->mut_head_seq);
	struct kvm_shadow_mut_entry *e =
		&shadow->mut_ring[seq % KVM_SHADOW_MUT_RING_SIZE];

	e->addr     = addr;
	e->ume      = ume;
	e->old_spte = old_spte;
	e->new_spte = new_spte;
	e->action   = action;
	smp_wmb();
	WRITE_ONCE(shadow->mut_head_seq, seq + 1);
}

/*
 * Dump the most recent ring entries that touch [target_addr & ~mask,
 * target_addr | mask). Called from the fatal-fault path when we want
 * to know what mutations led to the corrupted VA. mask=0xfff scans
 * exact-page; larger masks widen the search.
 */
void kvm_shadow_mut_dump_for(struct kvm_shadow_mm *shadow,
			     u64 target_addr, u64 mask, unsigned int max_log)
{
	u64 seq;
	unsigned int found = 0;
	u64 target = target_addr & ~mask;

	if (!shadow)
		return;
	seq = READ_ONCE(shadow->mut_head_seq);
	pr_info("um: kvm mut_dump: looking for entries near va=0x%llx (mask=0x%llx) in last %u mutations (head_seq=%llu)\n",
		(unsigned long long)target,
		(unsigned long long)mask, KVM_SHADOW_MUT_RING_SIZE,
		(unsigned long long)seq);

	for (u64 i = 1; i <= KVM_SHADOW_MUT_RING_SIZE && i <= seq; i++) {
		u64 idx = (seq - i) % KVM_SHADOW_MUT_RING_SIZE;
		struct kvm_shadow_mut_entry *e = &shadow->mut_ring[idx];
		u64 ea = READ_ONCE(e->addr);

		if ((ea & ~mask) != target)
			continue;
		pr_info("um: kvm mut_dump[#%llu]: va=0x%llx ume=0x%llx old_spte=0x%llx new_spte=0x%llx action=%u\n",
			(unsigned long long)(seq - i),
			(unsigned long long)ea,
			(unsigned long long)e->ume,
			(unsigned long long)e->old_spte,
			(unsigned long long)e->new_spte,
			e->action);
		if (++found >= max_log)
			break;
	}
	if (!found)
		pr_info("um: kvm mut_dump: no recent mutations near va=0x%llx\n",
			(unsigned long long)target);
}
EXPORT_SYMBOL_GPL(kvm_shadow_mut_dump_for);

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
		if (!spte) {
			WRITE_ONCE(shadow->direct_sync_absent,
				   READ_ONCE(shadow->direct_sync_absent) + 1);
			kvm_shadow_record_mut(shadow, addr, ume, 0, 0,
					      KVM_SHADOW_MUT_NOOP_ABSENT);
			return 0;
		}
		{
			u64 old = READ_ONCE(*spte);

			if (old & 1ULL) {
				WRITE_ONCE(*spte, 0);
				WRITE_ONCE(shadow->dirty, true);
				WRITE_ONCE(shadow->direct_sync_clear,
					   READ_ONCE(shadow->direct_sync_clear) + 1);
				kvm_shadow_record_mut(shadow, addr, ume,
						      old, 0,
						      KVM_SHADOW_MUT_CLEAR);
			} else {
				WRITE_ONCE(shadow->direct_sync_absent,
					   READ_ONCE(shadow->direct_sync_absent) + 1);
				kvm_shadow_record_mut(shadow, addr, ume,
						      0, 0,
						      KVM_SHADOW_MUT_NOOP_ABSENT);
			}
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
		if (spte && (READ_ONCE(*spte) & 1ULL)) {
			u64 old = READ_ONCE(*spte);

			WRITE_ONCE(*spte, 0);
			WRITE_ONCE(shadow->dirty, true);
			WRITE_ONCE(shadow->direct_sync_clear,
				   READ_ONCE(shadow->direct_sync_clear) + 1);
			kvm_shadow_record_mut(shadow, addr, ume, old, 0,
					      KVM_SHADOW_MUT_CLEAR);
		} else {
			WRITE_ONCE(shadow->direct_sync_absent,
				   READ_ONCE(shadow->direct_sync_absent) + 1);
			kvm_shadow_record_mut(shadow, addr, ume, 0, 0,
					      KVM_SHADOW_MUT_NOOP_ABSENT);
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
		WRITE_ONCE(shadow->direct_sync_alloc_fail,
			   READ_ONCE(shadow->direct_sync_alloc_fail) + 1);
		kvm_shadow_record_mut(shadow, addr, ume, 0, 0,
				      KVM_SHADOW_MUT_ABSENT_PATH);
		return 0;
	}

	/*
	 * Path exists — install the leaf atomically.
	 */
	{
		u64 old = READ_ONCE(*spte);
		u64 new = (x86e & 0x000ffffffffff000ULL) |
			  (x86e & ~0x000ffffffffff000ULL);

		was_present = (old & 1ULL) != 0;
		WRITE_ONCE(*spte, new);
		WRITE_ONCE(shadow->dirty, true);
		WRITE_ONCE(shadow->direct_sync_install,
			   READ_ONCE(shadow->direct_sync_install) + 1);
		kvm_shadow_record_mut(shadow, addr, ume, old, new,
				      KVM_SHADOW_MUT_INSTALL);
	}

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
 * Read the current UML PTE for `addr` from `mm` without taking
 * mmap_lock. Used by sync-on-flush below — the caller holds
 * either pte_lockptr or is in a flush-tlb path where the pgd
 * walker pages are stable. Returns __pte(0) if any intermediate
 * level is absent (caller will treat as "clear shadow").
 */
static pte_t kvm_um_pgd_read_pte(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if (!mm || !mm->pgd)
		return __pte(0);
	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return __pte(0);
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return __pte(0);
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return __pte(0);
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return __pte(0);
	pte = pte_offset_kernel(pmd, addr);
	return *pte;
}

/*
 * #274 / F1: sync-on-flush. Replaces the earlier clear-on-flush
 * which was overwriting good direct-sync work. ptep_set_access_flags
 * calls set_pte_at (→ kvm_shadow_sync_pte installs new leaf), then
 * flush_tlb_fix_spurious_fault → flush_tlb_page. The latter must
 * NOT erase the just-installed leaf; instead, re-derive the leaf
 * from the current UML PTE so any concurrent change is reflected
 * but a recent direct-sync write is preserved.
 *
 * Range variant: same logic per page in [start, end).
 *
 * Both atomic — no allocation. Uses kvm_shadow_sync_pte which
 * itself defers allocation via needs_full_resync.
 */
void kvm_shadow_sync_va_atomic(struct mm_struct *mm, unsigned long addr)
{
	pte_t pte;

	if (!mm || !mm->context.id.kvm_shadow)
		return;
	pte = kvm_um_pgd_read_pte(mm, addr);
	(void)kvm_shadow_sync_pte(mm, addr, pte);
}
EXPORT_SYMBOL_GPL(kvm_shadow_sync_va_atomic);

void kvm_shadow_sync_range_atomic(struct mm_struct *mm,
				  unsigned long start, unsigned long end)
{
	unsigned long addr;
	struct kvm_shadow_mm *shadow;
	unsigned long count = 0;

	if (!mm)
		return;
	shadow = mm->context.id.kvm_shadow;
	if (!shadow || !shadow->pgd)
		return;

	/*
	 * F9 threshold: very large ranges (>= 512 pages = 2 MiB)
	 * become pathological page-by-page. Bail to needs_full_resync
	 * — kvm_enter_guest's repair path will do the full pgd walk
	 * once instead of N billion lookups.
	 */
	if ((end - start) >> PAGE_SHIFT >= 512) {
		WRITE_ONCE(shadow->needs_full_resync, true);
		WRITE_ONCE(shadow->dirty, true);
		WRITE_ONCE(shadow->direct_sync_range_clear,
			   READ_ONCE(shadow->direct_sync_range_clear) + 1);
		return;
	}

	for (addr = start & ~0xfffUL;
	     addr < ((end + 0xfffUL) & ~0xfffUL);
	     addr += PAGE_SIZE) {
		pte_t pte = kvm_um_pgd_read_pte(mm, addr);

		(void)kvm_shadow_sync_pte(mm, addr, pte);
		count++;
	}
	if (count)
		WRITE_ONCE(shadow->direct_sync_range_clear,
			   READ_ONCE(shadow->direct_sync_range_clear) + 1);
}
EXPORT_SYMBOL_GPL(kvm_shadow_sync_range_atomic);

/*
 * Legacy clear-only API kept for ABI compat with existing callers
 * that explicitly want to drop shadow leaves regardless of pgd.
 * No current in-tree caller after F1; safe to remove later.
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
	if (any_cleared) {
		WRITE_ONCE(shadow->dirty, true);
		WRITE_ONCE(shadow->direct_sync_range_clear,
			   READ_ONCE(shadow->direct_sync_range_clear) + 1);
	}
}
EXPORT_SYMBOL_GPL(kvm_shadow_clear_range_atomic);
