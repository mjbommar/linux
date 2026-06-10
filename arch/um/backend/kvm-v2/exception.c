// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend exception tables and per-vCPU descriptor state.
 *
 * This file builds the guest-visible descriptor substrate used when KVM
 * delivers exceptions while UML user code is running in a vCPU:
 *
 *   - a full 256-entry IDT;
 *   - a handler-stub page whose stubs exit to the host through I/O ports;
 *   - a GDT with kernel/user segments plus a long-mode TSS descriptor;
 *   - per-vCPU IST stacks and TSS bodies;
 *   - descriptor SREGS updates for existing vCPUs.
 *
 * The IDT, handler stubs, and GDT occupy separate pages. A full IDT is
 * already one 4KB page, and exception delivery also needs a valid GDT
 * because each IDT gate resolves selector 0x08 through the guest GDT.
 *
 * Every IDT entry uses IST1. Each vCPU therefore gets its own IST stack
 * and TSS body so two vCPUs never share the hardware exception stack.
 * The per-vCPU SREGS.tr cache is authoritative for exception delivery;
 * the GDT TSS descriptor remains valid so selector 0x30 is never backed
 * by a null descriptor.
 *
 * Handler stubs are intentionally small. Most of them execute
 * out %al, $port and then iretq; stubs for vectors with an error
 * code discard that error code before returning. The #PF stub captures
 * CR2 in an IST scratch slot before the vmexit, preserving user RAX
 * across the host-side register marshal.
 */

#include <linux/bits.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/pgtable.h>
#include <linux/printk.h>
#include <linux/processor.h>
#include <linux/string.h>
#include <linux/threads.h>
#include <linux/types.h>

#include <asm/desc_defs.h>		/* gate_desc, GATE_INTERRUPT */
#include <asm/page.h>
#include <asm/trace/um_backend.h>

#include <os.h>				/* os_ioctl_generic */

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

/*
 * Handler stub byte sequences. Two shapes: vectors that push an
 * error code (#PF, #GP) need to pop it before iretq; vectors that
 * don't (#DE, #BP, #OF, #UD) iretq directly.
 *
 * Non-error-code shape (4 bytes):
 *   e6 XX           out %al, $XX     ; trap to host with port=XX
 *   48 cf           iretq            ; return to user, pop 5*8B frame
 *
 * Error-code shape (8 bytes):
 *   e6 XX           out %al, $XX     ; trap to host with port=XX
 *   48 83 c4 08     add $8, %rsp     ; advance past CPU-pushed error
 *                                     ; code (iretq pops 5*8B, not
 *                                     ; the error code)
 *   48 cf           iretq            ; return to user
 *
 * Stubs are laid out at KVM_V2_HANDLER_SLOT_STRIDE-byte intervals within
 * the handlers page
 * (KVM_V2_HANDLERS_GVA + slot * KVM_V2_HANDLER_SLOT_STRIDE).
 * Trailing slot bytes are zero from __GFP_ZERO and are never reached.
 *
 * Error-code stubs must add $8, %rsp before iretq. The CPU-pushed
 * error code sits below the iret frame and iretq does not consume it.
 */
/*
 * #PF captures CR2 to a known IST stack slot before the out vmexit, so
 * handle_io_pf can read the fault address from a non-volatile location.
 *
 * On entry to the stub, the CPU has already pushed the iretq frame +
 * error_code (5*8 + 8 = 48 bytes) to IST stack. RSP = ist_top - 48.
 * [rsp - 16] is a safe scratch slot below the iretq frame and within
 * the 4KB IST page.
 *
 * Bytes (32 bytes total; fits in the handler slot):
 *
 *   50               push %rax                  ; save user RAX (rsp-=8)
 *   48 c7 44 24 e8.. movq $-1, -24(%rsp)        ; sentinel
 *   0f 20 d0         mov %cr2, %rax             ; read fault address
 *   48 89 44 24 f8   mov %rax, -8(%rsp)         ; stash at ist_top - 64
 *   48 89 54 24 f0   mov %rdx, -16(%rsp)        ; stash at ist_top - 72
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
	/*
	 * Store a sentinel at ist_top-80 before CR2 capture. The host
	 * uses it to distinguish an unused slot from CR2 == 0.
	 */
	0x48, 0xc7, 0x44, 0x24, 0xe8, 0xff, 0xff, 0xff, 0xff,
						/* movq $-1, -24(%rsp) ; sentinel -> ist_top-80 */
	0x0f, 0x20, 0xd0,			/* mov %cr2, %rax */
	0x48, 0x89, 0x44, 0x24, 0xf8,		/* mov %rax, -8(%rsp)   ; CR2 -> ist_top-64 */
	0x48, 0x89, 0x54, 0x24, 0xf0,		/* mov %rdx, -16(%rsp)  ; RDX -> ist_top-72 */
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
 * host-side UM_KVM_TRAP_BP dispatch. No error code is pushed by the
 * CPU; user-space debuggers and runtime breakpoint hooks reach this
 * path.
 */
static const u8 kvm_v2_handler_stub_bp[]    = { 0xe6, UM_KVM_TRAP_BP, 0x48, 0xcf };

