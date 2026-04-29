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

struct kvm_v2_vm;

/*
 * Host-side trap-class enum. The IO port number identifies the class
 * on the wire (0xf4 = SYSCALL — only class today). Future Phase E.3
 * exception-handler ports (PF/GP/UD/...) will land each as a separate
 * enum tag at the commit that wires the handler. Values match v1's
 * UM_KVM_*_PORT scheme so a future restoration of the gadget paths
 * (Phase H) can lift v1 dispatch wholesale.
 */
enum um_kvm_iotrap {
	UM_KVM_TRAP_SYSCALL = 0xf4,	/* matches v1 UM_KVM_SYSCALL_PORT
					 * (kvm-v1-archive/kvm_backend.h:1004) */
	/* Phase E.3 will add: UM_KVM_TRAP_PF = 0xf6, UM_KVM_TRAP_GP = 0xf9,
	 * UM_KVM_TRAP_UD = 0xfd, etc. — each at the commit that wires the
	 * handler, so the enum stays in lockstep with what's actually
	 * dispatched.
	 */
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

#endif /* __ARCH_UM_BACKEND_KVM_V2_SYSCALL_TRAP_H */
