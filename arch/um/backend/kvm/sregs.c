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

#define KVM_HARNESS_GDT_ENTRIES		3
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
 * Minimal flat 64-bit GDT. Three entries: null, ring-0 code
 * (selector 0x08), ring-0 data (selector 0x10). Ring-0 code has
 * L=1 (long mode) + P=1 + S=1 + type=11 (code r/x, accessed);
 * data is L=0 + P=1 + S=1 + type=3 (r/w). Written in-place at
 * `gdt` (which must point at the harness region's GDT_OFFSET).
 */
void kvm_setup_harness_gdt(u64 *gdt)
{
	gdt[0] = 0x0000000000000000ULL;	/* null */
	gdt[1] = 0x00af9a000000ffffULL;	/* ring-0 code, L=1 */
	gdt[2] = 0x00cf92000000ffffULL;	/* ring-0 data */
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