/*
 * #NM (vec 7) handler stub: vmexit-on-fault.
 *
 *   e6 fd           out %al, $0xfd  ; vmexit with port = UM_KVM_TRAP_NM
 *   48 cf           iretq           ; not reached; host marshals user
 *                                     state via SYNC_REGS and resumes
 *                                     at frame.user_rip directly
 *
 * The stub exits to the host instead of clearing CR0.TS and returning
 * in guest. The host clears TS through SREGS and resumes at the user
 * RIP from the hardware frame, matching the other exception traps.
 *
 * The iretq tail bytes (48 cf) are kept for stub-byte-length
 * symmetry with peer stubs; unreachable under correct operation.
 *
 * Lazy-FPU coexistence: paired with arch_thread.kvm_v2.nm_ts_bypass
 * one-shot flag in load_user_sregs (vcpu.c). Without that flag, the
 * host-side TS clear in handle_io_nm is undone before user retries the
 * FP instruction, causing an infinite loop at boot.
 */
static const u8 kvm_v2_handler_stub_nm[]    = { 0xe6, UM_KVM_TRAP_NM, 0x48, 0xcf };

/*
 * GDT entries (long-mode, 8 bytes each except TSS, which is 16 bytes
 * spanning slots 6+7). Pre-set Accessed bit (A=1) on every used
 * descriptor. Without it the CPU writes back A=1 on segment load,
 * which would fault on a read-only GDT page during exception delivery.
 *
 * Slot map (also documented at syscall_trap.h KVM_V2_HANDLER_SLOT_*):
 *   GDT[0] = 0                          - null
 *   GDT[1] = 0x00af9b000000ffff         - kernel CS (sel 0x08, L=1, DPL=0, A=1)
 *   GDT[2] = 0x00cf93000000ffff         - kernel DS (sel 0x10, DPL=0, A=1)
 *   GDT[3] = 0                          - STAR-base padding (SYSRETQ
 *                                         architectural anchor; AMD64
 *                                         SDM vol 3 section 6.1.1)
 *   GDT[4] = 0x00cff3000000ffff         - user DS (sel 0x23, DPL=3, A=1)
 *   GDT[5] = 0x00affb000000ffff         - user CS (sel 0x2b, L=1, DPL=3, A=1)
 *   GDT[6..7]                           - TSS descriptor
 *
 * SYSRET selector encoding: STAR[63:48] = 0x0018; SYSRET CS = 0x0018+0x10
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

/*
 * Hardware x86 PTE bits used in the private trampoline page-table chain.
 * UML's software pgtable bit definitions are not suitable for these guest
 * leaf entries.
 */
#define KVM_V2_X86_PTE_PRESENT	BIT_ULL(0)
#define KVM_V2_X86_PTE_RW	BIT_ULL(1)
#define KVM_V2_X86_PTE_ACCESSED	BIT_ULL(5)
#define KVM_V2_X86_PTE_DIRTY	BIT_ULL(6)
#define KVM_V2_X86_LEAF_RO	(KVM_V2_X86_PTE_PRESENT | \
				 KVM_V2_X86_PTE_ACCESSED)
#define KVM_V2_X86_LEAF_RW	(KVM_V2_X86_PTE_PRESENT | \
				 KVM_V2_X86_PTE_RW | \
				 KVM_V2_X86_PTE_ACCESSED | \
				 KVM_V2_X86_PTE_DIRTY)

static void kvm_v2_idt_set_gate(void *idt_page, unsigned int vector,
				u64 handler_gva, u8 dpl, u8 ist)
{
	gate_desc *gate = &((gate_desc *)idt_page)[vector];

	/*
	 * Long-mode interrupt gate layout, mirrored by the kernel's
	 * struct gate_struct in arch/x86/include/asm/desc_defs.h. Use the
	 * kernel's typed struct so the field
	 * assignments compile-error if the layout ever shifts.
	 *
	 * Long-mode interrupt-gate bitfields:
	 *   ist  (low 3): IST stack index in TSS. All KVM v2 gates use
	 *                 IST1, backed by a per-vCPU TSS body.
	 *   zero (5):     reserved-must-be-zero
	 *   type (5):     0xE = long-mode interrupt gate (GATE_INTERRUPT)
	 *   dpl  (2):     descriptor privilege level; caller chooses
	 *                 (0 for involuntary; 3 for user-issued
	 *                 software-interrupt like int3 / into)
	 *   p    (1):     present
	 *
	 * The gate selector is kernel CS, resolved through the guest GDT at
	 * KVM_V2_GDT_GVA during exception delivery.
	 */
	gate->offset_low    = (u16)(handler_gva & 0xffff);
	gate->segment       = KVM_V2_KERNEL_CS_SEL;
	gate->bits.ist      = ist & 0x7;
	gate->bits.zero     = 0;
	gate->bits.type     = GATE_INTERRUPT;
	gate->bits.dpl      = dpl & 0x3;
	gate->bits.p        = 1;
	gate->offset_middle = (u16)((handler_gva >> 16) & 0xffff);
	gate->offset_high   = (u32)((handler_gva >> 32) & 0xffffffff);
	gate->reserved      = 0;
}

