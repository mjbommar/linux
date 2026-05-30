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
struct kvm_v2_vcpu;
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
	UM_KVM_TRAP_NM      = 0xfd,	/* #NM (vector  7); SMP-T22
					 * 2026-05-02: host-side TS clear
					 * via vmexit, eliminates in-guest
					 * iretq → eliminates the NM_stub+2
					 * iretq-popped-kernel-half-RIP
					 * Bug B race class. Paired with
					 * arch_thread.kvm_v2.nm_ts_bypass
					 * one-shot bypass in load_user_sregs
					 * to coexist with lazy-FPU TS arming. */
	/* #BP (vector 3) intentionally NOT in this enum — handled via
	 * KVM_GUESTDBG_USE_SW_BP → KVM_EXIT_DEBUG. */
	UM_KVM_TRAP_BP      = 0xfe,	/* #BP (vector  3); breakpoint (INT3) */
	UM_KVM_TRAP_DB      = 0xf5,	/* #DB (vector  1); debug exception */
	UM_KVM_TRAP_DF      = 0xf2,	/* #DF (vector  8); double fault */
	UM_KVM_TRAP_SS      = 0xf7,	/* #SS (vector 12); stack segment */
	UM_KVM_TRAP_AC      = 0xf3,	/* #AC (vector 17); alignment check */
	UM_KVM_TRAP_TS      = 0xf1,	/* #TS (vector 10); invalid TSS */
	UM_KVM_TRAP_NP      = 0xef,	/* #NP (vector 11); segment not present */
	UM_KVM_TRAP_MF      = 0xee,	/* #MF (vector 16); x87 FP exception */
	UM_KVM_TRAP_XM      = 0xed,	/* #XM (vector 19); SIMD FP exception */
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
 * Phase H gadget — per-vCPU state pages (2026-05-04, Phase 2).
 *
 * Each vCPU owns a dedicated 4KB "gadget state page" containing the
 * fields the LSTAR fast-path reads via `swapgs ; mov %gs:OFFSET, %eax`.
 * Per-vCPU isolation is required because v2's per-host-CPU vCPU pool
 * runs different tasks on different vCPUs simultaneously — a single
 * shared state page (Phase 1) was UP-only because a writer race on
 * SMP would hand the gadget stale values.
 *
 * Layout (parallel to E.2's per-vCPU IST/TSS PTE region — same chain
 * trampoline_pte_kva owns):
 *   PTE[0]                              trampoline (per-VM, +0x000)
 *   PTE[1]                              IDT       (per-VM, +0x1000)
 *   PTE[2]                              handlers  (per-VM, +0x2000)
 *   PTE[3]                              GDT       (per-VM, +0x3000)
 *   PTE[KVM_V2_IST_BASE_SLOT + 2*cpu]   IST stack (per-vCPU)
 *   PTE[KVM_V2_IST_BASE_SLOT + 2*cpu+1] TSS body  (per-vCPU)
 *   PTE[KVM_V2_GADGET_BASE_SLOT + cpu]  gadget state page (per-vCPU,
 *                                                          this region)
 *
 * Per-vCPU MSR_KERNEL_GS_BASE = KVM_V2_GADGET_STATE_GVA(cpu) so each
 * vCPU's swapgs lands on its own state page. Refreshed in
 * load_user_sregs from the running task (task_tgid_vnr(current) etc).
 *
 * Why a separate page (vs a slot in the existing per-vCPU TSS or IST
 * page): the gadget reads via %gs:disp32 against KERNEL_GS_BASE; that
 * MSR is one register, so it must point at exactly one base GVA. The
 * IST stack page is RW with kernel-only and pushed-to during
 * exception delivery — co-locating gadget state with IST risks an
 * exception during gadget execution corrupting the gadget's data.
 * The TSS page is similarly load-bearing for TR. Cleanest is its own
 * dedicated 4KB page.
 *
 * Field offsets within the state page match v1's KVM_GADGET_OFF_*
 * scheme (kvm-v1-archive/kvm_backend.h:305-313) so Phase 3's expansion
 * to 5+ gadgets reuses the v1 dispatch tree's offsets verbatim:
 *   +0x00 SEQ        (reserved for SMP seqlock if ever needed)
 *   +0x04 CPU_ID     (reserved for getcpu — Phase 3+)
 *   +0x08 TGID       (getpid — Phase 1+2)
 *   +0x0c TID        (gettid — Phase 3)
 *   +0x14 UID        (getuid — Phase 3)
 *   +0x18 EUID       (geteuid — Phase 3)
 *   +0x1c GID        (getgid — Phase 3)
 *
 * Phase 2 still ONLY uses TGID. Other slots are zero-filled by
 * __GFP_ZERO and never read by the LSTAR bytes installed today.
 *
 * Slot arithmetic: GADGET_BASE_SLOT = IST_BASE_SLOT (4) + 2*NR_CPUS.
 * For NR_CPUS=64 worst case, GADGET_BASE_SLOT = 132 and the last
 * gadget slot is 132 + 63 = 195 — comfortably under the 512-PTE
 * table limit. Same defensive overflow guard as IST/TSS install.
 */
