// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — long-mode SREGS setup.
 *
 * Workstream D-04b.1a. Pure data-structure helpers that populate
 * `struct kvm_sregs` for IA-32e long-mode ring-0 execution. Ported
 * from the D-01 spike at
 *   Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
 *   spikes/04-longmode-lstar-vmcall/spike.c
 * (setup_sregs() + setup_gdt() + setup_paging()). The spike
 * values are what KVM accepts for a minimal long-mode guest; we
 * reuse them unchanged here.
 *
 * This TU does NOT yet wire the helpers into run_userspace or
 * register any memslot. D-04b.1b glues the pieces together with
 * a harness region + KVM_RUN call; D-04b.2 swaps the handcrafted
 * page tables for UML's own init_mm.pgd. Splitting the port
 * across sub-steps keeps each commit reviewable.
 */
#include <linux/kvm.h>
#include <linux/types.h>

#include "kvm_backend.h"

/*
 * Fixed offsets within a 2 MiB harness slot (matching spike 04's
 * layout for easy diff-review; these constants move when D-04b.2
 * replaces the handcrafted layout with UML's own pgd).
 */
#define KVM_HARNESS_PML4_OFFSET		0x0000
#define KVM_HARNESS_PDPT_OFFSET		0x1000
#define KVM_HARNESS_PD_OFFSET		0x2000
#define KVM_HARNESS_GDT_OFFSET		0x3000
#define KVM_HARNESS_CODE_OFFSET		0x4000

/*
 * Six-entry GDT: null, ring-0 code, ring-0 data, unused padding,
 * ring-3 data, ring-3 code. The padding slot at selector 0x18 is
 * required so that SYSRETQ-with-STAR[63:48]=0x18 lands CS at
 * 0x28 (idx 5) and SS at 0x20 (idx 4) — AMD64 SDM §6.1.1. Entries
 * 4 + 5 are DPL=3 so SYSRETQ's forced RPL=3 doesn't trip #GP on
 * descriptor-load.
 */
#define KVM_HARNESS_GDT_ENTRIES		6
#define KVM_HARNESS_GDT_LIMIT		(KVM_HARNESS_GDT_ENTRIES * 8 - 1)

/* Control-register bits (CR0/CR4/EFER) for long-mode ring-0. */
#define KVM_CR0_PE	(1UL << 0)	/* protected mode */
#define KVM_CR0_MP	(1UL << 1)	/* monitor coprocessor */
#define KVM_CR0_NE	(1UL << 5)	/* numeric error */
#define KVM_CR0_WP	(1UL << 16)	/* write protect */
#define KVM_CR0_PG	(1UL << 31)	/* paging */
#define KVM_CR4_PAE	(1UL << 5)	/* physical-addr extension */
#define KVM_EFER_SCE	(1UL << 0)	/* SYSCALL enable */
#define KVM_EFER_LME	(1UL << 8)	/* long-mode enable */
#define KVM_EFER_LMA	(1UL << 10)	/* long-mode active */

/* PTE flags for the identity-paged 2 MiB harness region. */
#define KVM_PTE_P	(1ULL << 0)
#define KVM_PTE_RW	(1ULL << 1)
#define KVM_PTE_US	(1ULL << 2)
#define KVM_PTE_PS	(1ULL << 7)	/* 2 MiB page */

/*
 * Flat 64-bit GDT for the harness. Six entries:
 *
 *   idx 0 sel 0x00 — null
 *   idx 1 sel 0x08 — ring-0 code, L=1
 *   idx 2 sel 0x10 — ring-0 data
 *   idx 3 sel 0x18 — unused padding (SYSRETQ offset anchor; see
 *                    AMD64 SDM §6.1.1: STAR[63:48] is the "base"
 *                    from which SS = base+8, CS = base+16)
 *   idx 4 sel 0x20 — ring-3 data (DPL=3)
 *   idx 5 sel 0x28 — ring-3 code, L=1, DPL=3
 *
 * The Ring-0 pair is identical to the pre-D-04d 3-entry GDT; the
 * ring-3 pair is new in Phase III Lift #1b. Descriptor bit
 * derivation per AMD64 SDM vol 3 §3.4.5:
 *   ring-3 code = 0x00af_fa_00_000000_ffff (base 0x9a | (DPL3<<5))
 *   ring-3 data = 0x00cf_f2_00_000000_ffff (base 0x92 | (DPL3<<5))
 *
 * Written in-place at `gdt` (must point at the harness region's
 * GDT_OFFSET).
 */
void kvm_setup_harness_gdt(u64 *gdt)
{
	gdt[0] = 0x0000000000000000ULL;	/* null */
	gdt[1] = 0x00af9a000000ffffULL;	/* ring-0 code, L=1 */
	gdt[2] = 0x00cf92000000ffffULL;	/* ring-0 data */
	gdt[3] = 0x0000000000000000ULL;	/* unused padding */
	gdt[4] = 0x00cff2000000ffffULL;	/* ring-3 data, DPL=3 */
	gdt[5] = 0x00affa000000ffffULL;	/* ring-3 code, L=1, DPL=3 */
}

