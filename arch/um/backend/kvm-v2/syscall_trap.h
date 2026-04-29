/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML backend v2 (KVM) — Phase D.1: IO-port LSTAR trampoline + ABI.
 *
 * Per memo 26 §D.1. Internal header for the 5-byte LSTAR trampoline +
 * the host-side trap-class enum. The trampoline page itself is owned
 * by syscall_trap.c (allocated on the per-VM struct, see
 * kvm_v2_backend.h's struct kvm_v2_vm). MSR_LSTAR programming +
 * PML4[508] kernel-half install land in D.4; D.2 wires the
 * KVM_EXIT_IO dispatch into kvm_v2_vcpu_run; D.5 flips ops.vcpu_run.
 *
 * The trap-class enum below is the host-side switch tag for the
 * KVM_EXIT_IO handler in D.2. The IO port number on the wire encodes
 * the class (0xf4 = SYSCALL today; PF/GP/UD will land in Phase E.3
 * with their own port numbers per §E.3 — same wire pattern v1 used at
 * kvm-v1-archive/kvm_backend.h:1004-1005,1399-1402, but each Phase E
 * class earns its own enum tag at the time it's wired so D.1's enum
 * stays minimal — single value today).
 *
 * v1 reference: kvm-v1-archive/thread.c:637 (KVM_BOOTSTRAP_GUEST_VA),
 * 665 (KVM_BOOTSTRAP_LSTAR_OFFSET), 1214-1219 (#else 5-byte LSTAR
 * bytes — verbatim port). v1's gadget paths (lines 814-1213) are out
 * of scope for v2; Phase H may revisit per memo 26 §D.1.
 */
#ifndef __ARCH_UM_BACKEND_KVM_V2_SYSCALL_TRAP_H
#define __ARCH_UM_BACKEND_KVM_V2_SYSCALL_TRAP_H

#include <linux/types.h>

struct kvm_run;
struct kvm_v2_vm;
struct uml_pt_regs;

/*
 * Host-side trap-class enum. The IO port number identifies the class
 * on the wire. Phase E.1 lands the exception-class ports alongside the
 * IDT install — the in-guest handler bytes are part of the same E.1
 * commit (each handler is `out %al, $port ; iretq`), so the enum
 * tags MUST be in place at handler-write time even though no host-
 * side dispatch arm exists for them yet (E.3 wires the dispatch arms).
 *
 * #BP (vector 3) is intentionally absent from this enum: per memo 26
 * §E.3 the in-handler trap-out for #BP uses KVM_GUESTDBG_USE_SW_BP →
 * KVM_EXIT_DEBUG (a separate KVM exit path), not an IO port. The
 * spec line for #BP is at memo 26 §E.3 ("KVM_GUESTDBG_USE_SW_BP →
 * KVM_EXIT_DEBUG (option 2; cleaner than IDT-handler-emits-IO-port
 * for the software-breakpoint case)"). E.1 still installs the IDT
 * gate for #BP at DPL=3 so user int3 is reachable; the gate offset
 * points at a stub byte sequence in the handlers page that's never
 * executed when KVM_GUESTDBG_USE_SW_BP is wired.
 *
 * Values match v1's UM_KVM_*_PORT scheme so a future restoration of
 * v1's gadget paths (Phase H) can lift v1 dispatch wholesale. v1
 * reference: kvm-v1-archive/thread.c:1316 (UM_KVM_PF_PORT = 0xfb),
 * 1378 (UM_KVM_GP_PORT = 0xf9), 1399-1402 (DE/BP/OF/UD).
 *
 * Note v1 used 0xfb for #PF and 0xfa for #DF; v2 reassigns 0xf6 →
 * #PF (matching memo 26 §E.3 spec) and reserves 0xfa for #UD (memo
 * 26 §E.3 column). v1's ports 0xfa/0xfb were tied to v1's bootstrap-
 * page layout; v2's per-port assignment is independent. Cross-
 * reference: memo 26 §E.3 "Per-vector mapping" table.
 */
enum um_kvm_iotrap {
	UM_KVM_TRAP_SYSCALL = 0xf4,	/* matches v1 UM_KVM_SYSCALL_PORT
					 * (kvm-v1-archive/kvm_backend.h:1004) */
	UM_KVM_TRAP_PF      = 0xf6,	/* #PF (vector 14); memo 26 §E.3 */
	UM_KVM_TRAP_GP      = 0xf9,	/* #GP (vector 13); memo 26 §E.3 */
	UM_KVM_TRAP_UD      = 0xfa,	/* #UD (vector  6); memo 26 §E.3 */
	UM_KVM_TRAP_DE      = 0xfb,	/* #DE (vector  0); memo 26 §E.3 */
	UM_KVM_TRAP_OF      = 0xfc,	/* #OF (vector  4); memo 26 §E.3 */
	/* #BP (vector 3) intentionally NOT in this enum — handled via
	 * KVM_GUESTDBG_USE_SW_BP → KVM_EXIT_DEBUG. */
	UM_KVM_TRAP_PANIC   = 0xf8,	/* unhandled-vector stub; host
					 * panics on this port (E.3+
					 * dispatch coverage gap signal). */
};

/*
 * Guest VA where the trampoline page is mapped via PML4[508]. Matches
 * v1's KVM_BOOTSTRAP_GUEST_VA (kvm-v1-archive/thread.c:637) — kernel-
 * half (canonical sign-extended past x86's user/kernel boundary), so a
 * CPL=3 guest walking any user-half VA never reaches this slot and the
 * trampoline page can carry US=0 flags safely (defense in depth — the
 * page is also read+execute kernel-only via host-side pagetable bits).
 *
 * D.1 lands the bytes at this (guest-) GVA's expected GPA but does NOT
 * install PML4[508] yet; that's D.4. The GVA is recorded here so D.4
 * has a single source of truth.
 */
#define KVM_V2_TRAMPOLINE_GVA		0xffffe00000000000ULL

/*
 * Offset within the trampoline page where MSR_LSTAR points. Matches
 * v1's KVM_BOOTSTRAP_LSTAR_OFFSET (kvm-v1-archive/thread.c:665). 0x40
 * leaves room for a future GDT (v1 used 0x000-0x040) and sits at a
 * 64 B alignment boundary so SYSCALL's RIP load lands on a cache line
 * cleanly. Phase E exception handlers will populate other offsets in
 * the same page (#PF at 0x4a0, etc. — see v1's offsets at
 * kvm-v1-archive/thread.c:666-690).
 */
#define KVM_V2_TRAMPOLINE_LSTAR_OFFSET	0x40

/*
 * Derived: guest VA the host writes to MSR_LSTAR in D.4. Computed once
 * here so D.4's MSR programming and any downstream introspection share
 * the same constant — divergence between LSTAR and the trampoline page
 * layout is the kind of bug that surfaces as a #UD/#GP at first guest
 * SYSCALL with no obvious ROOT cause; centralising eliminates that.
 */
#define KVM_V2_LSTAR_GVA		(KVM_V2_TRAMPOLINE_GVA + \
					 KVM_V2_TRAMPOLINE_LSTAR_OFFSET)

