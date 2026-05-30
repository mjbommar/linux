// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase E.1/E.2: IDT + handler stubs + GDT
 * install (E.1) + per-vCPU IST stacks + TSS bodies (E.2).
 *
 * Per memo 26 §E.1. Lands the data structures the guest needs to
 * deliver any exception at all: a real IDT (so KVM has a vector table
 * to dispatch through), a real GDT (so IDT-gate selector 0x08 walks
 * to a valid kernel-CS descriptor during exception delivery), and the
 * minimal handler stubs the IDT entries reference. Without these:
 *
 *   - guest #PF on first instruction fetch lands in the void (no IDT
 *     entry) and cascades to triple fault;
 *   - even with an IDT, the gate's segment selector field walks
 *     through the GDT, and a missing GDT base causes a #GP-during-
 *     delivery cascade that ends the same way.
 *
 * E.1 is the data layer; E.3.5's flip of .vcpu_run from seccomp to
 * kvm-v2 dispatch needs all of E.1 + E.2 (per-vCPU IST stacks + TSS
 * body) + E.3 (host-side dispatch arms for the exception-class IO
 * ports the handler stubs trap on) in place.
 *
 * Codex --search audit findings (every one folded in here):
 *
 *   #1 256-entry IDT fills an entire 4KB page. Three SEPARATE pages
 *      are required: IDT, handler stubs, GDT. v1's single-page
 *      layout (kvm-v1-archive/thread.c:664-726) only worked because
 *      v1's IDT held just 33 entries — v2's full 256 entries can't
 *      replicate that packing.
 *
 *   #2 GDT is required for IDT-gate selector resolution. The IDT
 *      entry's segment field holds 0x08 (kernel CS); during exception
 *      delivery the CPU walks that selector through the GDT base
 *      installed via SREGS. v1's bootstrap installed GDT alongside
 *      IDT/TSS at kvm-v1-archive/thread.c:1451 (kvm_setup_harness_gdt)
 *      + 1574-1590 (TSS descriptor in slots 6-7).
 *
 *   #3 #BP and #OF need DPL=3 so user code's `int3` and `into` reach
 *      their handlers (DPL=0 would #GP on those user-issued software
 *      interrupts). #PF / #GP / #UD / #DE stay DPL=0 (they're
 *      involuntary). v1 mirror: kvm-v1-archive/thread.c:1716-1751
 *      (INSTALL_IDT_GATE macro with explicit per-vector DPL).
 *
 *   #4 Handler stubs MUST NOT do `mov %cr2, %rax` — that read
 *      clobbers user RAX before the host's marshal-from-kvm-regs
 *      captures the vCPU state, leaving regs->gp[HOST_AX] = the
 *      clobbered cr2 instead of the user's pre-fault value. Host
 *      reads cr2 from kvm_run->s.regs.sregs.cr2 (sync-regs path
 *      enabled by KVM_SYNC_X86_SREGS at vcpu_create). v1 archive's
 *      RAX-preservation comment at kvm-v1-archive/thread.c:1297-1308
 *      documents the same trap. Stubs are minimal: 4 bytes
 *      (`out %al, $port ; iretq`).
 *
 *   #5 Pre-existing vCPUs (created at vcpu_create_one before E.1's
 *      pages exist) need a follow-up KVM_SET_SREGS to update
 *      idt.base / gdt.base after this helper runs. Option B from the
 *      spec: a separate kvm_v2_install_descriptors_sregs helper
 *      iterates the pool. (Option A would have folded
 *      idt/gdt updates into kvm_v2_install_production_sregs gated
 *      on vm->idt_kva != NULL — rejected to keep one-helper-per-
 *      concern symmetry with the existing CPUID / MSR / SREGS / sigmask
 *      install pattern in vcpu.c.)
 *
 * Allocation pattern: `__get_free_page(GFP_KERNEL | __GFP_ZERO)` per
 * page, matching D.4b's PT chain allocation pattern (syscall_trap.c:
 * 401-403). Pages live in physmem; their __pa() resolves through
 * D.4b-pre's physmem identity-offset memslot.
 *
 * No production caller today: .vcpu_run is still routed through
 * seccomp_vcpu_run (memo 26 §D.5 deferred the flip to Phase E
 * activation). E.1 is observable via dmesg + the
 * um_backend_kvm_v2_exception_install tracepoint at boot but the
 * data structures sit unused until E.3.5 flips the dispatcher.
 *
 * Phase E.2 (memo 26 §E.2) — added in this commit:
 *
 *   Per-vCPU IST stacks + TSS bodies. E.1 set IST=1 on every
 *   exception-delivery gate (kvm_v2_idt_set_gate's third arg) but
 *   left the TSS body's IST1 field unset; without E.2's per-vCPU
 *   TSS install, the first exception delivery would dereference a
 *   zero IST1 RSP and triple-fault. E.2 lands:
 *
 *     - kvm_v2_install_per_vcpu_ist_tss: allocates per-vCPU IST
 *       stack page (RW) + TSS page (RW) from buddy, builds the
 *       104-byte long-mode TSS body with IST1 = stack-top GVA,
 *       installs PTE entries at PTE[KVM_V2_IST_BASE_SLOT + cpu*2 ..
 *       +1] of trampoline_pte_kva, stashes vcpu->{ist_stack_*,
 *       tss_*}.
 *     - kvm_v2_gdt_write_tss_desc: writes a 16-byte long-mode TSS
 *       descriptor at GDT[6+7]. Pinned to vCPU 0's TSS body for
 *       concreteness; SREGS.tr (per-vCPU) overrides per dispatch.
 *     - kvm_v2_install_descriptors_sregs (extended): now also writes
 *       sregs.tr.{base=tss_gva, limit=103, selector=0x30, type=0xb,
 *       s=0, present=1, dpl=0, g=0, l=0, db=0} — per-vCPU TR cache
 *       authoritative for IST1 RSP lookup during exception delivery.
 *
 *   Codex --search audit (independent finding) verified:
 *
 *     CLAIM C: KVM_SET_TSS_ADDR (gpa=0xfffbd000, set at vm_create)
 *     is unrestricted-guest scaffolding — vestigial under our
 *     paged-from-vcpu-create flow. SREGS.tr (set per-vCPU) is the
 *     real TR cache; KVM_SET_TSS_ADDR is unrelated.
 *
 *     CLAIM D: 4KB IST stack is sufficient. Guest exception
 *     handlers do `out %al, $port ; iretq` only — no C call chain
 *     runs in guest. iretq frame (5 × 8 bytes) plus a possible
 *     8-byte error code is the entire stack budget; ~4080 bytes of
 *     headroom on a 4KB page.
 *
 *     Per-vCPU TSS REQUIRED (independent finding): IST1 differs per
 *     vCPU; sharing a TSS would either collision-risk two vCPUs
 *     pushing to the same physical stack or require dynamic IST1
 *     rewrites every dispatch (race-prone). Per-vCPU is the clean
 *     answer.
 *
 *   v1 archive references for E.2:
 *     - TSS body layout: kvm-v1-archive/thread.c:1615-1628 (offsets
 *       36 = IST1, 102 = io_bitmap_base).
 *     - TSS_DESC byte assembly: kvm-v1-archive/thread.c:1574-1590
 *       (16-byte long-mode descriptor spanning two GDT slots).
 *     - SREGS.tr programming: kvm-v1-archive/thread.c:2889-2898
 *       (sregs.tr = { base, limit=103, selector=0x30, type=11,
 *       present=1, dpl=0, s=0, g=0 }).
 */

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/threads.h>
#include <linux/types.h>

#include <asm/desc_defs.h>		/* gate_desc, GATE_INTERRUPT */
#include <asm/page.h>
#include <asm/processor.h>		/* task_size — Phase 5 gadget bounds check */
#include <asm/pgtable.h>		/* _PAGE_PRESENT, _PAGE_RW,
					 * _PAGE_ACCESSED, _PAGE_DIRTY */
#include <asm/trace/um_backend.h>

#include <os.h>				/* os_ioctl_generic */

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

/*
 * Handler stub byte sequences. Two shapes — vectors that push an
 * error code (#PF, #GP) need to pop it before iretq; vectors that
 * don't (#DE, #BP, #OF, #UD) iretq directly.
 *
 * Non-error-code shape (4 bytes):
 *   e6 XX           out %al, $XX     ; trap to host with port=XX
 *   48 cf           iretq            ; return to user, pop 5×8B frame
 *
 * Error-code shape (8 bytes):
 *   e6 XX           out %al, $XX     ; trap to host with port=XX
 *   48 83 c4 08     add $8, %rsp     ; advance past CPU-pushed error
 *                                     ; code (iretq pops 5×8B, NOT
 *                                     ; the error code — SDM §6.14.5)
 *   48 cf           iretq            ; return to user
 *
 * Stubs are laid out at 16-byte intervals within the handlers page
 * (`KVM_V2_HANDLERS_GVA + (slot << 4)`); 8-byte stubs fit comfortably
 * in the 16-byte slot. Trailing slot bytes are zero from __GFP_ZERO
 * (dead code — iretq exits before they execute).
 *
 * Codex audit finding #4: NO `mov %cr2, %rax` here. v1's archive
 * removed that opcode at kvm-v1-archive/thread.c:1297-1308 because
 * it clobbered user RAX before the host's GET_REGS captured vCPU
 * state. v2 reads CR2 via the sync-regs sregs.cr2 path (KVM populates
 * the mmap on every exit when KVM_SYNC_X86_SREGS is set in
 * kvm_valid_regs at vcpu_create — vcpu.c:631-632).
 *
 * E.5 fix: error-code stubs MUST `add $8, %rsp` before iretq.
 * Earlier (E.1) shape "out + iretq" without the add corrupted iretq's
 * pop sequence on #PF/#GP delivery: the CPU's pushed error_code
 * remained at the bottom of the stack frame, so iretq popped it as
 * RIP, popped the real RIP as CS, etc. Boot smoke under E.5 caught
 * it as `segfault at 10 ip 0x10 sp 0x10002 error 10` in a
 * single-RIP-stuck loop — RIP=0x10 is a #PF instruction-fetch error
 * code, RSP=0x10002 has bit-16 RF set (RFLAGS during fault delivery).
 * v1's archive at kvm-v1-archive/thread.c:1310-1314 had the explicit
 * `add $8, %rsp ; iretq` for the same reason.
 */
/*
 * #PF stub (#121 instrumentation, 2026-05-01): captures CR2 to a known
 * IST stack slot BEFORE the `out` vmexit, so handle_io_pf can read the
 * fault address from a non-volatile location instead of relying on
 * sregs.cr2 (which empirically reads 0 in some race conditions where
 * KVM/host activity between vmexit and store_regs clobbers
 * vcpu->arch.cr2).
 *
 * On entry to the stub, the CPU has already pushed the iretq frame +
 * error_code (5*8 + 8 = 48 bytes) to IST stack. RSP = ist_top - 48.
 * We use [rsp - 16] as a safe scratch slot (well below the iretq
 * frame, within the 4KB IST page).
 *
 * Bytes (18 bytes total — fits in the 32-byte slot we now use for #PF):
 *
 *   50               push %rax                  ; save user RAX (rsp-=8)
 *   0f 20 d0         mov %cr2, %rax             ; read fault address
 *   48 89 44 24 f8   mov %rax, -8(%rsp)         ; stash at ist_top - 64
 *   58               pop %rax                   ; restore user RAX
 *   e6 f6            out %al, $UM_KVM_TRAP_PF   ; vmexit
 *   48 83 c4 08      add $8, %rsp               ; skip CPU-pushed err_code
 *   48 cf            iretq                       ; resume user
 *
 * The captured CR2 lives at byte offset PAGE_SIZE - 64 of the IST page.
 * handle_io_pf reads it via vcpu->ist_stack_kva + (PAGE_SIZE - 64).
 */
static const u8 kvm_v2_handler_stub_pf[]    = {
	0x50,					/* push %rax */
	/* DIAG #121 sentinel: write 0xffffffffffffffff to ist_top-80 first
	 * so we can distinguish "stub never ran (slot is stale)" from
	 * "stub ran and captured CR2=0". */
	0x48, 0xc7, 0x44, 0x24, 0xe8, 0xff, 0xff, 0xff, 0xff,
						/* movq $-1, -24(%rsp) ; sentinel -> ist_top-80 */
	0x0f, 0x20, 0xd0,			/* mov %cr2, %rax */
	0x48, 0x89, 0x44, 0x24, 0xf8,		/* mov %rax, -8(%rsp)   ; CR2 -> ist_top-64 */
	0x48, 0x89, 0x54, 0x24, 0xf0,		/* mov %rdx, -16(%rsp)  ; RDX -> ist_top-72 (DIAG #121) */
	0x58,					/* pop %rax */
	0xe6, UM_KVM_TRAP_PF,			/* out %al, $port */
	0x48, 0x83, 0xc4, 0x08,			/* add $8, %rsp */
	0x48, 0xcf,				/* iretq */
};
static const u8 kvm_v2_handler_stub_gp[]    = {
	0xe6, UM_KVM_TRAP_GP,			/* out %al, $port */
	0x48, 0x83, 0xc4, 0x08,			/* add $8, %rsp */
	0x48, 0xcf,				/* iretq */
};
static const u8 kvm_v2_handler_stub_ud[]    = { 0xe6, UM_KVM_TRAP_UD,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_de[]    = { 0xe6, UM_KVM_TRAP_DE,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_of[]    = { 0xe6, UM_KVM_TRAP_OF,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_panic[] = { 0xe6, UM_KVM_TRAP_PANIC, 0x48, 0xcf };
static const u8 kvm_v2_handler_stub_db[]    = { 0xe6, UM_KVM_TRAP_DB,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_ss[]    = { 0xe6, UM_KVM_TRAP_SS,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_ac[]    = { 0xe6, UM_KVM_TRAP_AC,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_df[]    = { 0xe6, UM_KVM_TRAP_DF,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_ts[]    = { 0xe6, UM_KVM_TRAP_TS,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_np[]    = { 0xe6, UM_KVM_TRAP_NP,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_mf[]    = { 0xe6, UM_KVM_TRAP_MF,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_xm[]    = { 0xe6, UM_KVM_TRAP_XM,    0x48, 0xcf };

/*
 * #BP (INT3) stub: delivers SIGTRAP to the faulting process via the
 * host-side UM_KVM_TRAP_BP dispatch. No error code pushed by the CPU.
 * Reachable from ld.so's _dl_debug_state (INT3 marker), CPython's
 * faulthandler, and any user-space debugger interaction.
 */
static const u8 kvm_v2_handler_stub_bp[]    = { 0xe6, UM_KVM_TRAP_BP, 0x48, 0xcf };

/*
 * SMP-T22 (2026-05-02) — #NM (vec 7) handler stub: vmexit-on-fault.
 *
 *   e6 fd           out %al, $0xfd  ; vmexit with port = UM_KVM_TRAP_NM
 *   48 cf           iretq           ; UNREACHABLE — host marshals user
 *                                     state via SYNC_REGS and resumes
 *                                     at frame.user_rip directly
 *
 * History: prior to SMP-T22, the stub was `clts; iretq` — a fully
 * in-guest path. Under SMP-T13's per-host-CPU vCPU pool, multiple
 * UML tasks share each vCPU's IST page. The in-guest iretq pops
 * 5 qwords from IST top-40, transitioning to CPL=3. If anything
 * wrote a kernel-half RIP into IST top-40 between hardware push and
 * iretq pop (live-IST race), iretq lands user mode at a kernel-half
 * RIP — surfaces as the NM_stub+2 P=0 segfault (Bug B class).
 *
 * SMP-T17 closed the EINTR-mid-stub variant via inline #NM EINTR
 * handler; SMP-T19 closed software-side IST writes via sanity
 * guards. SMP-T22 closes the entire iretq-pops-bad-frame surface
 * by eliminating the in-guest iretq: stub vmexits, host clears
 * CR0.TS via sregs and resumes user at frame.user_rip directly via
 * SYNC_REGS marshaling (same flow as #PF/#GP/#UD/#DE/#OF).
 *
 * The iretq tail bytes (`48 cf`) are kept for stub-byte-length
 * symmetry with peer stubs; unreachable under correct operation.
 *
 * v1 archive precedent: kvm-v1-archive lazy-FPU model also exited
 * to host on #NM (thread.c around the FPU save/restore comments).
 *
 * Performance: ~5% of dispatches trip #NM per SMP-T13 empirics
 * (vcpu.c: "fpu_taken ≈ 5%, fpu_skipped ≈ 95%"). Adding one
 * vmexit per #NM is negligible; the cost is dwarfed by the
 * subsequent FPU instruction itself.
 *
 * Lazy-FPU coexistence: paired with arch_thread.kvm_v2.nm_ts_bypass
 * one-shot flag in load_user_sregs (vcpu.c). Without that flag, my
 * host's TS clear in handle_io_nm is undone before user retries the
 * FP instruction — infinite loop at boot.
 */
static const u8 kvm_v2_handler_stub_nm[]    = { 0xe6, UM_KVM_TRAP_NM, 0x48, 0xcf };

/*
 * GDT entries (long-mode, 8 bytes each — except TSS which is 16 bytes
 * spanning slots 6+7). Bytes lifted verbatim from v1's
 * kvm_setup_harness_gdt at kvm-v1-archive/sregs.c:102-138; per-bit
 * derivation per AMD64 SDM vol 3 §3.4.5 documented in v1's comment
 * block at sregs.c:81-101. Pre-set Accessed bit (A=1) on every used
 * descriptor — without it the CPU writes back A=1 on segment load,
 * which would fault on a read-only GDT page during exception delivery
 * (sregs.c:104-131 documents the same hazard).
 *
 * Slot map (also documented at syscall_trap.h KVM_V2_HANDLER_SLOT_*):
 *   GDT[0] = 0                          — null
 *   GDT[1] = 0x00af9b000000ffff         — kernel CS (sel 0x08, L=1, DPL=0, A=1)
 *   GDT[2] = 0x00cf93000000ffff         — kernel DS (sel 0x10, DPL=0, A=1)
 *   GDT[3] = 0                          — STAR-base padding (SYSRETQ
 *                                         architectural anchor; AMD64
 *                                         SDM vol 3 §6.1.1)
 *   GDT[4] = 0x00cff3000000ffff         — user DS (sel 0x23, DPL=3, A=1)
 *   GDT[5] = 0x00affb000000ffff         — user CS (sel 0x2b, L=1, DPL=3, A=1)
 *   GDT[6..7]                           — TSS descriptor; body filled in
 *                                         E.2 (16 bytes spanning slots 6+7)
 *
 * SYSRET selector encoding: STAR[63:48] = 0x0018 → SYSRET CS = 0x0018+0x10
 * = 0x0028 OR'd with RPL=3 = 0x002b (slot 5); SYSRET SS = 0x0018+0x08 =
 * 0x0020 OR'd with RPL=3 = 0x0023 (slot 4). Slots 0-2 are kernel
 * (sel 0x00/0x08/0x10), slot 3 is the SYSRET base padding, slots 4-5
 * are user (sel 0x23/0x2b), slots 6-7 are TSS.
 */
#define KVM_V2_GDT_NULL		0x0000000000000000ULL
#define KVM_V2_GDT_KERN_CS	0x00af9b000000ffffULL	/* sel 0x08 */
#define KVM_V2_GDT_KERN_DS	0x00cf93000000ffffULL	/* sel 0x10 */
#define KVM_V2_GDT_STAR_PAD	0x0000000000000000ULL	/* sel 0x18 anchor */
#define KVM_V2_GDT_USER_DS	0x00cff3000000ffffULL	/* sel 0x23 (DPL=3) */
#define KVM_V2_GDT_USER_CS	0x00affb000000ffffULL	/* sel 0x2b (DPL=3) */

/* Total GDT entries used today: 8 slots (slots 6+7 reserved for TSS,
 * body landing in E.2). limit = 8*8 - 1 = 63. */
#define KVM_V2_GDT_NR_SLOTS	8
#define KVM_V2_GDT_LIMIT	(KVM_V2_GDT_NR_SLOTS * 8 - 1)

/* IDT covers all 256 vectors; limit = 256*16 - 1 = 4095. Memo 26 §E.1
 * "KVM_SET_SREGS.idt.limit = 256*16 - 1". */
#define KVM_V2_IDT_NR_VECTORS	256
#define KVM_V2_IDT_LIMIT	(KVM_V2_IDT_NR_VECTORS * 16 - 1)

/*
 * Forward decl for the per-vCPU SREGS update issued at the tail of
 * exception_install. Definition lives in vcpu.c alongside
 * kvm_v2_install_production_sregs (one-helper-per-concern symmetry —
 * Option B from the spec).
 */
extern int kvm_v2_install_descriptors_sregs(struct kvm_v2_vm *vm,
					    struct kvm_v2_vcpu *vcpu);

static void kvm_v2_idt_set_gate(void *idt_page, unsigned int vector,
				u64 handler_gva, u8 dpl, u8 ist)
{
	gate_desc *gate = &((gate_desc *)idt_page)[vector];

	/*
	 * Long-mode interrupt gate layout (Intel SDM vol 3 §6.14.1; mirrored
	 * by the kernel's struct gate_struct at arch/x86/include/asm/desc_
	 * defs.h:134-143). v1's archive populated this same shape with raw
	 * byte writes at kvm-v1-archive/thread.c:1666-1714 (per-byte
	 * memcpy); v2 uses the kernel's typed struct so the field
	 * assignments compile-error if the layout ever shifts.
	 *
	 * Bits per the struct idt_bits at desc_defs.h:119-125:
	 *   ist  (low 3): IST stack index in TSS — E.2 fills IST[1] body;
	 *                 E.1 sets ist=1 here so the SREGS load already
	 *                 references the slot E.2 will populate.
	 *   zero (5):     reserved-must-be-zero
	 *   type (5):     0xE = long-mode interrupt gate (GATE_INTERRUPT)
	 *   dpl  (2):     descriptor privilege level — caller chooses
	 *                 (0 for involuntary; 3 for user-issued
	 *                 software-interrupt like int3 / into)
	 *   p    (1):     present
	 *
	 * segment = 0x08 (kernel CS). Resolved through the GDT at
	 * KVM_V2_GDT_GVA — codex audit #2 reason this commit had to land
	 * the GDT alongside the IDT.
	 */
	gate->offset_low    = (u16)(handler_gva & 0xffff);
	gate->segment       = 0x08;
	gate->bits.ist      = ist & 0x7;
	gate->bits.zero     = 0;
	gate->bits.type     = GATE_INTERRUPT;
	gate->bits.dpl      = dpl & 0x3;
	gate->bits.p        = 1;
	gate->offset_middle = (u16)((handler_gva >> 16) & 0xffff);
	gate->offset_high   = (u32)((handler_gva >> 32) & 0xffffffff);
	gate->reserved      = 0;
}

/*
 * Slot stride. Bumped from 16 to 32 bytes in #121 instrumentation so the
 * #PF stub (now 18 bytes with CR2-capture prologue) fits cleanly. All
 * other stubs are 4-8 bytes and waste the unused trailing slot bytes
 * (zero from __GFP_ZERO — dead code, never executed because iretq
 * exits before reaching them).
 */
#define KVM_V2_HANDLER_SLOT_STRIDE	64

static void kvm_v2_populate_handlers(void *handlers_kva)
{
	u8 *base = handlers_kva;

	memcpy(base + KVM_V2_HANDLER_SLOT_DE    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_de,    sizeof(kvm_v2_handler_stub_de));
	memcpy(base + KVM_V2_HANDLER_SLOT_BP    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_bp,    sizeof(kvm_v2_handler_stub_bp));
	memcpy(base + KVM_V2_HANDLER_SLOT_OF    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_of,    sizeof(kvm_v2_handler_stub_of));
	memcpy(base + KVM_V2_HANDLER_SLOT_UD    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_ud,    sizeof(kvm_v2_handler_stub_ud));
	memcpy(base + KVM_V2_HANDLER_SLOT_GP    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_gp,    sizeof(kvm_v2_handler_stub_gp));
	memcpy(base + KVM_V2_HANDLER_SLOT_PF    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_pf,    sizeof(kvm_v2_handler_stub_pf));
	memcpy(base + KVM_V2_HANDLER_SLOT_PANIC * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_panic, sizeof(kvm_v2_handler_stub_panic));
	memcpy(base + KVM_V2_HANDLER_SLOT_NM    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_nm,    sizeof(kvm_v2_handler_stub_nm));
	memcpy(base + KVM_V2_HANDLER_SLOT_DB    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_db,    sizeof(kvm_v2_handler_stub_db));
	memcpy(base + KVM_V2_HANDLER_SLOT_SS    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_ss,    sizeof(kvm_v2_handler_stub_ss));
	memcpy(base + KVM_V2_HANDLER_SLOT_AC    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_ac,    sizeof(kvm_v2_handler_stub_ac));
	memcpy(base + KVM_V2_HANDLER_SLOT_DF    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_df,    sizeof(kvm_v2_handler_stub_df));
	memcpy(base + KVM_V2_HANDLER_SLOT_TS    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_ts,    sizeof(kvm_v2_handler_stub_ts));
	memcpy(base + KVM_V2_HANDLER_SLOT_NP    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_np,    sizeof(kvm_v2_handler_stub_np));
	memcpy(base + KVM_V2_HANDLER_SLOT_MF    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_mf,    sizeof(kvm_v2_handler_stub_mf));
	memcpy(base + KVM_V2_HANDLER_SLOT_XM    * KVM_V2_HANDLER_SLOT_STRIDE,
	       kvm_v2_handler_stub_xm,    sizeof(kvm_v2_handler_stub_xm));
}

static void kvm_v2_populate_gdt(void *gdt_kva)
{
	u64 *gdt = gdt_kva;

	/*
	 * v1 reference: kvm-v1-archive/sregs.c:132-137 (
	 * kvm_setup_harness_gdt). Bytes verbatim — see header for per-
	 * field derivation. Slots 6+7 stay zero from __GFP_ZERO; E.2
	 * writes the TSS descriptor body there.
	 */
	gdt[0] = KVM_V2_GDT_NULL;
	gdt[1] = KVM_V2_GDT_KERN_CS;
	gdt[2] = KVM_V2_GDT_KERN_DS;
	gdt[3] = KVM_V2_GDT_STAR_PAD;
	gdt[4] = KVM_V2_GDT_USER_DS;
	gdt[5] = KVM_V2_GDT_USER_CS;
	/* gdt[6] / gdt[7] — TSS descriptor, filled in E.2. */
}

static void kvm_v2_populate_idt(void *idt_kva)
{
	unsigned int vec;
	const u64 panic_va = KVM_V2_HANDLERS_GVA +
				KVM_V2_HANDLER_SLOT_PANIC * KVM_V2_HANDLER_SLOT_STRIDE;

	/*
	 * Default every vector to the panic stub (DPL=0, IST=1). E.1's
	 * job is "unhandled vectors fail loudly"; E.3+ replaces specific
	 * gates with real dispatch as it lands. v1 left non-installed
	 * vectors as raw zero (kvm-v1-archive/thread.c:1662 memset(idt,
	 * 0, ...)) — but a zero gate has P=0, so any vector firing on
	 * such a gate raises #NP-during-delivery and cascades to #DF and
	 * triple fault. Routing to a panic stub gives the host an
	 * actionable port (UM_KVM_TRAP_PANIC = 0xf8) instead of a silent
	 * triple.
	 */
	for (vec = 0; vec < KVM_V2_IDT_NR_VECTORS; vec++)
		kvm_v2_idt_set_gate(idt_kva, vec, panic_va, 0, 1);

	/*
	 * Per-vector overrides — DPL per codex audit #3:
	 *
	 *   #DE (vec 0):  involuntary → DPL=0
	 *   #BP (vec 3):  user int3   → DPL=3
	 *   #OF (vec 4):  user into   → DPL=3
	 *   #UD (vec 6):  involuntary → DPL=0
	 *   #GP (vec 13): involuntary → DPL=0
	 *   #PF (vec 14): involuntary → DPL=0
	 *
	 * v1 mirror: kvm-v1-archive/thread.c:1716-1751 INSTALL_IDT_GATE
	 * macro with explicit per-vector DPL.
	 */
	kvm_v2_idt_set_gate(idt_kva, 0,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_DE * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 3,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_BP * KVM_V2_HANDLER_SLOT_STRIDE, 3, 1);
	kvm_v2_idt_set_gate(idt_kva, 4,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_OF * KVM_V2_HANDLER_SLOT_STRIDE, 3, 1);
	kvm_v2_idt_set_gate(idt_kva, 6,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_UD * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 13,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_GP * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 14,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_PF * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	/*
	 * Phase H.2: #NM (vec 7) — DPL=0 (involuntary). Stub does
	 * `clts; iretq` fully in-guest; no host vmexit. CR0.TS=0 after
	 * the stub allows the guest to retry the FP instruction.
	 */
	kvm_v2_idt_set_gate(idt_kva, 7,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_NM * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 1,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_DB * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 12,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_SS * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 17,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_AC * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 8,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_DF * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 10,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_TS * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 11,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_NP * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 16,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_MF * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 19,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_XM * KVM_V2_HANDLER_SLOT_STRIDE, 0, 1);
}

/*
 * Phase E.2 (memo 26 §E.2): write a 16-byte long-mode TSS descriptor
 * at GDT[slot]/[slot+1]. Long-mode TSS descriptors span TWO 8-byte
 * GDT entries (Intel SDM Vol.3 §7.2.3 / AMD64 Vol.2 §4.8.3) — slot
 * is the LOW half (carrying limit_low/base_low/type/DPL/P/limit_high/G/
 * base_mid), slot+1 is the HIGH half (carrying base_high in its low
 * 32 bits, reserved zero in the upper 32). v1 archive lays out the
 * same shape at kvm-v1-archive/thread.c:1574-1590; bytes verbatim
 * from there.
 *
 * Why this exists despite SREGS.tr being authoritative per-vCPU:
 *
 *   GDT is per-VM (one page covers the whole pool — rebuild on add/
 *   remove of vCPUs would race the live LTR cache anyway), so the
 *   GDT's TSS_DESC at slots 6+7 can only point at ONE vCPU's TSS.
 *   That's fine because SREGS.tr.{base,limit,selector,...} is the
 *   per-vCPU TR cache KVM loads into vmcs.GUEST_TR_*, overriding
 *   what the GDT entry would resolve to on a live LTR. So this GDT
 *   slot is mostly defensive — it stops a future SREGS validation
 *   change from rejecting selector=0x30 when GDT[0x30] is null.
 *
 *   We pin it to vCPU 0's TSS body for concreteness: vCPU 0 is
 *   guaranteed to be in the pool (nr_cpu_ids ≥ 1), and the per-vCPU
 *   SREGS.tr override will point each vCPU at its own TSS regardless.
 *   Match v1's pattern at kvm-v1-archive/thread.c:1574-1590 (single
 *   TSS_DESC even though v1 was effectively single-vCPU).
 */
static void kvm_v2_gdt_write_tss_desc(void *gdt_kva, unsigned int slot,
				      u64 tss_gva, u32 tss_limit)
{
	u64 *gdt = gdt_kva;
	u64 low;

	/*
	 * Low half (Intel SDM Vol.3 §7.2.3 / matches v1 at thread.c:
	 * 1581-1586):
	 *   bits 0-15  = limit[15:0]
	 *   bits 16-31 = base[15:0]
	 *   bits 32-39 = base[23:16]
	 *   bits 40-47 = type+S+DPL+P  (0x89 = type=9 available 64-bit
	 *                               TSS, S=0 system, DPL=0, P=1 —
	 *                               KVM flips type from 9→11 on
	 *                               LTR, but SREGS.tr.type=11 below
	 *                               is the per-vCPU authoritative
	 *                               value the cache holds anyway)
	 *   bits 48-51 = limit[19:16]   (= 0 for our 103-byte limit)
	 *   bits 52    = AVL = 0
	 *   bits 53    = reserved = 0
	 *   bits 54    = G = 0  (byte granularity — TSS limit is in
	 *                       bytes, not pages)
	 *   bits 55    = reserved = 0
	 *   bits 56-63 = base[31:24]
	 */
	low  = (u64)(tss_limit & 0xffff);
	low |= ((u64)(tss_gva & 0xffff)) << 16;
	low |= ((u64)((tss_gva >> 16) & 0xff)) << 32;
	low |= ((u64)0x89) << 40;
	low |= ((u64)((tss_limit >> 16) & 0xf)) << 48;
	low |= ((u64)((tss_gva >> 24) & 0xff)) << 56;

	gdt[slot]     = low;
	/*
	 * High half: low 32 bits = base[63:32]; upper 32 bits reserved
	 * (must be zero). v1 mirror at kvm-v1-archive/thread.c:1589.
	 */
	gdt[slot + 1] = (tss_gva >> 32) & 0xffffffffULL;
}

/*
 * Phase E.2 (memo 26 §E.2): build the long-mode TSS body in `tss_kva`.
 * Layout per Intel SDM Vol.3 §7.7 / kernel's `struct x86_hw_tss`
 * (arch/x86/include/asm/processor.h:313-332). 104 bytes total. We
 * hand-compute the offsets rather than declaring a struct here because
 * UML's <asm/processor.h> doesn't include the x86 hw-tss definition
 * (UML overrides arch/x86/um/asm/processor.h to its own layout); a
 * private struct here would risk drifting from the architectural one.
 *
 * v1 archive mirror: kvm-v1-archive/thread.c:1615-1628 — same byte
 * offsets, same "set ONLY ist[0] (= IST1) and io_bitmap_base" pattern.
 *
 * Offsets (see processor.h:313-332 — arch/x86 long-mode struct):
 *   0x00  u32 reserved1
 *   0x04  u64 sp0
 *   0x0c  u64 sp1
 *   0x14  u64 sp2
 *   0x1c  u64 reserved2
 *   0x24  u64 ist[0]   <-- IST1 (the one E.1's IDT entries reference)
 *   0x2c  u64 ist[1..6]
 *   0x5c  u32 reserved3
 *   0x60  u32 reserved4
 *   0x64  u16 reserved5
 *   0x66  u16 io_bitmap_base
 *
 * RSP0/1/2 stay zero — under our SYSCALL-not-INT model nothing
 * transitions through ring switches (CPL=3 user runs through SYSCALL
 * → kernel CPL=0 dispatch via LSTAR trampoline, never via INT/IRET
 * to ring 0; exception delivery uses IST stack). io_bitmap_base = 104
 * (= sizeof(tss)) parks the IO permission bitmap past the TSS limit
 * so any IO instruction at CPL > IOPL traps directly to #GP without
 * the CPU dereferencing a phantom bitmap.
 */
static void kvm_v2_populate_tss_body(void *tss_kva, u64 ist1_top_gva)
{
	u8 *t = tss_kva;

	memset(t, 0, KVM_V2_TSS_BODY_SIZE);
	*(u64 *)(t + 0x24) = ist1_top_gva;	/* IST1 RSP top */
	*(u16 *)(t + 0x66) = KVM_V2_TSS_BODY_SIZE;	/* IOPB off-of-limit */
}

int kvm_v2_install_per_vcpu_ist_tss(struct kvm_v2_vm *vm,
				    struct kvm_v2_vcpu *vcpu, int cpu)
{
	void *ist_stack_kva = NULL, *tss_kva = NULL;
	phys_addr_t ist_stack_gpa, tss_gpa;
	u64 ist_stack_top_gva, tss_gva;
	unsigned int ist_pte_idx, tss_pte_idx;
	u64 *pte_table;

	if (!vm || !vcpu || vcpu->vcpu_fd < 0)
		return -EINVAL;
	if (cpu < 0 || cpu >= NR_CPUS)
		return -EINVAL;
	if (!vm->trampoline_pte_kva || !vm->gdt_kva)
		return -EINVAL;

	/*
	 * Idempotent: re-running through the late-install path must not
	 * re-allocate (codex audit's spec line). The first successful
	 * install populates ist_stack_kva; further entries short-circuit.
	 */
	if (vcpu->ist_stack_kva)
		return 0;

	ist_stack_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	tss_kva       = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!ist_stack_kva || !tss_kva) {
		pr_err("um: kvm-v2 per_vcpu_ist_tss: __get_free_page returned NULL (cpu=%d)\n",
		       cpu);
		if (ist_stack_kva)
			free_page((unsigned long)ist_stack_kva);
		if (tss_kva)
			free_page((unsigned long)tss_kva);
		return -ENOMEM;
	}

	ist_stack_gpa = __pa(ist_stack_kva);
	tss_gpa       = __pa(tss_kva);
	/*
	 * Per-vCPU GVAs match the layout documented at syscall_trap.h:
	 *   IST_GVA(cpu) = TRAMPOLINE_GVA + (4 + cpu*2)     * 0x1000
	 *   TSS_GVA(cpu) = TRAMPOLINE_GVA + (4 + cpu*2 + 1) * 0x1000
	 * Top-of-stack = IST_GVA + PAGE_SIZE (stacks grow down — first
	 * push lands at top - 8). v1 mirror: kvm-v1-archive/thread.c:1623.
	 */
	ist_stack_top_gva = KVM_V2_IST_STACK_TOP_GVA(cpu);
	tss_gva           = KVM_V2_TSS_GVA(cpu);

	/*
	 * Build the TSS body BEFORE installing the PTE so KVM (and the
	 * guest CPU on first LTR/exception delivery) never observes a
	 * half-built TSS. IST1 = stack top GVA (exception delivery
	 * pushes iretq frame there).
	 */
	kvm_v2_populate_tss_body(tss_kva, ist_stack_top_gva);

	/*
	 * Install PTE entries. PTE indices follow the layout documented
	 * at syscall_trap.h: PTE[4 + cpu*2] = IST stack page,
	 * PTE[5 + cpu*2] = TSS page.
	 *
	 * Flags (matches D.4b's _KERNPG_TABLE pattern at syscall_trap.c:
	 * 427/431 — same UML-bit shape):
	 *   _PAGE_PRESENT — required for the walk to land at the leaf.
	 *   _PAGE_RW      — IST stack: CPU pushes iretq frame on
	 *                   exception delivery. TSS: defensive (the
	 *                   CPU writes the busy bit to the GDT
	 *                   TSS_DESC entry, NOT the TSS body itself,
	 *                   so the TSS page could be RO architecturally
	 *                   — but matching the IST flags reduces the
	 *                   number of distinct bit patterns in the chain
	 *                   and matches v1's "stack page = P|RW|NX"
	 *                   pattern at thread.c:1600).
	 *   _PAGE_ACCESSED — pre-set so the CPU doesn't write-back an
	 *                   A-bit update on first access.
	 *   _PAGE_DIRTY    — pre-set on the IST stack so the first push
	 *                   doesn't trigger a CPU write-back of the D
	 *                   bit. Same defense-in-depth as _PAGE_ACCESSED.
	 *
	 * Kernel-only (no _PAGE_USER) — guest CPL=3 has no business
	 * reading either page; it'll #PF if it tries.
	 *
	 * Note on the UML→x86 bit encoding: D66 in decisions-log.md
	 * documents that UML's PTE bits are software-only and don't
	 * match x86 architectural positions (UML _PAGE_RW=0x020 vs
	 * x86 R/W=0x002). D.4b chose to write UML bits anyway because
	 * the dispatch path is still seccomp (E.3.5 hasn't flipped
	 * .vcpu_run yet) so KVM's TDP never actually walks these
	 * PTEs — they sit dormant in the chain. E.2 mirrors that
	 * choice for consistency. If E.3.5's flip surfaces a #PF
	 * during exception delivery, an audit of PTE bit encodings
	 * across the chain (D.4b PUD/PMD/PTE + E.1 PTE[1..3] + E.2
	 * PTE[4 + cpu*2 .. +1]) is the first thing to check.
	 *
	 * Direct u64 stores match D.4b's PT chain pattern (syscall_trap.c:
	 * 442-443) — the PT entries aren't UML-managed pgtable structures
	 * so we don't route through set_pte.
	 */
	ist_pte_idx = KVM_V2_IST_BASE_SLOT + cpu * 2;
	tss_pte_idx = ist_pte_idx + 1;
	if (ist_pte_idx >= 512 || tss_pte_idx >= 512) {
		/* Defensive — NR_CPUS=64 max gives tss_pte_idx=131; far
		 * under 512. Catch a future bump that would overflow. */
		pr_err("um: kvm-v2 per_vcpu_ist_tss: PTE index overflow (cpu=%d ist_pte_idx=%u)\n",
		       cpu, ist_pte_idx);
		free_page((unsigned long)ist_stack_kva);
		free_page((unsigned long)tss_kva);
		return -EINVAL;
	}

	/*
	 * E.3.5 root-cause fix: x86 hardware bits, not UML software bits.
	 * P (0x1) | RW (0x2) | A (0x20) | D (0x40). UML _PAGE_DIRTY=0x100
	 * is bit 8 (ignored on leaf) but UML _PAGE_ACCESSED=0x080 is bit 7
	 * = PS-bit, which on a leaf marks 2MB page → silent map-corruption.
	 */
	#define V2_X86_P_LOCAL  (1ull << 0)
	#define V2_X86_RW_LOCAL (1ull << 1)
	#define V2_X86_A_LOCAL  (1ull << 5)
	#define V2_X86_D_LOCAL  (1ull << 6)
	#define V2_X86_LEAF_RW (V2_X86_P_LOCAL | V2_X86_RW_LOCAL | V2_X86_A_LOCAL | V2_X86_D_LOCAL)
	pte_table = (u64 *)vm->trampoline_pte_kva;
	pte_table[ist_pte_idx] = (u64)(ist_stack_gpa | V2_X86_LEAF_RW);
	pte_table[tss_pte_idx] = (u64)(tss_gpa       | V2_X86_LEAF_RW);

	/*
	 * Stash the kva/gpa/gva trio on the vcpu — the SREGS.tr install
	 * (kvm_v2_install_descriptors_sregs, called next from the
	 * exception_install loop) reads vcpu->tss_gva / ist_stack_top_gva
	 * to populate the per-vCPU TR cache. vm-lifetime; freed in
	 * kvm_v2_exception_free_per_vcpu before the IDT/handlers/GDT
	 * pages drop.
	 */
	vcpu->ist_stack_kva     = ist_stack_kva;
	vcpu->ist_stack_gpa     = ist_stack_gpa;
	vcpu->ist_stack_top_gva = ist_stack_top_gva;
	vcpu->tss_kva           = tss_kva;
	vcpu->tss_gpa           = tss_gpa;
	vcpu->tss_gva           = tss_gva;

	pr_info("um: kvm-v2 per_vcpu_ist_tss: cpu=%d vcpu_fd=%d ist_gpa=%pa ist_top_gva=%#llx tss_gpa=%pa tss_gva=%#llx\n",
		cpu, vcpu->vcpu_fd, &ist_stack_gpa,
		(unsigned long long)ist_stack_top_gva,
		&tss_gpa, (unsigned long long)tss_gva);
	trace_um_backend_kvm_v2_per_vcpu_ist_tss_install(vcpu->vcpu_fd,
							 ist_stack_top_gva,
							 tss_gva);
	return 0;
}

/*
 * Phase H gadget Phase 2 (2026-05-04): per-vCPU gadget state page
 * install. Mirrors kvm_v2_install_per_vcpu_ist_tss above — same
 * trampoline_pte_kva chain, slot KVM_V2_GADGET_BASE_SLOT + cpu,
 * same x86 leaf bit pattern (P|RW|A|D, kernel-only).
 *
 * The page is RW because the host CPU pthread writes the gadget
 * fields (tgid etc.) on every dispatch in load_user_sregs. The
 * guest-side gadget reads via `mov %gs:OFFSET, %eax` (no writes
 * from CPL=0 trampoline code; the gadget never stores to %gs:).
 *
 * Per-vCPU isolation is the whole point of Phase 2 — see syscall_trap.h
 * KVM_V2_GADGET_STATE_GVA documentation for the design rationale.
 */
int kvm_v2_install_per_vcpu_gadget_state(struct kvm_v2_vm *vm,
					 struct kvm_v2_vcpu *vcpu, int cpu)
{
	void *kva;
	phys_addr_t gpa;
	u64 gva;
	unsigned int pte_idx;
	u64 *pte_table;

	if (!vm || !vcpu || vcpu->vcpu_fd < 0)
		return -EINVAL;
	if (cpu < 0 || cpu >= NR_CPUS)
		return -EINVAL;
	if (!vm->trampoline_pte_kva)
		return -EINVAL;

	if (vcpu->gadget_state_kva)
		return 0;	/* idempotent */

	/*
	 * CONFIG_UM_BACKEND_KVM_V2_GADGET=n: skip per-vCPU state page
	 * allocation entirely. With gadget_state_kva left NULL, the
	 * refresh hook in kvm_v2_load_user_sregs naturally short-
	 * circuits, and the LSTAR stays at the 5-byte fallback
	 * (kvm_v2_trampoline_upgrade_to_gadget is also skipped at the
	 * caller site for the =n case). SYSCALLs route through the
	 * standard KVM_EXIT_IO → handle_io_trap → handle_syscall path.
	 */
	if (!IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_GADGET))
		return 0;

	kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!kva) {
		pr_err("um: kvm-v2 per_vcpu_gadget_state: __get_free_page returned NULL (cpu=%d)\n",
		       cpu);
		return -ENOMEM;
	}
	gpa = __pa(kva);
	gva = KVM_V2_GADGET_STATE_GVA(cpu);

	pte_idx = KVM_V2_GADGET_BASE_SLOT + cpu;
	if (pte_idx >= 512) {
		/* Defensive — NR_CPUS=64 worst case gives last index 195;
		 * far under 512. Catch a future NR_CPUS bump. */
		pr_err("um: kvm-v2 per_vcpu_gadget_state: PTE index overflow (cpu=%d pte_idx=%u)\n",
		       cpu, pte_idx);
		free_page((unsigned long)kva);
		return -EINVAL;
	}

	/*
	 * Same x86 hardware bit pattern as IST/TSS PTEs above:
	 *   P (1<<0) | RW (1<<1) | A (1<<5) | D (1<<6).
	 * The leaf bits are deliberately the same shape as IST/TSS so
	 * a future audit of the chain only has to verify one pattern.
	 * NOT _PAGE_USER — the gadget runs at CPL=0 (in the LSTAR
	 * trampoline body); CPL=3 user code has no business reading
	 * its own task's tgid via this side channel.
	 */
	pte_table = (u64 *)vm->trampoline_pte_kva;
	pte_table[pte_idx] = (u64)(gpa |
				   (1ULL << 0) | (1ULL << 1) |
				   (1ULL << 5) | (1ULL << 6));

	vcpu->gadget_state_kva = kva;
	vcpu->gadget_state_gpa = gpa;
	vcpu->gadget_state_gva = gva;

	/*
	 * Phase H gadget Phase 5 (2026-05-04): seed TASK_SIZE_CAP once at
	 * install. v1 reference: kvm-v1-archive/lifecycle.c:954-980 — same
	 * "task_size doesn't change after boot, so set once" rationale, same
	 * 16-byte safety margin (so an 8-byte store at task_size_cap-N
	 * stays in user-half even if N rounds down). Used by the getcpu
	 * gadget body's `cmp %rdi, %gs:OFF_TASK_SIZE_CAP ; jbe → fallback`
	 * pre-store bounds check on the user-supplied cpu and node pointers.
	 *
	 * task_size is set in arch/um/kernel/um_arch.c::linux_main_after_args
	 * before init_backend completes; by exception_install (subsys_initcall)
	 * it's stable. Read directly without locking.
	 *
	 * The page is __GFP_ZERO'd above so all other gadget fields stay 0
	 * until kvm_v2_load_user_sregs's per-dispatch refresh writes them.
	 * TASK_SIZE_CAP is the only "set once at install" field — everything
	 * else (TGID, TID, PPID, UID, EUID, GID, EGID, CPU_ID) is per-task
	 * or per-vCPU and refreshed before every KVM_RUN.
	 */
	WRITE_ONCE(*(u64 *)((u8 *)kva + KVM_V2_GADGET_OFF_TASK_SIZE_CAP),
		   (u64)task_size - 16);

#ifdef CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH
	/*
	 * h_aperfmperf gate.  The gadget body checks this byte BEFORE
	 * issuing rdmsr 0xE7/0xE8; with the byte clear it falls back
	 * to the host trap path, avoiding the #GP injection KVM would
	 * deliver when the disable-exits cap isn't set.
	 *
	 * The predicate is set globally by vm_create when the
	 * KVM_ENABLE_CAP ioctl succeeds.  We use kvm_v2_aperfmperf_enabled()
	 * as a near-proxy: it's true iff the operator requested the
	 * cap.  If KVM later rejected the cap (host lacks the feature),
	 * the toggle would still read true but the rdmsr would #GP --
	 * so we also gate on the ioctl_attempted/ioctl_rc record from
	 * aperfmperf.c via kvm_v2_aperfmperf_cap_active().
	 */
	WRITE_ONCE(*((u8 *)kva + KVM_V2_GADGET_OFF_APERF_CAP),
		   kvm_v2_aperfmperf_cap_active() ? 1 : 0);
#endif

	pr_info("um: kvm-v2 per_vcpu_gadget_state: cpu=%d vcpu_fd=%d gpa=%pa gva=%#llx (pte_idx=%u, task_size_cap=%#llx)\n",
		cpu, vcpu->vcpu_fd, &gpa, (unsigned long long)gva, pte_idx,
		(unsigned long long)((u64)task_size - 16));
	return 0;
}

void kvm_v2_exception_free_per_vcpu_gadget_state(struct kvm_v2_vm *vm,
						 struct kvm_v2_vcpu *vcpu,
						 int cpu)
{
	unsigned int pte_idx;
	u64 *pte_table;

	if (!vm || !vcpu)
		return;
	if (cpu < 0 || cpu >= NR_CPUS)
		return;
	if (!vcpu->gadget_state_kva)
		return;

	pte_idx = KVM_V2_GADGET_BASE_SLOT + cpu;
	if (vm->trampoline_pte_kva && pte_idx < 512) {
		pte_table = (u64 *)vm->trampoline_pte_kva;
		pte_table[pte_idx] = 0;
	}

	free_page((unsigned long)vcpu->gadget_state_kva);
	vcpu->gadget_state_kva = NULL;
	vcpu->gadget_state_gpa = 0;
	vcpu->gadget_state_gva = 0;
}

void kvm_v2_exception_free_per_vcpu(struct kvm_v2_vm *vm,
				    struct kvm_v2_vcpu *vcpu, int cpu)
{
	unsigned int ist_pte_idx, tss_pte_idx;
	u64 *pte_table;

	if (!vm || !vcpu)
		return;
	if (cpu < 0 || cpu >= NR_CPUS)
		return;
	if (!vcpu->ist_stack_kva)
		return;

	/*
	 * Clear PTE entries BEFORE freeing the pages — same ordering
	 * concern as kvm_v2_exception_free's PTE[1..3] clear (a
	 * concurrent guest walk seeing zero == not present is fine; one
	 * seeing a stale GPA pointing at released memory is not). The
	 * trampoline_pte_kva chain is owned by D.4b and freed by
	 * kvm_v2_kernel_half_free; this helper MUST run before that one.
	 */
	ist_pte_idx = KVM_V2_IST_BASE_SLOT + cpu * 2;
	tss_pte_idx = ist_pte_idx + 1;
	if (vm->trampoline_pte_kva && ist_pte_idx < 512 && tss_pte_idx < 512) {
		pte_table = (u64 *)vm->trampoline_pte_kva;
		pte_table[ist_pte_idx] = 0;
		pte_table[tss_pte_idx] = 0;
	}

	free_page((unsigned long)vcpu->ist_stack_kva);
	free_page((unsigned long)vcpu->tss_kva);

	vcpu->ist_stack_kva     = NULL;
	vcpu->ist_stack_gpa     = 0;
	vcpu->ist_stack_top_gva = 0;
	vcpu->tss_kva           = NULL;
	vcpu->tss_gpa           = 0;
	vcpu->tss_gva           = 0;
}

int kvm_v2_exception_install(struct kvm_v2_vm *vm)
{
	void *idt_kva = NULL, *handlers_kva = NULL, *gdt_kva = NULL;
	phys_addr_t idt_gpa, handlers_gpa, gdt_gpa;
	u64 *pte_table;
	int cpu, rc;

	if (!vm)
		return -EINVAL;

	/*
	 * Idempotent short-circuit: a successful prior install means the
	 * three KVAs are non-NULL, the PT entries are populated, and the
	 * pool members already have idt/gdt installed via SREGS. Re-running
	 * would leak the old pages and re-issue redundant ioctls.
	 */
	if (vm->idt_kva)
		return 0;

	/*
	 * D.4b's PT chain must be installed first — we install entries at
	 * PTE[1..3] of the same chain trampoline_pte_kva owns. Without the
	 * chain there's nowhere to plug into. The chain install is reachable
	 * from the same subsys_initcall path (syscall_trap.c:
	 * kvm_v2_trampoline_late_install runs trampoline alloc → kernel-half
	 * install → exception_install in order), so this prerequisite is
	 * satisfied by the time we run in production. Defensive check
	 * anyway in case a future caller invokes us out-of-order.
	 */
	if (!vm->trampoline_pte_kva) {
		pr_err("um: kvm-v2 exception_install: D.4b PT chain not installed (trampoline_pte_kva=NULL); E.1 must run after D.4b\n");
		return -EINVAL;
	}

	/*
	 * Three pages from buddy. Each goes in physmem (alloc_page →
	 * page_address → __pa = offset in [0, physmem_size)) so the GPAs
	 * resolve through D.4b-pre's physmem identity-offset memslot.
	 * GFP_KERNEL because we run from subsys_initcall (process context);
	 * __GFP_ZERO so unwritten regions are deterministic.
	 *
	 * Allocation pattern matches D.4b's PT chain alloc at
	 * syscall_trap.c:401-403: __get_free_page (not alloc_page +
	 * page_address) for symmetry with the existing pgtable-style
	 * pages already in the chain.
	 */
	idt_kva      = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	handlers_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	gdt_kva      = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!idt_kva || !handlers_kva || !gdt_kva) {
		pr_err("um: kvm-v2 exception_install: __get_free_page returned NULL (buddy not up?)\n");
		rc = -ENOMEM;
		goto err_free_pages;
	}

	idt_gpa      = __pa(idt_kva);
	handlers_gpa = __pa(handlers_kva);
	gdt_gpa      = __pa(gdt_kva);

	/*
	 * Populate the three pages BEFORE installing PT entries so KVM
	 * never observes a half-built layout. Order: handlers first
	 * (IDT entries reference handler GVAs), GDT second (independent),
	 * IDT last (references the handler GVAs we just placed).
	 */
	kvm_v2_populate_handlers(handlers_kva);

	/* Verify GP stub bytes immediately after populate */
	{
		u8 *gp = (u8 *)handlers_kva + KVM_V2_HANDLER_SLOT_GP * KVM_V2_HANDLER_SLOT_STRIDE;

		pr_info("um: kvm-v2 handler_verify: GP stub @+0x%x = [%02x %02x %02x %02x %02x %02x %02x %02x] (expect e6 f9 48 83 c4 08 48 cf)\n",
			KVM_V2_HANDLER_SLOT_GP * KVM_V2_HANDLER_SLOT_STRIDE,
			gp[0], gp[1], gp[2], gp[3], gp[4], gp[5], gp[6], gp[7]);
	}

	kvm_v2_populate_gdt(gdt_kva);
	kvm_v2_populate_idt(idt_kva);

	/*
	 * Install PT entries at PTE[1..3] of the trampoline PT chain
	 * D.4b built. PTE[0] is the trampoline (D.4b set it); PTE[1..3]
	 * are E.1's pages. The kernel-half walk is the same: any guest
	 * CR3 → PML4[448] → PUD[0] → PMD[0] → PTE[idx] → page.
	 *
	 * Leaf flags: P|A only. RO (no _PAGE_RW) — IDT/handlers are code-
	 * adjacent data the guest never writes; GDT is read-only after
	 * install (E.2 writes the TSS descriptor body — that touches the
	 * page from the host side, not the guest). Kernel-only (no
	 * _PAGE_USER) — guest CPL=3 walking these GVAs gets a #PF, not a
	 * successful read. Executable is implicit on UML (no _PAGE_NX
	 * defined in arch/um/include/asm/pgtable.h) — matters for the
	 * handlers page; harmless for IDT/GDT (CPU never executes from
	 * those during normal flow).
	 *
	 * Direct u64 stores match D.4b's pattern (syscall_trap.c:
	 * 442-443) — the PT entries aren't UML-managed pgtable structures
	 * so we don't route through set_pte (which adds NEEDSYNC tracking
	 * the guest TDP walker doesn't understand).
	 */
	/*
	 * E.3.5 root-cause fix (2026-04-29): write x86 hardware bits, not
	 * UML's software-only bits — see syscall_trap.c::kvm_v2_kernel_
	 * half_install for the full rationale. UML _PAGE_ACCESSED=0x080
	 * = x86 PS-bit on a leaf would silently mark it as a 2MB page;
	 * use x86 A-bit (0x20) here.
	 */
	#define V2_X86_LEAF_RO ((1ull << 0) | (1ull << 5))
	pte_table = (u64 *)vm->trampoline_pte_kva;
	pte_table[1] = (u64)(idt_gpa      | V2_X86_LEAF_RO);
	pte_table[2] = (u64)(handlers_gpa | V2_X86_LEAF_RO);
	pte_table[3] = (u64)(gdt_gpa      | V2_X86_LEAF_RO);

	/*
	 * Stash on the VM struct BEFORE the per-vCPU SREGS update —
	 * install_descriptors_sregs reads vm->idt_kva to gate its work
	 * (the helper is a no-op on a still-NULL idt_kva so it can be
	 * called blindly from kvm_v2_install_production_sregs in a
	 * future Phase E refactor without a separate gate).
	 */
	vm->idt_kva       = idt_kva;
	vm->idt_gpa       = idt_gpa;
	vm->handlers_kva  = handlers_kva;
	vm->handlers_gpa  = handlers_gpa;

	/*
	 * Write-protect the handler page from the host side so KVM's
	 * EPT marks the GPA non-writable. The guest PTE at PTE[2] is
	 * already RO, but the physmem memslot provides a second
	 * writable path to the same physical page. A stale or
	 * corrupted GS_BASE during the LSTAR gadget can cause the
	 * gadget's save block to write through the physmem path,
	 * overwriting handler stub bytes (observed: GP stub port
	 * byte 0xf9 → 0xf8). Host-side mprotect propagates through
	 * KVM's mmu_notifier → EPT write-protect, closing this path.
	 */
	os_protect_memory(handlers_kva, PAGE_SIZE, 1, 0, 1); /* r-x */
	vm->gdt_kva       = gdt_kva;
	vm->gdt_gpa       = gdt_gpa;

	/*
	 * Codex audit #5: the SREGS install at vcpu_create_one
	 * (kvm_v2_install_production_sregs) ran before E.1's pages
	 * existed, so its sregs.idt / sregs.gdt fields stayed at the
	 * KVM_GET_SREGS defaults (zero base, zero limit). Re-issue
	 * KVM_SET_SREGS for every existing pool member with the new
	 * idt/gdt VAs — without this, .vcpu_run flip in E.3.5 would
	 * see the descriptor-table bases as zero and any guest
	 * exception would fail at IDT-base-fetch time.
	 *
	 * iterate NR_CPUS — kvm_v2_vcpu_get returns NULL for slots
	 * whose vcpu_fd is < 0 (uninitialised or torn down), so the
	 * loop naturally bounds itself to populated entries.
	 */
	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		struct kvm_v2_vcpu *v = kvm_v2_vcpu_get(cpu);

		if (!v)
			continue;

		/*
		 * E.2: per-vCPU IST stack + TSS pages must land BEFORE
		 * install_descriptors_sregs — the latter writes
		 * sregs.tr.{base,limit} from vcpu->tss_gva (Phase E.2
		 * extension), which install_per_vcpu_ist_tss is what
		 * populates. Order matters: TSS body → SREGS.tr cache.
		 */
		rc = kvm_v2_install_per_vcpu_ist_tss(vm, v, cpu);
		if (rc < 0) {
			pr_err("um: kvm-v2 exception_install: install_per_vcpu_ist_tss(cpu=%d) failed (%d)\n",
			       cpu, rc);
			goto err_unwind_per_vcpu;
		}

		/*
		 * Phase H gadget Phase 2 (2026-05-04): per-vCPU gadget
		 * state page. Slot KVM_V2_GADGET_BASE_SLOT + cpu in the
		 * same trampoline_pte_kva chain as IST/TSS. Independent
		 * of descriptor SREGS — installs unconditionally so the
		 * LSTAR getpid gadget always has a backing page (the LSTAR
		 * bytes themselves are written by trampoline_install
		 * unconditionally as of Phase 2).
		 *
		 * Failure here unwinds via err_unwind_per_vcpu, which
		 * iterates the pool calling exception_free_per_vcpu —
		 * extended to also call the gadget-state free helper so
		 * any partially-installed state pages are reclaimed.
		 */
		rc = kvm_v2_install_per_vcpu_gadget_state(vm, v, cpu);
		if (rc < 0) {
			pr_err("um: kvm-v2 exception_install: install_per_vcpu_gadget_state(cpu=%d) failed (%d)\n",
			       cpu, rc);
			goto err_unwind_per_vcpu;
		}

		rc = kvm_v2_install_descriptors_sregs(vm, v);
		if (rc < 0) {
			pr_err("um: kvm-v2 exception_install: install_descriptors_sregs(cpu=%d) failed (%d)\n",
			       cpu, rc);
			goto err_unwind_per_vcpu;
		}
	}

	/*
	 * E.2: write the TSS_DESC into GDT slots 6+7 AFTER per-vCPU
	 * install so we can point at vCPU 0's TSS body (concrete base —
	 * defensive in case a future SREGS validation tightens up and
	 * rejects selector=0x30 when GDT[0x30] is null). SREGS.tr
	 * (per-vCPU, set above by install_descriptors_sregs) is the
	 * authoritative TR cache; the GDT slot is the lookup the CPU
	 * would hit if SREGS.tr's cached values somehow expired and an
	 * LTR re-fetch happened (KVM doesn't do this in practice, but
	 * matching v1's archive shape at thread.c:1574-1590 keeps the
	 * substrate symmetric with v1).
	 *
	 * Pin to vCPU 0's TSS — the per-vCPU SREGS.tr override means
	 * each vCPU runs against its own TSS regardless. If vCPU 0
	 * isn't in the pool (impossible at boot — nr_cpu_ids ≥ 1) we
	 * leave slots 6+7 zero (the E.1 zero-init); SREGS validation
	 * would still pass because the per-vCPU TR cache is the value
	 * KVM checks, not the GDT walk.
	 */
	{
		struct kvm_v2_vcpu *v0 = kvm_v2_vcpu_get(0);

		if (v0 && v0->tss_gva) {
			kvm_v2_gdt_write_tss_desc(gdt_kva, 6,
						  v0->tss_gva,
						  KVM_V2_TSS_LIMIT);
			pr_info("um: kvm-v2 exception_install: GDT TSS_DESC slot=6 base=%#llx limit=%u (vCPU 0 TSS — per-vCPU SREGS.tr overrides)\n",
				(unsigned long long)v0->tss_gva,
				KVM_V2_TSS_LIMIT);
		}
	}

	pr_info("um: kvm-v2 exception_install: idt_gpa=%pa idt_gva=%#llx handlers_gpa=%pa handlers_gva=%#llx gdt_gpa=%pa gdt_gva=%#llx (%u IDT vectors / %u GDT slots)\n",
		&idt_gpa,      (u64)KVM_V2_IDT_GVA,
		&handlers_gpa, (u64)KVM_V2_HANDLERS_GVA,
		&gdt_gpa,      (u64)KVM_V2_GDT_GVA,
		KVM_V2_IDT_NR_VECTORS, KVM_V2_GDT_NR_SLOTS);

	trace_um_backend_kvm_v2_exception_install((u64)idt_gpa,
						  (u64)handlers_gpa,
						  (u64)gdt_gpa);

	/*
	 * Phase H gadget Phase 3 (2026-05-04): atomic two-phase LSTAR
	 * install. Every per-vCPU gadget state page is now mapped (we
	 * just succeeded the loop above without taking err_unwind_per_vcpu),
	 * so it's safe to upgrade the LSTAR body from the 5-byte fallback
	 * to the 94-byte stay-in-guest gadget. Failure here is treated as
	 * an upgrade-skip (LSTAR stays at the safe fallback) rather than
	 * a hard error: gadget unavailability is a perf regression, not a
	 * correctness break — every SYSCALL still works via the slow
	 * KVM_EXIT_IO path.
	 */
	if (IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_GADGET)) {
		int upg_rc = kvm_v2_trampoline_upgrade_to_gadget(vm);

		if (upg_rc < 0)
			pr_warn("um: kvm-v2 exception_install: trampoline_upgrade_to_gadget failed (%d) — LSTAR stays at 5-byte fallback (slow path); per-vCPU state pages installed but gadget body not enabled\n",
				upg_rc);
	} else {
		pr_info("um: kvm-v2 exception_install: CONFIG_UM_BACKEND_KVM_V2_GADGET=n — gadget disabled, LSTAR stays at 5-byte fallback (KVM_EXIT_IO slow path)\n");
	}

	return 0;

err_unwind_per_vcpu:
	/*
	 * E.2: a per-vCPU install (or the descriptor SREGS for a vcpu we
	 * already installed IST/TSS for) failed mid-pool. Walk back over
	 * every pool member that had its pages allocated and tear them
	 * down — kvm_v2_exception_free_per_vcpu and the Phase-2 gadget-
	 * state free helper are no-ops on never-installed entries, so we
	 * can iterate the whole pool blindly without tracking which
	 * entries are partially live.
	 */
	{
		int c;

		for (c = 0; c < NR_CPUS; c++) {
			struct kvm_v2_vcpu *v = kvm_v2_vcpu_get(c);

			if (v) {
				kvm_v2_exception_free_per_vcpu_gadget_state(vm, v, c);
				kvm_v2_exception_free_per_vcpu(vm, v, c);
			}
		}
	}
	/* fallthrough */
err_unwind_pt:
	/* Failed mid-pool — clear PT entries and free the pages. The
	 * VM struct's idt_kva/etc. have been written; revert so any
	 * future re-entry treats us as never-installed. */
	pte_table[1] = 0;
	pte_table[2] = 0;
	pte_table[3] = 0;
	vm->idt_kva       = NULL;
	vm->idt_gpa       = 0;
	vm->handlers_kva  = NULL;
	vm->handlers_gpa  = 0;
	vm->gdt_kva       = NULL;
	vm->gdt_gpa       = 0;
err_free_pages:
	if (idt_kva)
		free_page((unsigned long)idt_kva);
	if (handlers_kva)
		free_page((unsigned long)handlers_kva);
	if (gdt_kva)
		free_page((unsigned long)gdt_kva);
	return rc;
}

void kvm_v2_exception_free(struct kvm_v2_vm *vm)
{
	u64 *pte_table;
	int cpu;

	if (!vm)
		return;

	/*
	 * E.2: free per-vCPU IST stack + TSS pages BEFORE the IDT/handlers/
	 * GDT pages drop. Each kvm_v2_exception_free_per_vcpu clears its
	 * own PTE pair (PTE[KVM_V2_IST_BASE_SLOT + cpu*2 .. +1]) and frees
	 * the page; safe on a never-installed vcpu (NULL ist_stack_kva
	 * short-circuit). We iterate the pool unconditionally — even if
	 * vm->idt_kva is NULL (E.1 never landed), some vCPU might still
	 * have IST/TSS pages live (defensive — shouldn't happen given the
	 * install order, but covers a future caller invoking out-of-order).
	 */
	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		struct kvm_v2_vcpu *v = kvm_v2_vcpu_get(cpu);

		if (v) {
			kvm_v2_exception_free_per_vcpu_gadget_state(vm, v, cpu);
			kvm_v2_exception_free_per_vcpu(vm, v, cpu);
		}
	}

	if (!vm->idt_kva)
		return;

	/*
	 * Clear PTE[1..3] BEFORE freeing the pages so any concurrent
	 * mm operation sees zero (= not present) rather than dangling.
	 * At vm_destroy time on shutdown no mms should be operating —
	 * defensive ordering matches D.4b's free path at syscall_trap.c
	 * (kvm_v2_kernel_half_free).
	 *
	 * Caller responsibility: this helper MUST run before
	 * kvm_v2_kernel_half_free, because pte_table is part of the
	 * D.4b chain — once kernel_half_free runs, pte_table is freed
	 * memory and the writes below would touch released pages.
	 * Documented at the helper's prototype in syscall_trap.h.
	 */
	if (vm->trampoline_pte_kva) {
		pte_table = (u64 *)vm->trampoline_pte_kva;
		pte_table[1] = 0;
		pte_table[2] = 0;
		pte_table[3] = 0;
	}

	free_page((unsigned long)vm->idt_kva);
	free_page((unsigned long)vm->handlers_kva);
	free_page((unsigned long)vm->gdt_kva);

	vm->idt_kva       = NULL;
	vm->idt_gpa       = 0;
	vm->handlers_kva  = NULL;
	vm->handlers_gpa  = 0;
	vm->gdt_kva       = NULL;
	vm->gdt_gpa       = 0;
}