static u64 kvm_v2_handler_va(unsigned int slot)
{
	return KVM_V2_HANDLERS_GVA + slot * KVM_V2_HANDLER_SLOT_STRIDE;
}

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

	gdt[0] = KVM_V2_GDT_NULL;
	gdt[1] = KVM_V2_GDT_KERN_CS;
	gdt[2] = KVM_V2_GDT_KERN_DS;
	gdt[3] = KVM_V2_GDT_STAR_PAD;
	gdt[4] = KVM_V2_GDT_USER_DS;
	gdt[5] = KVM_V2_GDT_USER_CS;
	/* gdt[6] / gdt[7] get the TSS descriptor after TSS allocation. */
}

static void kvm_v2_idt_set_default_gates(void *idt_kva)
{
	unsigned int vec;
	const u64 panic_va = kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_PANIC);

	/*
	 * Default every vector to the panic stub (DPL=0, IST=1). A zero
	 * gate is not present, so an unexpected vector would cascade
	 * through delivery faults. Routing to a panic stub gives the host
	 * an actionable port instead of a silent triple fault.
	 */
	for (vec = 0; vec < KVM_V2_IDT_NR_VECTORS; vec++)
		kvm_v2_idt_set_gate(idt_kva, vec, panic_va, 0, 1);
}

static void kvm_v2_idt_set_user_exception_gates(void *idt_kva)
{
	/*
	 * Per-vector overrides. #BP and #OF need DPL=3 because user code
	 * may issue int3/into directly; involuntary exceptions stay DPL=0.
	 *
	 *   #DE (vec 0):  involuntary -> DPL=0
	 *   #BP (vec 3):  user int3   -> DPL=3
	 *   #OF (vec 4):  user into   -> DPL=3
	 *   #UD (vec 6):  involuntary -> DPL=0
	 *   #GP (vec 13): involuntary -> DPL=0
	 *   #PF (vec 14): involuntary -> DPL=0
	 */
	kvm_v2_idt_set_gate(idt_kva, 0,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_DE), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 3,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_BP), 3, 1);
	kvm_v2_idt_set_gate(idt_kva, 4,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_OF), 3, 1);
	kvm_v2_idt_set_gate(idt_kva, 6,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_UD), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 13,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_GP), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 14,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_PF), 0, 1);
}

static void kvm_v2_idt_set_runtime_exception_gates(void *idt_kva)
{
	/* #NM is involuntary and exits to the host for lazy-FPU handling. */
	kvm_v2_idt_set_gate(idt_kva, 7,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_NM), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 1,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_DB), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 12,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_SS), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 17,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_AC), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 8,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_DF), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 10,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_TS), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 11,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_NP), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 16,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_MF), 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 19,
			    kvm_v2_handler_va(KVM_V2_HANDLER_SLOT_XM), 0, 1);
}

static void kvm_v2_populate_idt(void *idt_kva)
{
	kvm_v2_idt_set_default_gates(idt_kva);
	kvm_v2_idt_set_user_exception_gates(idt_kva);
	kvm_v2_idt_set_runtime_exception_gates(idt_kva);
}

/*
 * Write a 16-byte long-mode TSS descriptor at GDT[slot]/[slot+1].
 * Long-mode TSS descriptors span two 8-byte GDT entries: slot is the
 * low half and slot+1 is the high half carrying base[63:32].
 *
 * SREGS.tr is authoritative per vCPU. The shared GDT TSS descriptor is still
 * populated so selector 0x30 resolves to a valid descriptor during SREGS
 * validation. It points at vCPU 0's TSS body; each live vCPU overrides the
 * cached TR base/limit through its own SREGS.tr state.
 */
static void kvm_v2_gdt_write_tss_desc(void *gdt_kva, unsigned int slot,
				      u64 tss_gva, u32 tss_limit)
{
	u64 *gdt = gdt_kva;
	u64 low;

	/*
	 * Low half:
	 *   bits 0-15  = limit[15:0]
	 *   bits 16-31 = base[15:0]
	 *   bits 32-39 = base[23:16]
	 *   bits 40-47 = type+S+DPL+P  (0x89 = type=9 available 64-bit
	 *                               TSS, S=0 system, DPL=0, P=1.
	 *                               KVM flips type from 9 to 11 on
	 *                               LTR, but SREGS.tr.type=11 below
	 *                               is the per-vCPU authoritative
	 *                               value the cache holds anyway)
	 *   bits 48-51 = limit[19:16]   (= 0 for the 103-byte limit)
	 *   bits 52    = AVL = 0
	 *   bits 53    = reserved = 0
	 *   bits 54    = G = 0  (byte granularity; TSS limit is in
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
	 * High half: low 32 bits = base[63:32]; upper 32 bits are
	 * reserved and must be zero.
	 */
	gdt[slot + 1] = (tss_gva >> 32) & 0xffffffffULL;
}