/*
 * Phase E.1: IDT + handler stubs + GDT pages live in the same PML4[448]
 * subtree as the trampoline (D.4b). PTE[0] is the trampoline (offset
 * 0x000); E.1 uses PTE[1..3] for IDT, handler stubs, and GDT
 * respectively.
 *
 * Why three SEPARATE pages (codex --search audit finding #1):
 *   A 256-entry IDT is 256 * 16 = 4096 bytes — exactly one page. We
 *   cannot co-locate handler stubs in the IDT page because every byte
 *   of the page is a valid IDT entry, and we cannot co-locate the GDT
 *   either because the IDT's segment selector field (0x08) walks
 *   through GDT[1] which the IDT page itself can't be. v1's archive
 *   packed everything into one page (kvm-v1-archive/thread.c:664-726
 *   defines BOOTSTRAP_GDT_OFFSET=0x000, LSTAR=0x040, TSS=0x200,
 *   IDT=0x280...) because v1's IDT was only 33 entries — a layout
 *   v2's full 256-entry IDT can't replicate.
 *
 * GVA → PML4 walk (each share PML4[448] / PUD[0] / PMD[0]):
 *   trampoline: PTE[0] → 0xffffe00000000000
 *   IDT:        PTE[1] → 0xffffe00000001000
 *   handlers:   PTE[2] → 0xffffe00000002000
 *   GDT:        PTE[3] → 0xffffe00000003000
 *
 * KVM_SET_SREGS.idt.base / .gdt.base point at these GVAs (memo 26
 * §E.1 "KVM_SET_SREGS.idt.base = the guest VA, idt.limit = 256*16-1").
 */
#define KVM_V2_IDT_GVA			(KVM_V2_TRAMPOLINE_GVA + 0x1000)
#define KVM_V2_HANDLERS_GVA		(KVM_V2_TRAMPOLINE_GVA + 0x2000)
#define KVM_V2_GDT_GVA			(KVM_V2_TRAMPOLINE_GVA + 0x3000)

/*
 * Within the handlers page, each in-guest exception stub is 16 bytes
 * apart (handler is 4 bytes; pad with NOPs to a clean 16-byte slot so
 * IDT-gate-offset arithmetic is `KVM_V2_HANDLERS_GVA + (slot << 4)`
 * with no per-handler size table). Per memo 26 §E.3: each stub is
 * `out %al, $port ; iretq` = 4 bytes. v1's analogous handlers at
 * kvm-v1-archive/thread.c:1310-1397 used the same `out + hlt` /
 * `out + iretq` shape but laid them out at byte-precise offsets;
 * v2's 16-byte slot grid is denser per offset arithmetic but identical
 * per byte content of the executed instructions.
 */