/*
 * UML-private syscall NR for the APERF/MPERF gadget consumer.
 *
 * 0xc0de is well above the highest Linux x86_64 syscall number
 * (~470 as of v7.1) so it cannot collide with a present or
 * reasonably-future upstream NR.  The high bits of the NR (0xff00)
 * mean the gadget's upper-byte guard would reject it; the
 * h_aperfmperf pre-check therefore runs BEFORE the guard, mirroring
 * the getcpu (NR=309) escape hatch.
 *
 * The interface, documented in
 * Documentation/virt/uml/examples/aperf-mperf/README.md:
 *
 *   long uml_aperfmperf(struct um_aperfmperf *out);
 *
 *   struct um_aperfmperf { u64 aperf; u64 mperf; };
 *
 *   Returns 0; writes raw IA32_APERF / IA32_MPERF host-counter
 *   values into *out.  Out-of-range pointer falls back to the host
 *   trap path (which will return -ENOSYS).  NULL pointer is a
 *   silent no-op that returns 0 (used by the kselftest as a
 *   gadget-presence probe).
 *
 * The rdmsr instructions execute at guest CPL=0 inside the KVM
 * guest; with CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y +
 * kvm_v2_aperfmperf=on the cap-enable in vm_create lets KVM pass
 * the reads through to hardware.  Without it, KVM intercepts and
 * returns zero -- so the same gadget body works in both modes and
 * the demo can use the zero/non-zero split as a self-check on the
 * cap-plumbing layer.
 */
#define KVM_V2_NR_UML_APERFMPERF	0xc0de

#define KVM_V2_GADGET_BASE_SLOT		(KVM_V2_IST_BASE_SLOT + 2 * NR_CPUS)
#define KVM_V2_GADGET_STATE_GVA(cpu)	(KVM_V2_TRAMPOLINE_GVA + \
					 (KVM_V2_GADGET_BASE_SLOT + (cpu)) * 0x1000ULL)

#define KVM_V2_GADGET_OFF_SEQ		0x00	/* reserved (Phase 7+ seqlock) */
#define KVM_V2_GADGET_OFF_CPU_ID	0x04	/* Phase 5: getcpu */
#define KVM_V2_GADGET_OFF_TGID		0x08	/* Phase 1+2: getpid */
#define KVM_V2_GADGET_OFF_TID		0x0c	/* Phase 3: gettid */
#define KVM_V2_GADGET_OFF_PPID		0x10	/* Phase 4: getppid */
#define KVM_V2_GADGET_OFF_UID		0x14	/* Phase 3: getuid */
#define KVM_V2_GADGET_OFF_EUID		0x18	/* Phase 3: geteuid */
#define KVM_V2_GADGET_OFF_GID		0x1c	/* Phase 3: getgid */
#define KVM_V2_GADGET_OFF_EGID		0x20	/* Phase 4: getegid */
#define KVM_V2_GADGET_OFF_RECORD	0x24	/* #169 Phase 4: u8 record-mode
						 * gadget-bypass flag. When non-zero
						 * the gadget body unconditionally
						 * branches to the fallback path so
						 * record/replay sees every syscall
						 * via the handle_io_trap observe
						 * hook. Cleared at install; set by
						 * kvm_v2_record_start (all-pool
						 * walk) and re-cleared by the
						 * record-stop disarm. Cost when
						 * unset: one untaken cmp+jne per
						 * gadget entry. Memo 27 §3.3
						 * Option A. */