/*
 * Build the long-mode TSS body in tss_kva. The architectural body is
 * 104 bytes. The offsets are written directly because UML's
 * <asm/processor.h> does not expose the x86 hardware TSS layout.
 *
 * Offsets:
 *   0x00  u32 reserved1
 *   0x04  u64 sp0
 *   0x0c  u64 sp1
 *   0x14  u64 sp2
 *   0x1c  u64 reserved2
 *   0x24  u64 ist[0]   <-- IST1
 *   0x2c  u64 ist[1..6]
 *   0x5c  u32 reserved3
 *   0x60  u32 reserved4
 *   0x64  u16 reserved5
 *   0x66  u16 io_bitmap_base
 *
 * RSP0/1/2 stay zero: the SYSCALL-not-INT model does not transition
 * through ring switches (CPL=3 user runs through SYSCALL
 * to kernel CPL=0 dispatch via LSTAR trampoline, never via INT/IRET
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

static void kvm_v2_free_ist_tss_pages(void *ist_stack_kva, void *tss_kva)
{
	if (ist_stack_kva)
		free_page((unsigned long)ist_stack_kva);
	if (tss_kva)
		free_page((unsigned long)tss_kva);
}

static int kvm_v2_alloc_ist_tss_pages(void **ist_stack_kva, void **tss_kva,
				      int cpu)
{
	*ist_stack_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	*tss_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!*ist_stack_kva || !*tss_kva) {
		pr_err("um: kvm-v2 per_vcpu_ist_tss: __get_free_page returned NULL (cpu=%d)\n",
		       cpu);
		kvm_v2_free_ist_tss_pages(*ist_stack_kva, *tss_kva);
		*ist_stack_kva = NULL;
		*tss_kva = NULL;
		return -ENOMEM;
	}

	return 0;
}

static int kvm_v2_install_ist_tss_ptes(struct kvm_v2_vm *vm, int cpu,
				       phys_addr_t ist_stack_gpa,
				       phys_addr_t tss_gpa)
{
	unsigned int ist_pte_idx, tss_pte_idx;
	u64 *pte_table;

	/*
	 * Install PTE entries. PTE indices follow the layout documented
	 * at syscall_trap.h: PTE[4 + cpu*2] = IST stack page,
	 * PTE[5 + cpu*2] = TSS page.
	 */
	ist_pte_idx = KVM_V2_IST_BASE_SLOT + cpu * 2;
	tss_pte_idx = ist_pte_idx + 1;
	if (ist_pte_idx >= PTRS_PER_PTE || tss_pte_idx >= PTRS_PER_PTE) {
		pr_err("um: kvm-v2 per_vcpu_ist_tss: PTE index overflow (cpu=%d ist_pte_idx=%u)\n",
		       cpu, ist_pte_idx);
		return -EINVAL;
	}

	pte_table = (u64 *)vm->trampoline_pte_kva;
	pte_table[ist_pte_idx] = (u64)(ist_stack_gpa | KVM_V2_X86_LEAF_RW);
	pte_table[tss_pte_idx] = (u64)(tss_gpa       | KVM_V2_X86_LEAF_RW);
	return 0;
}

static void kvm_v2_record_ist_tss_state(struct kvm_v2_vcpu *vcpu,
					void *ist_stack_kva,
					phys_addr_t ist_stack_gpa,
					u64 ist_stack_top_gva,
					void *tss_kva, phys_addr_t tss_gpa,
					u64 tss_gva)
{
	/*
	 * Stash the kva/gpa/gva trio on the vcpu. The SREGS.tr install reads
	 * vcpu->tss_gva / ist_stack_top_gva to populate the per-vCPU TR cache.
	 */
	vcpu->ist_stack_kva     = ist_stack_kva;
	vcpu->ist_stack_gpa     = ist_stack_gpa;
	vcpu->ist_stack_top_gva = ist_stack_top_gva;
	vcpu->tss_kva           = tss_kva;
	vcpu->tss_gpa           = tss_gpa;
	vcpu->tss_gva           = tss_gva;
}

struct kvm_v2_ist_tss_layout {
	void *ist_stack_kva;
	void *tss_kva;
	phys_addr_t ist_stack_gpa;
	phys_addr_t tss_gpa;
	u64 ist_stack_top_gva;
	u64 tss_gva;
};

static int kvm_v2_validate_ist_tss_install(struct kvm_v2_vm *vm,
					   struct kvm_v2_vcpu *vcpu,
					   int cpu)
{
	if (!vm || !vcpu || vcpu->vcpu_fd < 0)
		return -EINVAL;
	if (cpu < 0 || cpu >= KVM_V2_MAX_VCPUS)
		return -EINVAL;
	if (!vm->trampoline_pte_kva || !vm->gdt_kva)
		return -EINVAL;

	return 0;
}

static void kvm_v2_init_ist_tss_layout(struct kvm_v2_ist_tss_layout *layout,
				       void *ist_stack_kva, void *tss_kva,
				       int cpu)
{
	layout->ist_stack_kva = ist_stack_kva;
	layout->tss_kva = tss_kva;
	layout->ist_stack_gpa = __pa(ist_stack_kva);
	layout->tss_gpa = __pa(tss_kva);

	/*
	 * Per-vCPU GVAs match the layout documented at syscall_trap.h:
	 *   IST_GVA(cpu) = TRAMPOLINE_GVA + (4 + cpu*2)     * 0x1000
	 *   TSS_GVA(cpu) = TRAMPOLINE_GVA + (4 + cpu*2 + 1) * 0x1000
	 * Top-of-stack = IST_GVA + PAGE_SIZE. Stacks grow down; the first
	 * push lands at top - 8.
	 */
	layout->ist_stack_top_gva = KVM_V2_IST_STACK_TOP_GVA(cpu);
	layout->tss_gva = KVM_V2_TSS_GVA(cpu);
}

static int kvm_v2_publish_ist_tss_layout(struct kvm_v2_vm *vm,
					 struct kvm_v2_vcpu *vcpu, int cpu,
					 struct kvm_v2_ist_tss_layout *layout)
{
	int rc;

