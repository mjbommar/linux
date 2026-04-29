// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase D.1: IO-port LSTAR trampoline.
 *
 * Per memo 26 §D.1. Lands the storage + bytes for the 5-byte LSTAR
 * trampoline:
 *
 *   out %al, $0xf4   ; e6 f4 — KVM_EXIT_IO trap (port 0xf4 = SYSCALL)
 *   sysretq          ; 48 0f 07 — drops to CPL=3, RIP=RCX, RFLAGS=R11
 *
 * v1 reference: kvm-v1-archive/thread.c:1214-1219 (the #else
 * non-gadget branch — bytes verbatim). v1's gadget paths
 * (kvm-v1-archive/thread.c:814-1213's 7-syscall + vDSO clock_gettime
 * fast path) are out of scope for v2; Phase H may revisit per memo
 * 26 §D.1 ("No swapgs. No stack switch. No %gs: scratch storage. v1's
 * swapgs+stack-switch was for the in-trampoline gadget paths —
 * Phase H optimisation, not in scope.").
 *
 * Mechanism (memo 26 §D.1, expanded):
 *   - Guest at CPL=3 issues SYSCALL with NR in RAX.
 *   - SYSCALL switches to CPL=0, loads RIP from MSR_LSTAR (programmed
 *     in D.4 to KVM_V2_LSTAR_GVA = 0xffffe00000000040). FMASK clears
 *     IF so interrupts stay masked across the trampoline.
 *   - Trampoline runs at CPL=0: `out %al, $0xf4` is a kernel-priv I/O
 *     instruction, executes without fault, traps to host with
 *     kvm_run->exit_reason = KVM_EXIT_IO and kvm_run->io.port = 0xf4.
 *   - D.2's KVM_EXIT_IO dispatch reads the port, marshals
 *     kvm_run->s.regs.regs into uml_pt_regs (sync-regs, no
 *     KVM_GET_REGS), calls into arch/um/kernel/skas/syscall.c's
 *     handle_syscall, marshals the return value back.
 *   - KVM_RUN re-entry advances RIP past the trapping `out` (KVM does
 *     this internally for IO exits) and lands on the next instruction —
 *     `sysretq` — which drops back to CPL=3 with RIP=RCX, RFLAGS=R11.
 *   - The user-resume RIP/RFLAGS were placed in RCX/R11 by D.3's
 *     marshal-out — sysretq's architectural semantics pop them
 *     correctly without per-syscall plumbing on the trampoline side.
 *
 * Today (D.1) this trampoline is unreferenced from any KVM_RUN path:
 *   - MSR_LSTAR isn't programmed yet — that's D.4 (CPUID install +
 *     SET_MSRS for STAR/LSTAR/CSTAR/SFMASK/EFER).
 *   - PML4[508] isn't installed yet — also D.4 (the kernel-half
 *     identity-walk that makes 0xffffe00000000000ULL → __pa(page)
 *     reachable from the guest CR3 walk).
 *   - ops.vcpu_run still routes to seccomp_vcpu_run; D.5 flips the
 *     pointer.
 *
 * D.1's job is just "land the bytes + the per-VM storage so D.4 has
 * something to point MSR_LSTAR at." The trace event below fires once
 * per VM at install time; it stays silent on a force=kvm-v2 boot if
 * the buddy allocator wasn't up at vm_create time and the lazy retry
 * path hasn't run yet (see kvm_v2_trampoline_alloc_and_install
 * comment).
 *
 * Buddy allocator timing (memo 26 §D.0a's lesson):
 *   kvm_v2_vm_create runs from kvm_v2_init -> linux_main, BEFORE
 *   setup_arch -> setup_physmem brings the buddy allocator up. So
 *   alloc_page(GFP_KERNEL) at vm_create returns NULL; D.0a hit the
 *   same wall for kzalloc and resolved it by deferring to first
 *   KVM_RUN. We follow the same pattern: vm_create attempts the
 *   install best-effort; if alloc_page returns NULL, the per-VM
 *   `trampoline_page` field stays NULL and a future caller (Phase D.4
 *   when it programs MSR_LSTAR, or Phase D.5's vcpu_run lazy hook)
 *   re-invokes this helper after mm_init. The idempotency check at
 *   the top of the function makes the retry safe.
 */

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/set_memory.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/page.h>
#include <asm/trace/um_backend.h>

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

/*
 * The 5-byte LSTAR body — byte-identical to v1's #else branch at
 * kvm-v1-archive/thread.c:1215-1218. Documented per byte so a future
 * read of this file doesn't have to cross-reference the disassembly:
 *
 *   0xe6 0xf4         out %al, $0xf4   (2 bytes; port-imm8 form)
 *   0x48 0x0f 0x07    sysretq          (3 bytes; REX.W + 0F 07)
 *
 * Total 5 bytes; lives at offset KVM_V2_TRAMPOLINE_LSTAR_OFFSET (0x40)
 * within the page. The remainder of the page is zero on alloc_page
 * (GFP from buddy clears) — Phase E.3 exception handlers will populate
 * other offsets per the layout v1 had at kvm-v1-archive/thread.c:
 * 666-690.
 */
static const u8 kvm_v2_lstar_bytes[] = {
	0xe6, 0xf4,		/* out %al, $0xf4 */
	0x48, 0x0f, 0x07,	/* sysretq */
};

/*
 * Compile-time guard: the LSTAR body must fit in a single page even
 * once future Phase E.3 handlers grow the populated region around it.
 * v1's whole bootstrap layout (LSTAR + IDT + GDT + TSS + #PF/#DF/...
 * handlers) fit in one page; v2's plan is the same per memo 26 §D.1
 * "Total trampoline budget: ~16 B including alignment padding".
 */
static_assert(sizeof(kvm_v2_lstar_bytes) <= PAGE_SIZE,
	      "LSTAR trampoline body exceeds PAGE_SIZE");
static_assert(KVM_V2_TRAMPOLINE_LSTAR_OFFSET + sizeof(kvm_v2_lstar_bytes)
	      <= PAGE_SIZE,
	      "LSTAR trampoline at offset 0x40 spills past PAGE_SIZE");

int kvm_v2_trampoline_alloc_and_install(struct kvm_v2_vm *vm)
{
	struct page *page;
	void *kva;
	phys_addr_t gpa;
	int rc;

	if (!vm)
		return -EINVAL;

	/*
	 * Idempotent: a successful prior install short-circuits. The
	 * vm_create attempt may have failed (buddy allocator not up at
	 * init_backend time per the file-scope comment); the lazy retry
	 * path from D.4/D.5 calls back in here once the buddy is up, and
	 * this guard keeps a multiply-installed trampoline impossible.
	 */
	if (vm->trampoline_page)
		return 0;

	/*
	 * v1 used get_zeroed_page (kvm-v1-archive/thread.c:1431); we use
	 * alloc_page + memset so we can BUILD_BUG_ON the layout against
	 * struct page semantics. GFP_KERNEL because we're called from
	 * kvm_v2_vm_create (process context, not atomic) and from the
	 * lazy retry path which is also process context. __GFP_ZERO so
	 * the unpopulated regions of the page (everything outside the
	 * 5-byte LSTAR window) are deterministic — Phase E.3 handlers
	 * may rely on the surrounding bytes being 0x00 for IDT-entry
	 * fall-through behaviour (v1's bootstrap relied on the same).
	 */
	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		/*
		 * Pre-mm_init the buddy allocator returns NULL silently.
		 * Don't pr_err — D.0a's lesson is that this is expected at
		 * vm_create and the retry path picks it up. Use pr_info so
		 * the deferred-install message lands in dmesg for boot-log
		 * forensics but doesn't ratchet log severity.
		 */
		pr_info("um: kvm-v2 trampoline_install: alloc_page returned NULL (buddy allocator not up at init_backend time per memo 26 §D.0a); deferring to first lazy retry\n");
		return -ENOMEM;
	}

	kva = page_address(page);
	gpa = __pa(kva);

	/*
	 * Write the 5 LSTAR bytes at offset 0x40. The page is zero-filled
	 * around it; Phase E.3 handlers will populate other offsets in
	 * subsequent commits. Use memcpy not __builtin_memcpy_inline (the
	 * latter triggers FORTIFY_SOURCE noise on small fixed copies on
	 * some toolchains).
	 */
	memcpy((u8 *)kva + KVM_V2_TRAMPOLINE_LSTAR_OFFSET,
	       kvm_v2_lstar_bytes, sizeof(kvm_v2_lstar_bytes));

	/*
	 * Drop host-side write privilege. set_memory_ro is a no-op stub
	 * on UML (CONFIG_ARCH_HAS_SET_MEMORY is not selected by
	 * arch/um/Kconfig — see include/linux/set_memory.h:11), so this
	 * call returns 0 without changing protection. We call it anyway
	 * for ABI symmetry with v1's bootstrap (kvm-v1-archive/thread.c
	 * called set_memory_ro after writing) and so that any future UML
	 * implementation of set_memory_ro automatically picks the
	 * trampoline page up.
	 *
	 * Defense-in-depth: even with set_memory_ro stubbed, the guest
	 * cannot write the page from CPL=3 because PML4[508] (installed
	 * in D.4) carries US=0. CPL=0 trampoline code only reads/executes,
	 * never writes. Host-side stray writes to `kva` would still
	 * succeed today but are not a known threat surface — the only
	 * code that touches the page is in this file.
	 */
	rc = set_memory_ro((unsigned long)kva, 1);
	if (rc) {
		pr_warn("um: kvm-v2 trampoline_install: set_memory_ro(%p) returned %d (continuing — UML stub returns 0; treat non-zero as advisory)\n",
			kva, rc);
		/* Not fatal — proceed. */
	}

	vm->trampoline_page = kva;
	vm->trampoline_gpa  = gpa;

	pr_info("um: kvm-v2 trampoline_install: kva=%p gpa=%pa gva=%#llx (LSTAR body at +%#x; %zu bytes)\n",
		kva, &gpa, (u64)KVM_V2_LSTAR_GVA,
		KVM_V2_TRAMPOLINE_LSTAR_OFFSET,
		sizeof(kvm_v2_lstar_bytes));
	trace_um_backend_kvm_v2_trampoline_install((u64)gpa,
						   (u64)KVM_V2_LSTAR_GVA);
	return 0;
}

/*
 * Lazy retry path. vm_create's best-effort install happens at
 * init_backend time before the buddy allocator is up; this initcall
 * runs after mm_init, picks up the deferral, and does the install.
 *
 * Why subsys_initcall and not later: D.4 will likely program MSR_LSTAR
 * from a similarly early hook, and it must see the trampoline page
 * already installed. subsys_initcall runs before device_initcall and
 * late_initcall, both of which Phase D/E may use for downstream wiring.
 *
 * Why an initcall at all (vs a vcpu_run-first-call hook a la D.0a): the
 * trampoline is per-VM, not per-vCPU. CPUID is per-vCPU and naturally
 * lazy-installs from the dispatcher. The trampoline fires once total —
 * no per-vCPU re-install — so an initcall is the natural shape. D.0a's
 * vcpu_run hook would also work but would only run after D.5 flips
 * .vcpu_run; surfacing the install at boot keeps the verification
 * shape from memo 26 §D.1 (trace fires once at boot under
 * force=kvm-v2).
 *
 * Skip on non-v2 boots — the VM context is NULL when seccomp is
 * selected, and an initcall that ran unconditionally would be
 * misleading in dmesg.
 */
static int __init kvm_v2_trampoline_late_install(void)
{
	struct kvm_v2_vm *vm = kvm_v2_vm_get();

	if (!vm)
		return 0;	/* not v2 — silent skip */

	if (vm->trampoline_page)
		return 0;	/* already installed — silent skip */

	(void)kvm_v2_trampoline_alloc_and_install(vm);
	return 0;
}
subsys_initcall(kvm_v2_trampoline_late_install);

void kvm_v2_trampoline_free(struct kvm_v2_vm *vm)
{
	if (!vm || !vm->trampoline_page)
		return;

	/*
	 * Symmetric teardown — restore writable protection (no-op stub
	 * on UML, see install path) before freeing so the page returns
	 * to the buddy in a predictable state. v1 archive's free path
	 * documented the same ordering at kvm-v1-archive/thread.c:1444+.
	 */
	(void)set_memory_rw((unsigned long)vm->trampoline_page, 1);

	free_page((unsigned long)vm->trampoline_page);
	vm->trampoline_page = NULL;
	vm->trampoline_gpa  = 0;
}