/* +0x25..+0x27 padding (3 bytes — alignment for the 8B TASK_SIZE_CAP below). */
#define KVM_V2_GADGET_OFF_TASK_SIZE_CAP	0x28	/* Phase 5: u64, set once at install
						 * (task_size - 16, 16B safety margin
						 * matches v1's lifecycle.c:974). Used
						 * by getcpu's user-pointer bounds
						 * check before storing cpu/node ids. */
#define KVM_V2_GADGET_OFF_REAL_SEC	0x30	/* Phase 6: s64 CLOCK_REALTIME seconds.
						 * Refreshed in load_user_sregs via
						 * ktime_get_real_ts64(). NO seqlock —
						 * 1-second resolution makes a torn read
						 * at-worst off-by-one, identical to
						 * native vDSO behavior. v1 reference:
						 * kvm-v1-archive/thread.c:1110-1115. */
#define KVM_V2_GADGET_OFF_MONO_SEC	0x38	/* Phase 7: s64 CLOCK_MONOTONIC seconds */
#define KVM_V2_GADGET_OFF_MONO_NSEC	0x40	/* Phase 7: s64 CLOCK_MONOTONIC ns-within-sec */
#define KVM_V2_GADGET_OFF_BUDGET	0x48	/* Phase 7: s32 gadget call budget. Decrement
						 * + js → fallback when negative. Reset to
						 * KVM_V2_VVAR_BUDGET_INITIAL on every host
						 * refresh. Bounds vvar staleness to at most
						 * BUDGET_INITIAL gadget calls between host
						 * refreshes, since SIGALRM is masked during
						 * KVM_RUN and a tight clock_gettime loop
						 * would otherwise freeze guest time forever.
						 * v1 reference: kvm-v1-archive/lifecycle.c:
						 * 1051-1066. */
/* +0x4c is a 4-byte gap (alignment for the 8B SAVE_* slots below). */
#define KVM_V2_GADGET_OFF_SAVE_RDX	0x50	/* Mainstream #1+#4: user RDX
						 * saved at gadget entry,
						 * restored at every gadget exit.
						 * Linux x86_64 syscall ABI
						 * preserves all GPRs except
						 * RAX/RCX/R11; the gadget's
						 * h_time/h_getcpu/h_clock_gettime
						 * paths use RDX as scratch and
						 * MUST restore it before SYSRETQ
						 * to comply. */
#define KVM_V2_GADGET_OFF_SAVE_R8	0x58	/* Mainstream #1+#4: user R8
						 * — clobbered by
						 * h_clock_gettime (MONO_NSEC
						 * staging). */
#define KVM_V2_GADGET_OFF_SAVE_R10	0x60	/* Mainstream #1+#4: user R10
						 * — clobbered by
						 * h_clock_gettime (MONO_SEC
						 * staging) and h_getcpu (zero
						 * for *node). */
#define KVM_V2_GADGET_OFF_SAVE_RCX	0x68	/* h_aperfmperf scratch: user
						 * RCX (= user RIP for SYSRETQ)
						 * saved before rdmsr (which
						 * reads ECX as MSR index input
						 * and clobbers it), restored
						 * before tail.  Saved ONLY by
						 * h_aperfmperf, not by the
						 * gadget entry preamble — no
						 * other handler touches RCX, so
						 * the cost stays localized to
						 * the consumer.
						 */
#define KVM_V2_GADGET_OFF_APERF_CAP	0x70	/* u8.  Set to 1 by exception.c
						 * after a successful APERFMPERF
						 * disable-exits ioctl; left at
						 * 0 otherwise.  h_aperfmperf
						 * checks this byte FIRST and
						 * falls back to the host trap
						 * path if zero — guards against
						 * rdmsr #GP injection when the
						 * cap is off (KVM would not let
						 * the gadget read through to
						 * hardware in that case).
						 */
#define KVM_V2_VVAR_BUDGET_INITIAL	10000	/* Phase 7: ~30µs of clock_gettime work
						 * before falling back to host refresh.
						 * v1 used the same value at lifecycle.c
						 * KVM_VVAR_BUDGET_INITIAL definition. */

/*
 * Mainstream-readiness item #3 — compile-time invariants on the
 * gadget state-page layout. These catch:
 *   - SAVE_RDX/R8/R10 slot misalignment (must be 8B-aligned for
 *     a clean qword move with no #AC trap risk under SMAP/SMEP).
 *   - Slot collisions if a future field is squeezed between
 *     existing offsets without bumping subsequent ones.
 *   - State-page overflow against PAGE_SIZE.
 *
 * These are static_assert (compile-time), guarded by __KERNEL__ so
 * the same header can later be #include'd from an asm-safe sub-
 * header without breaking GAS (which doesn't grok _Static_assert).
 */
#ifdef __KERNEL__
#include <linux/build_bug.h>

static_assert(KVM_V2_GADGET_OFF_SAVE_RDX % 8 == 0,
	      "SAVE_RDX must be 8-byte aligned for movq %gs:disp32 stores");
static_assert(KVM_V2_GADGET_OFF_SAVE_R8  % 8 == 0,
	      "SAVE_R8 must be 8-byte aligned");
static_assert(KVM_V2_GADGET_OFF_SAVE_R10 % 8 == 0,
	      "SAVE_R10 must be 8-byte aligned");

static_assert(KVM_V2_GADGET_OFF_SAVE_RDX + 8 <= KVM_V2_GADGET_OFF_SAVE_R8,
	      "SAVE_RDX overlaps SAVE_R8");
static_assert(KVM_V2_GADGET_OFF_SAVE_R8 + 8 <= KVM_V2_GADGET_OFF_SAVE_R10,
	      "SAVE_R8 overlaps SAVE_R10");
static_assert(KVM_V2_GADGET_OFF_SAVE_R10 + 8 <= KVM_V2_GADGET_OFF_SAVE_RCX,
	      "SAVE_R10 overlaps SAVE_RCX");
static_assert(KVM_V2_GADGET_OFF_SAVE_RCX % 8 == 0,
	      "SAVE_RCX must be 8-byte aligned");
static_assert(KVM_V2_GADGET_OFF_SAVE_RCX + 8 <= KVM_V2_GADGET_OFF_APERF_CAP,
	      "SAVE_RCX overlaps APERF_CAP");
static_assert(KVM_V2_GADGET_OFF_APERF_CAP + 1 <= 4096,
	      "APERF_CAP spills past state-page PAGE_SIZE");

/* Pre-existing fields must not collide with the SAVE block. */
static_assert(KVM_V2_GADGET_OFF_BUDGET + 4 <= KVM_V2_GADGET_OFF_SAVE_RDX,
	      "BUDGET overlaps SAVE_RDX");
static_assert(KVM_V2_GADGET_OFF_MONO_NSEC + 8 <= KVM_V2_GADGET_OFF_BUDGET,
	      "MONO_NSEC overlaps BUDGET");
#endif /* __KERNEL__ */

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
#define KVM_V2_HANDLER_SLOT_NM		7	/* #NM (vec 7) — Phase H.2 lazy FPU */
#define KVM_V2_HANDLER_SLOT_DB		8	/* #DB (vec 1) */
#define KVM_V2_HANDLER_SLOT_SS		9	/* #SS (vec 12) */
#define KVM_V2_HANDLER_SLOT_AC		10	/* #AC (vec 17) */
#define KVM_V2_HANDLER_SLOT_DF		11	/* #DF (vec 8) */
#define KVM_V2_HANDLER_SLOT_TS		12	/* #TS (vec 10) */
#define KVM_V2_HANDLER_SLOT_NP		13	/* #NP (vec 11) */
#define KVM_V2_HANDLER_SLOT_MF		14	/* #MF (vec 16) */
#define KVM_V2_HANDLER_SLOT_XM		15	/* #XM (vec 19) */

/*
 * Phase E.2: per-vCPU IST stack + TSS pages. Per memo 26 §E.2 each
 * vCPU needs its own IST1 stack (the CPU pushes the iretq frame there
 * during exception delivery, so a shared stack would collide if two
 * vCPUs took an exception simultaneously) and its own TSS body (TSS
 * holds the IST1 RSP pointer; only one IST1 can be loaded at a time
 * via TR, so two vCPUs sharing a TSS would point both at the same
 * stack top — same collision risk). The codex --search audit
 * independent finding spelled this out: per-vCPU TSS is REQUIRED.
 *
 * Layout: PTE[4..3 + NR_CPUS*2] of the same trampoline_pte_kva chain
 * that owns PTE[0..3] (trampoline + IDT/handlers/GDT). Per vCPU we
 * consume two PTE slots:
 *
 *   PTE[4 + cpu*2]: IST stack page (RW, US=0 — handlers push iretq
 *                   frame to it; CPU writes occur during exception
 *                   delivery, so RW is required).
 *   PTE[5 + cpu*2]: TSS page (RW, US=0 — CPU writes the busy bit
 *                   on LTR, plus our TSS body sits at offset 0;
 *                   only the first 104 bytes are TSS, the rest is
 *                   zeroed padding from __GFP_ZERO).
 *
 * The KVM_V2_TRAMPOLINE_GVA + slot * 0x1000 arithmetic gives the
 * guest VA each page lives at; per-vCPU GVAs are computed via
 * KVM_V2_IST_GVA(cpu) / KVM_V2_TSS_GVA(cpu). NR_CPUS is bounded
 * (≤16 typical for UML, NR_CPUS_RANGE_END=64 worst case), so the
 * 4 + 2*NR_CPUS = 132 max PTEs fit comfortably in the 512-entry
 * PTE table.
 *
 * codex --search audit CLAIM C (verified): KVM_SET_TSS_ADDR
 * (gpa=0xfffbd000, set at vm_create) is unrestricted-guest
 * scaffolding — vestigial under our paged-from-vcpu-create flow.
 * SREGS.tr (set per-vCPU by E.2's extended descriptors_sregs
 * helper) is the real TR cache. Don't conflate the two.
 *
 * codex --search audit CLAIM D (verified): 4KB IST stack is
 * sufficient — guest handler stubs do `out` only, no C call chain
 * runs in guest, so the iretq frame (5 × 8 = 40 bytes) plus any
 * pushed error code (8 bytes) is the entire stack budget.
 */
#define KVM_V2_IST_BASE_SLOT		4
#define KVM_V2_IST_GVA(cpu)		(KVM_V2_TRAMPOLINE_GVA + \
					 (KVM_V2_IST_BASE_SLOT + (cpu) * 2) * 0x1000ULL)