	/*
	 * Build the TSS body before installing the PTE so KVM (and the guest
	 * CPU on first LTR/exception delivery) never observes a half-built
	 * TSS. IST1 = stack top GVA (exception delivery pushes iretq frame
	 * there).
	 */
	kvm_v2_populate_tss_body(layout->tss_kva, layout->ist_stack_top_gva);

	rc = kvm_v2_install_ist_tss_ptes(vm, cpu, layout->ist_stack_gpa,
					 layout->tss_gpa);
	if (rc)
		return rc;

	kvm_v2_record_ist_tss_state(vcpu, layout->ist_stack_kva,
				    layout->ist_stack_gpa,
				    layout->ist_stack_top_gva,
				    layout->tss_kva, layout->tss_gpa,
				    layout->tss_gva);

	pr_debug("um: kvm-v2 per_vcpu_ist_tss: cpu=%d vcpu_fd=%d ist_gpa=%pa ist_top_gva=%#llx tss_gpa=%pa tss_gva=%#llx\n",
		 cpu, vcpu->vcpu_fd, &layout->ist_stack_gpa,
		 (unsigned long long)layout->ist_stack_top_gva,
		 &layout->tss_gpa, (unsigned long long)layout->tss_gva);
	trace_um_backend_kvm_v2_per_vcpu_ist_tss_install(vcpu->vcpu_fd,
							 layout->ist_stack_top_gva,
							 layout->tss_gva);
	return 0;
}

int kvm_v2_install_per_vcpu_ist_tss(struct kvm_v2_vm *vm,
				    struct kvm_v2_vcpu *vcpu, int cpu)
{
	void *ist_stack_kva = NULL, *tss_kva = NULL;
	struct kvm_v2_ist_tss_layout layout;
	int rc;

	rc = kvm_v2_validate_ist_tss_install(vm, vcpu, cpu);
	if (rc)
		return rc;

	/*
	 * Idempotent: re-running through the late-install path must not
	 * re-allocate. The first successful install populates ist_stack_kva;
	 * further entries short-circuit.
	 */
	if (vcpu->ist_stack_kva)
		return 0;

	rc = kvm_v2_alloc_ist_tss_pages(&ist_stack_kva, &tss_kva, cpu);
	if (rc)
		return rc;

	kvm_v2_init_ist_tss_layout(&layout, ist_stack_kva, tss_kva, cpu);
	rc = kvm_v2_publish_ist_tss_layout(vm, vcpu, cpu, &layout);
	if (rc) {
		kvm_v2_free_ist_tss_pages(ist_stack_kva, tss_kva);
		return rc;
	}

	return 0;
}

static int kvm_v2_install_gadget_state_pte(struct kvm_v2_vm *vm, int cpu,
					   phys_addr_t gpa)
{
	unsigned int pte_idx = KVM_V2_GADGET_BASE_SLOT + cpu;
	u64 *pte_table;

	if (pte_idx >= PTRS_PER_PTE) {
		pr_err("um: kvm-v2 per_vcpu_gadget_state: PTE index overflow (cpu=%d pte_idx=%u)\n",
		       cpu, pte_idx);
		return -EINVAL;
	}

	pte_table = (u64 *)vm->trampoline_pte_kva;
	pte_table[pte_idx] = (u64)(gpa | KVM_V2_X86_LEAF_RW);
	return 0;
}

static void kvm_v2_seed_gadget_state_page(void *kva)
{
	/*
	 * Seed TASK_SIZE_CAP once at install. task_size does not change after
	 * boot, and the 16-byte margin keeps an 8-byte store at task_size_cap-N
	 * in user-half even if N rounds down.
	 *
	 * The page is __GFP_ZERO'd, so all other gadget fields stay zero until
	 * kvm_v2_load_user_sregs() refreshes them before KVM_RUN.
	 */
	WRITE_ONCE(*(u64 *)((u8 *)kva + KVM_V2_GADGET_OFF_TASK_SIZE_CAP),
		   (u64)task_size - 16);

#ifdef CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH
	/*
	 * The gadget body checks this byte before issuing rdmsr 0xE7/0xE8.
	 * A clear byte keeps the path on the host trap fallback.
	 */
	WRITE_ONCE(*((u8 *)kva + KVM_V2_GADGET_OFF_APERF_CAP),
		   kvm_v2_aperfmperf_cap_active() ? 1 : 0);
#endif
}

static void kvm_v2_record_gadget_state(struct kvm_v2_vcpu *vcpu, void *kva,
				       phys_addr_t gpa, u64 gva)
{
	vcpu->gadget_state_kva = kva;
	vcpu->gadget_state_gpa = gpa;
	vcpu->gadget_state_gva = gva;
}

/*
 * Install the per-vCPU gadget state page. The page uses the same
 * trampoline_pte_kva chain as IST/TSS, at slot
 * KVM_V2_GADGET_BASE_SLOT + cpu, with the same x86 leaf bit pattern
 * (P|RW|A|D, kernel-only).
 *
 * The page is RW because the host CPU pthread writes the gadget
 * fields (tgid etc.) on every dispatch in load_user_sregs. The
 * guest-side gadget reads via mov %gs:OFFSET, %eax (no writes
 * from CPL=0 trampoline code; the gadget never stores to %gs:).
 *
 * Per-vCPU isolation is required; see syscall_trap.h
 * KVM_V2_GADGET_STATE_GVA documentation for the design rationale.
 */
