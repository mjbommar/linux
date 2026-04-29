// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase E.1: IDT + handler stubs + GDT install.
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
#include <asm/pgtable.h>		/* _PAGE_PRESENT, _PAGE_ACCESSED */
#include <asm/trace/um_backend.h>

#include <os.h>				/* os_ioctl_generic */

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

/*
 * The handler stub byte sequence. Each stub is 4 bytes:
 *
 *   e6 XX           out %al, $XX     ; trap to host with port=XX
 *   48 cf           iretq            ; return to user RIP/RFLAGS popped
 *                                     ; from CPU's pushed iretq frame
 *
 * Total stub length is 4 bytes; we lay them out at 16-byte intervals
 * within the handlers page so IDT-gate-offset arithmetic is
 * `KVM_V2_HANDLERS_GVA + (slot << 4)` with no per-handler size table.
 * The leftover 12 bytes of each slot are zero from __GFP_ZERO (the
 * x86 decoder treats `00 00` as `add %al, (%rax)` which would fault
 * on the iretq's RSP target — but the iretq always exits to user
 * before any zero bytes execute, so the padding is dead code).
 *
 * Codex audit finding #4: NO `mov %cr2, %rax` here. v1's archive
 * removed that opcode at kvm-v1-archive/thread.c:1297-1308 because
 * it clobbered user RAX before the host's GET_REGS captured vCPU
 * state. v2 reads CR2 via the sync-regs sregs.cr2 path (KVM populates
 * the mmap on every exit when KVM_SYNC_X86_SREGS is set in
 * kvm_valid_regs at vcpu_create — vcpu.c:631-632).
 *
 * Wire-format bytes are byte-identical to the `out + iretq` shape
 * v1 used for its #PF handler at kvm-v1-archive/thread.c:1310-1314
 * (where v1 kept the trailing `iretq` after the `add $8, %rsp` to
 * pop the #PF error code). v2's stubs don't pop an error code:
 * non-error-code vectors (#DE/#BP/#OF/#UD) push only RIP/CS/RFLAGS
 * /RSP/SS, and error-code vectors (#PF/#GP) leave the error code on
 * stack — but Phase E.3's host-side dispatch reads error code from
 * sync-regs / KVM exit info, so the in-guest stub doesn't need to
 * pop it before iretq. (E.3 may revisit if iretq's frame shape
 * for the error-code case requires explicit `add $8, %rsp` in the
 * stub; deferring per spec.)
 */
static const u8 kvm_v2_handler_stub_pf[]    = { 0xe6, UM_KVM_TRAP_PF,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_gp[]    = { 0xe6, UM_KVM_TRAP_GP,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_ud[]    = { 0xe6, UM_KVM_TRAP_UD,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_de[]    = { 0xe6, UM_KVM_TRAP_DE,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_of[]    = { 0xe6, UM_KVM_TRAP_OF,    0x48, 0xcf };
static const u8 kvm_v2_handler_stub_panic[] = { 0xe6, UM_KVM_TRAP_PANIC, 0x48, 0xcf };

/*
 * #BP slot stub: kept for symmetry with the IDT[3] gate that points at
 * it, but in-guest #BP dispatch uses KVM_GUESTDBG_USE_SW_BP →
 * KVM_EXIT_DEBUG (memo 26 §E.3 option 2), not the IO-port path. The
 * stub IS reachable if KVM_GUESTDBG isn't wired (E.3 may not enable
 * the cap on every vcpu); use UM_KVM_TRAP_PANIC so any actual entry
 * via this stub trips the host-side panic dispatch loud and clear
 * rather than silently consuming an unhandled port. This matches the
 * "stub is unused under normal operation" semantics the spec calls
 * for at the artefact-3 sketch.
 */
static const u8 kvm_v2_handler_stub_bp[]    = { 0xe6, UM_KVM_TRAP_PANIC, 0x48, 0xcf };

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

static void kvm_v2_populate_handlers(void *handlers_kva)
{
	u8 *base = handlers_kva;

	/*
	 * Each stub at slot * 16. memcpy length = sizeof(stub) = 4 bytes;
	 * the page is zero-filled around them so the trailing 12 bytes
	 * stay 0x00 (dead code — iretq exits before they execute).
	 *
	 * Stub-to-port-to-vector mapping documented at the static const u8
	 * declarations above. Cross-reference: memo 26 §E.3 per-vector
	 * mapping table.
	 */
	memcpy(base + KVM_V2_HANDLER_SLOT_DE    * 16,
	       kvm_v2_handler_stub_de,    sizeof(kvm_v2_handler_stub_de));
	memcpy(base + KVM_V2_HANDLER_SLOT_BP    * 16,
	       kvm_v2_handler_stub_bp,    sizeof(kvm_v2_handler_stub_bp));
	memcpy(base + KVM_V2_HANDLER_SLOT_OF    * 16,
	       kvm_v2_handler_stub_of,    sizeof(kvm_v2_handler_stub_of));
	memcpy(base + KVM_V2_HANDLER_SLOT_UD    * 16,
	       kvm_v2_handler_stub_ud,    sizeof(kvm_v2_handler_stub_ud));
	memcpy(base + KVM_V2_HANDLER_SLOT_GP    * 16,
	       kvm_v2_handler_stub_gp,    sizeof(kvm_v2_handler_stub_gp));
	memcpy(base + KVM_V2_HANDLER_SLOT_PF    * 16,
	       kvm_v2_handler_stub_pf,    sizeof(kvm_v2_handler_stub_pf));
	memcpy(base + KVM_V2_HANDLER_SLOT_PANIC * 16,
	       kvm_v2_handler_stub_panic, sizeof(kvm_v2_handler_stub_panic));
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
				KVM_V2_HANDLER_SLOT_PANIC * 16;

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
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_DE * 16, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 3,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_BP * 16, 3, 1);
	kvm_v2_idt_set_gate(idt_kva, 4,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_OF * 16, 3, 1);
	kvm_v2_idt_set_gate(idt_kva, 6,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_UD * 16, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 13,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_GP * 16, 0, 1);
	kvm_v2_idt_set_gate(idt_kva, 14,
		KVM_V2_HANDLERS_GVA + KVM_V2_HANDLER_SLOT_PF * 16, 0, 1);
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
	pte_table = (u64 *)vm->trampoline_pte_kva;
	pte_table[1] = (u64)(idt_gpa      | _PAGE_PRESENT | _PAGE_ACCESSED);
	pte_table[2] = (u64)(handlers_gpa | _PAGE_PRESENT | _PAGE_ACCESSED);
	pte_table[3] = (u64)(gdt_gpa      | _PAGE_PRESENT | _PAGE_ACCESSED);

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

		rc = kvm_v2_install_descriptors_sregs(vm, v);
		if (rc < 0) {
			pr_err("um: kvm-v2 exception_install: install_descriptors_sregs(cpu=%d) failed (%d)\n",
			       cpu, rc);
			goto err_unwind_pt;
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
	return 0;

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

	if (!vm || !vm->idt_kva)
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