#define KVM_V2_TSS_GVA(cpu)		(KVM_V2_TRAMPOLINE_GVA + \
					 (KVM_V2_IST_BASE_SLOT + (cpu) * 2 + 1) * 0x1000ULL)

/*
 * IST stack TOP — stacks grow down on x86_64, so the architectural
 * RSP-style pointer the TSS IST1 field carries is GVA + PAGE_SIZE.
 * The first push lands at GVA + PAGE_SIZE - 8 inside the same
 * RW page. v1 archive mirror: kvm-v1-archive/thread.c:1623
 * (`KVM_BOOTSTRAP_GUEST_VA + KVM_BOOTSTRAP_STACK_TOP`).
 */
#define KVM_V2_IST_STACK_TOP_GVA(cpu)	(KVM_V2_IST_GVA(cpu) + 0x1000ULL)

/*
 * TR selector + TSS limit, matching v1's KVM_BOOTSTRAP_TSS_SEL
 * (kvm-v1-archive/thread.c — 0x30 = GDT slot 6 × 8). Long-mode TSS
 * structure is 104 bytes (Intel SDM Vol.3 §7.7 / kernel's
 * `struct x86_hw_tss` at arch/x86/include/asm/processor.h:313-332);
 * limit = sizeof - 1 = 103.
 */