int kvm_v2_install_per_vcpu_gadget_state(struct kvm_v2_vm *vm,
					 struct kvm_v2_vcpu *vcpu, int cpu)
{
	void *kva;
	phys_addr_t gpa;
	u64 gva;
	int rc;

	if (!vm || !vcpu || vcpu->vcpu_fd < 0)
		return -EINVAL;
	if (cpu < 0 || cpu >= KVM_V2_MAX_VCPUS)
		return -EINVAL;
	if (!vm->trampoline_pte_kva)
		return -EINVAL;

	if (vcpu->gadget_state_kva)
		return 0;	/* idempotent */

	/*
	 * CONFIG_UM_BACKEND_KVM_V2_GADGET=n leaves gadget_state_kva NULL.
	 * The refresh hook in kvm_v2_load_user_sregs then returns without
	 * touching gadget state, and LSTAR remains on the 5-byte IO trap
	 * path.
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

	rc = kvm_v2_install_gadget_state_pte(vm, cpu, gpa);
	if (rc) {
		free_page((unsigned long)kva);
		return rc;
	}

	kvm_v2_record_gadget_state(vcpu, kva, gpa, gva);
	kvm_v2_seed_gadget_state_page(kva);

	pr_debug("um: kvm-v2 per_vcpu_gadget_state: cpu=%d vcpu_fd=%d gpa=%pa gva=%#llx task_size_cap=%#llx\n",
		 cpu, vcpu->vcpu_fd, &gpa, (unsigned long long)gva,
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
	if (cpu < 0 || cpu >= KVM_V2_MAX_VCPUS)
		return;
	if (!vcpu->gadget_state_kva)
		return;

	pte_idx = KVM_V2_GADGET_BASE_SLOT + cpu;
	if (vm->trampoline_pte_kva && pte_idx < PTRS_PER_PTE) {
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
	if (cpu < 0 || cpu >= KVM_V2_MAX_VCPUS)
		return;
	if (!vcpu->ist_stack_kva)
		return;

	/*
	 * Clear PTE entries before freeing the pages. A concurrent guest walk
	 * may see a not-present PTE, but must not see a dangling GPA. The
	 * trampoline_pte_kva chain belongs to the kernel-half map, so this
	 * helper must run before kvm_v2_kernel_half_free().
	 */
	ist_pte_idx = KVM_V2_IST_BASE_SLOT + cpu * 2;
	tss_pte_idx = ist_pte_idx + 1;
	if (vm->trampoline_pte_kva &&
	    ist_pte_idx < PTRS_PER_PTE && tss_pte_idx < PTRS_PER_PTE) {
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

static void kvm_v2_exception_free_pages(void *idt_kva, void *handlers_kva,
					void *gdt_kva)
{
	if (idt_kva)
		free_page((unsigned long)idt_kva);
	if (handlers_kva)
		free_page((unsigned long)handlers_kva);
	if (gdt_kva)
		free_page((unsigned long)gdt_kva);
}

static int kvm_v2_exception_alloc_pages(void **idt_kva, void **handlers_kva,
					void **gdt_kva)
{
	/*
	 * Three pages from buddy. Each goes in physmem (alloc_page ->
	 * page_address -> __pa) so the GPAs resolve through the physmem
	 * identity-offset memslot. __GFP_ZERO keeps unwritten regions
	 * deterministic.
	 */
	*idt_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	*handlers_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	*gdt_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!*idt_kva || !*handlers_kva || !*gdt_kva) {
		pr_err("um: kvm-v2 exception_install: __get_free_page returned NULL (buddy not up?)\n");
		kvm_v2_exception_free_pages(*idt_kva, *handlers_kva,
					    *gdt_kva);
		*idt_kva = NULL;
		*handlers_kva = NULL;
		*gdt_kva = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void kvm_v2_exception_populate_pages(void *idt_kva, void *handlers_kva,
					    void *gdt_kva)
{
	/*
	 * Populate the three pages before installing PT entries so KVM never
	 * observes a half-built layout. Order: handlers first (IDT entries
	 * reference handler GVAs), GDT second (independent), IDT last
	 * (references the handler GVAs placed above).
	 */
	kvm_v2_populate_handlers(handlers_kva);
	kvm_v2_populate_gdt(gdt_kva);
	kvm_v2_populate_idt(idt_kva);
}

static void kvm_v2_exception_install_ptes(struct kvm_v2_vm *vm,
					  phys_addr_t idt_gpa,
					  phys_addr_t handlers_gpa,
					  phys_addr_t gdt_gpa)
{
	u64 *pte_table = (u64 *)vm->trampoline_pte_kva;

	/*
	 * Install PT entries at PTE[1..3] of the trampoline PT chain. PTE[0]
	 * is the trampoline; PTE[1..3] are the descriptor pages. Leaf flags are
	 * P|A only: read-only, supervisor-only, and directly encoded as x86
	 * hardware bits because these entries are not UML-managed pgtable
	 * structures.
	 */
	pte_table[1] = (u64)(idt_gpa      | KVM_V2_X86_LEAF_RO);
	pte_table[2] = (u64)(handlers_gpa | KVM_V2_X86_LEAF_RO);
	pte_table[3] = (u64)(gdt_gpa      | KVM_V2_X86_LEAF_RO);
}

static void kvm_v2_exception_clear_vm_state(struct kvm_v2_vm *vm)
{
	if (vm->trampoline_pte_kva) {
		u64 *pte_table = (u64 *)vm->trampoline_pte_kva;

		pte_table[1] = 0;
		pte_table[2] = 0;
		pte_table[3] = 0;
	}

	vm->idt_kva       = NULL;
	vm->idt_gpa       = 0;
	vm->handlers_kva  = NULL;
	vm->handlers_gpa  = 0;
	vm->gdt_kva       = NULL;
	vm->gdt_gpa       = 0;
}

static void kvm_v2_exception_publish_vm_state(struct kvm_v2_vm *vm,
					      void *idt_kva,
					      phys_addr_t idt_gpa,
					      void *handlers_kva,
					      phys_addr_t handlers_gpa,
					      void *gdt_kva,
					      phys_addr_t gdt_gpa)
{
	/*
	 * Stash on the VM struct before per-vCPU SREGS updates:
	 * install_descriptors_sregs checks these fields before writing IDT/GDT
	 * bases.
	 */
	vm->idt_kva       = idt_kva;
	vm->idt_gpa       = idt_gpa;
	vm->handlers_kva  = handlers_kva;
	vm->handlers_gpa  = handlers_gpa;
	vm->gdt_kva       = gdt_kva;
	vm->gdt_gpa       = gdt_gpa;

	/*
	 * Write-protect the handler page from the host side so KVM's EPT marks
	 * the GPA non-writable. The guest PTE is already read-only; host-side
	 * mprotect closes the physmem memslot write path too.
	 */
	os_protect_memory(handlers_kva, PAGE_SIZE, 1, 0, 1); /* r-x */
}

static void kvm_v2_exception_free_all_vcpu_state(struct kvm_v2_vm *vm)
{
	int cpu;

	for (cpu = 0; cpu < KVM_V2_MAX_VCPUS; cpu++) {
		struct kvm_v2_vcpu *v = kvm_v2_vcpu_get(cpu);

		if (v) {
			kvm_v2_exception_free_per_vcpu_gadget_state(vm, v, cpu);
			kvm_v2_exception_free_per_vcpu(vm, v, cpu);
		}
	}
}

static int kvm_v2_exception_install_per_vcpu_state(struct kvm_v2_vm *vm)
{
	int cpu;

	for (cpu = 0; cpu < KVM_V2_MAX_VCPUS; cpu++) {
		struct kvm_v2_vcpu *v = kvm_v2_vcpu_get(cpu);
		int rc;

		if (!v)
			continue;

		rc = kvm_v2_install_per_vcpu_ist_tss(vm, v, cpu);
		if (rc < 0) {
			pr_err("um: kvm-v2 exception_install: install_per_vcpu_ist_tss(cpu=%d) failed (%d)\n",
			       cpu, rc);
			return rc;
		}

		rc = kvm_v2_install_per_vcpu_gadget_state(vm, v, cpu);
		if (rc < 0) {
			pr_err("um: kvm-v2 exception_install: install_per_vcpu_gadget_state(cpu=%d) failed (%d)\n",
			       cpu, rc);
			return rc;
		}

		rc = kvm_v2_install_descriptors_sregs(vm, v);
		if (rc < 0) {
			pr_err("um: kvm-v2 exception_install: install_descriptors_sregs(cpu=%d) failed (%d)\n",
			       cpu, rc);
			return rc;
		}
	}

	return 0;
}

static void kvm_v2_exception_write_tss_desc(struct kvm_v2_vm *vm)
{
	struct kvm_v2_vcpu *v0 = kvm_v2_vcpu_get(0);

	if (!v0 || !v0->tss_gva)
		return;

	kvm_v2_gdt_write_tss_desc(vm->gdt_kva, KVM_V2_GDT_SLOT_TSS,
				  v0->tss_gva, KVM_V2_TSS_LIMIT);
	pr_debug("um: kvm-v2 exception_install: GDT TSS slot=%u base=%#llx limit=%u\n",
		 KVM_V2_GDT_SLOT_TSS, (unsigned long long)v0->tss_gva,
		 KVM_V2_TSS_LIMIT);
}

static void kvm_v2_exception_maybe_upgrade_gadget(struct kvm_v2_vm *vm)
{
	int rc;

	if (!IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_GADGET)) {
		pr_debug("um: kvm-v2 exception_install: gadget disabled; using fallback LSTAR\n");
		return;
	}

	rc = kvm_v2_trampoline_upgrade_to_gadget(vm);
	if (rc < 0)
		pr_warn("um: kvm-v2 exception_install: gadget upgrade failed (%d); using fallback LSTAR\n",
			rc);
}

struct kvm_v2_exception_pages {
	void *idt_kva;
	void *handlers_kva;
	void *gdt_kva;
	phys_addr_t idt_gpa;
	phys_addr_t handlers_gpa;
	phys_addr_t gdt_gpa;
};

static int kvm_v2_exception_validate_install(struct kvm_v2_vm *vm)
{
	if (!vm)
		return -EINVAL;

	/*
	 * Idempotent short-circuit: a successful prior install means the
	 * three KVAs are non-NULL, the PT entries are populated, and the
	 * pool members already have idt/gdt installed via SREGS. Re-running
	 * would leak the existing pages and re-issue redundant ioctls.
	 */
	if (vm->idt_kva)
		return 1;

	/*
	 * The trampoline PT chain must be installed first. Exception state
	 * occupies PTE[1..3] in the chain owned by trampoline_pte_kva. The
	 * normal subsys_initcall path installs the trampoline and kernel-half
	 * mapping before exception state; reject out-of-order callers.
	 */
	if (!vm->trampoline_pte_kva) {
		pr_err("um: kvm-v2 exception_install: trampoline PT chain not installed (trampoline_pte_kva=NULL)\n");
		return -EINVAL;
	}

	return 0;
}

static int kvm_v2_exception_prepare_pages(struct kvm_v2_exception_pages *pages)
{
	int rc;

	rc = kvm_v2_exception_alloc_pages(&pages->idt_kva,
					  &pages->handlers_kva,
					  &pages->gdt_kva);
	if (rc)
		return rc;

	pages->idt_gpa = __pa(pages->idt_kva);
	pages->handlers_gpa = __pa(pages->handlers_kva);
	pages->gdt_gpa = __pa(pages->gdt_kva);

	kvm_v2_exception_populate_pages(pages->idt_kva, pages->handlers_kva,
					pages->gdt_kva);
	return 0;
}

static void kvm_v2_exception_publish_pages(struct kvm_v2_vm *vm,
					   struct kvm_v2_exception_pages *pages)
{
	kvm_v2_exception_install_ptes(vm, pages->idt_gpa, pages->handlers_gpa,
				      pages->gdt_gpa);
	kvm_v2_exception_publish_vm_state(vm, pages->idt_kva, pages->idt_gpa,
					  pages->handlers_kva,
					  pages->handlers_gpa,
					  pages->gdt_kva, pages->gdt_gpa);
}

static void kvm_v2_exception_unwind_pages(struct kvm_v2_vm *vm,
					  struct kvm_v2_exception_pages *pages)
{
	kvm_v2_exception_free_all_vcpu_state(vm);
	kvm_v2_exception_clear_vm_state(vm);
	kvm_v2_exception_free_pages(pages->idt_kva, pages->handlers_kva,
				    pages->gdt_kva);
}

static void kvm_v2_exception_trace_install(struct kvm_v2_exception_pages *pages)
{
	pr_debug("um: kvm-v2 exception_install: idt=%pa handlers=%pa gdt=%pa vectors=%u slots=%u\n",
		 &pages->idt_gpa, &pages->handlers_gpa, &pages->gdt_gpa,
		 KVM_V2_IDT_NR_VECTORS, KVM_V2_GDT_NR_SLOTS);

	trace_um_backend_kvm_v2_exception_install((u64)pages->idt_gpa,
						  (u64)pages->handlers_gpa,
						  (u64)pages->gdt_gpa);
}

int kvm_v2_exception_install(struct kvm_v2_vm *vm)
{
	struct kvm_v2_exception_pages pages = {};
	int rc;

	rc = kvm_v2_exception_validate_install(vm);
	if (rc > 0)
		return 0;
	if (rc)
		return rc;

	rc = kvm_v2_exception_prepare_pages(&pages);
	if (rc)
		return rc;
	kvm_v2_exception_publish_pages(vm, &pages);

	rc = kvm_v2_exception_install_per_vcpu_state(vm);
	if (rc < 0)
		goto err_unwind;

	kvm_v2_exception_write_tss_desc(vm);
	kvm_v2_exception_trace_install(&pages);
	kvm_v2_exception_maybe_upgrade_gadget(vm);

	return 0;

err_unwind:
	kvm_v2_exception_unwind_pages(vm, &pages);
	return rc;
}

void kvm_v2_exception_free(struct kvm_v2_vm *vm)
{
	void *idt_kva, *handlers_kva, *gdt_kva;

	if (!vm)
		return;

	/*
	 * Free per-vCPU IST stack + TSS pages before the IDT/handlers/GDT
	 * pages drop. Each kvm_v2_exception_free_per_vcpu clears its own PTE
	 * pair (PTE[KVM_V2_IST_BASE_SLOT + cpu*2 .. +1]) and frees the page;
	 * safe on a never-installed vcpu (NULL ist_stack_kva short-circuit).
	 * Iterate the pool unconditionally, even if vm->idt_kva is NULL, so
	 * out-of-order cleanup still releases any installed per-vCPU pages.
	 */
	kvm_v2_exception_free_all_vcpu_state(vm);

	if (!vm->idt_kva)
		return;

	/*
	 * Clear PTE[1..3] before freeing the pages so any concurrent
	 * mm operation sees zero (= not present) rather than dangling.
	 * The ordering matches kvm_v2_kernel_half_free.
	 *
	 * Caller responsibility: this helper must run before
	 * kvm_v2_kernel_half_free, because pte_table is part of the
	 * kernel-half chain. Once kernel_half_free runs, pte_table is freed
	 * memory and the writes below would touch released pages.
	 */
	idt_kva = vm->idt_kva;
	handlers_kva = vm->handlers_kva;
	gdt_kva = vm->gdt_kva;

	kvm_v2_exception_clear_vm_state(vm);
	kvm_v2_exception_free_pages(idt_kva, handlers_kva, gdt_kva);
}