/*
 * Identity-map the first 2 MiB of the harness slot: virt 0..2
 * MiB → guest_phys 0..2 MiB via a single 2 MiB huge page
 * (CR4.PAE + PD.PS). pml4/pdpt/pd must point at the slot's
 * matching offsets.
 */
void kvm_setup_harness_paging(u64 *pml4, u64 *pdpt, u64 *pd)
{
	pml4[0] = KVM_HARNESS_PDPT_OFFSET |
		  KVM_PTE_P | KVM_PTE_RW | KVM_PTE_US;
	pdpt[0] = KVM_HARNESS_PD_OFFSET |
		  KVM_PTE_P | KVM_PTE_RW | KVM_PTE_US;
	pd[0]	= 0 |
		  KVM_PTE_P | KVM_PTE_RW | KVM_PTE_US | KVM_PTE_PS;
}

/*
 * D-04b.2a extension of kvm_setup_harness_paging: identity-map
 * a caller-specified range via 2 MiB huge pages. Used when the
 * harness wants to cover more than the initial 2 MiB of the
 * slot (e.g. reaching UML kernel text at physmem offsets
 * beyond 2 MiB). pml4 + pdpt are populated to point at pd_pages
 * enough PD pages to cover `npages_2m` 2 MiB pages; caller
 * provides a contiguous array of PD pages of the right count.
 *
 * Returns the number of PD entries written, or -1 if npages_2m
 * exceeds what the provided PD pages can hold.
 *
 * Layout assumption: this helper targets small ranges (up to
 * a few GiB). For physmem_size ≤ 1 GiB one PD page (512
 * entries × 2 MiB = 1 GiB) is sufficient; caller passes
 * `pd_pages` as a single-page pointer and npages_2m ≤ 512.
 * For larger ranges, the caller stitches PDPT entries to
 * multiple PD pages themselves (out of scope for D-04b.2a).
 */
int kvm_setup_harness_paging_range(u64 *pml4, u64 *pdpt, u64 *pd,
				   unsigned int pd_offset_in_slot,
				   unsigned int npages_2m)
{
	unsigned int i;

	if (npages_2m > 512)
		return -1;

	pml4[0] = KVM_HARNESS_PDPT_OFFSET |
		  KVM_PTE_P | KVM_PTE_RW | KVM_PTE_US;
	pdpt[0] = pd_offset_in_slot |
		  KVM_PTE_P | KVM_PTE_RW | KVM_PTE_US;

	for (i = 0; i < npages_2m; i++) {
		pd[i] = ((u64)i << 21) |	/* 2 MiB-aligned */
			KVM_PTE_P | KVM_PTE_RW | KVM_PTE_US | KVM_PTE_PS;
	}

	return (int)npages_2m;
}

/*
 * Populate SREGS for long-mode ring-0 execution with the GDT
 * and page tables placed at their spike-04-style fixed
 * offsets in the harness slot. kvm_segment fields mirror what
 * the CPU would derive from a real GDT load — KVM doesn't
 * re-fetch the descriptor bytes, it trusts this struct.
 *
 * Caller is expected to have done KVM_GET_SREGS first so any
 * vCPU-implicit fields (APIC state, etc.) are preserved. Only
 * the long-mode-relevant fields are overwritten here.
 */
void kvm_setup_harness_sregs(struct kvm_sregs *sregs)
{
	struct kvm_segment code = {
		.base		= 0,
		.limit		= 0xffffffff,
		.selector	= 0x08,
		.type		= 0xb,		/* code r/x, accessed */
		.present	= 1,
		.dpl		= 0,
		.db		= 0,
		.s		= 1,
		.l		= 1,		/* long mode code */
		.g		= 1,
	};
	struct kvm_segment data = {
		.base		= 0,
		.limit		= 0xffffffff,
		.selector	= 0x10,
		.type		= 0x3,		/* data r/w, accessed */
		.present	= 1,
		.dpl		= 0,
		.db		= 1,
		.s		= 1,
		.l		= 0,
		.g		= 1,
	};

	sregs->cs = code;
	sregs->ds = data;
	sregs->es = data;
	sregs->fs = data;
	sregs->gs = data;
	sregs->ss = data;

	sregs->gdt.base  = KVM_HARNESS_GDT_OFFSET;
	sregs->gdt.limit = KVM_HARNESS_GDT_LIMIT;

	sregs->cr3  = KVM_HARNESS_PML4_OFFSET;
	sregs->cr4  = KVM_CR4_PAE;
	sregs->cr0  = KVM_CR0_PE | KVM_CR0_MP | KVM_CR0_NE |
		      KVM_CR0_WP | KVM_CR0_PG;
	sregs->efer = KVM_EFER_SCE | KVM_EFER_LME | KVM_EFER_LMA;
}