#define KVM_V2_TSS_SEL			0x30
#define KVM_V2_TSS_BODY_SIZE		104
#define KVM_V2_TSS_LIMIT		(KVM_V2_TSS_BODY_SIZE - 1)

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
 * Phase H gadget Phase 3 (2026-05-04): upgrade the LSTAR body from the
 * SAFE 5-byte fallback (out + sysretq) installed by
 * kvm_v2_trampoline_alloc_and_install to the 94-byte stay-in-guest
 * gadget. Must be called ONLY after all per-vCPU gadget state pages
 * have been installed (kvm_v2_install_per_vcpu_gadget_state for every
 * pool member), because the gadget body's `mov %gs:OFF, %eax` reads
 * from KVM_V2_GADGET_STATE_GVA(cpu) and would SIGSEGV if those pages
 * aren't mapped behind that GVA.
 *
 * Called from kvm_v2_exception_install at the end of its per-vCPU
 * loop, gated on every loop iteration succeeding. Idempotent — a
 * second invocation re-writes the same bytes (no-op on the readback
 * check). Safe to call on a never-installed VM (NULL trampoline_page
 * short-circuits with a warning).
 *
 * Returns 0 on success / already-upgraded; -EINVAL if trampoline_page
 * is NULL (caller should have ensured trampoline_install ran first).
 */