#define KVM_V2_HANDLER_SLOT_DE		0	/* #DE (vec 0) */
#define KVM_V2_HANDLER_SLOT_BP		1	/* #BP (vec 3) — DPL=3 */
#define KVM_V2_HANDLER_SLOT_OF		2	/* #OF (vec 4) — DPL=3 */
#define KVM_V2_HANDLER_SLOT_UD		3	/* #UD (vec 6) */
#define KVM_V2_HANDLER_SLOT_GP		4	/* #GP (vec 13) */
#define KVM_V2_HANDLER_SLOT_PF		5	/* #PF (vec 14) */
#define KVM_V2_HANDLER_SLOT_PANIC	6	/* fallback for any other vector */

/*
 * Allocate the per-VM trampoline page, write the 5 LSTAR bytes at the
 * documented offset, and stash the kernel VA + GPA on `vm`. Idempotent:
 * a successful prior call short-circuits. Called from kvm_v2_vm_create
 * (best-effort — buddy allocator is not up at init_backend time, see
 * D.0a; failure there is non-fatal and the lazy path in kvm_v2_vcpu_run
 * retries on first dispatch). Returns 0 on success or -errno; -ENOMEM
 * specifically if the buddy allocator isn't up yet (alloc_page returns
 * NULL pre-mm_init).
 */
int  kvm_v2_trampoline_alloc_and_install(struct kvm_v2_vm *vm);

/*
 * Symmetric teardown — free the page, clear the vm fields. Called from
 * kvm_v2_vm_destroy. Safe on a never-installed VM (NULL page → no-op).
 */
void kvm_v2_trampoline_free(struct kvm_v2_vm *vm);

/*
 * D.2: KVM_EXIT_IO dispatch. Called from kvm_v2_vcpu_run's exit-reason
 * switch when the guest trapped via the LSTAR trampoline's
 * `out %al, $0xf4`. `regs` is already populated by C.3's sync-regs
 * marshal (vcpu.c:kvm_v2_marshal_from_kvm_regs); `run` is the mmap'd
 * shared struct kvm_run for the firing vCPU; `vcpu_fd` is reserved
 * for D.3's marshal-out path (it currently goes unread inside the
 * helper). Returns 0 on success, -ENOTSUPP on an unexpected port
 * (caller should panic — other ports are Phase E's territory). Direct
 * panics inside handle_syscall surface as themselves.
 *
 * The helper is unreferenced from any production .vcpu_run path
 * today — ops.c still routes to seccomp_vcpu_run; D.5 flips the
 * pointer. The case-arm in vcpu.c that calls this helper is wired,
 * but kvm_v2_vcpu_run itself has no caller until D.5.
 */
int kvm_v2_handle_io_trap(struct uml_pt_regs *regs,
			  struct kvm_run *run,
			  int vcpu_fd);

/*
 * Phase E.1 (memo 26 §E.1): install the IDT + handler stubs + GDT in
 * the PML4[448] subtree alongside the trampoline. Allocates 3 pages
 * (one per artefact), populates them with the layouts documented in
 * the constants above, installs the PT entries at PTE[1..3], and
 * re-issues KVM_SET_SREGS for every existing pool member to point
 * idt/gdt at the new GVAs.
 *
 * Without this install KVM has no GDT to walk for code-descriptor
 * resolution during exception delivery (the IDT gate selector 0x08
 * indexes into the GDT base) and no IDT to dispatch any vector at
 * all — guest #PF on first instruction fetch lands in a void and
 * cascades to triple fault. E.3.5's flip of .vcpu_run from seccomp
 * to kvm-v2 dispatch needs all of E.1 + E.2 + E.3 in place; E.1 is
 * the data-structures layer.
 *
 * Idempotent: a successful prior install short-circuits via the
 * vm->idt_kva sentinel. Re-runnable through the late-install path
 * (subsys_initcall) so reordering retries don't re-allocate.
 *
 * Returns 0 on success / already-installed; -EINVAL if prerequisites
 * are unmet (D.4b's PT chain must be installed first); -ENOMEM if
 * __get_free_page returns NULL; other negative errno on KVM_SET_SREGS
 * or other ioctl failure.
 */
int  kvm_v2_exception_install(struct kvm_v2_vm *vm);

/*
 * Symmetric teardown — clear PTE[1..3], free the three pages, NULL
 * the vm fields. Called from kvm_v2_vm_destroy BEFORE
 * kvm_v2_kernel_half_free so the PTEs are valid until the chain
 * frees. Safe on a never-installed VM (NULL idt_kva → no-op).
 */
void kvm_v2_exception_free(struct kvm_v2_vm *vm);

#endif /* __ARCH_UM_BACKEND_KVM_V2_SYSCALL_TRAP_H */