int kvm_v2_trampoline_upgrade_to_gadget(struct kvm_v2_vm *vm);

/*
 * D.2 + E.3: KVM_EXIT_IO dispatch. Called from kvm_v2_vcpu_run's
 * exit-reason switch when the guest trapped on any I/O port.
 * Today (post-E.3) the port set is:
 *
 *   UM_KVM_TRAP_SYSCALL (0xf4) — D.2 handle_syscall path.
 *   UM_KVM_TRAP_PF      (0xf6) — E.3 page-fault dispatcher.
 *   UM_KVM_TRAP_GP      (0xf9) — E.3 general-protection dispatcher.
 *   UM_KVM_TRAP_UD      (0xfa) — E.3 undefined-opcode dispatcher.
 *   UM_KVM_TRAP_DE      (0xfb) — E.3 divide-error dispatcher.
 *   UM_KVM_TRAP_OF      (0xfc) — E.3 overflow dispatcher.
 *   UM_KVM_TRAP_PANIC   (0xf8) + any other — E.3 panic stub.
 *
 * `regs` is already populated by C.3's sync-regs marshal
 * (vcpu.c:kvm_v2_marshal_from_kvm_regs); `run` is the mmap'd shared
 * struct kvm_run for the firing vCPU. `vcpu` is the per-host-CPU
 * pool member — E.3's exception dispatchers read vcpu->ist_stack_kva
 * to extract the long-mode iretq frame the CPU pushed onto the IST
 * stack. The signature was widened from `int vcpu_fd` to
 * `struct kvm_v2_vcpu *vcpu` at E.3 because the IST stack KVA
 * lives on the per-vCPU struct (E.2 added it).
 *
 * Returns 0 on success, -EINVAL if vcpu is NULL. Panic-on-unknown-
 * port lives inside the panic dispatcher.
 *
 * The helper still has no production .vcpu_run caller — ops.c still
 * routes to seccomp_vcpu_run; E.3.5 flips the pointer. The case-arm
 * in vcpu.c that calls this helper is wired, but kvm_v2_vcpu_run
 * itself has no caller until E.3.5.
 */
int kvm_v2_handle_io_trap(struct uml_pt_regs *regs,
			  struct kvm_run *run,
			  struct kvm_v2_vcpu *vcpu);

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
