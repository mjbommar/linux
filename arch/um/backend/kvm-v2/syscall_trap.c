// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase D.1+D.2+D.3: IO-port LSTAR trampoline +
 * KVM_EXIT_IO syscall-trap dispatcher + return-side marshal-out.
 *
 * Per memo 26 §D.1 (D.1 portion: storage + bytes) and §D.2 (D.2
 * portion: KVM_EXIT_IO → handle_syscall handler at the bottom of
 * this file). The trampoline (§D.1) puts an `out %al, $0xf4` byte at
 * the LSTAR target so a guest SYSCALL deterministically traps to the
 * host with kvm_run->exit_reason = KVM_EXIT_IO and io.port = 0xf4.
 * The dispatcher (§D.2) reads that exit, extracts the syscall NR from
 * the user's RAX, propagates the post-SYSCALL RIP/RFLAGS from RCX/R11
 * back into HOST_IP/HOST_EFLAGS, and calls into UML's common
 * handle_syscall path (arch/um/kernel/skas/syscall.c:19) — same shape
 * the seccomp backend uses from its SIGSYS branch
 * (arch/um/backend/seccomp/trap_user.c:159). D.3 closes the loop with
 * the post-handle_syscall marshal-out: regs->gp[] is copied back into
 * kvm_run->s.regs.regs (sync_regs path; KVM_SYNC_X86_REGS dirty bit
 * is OR'd in), and rcx/r11 are explicitly overwritten with the
 * user-resume RIP/RFLAGS so SYSRETQ at the trampoline tail lands the
 * guest at the right user RIP (or signal handler VA on do_signal
 * delivery) with the right RFLAGS.
 *
 * Both phases live in one file because the trampoline bytes (§D.1)
 * and the host-side decoder (§D.2) are two halves of the same ABI:
 * the byte at offset 0x40 issues the IO trap; the helper at the
 * bottom of this file consumes it. Splitting them across files made
 * the v1 archive harder to follow (the bytes lived in thread.c near
 * line 1215 and the dispatch lived 2000 lines later near line 4030);
 * v2 keeps both halves co-located.
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
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kvm.h>		/* struct kvm_run, KVM_EXIT_IO */
#include <linux/mm.h>
#include <linux/mm_types.h>	/* init_mm */
#include <linux/pgtable.h>	/* pgd_index, set_pgd */
#include <linux/printk.h>
#include <linux/set_memory.h>
#include <linux/signal.h>	/* clear_siginfo, kernel_siginfo_t */
#include <linux/string.h>
#include <linux/types.h>
#include <uapi/asm-generic/siginfo.h>	/* ILL_ILLOPN, FPE_INTOVF, SEGV_MAPERR */

#include <asm/page.h>
#include <asm/pgtable.h>	/* _KERNPG_TABLE, _PAGE_PRESENT, _PAGE_ACCESSED, swapper_pg_dir */
#include <asm/trace/um_backend.h>

#include <kern_util.h>		/* segv_handler, relay_signal */
#include <skas.h>		/* handle_syscall */
#include <sysdep/ptrace.h>	/* uml_pt_regs, HOST_AX/CX/IP/EFLAGS/R11, UPT_SYSCALL_NR */
#include <sysdep/ptrace_user.h>	/* PT_SYSCALL_NR */

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
	 * Boot-time self-check: read back the bytes we just wrote and
	 * panic on any mismatch. The trampoline is on the hot path post-
	 * D.5; a corrupt LSTAR is one of the worst possible failure
	 * modes (guest jumps to garbage at CPL=0). One memcmp at boot
	 * costs nothing and catches: byte-table corruption between
	 * compile and load, accidental MD5-style optimizer mangling,
	 * write-protected page silently dropping the memcpy, etc.
	 */
	if (memcmp((const u8 *)kva + KVM_V2_TRAMPOLINE_LSTAR_OFFSET,
		   kvm_v2_lstar_bytes,
		   sizeof(kvm_v2_lstar_bytes)) != 0) {
		const u8 *got = (const u8 *)kva + KVM_V2_TRAMPOLINE_LSTAR_OFFSET;

		panic("um: kvm-v2 trampoline_install: LSTAR readback MISMATCH at kva=%p+%#x — got %02x %02x %02x %02x %02x, want e6 f4 48 0f 07",
		      kva, KVM_V2_TRAMPOLINE_LSTAR_OFFSET,
		      got[0], got[1], got[2], got[3], got[4]);
	}

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
 * Phase D.4b: install the per-VM kernel-half PT chain at PML4[448] so
 * the LSTAR trampoline GVA (KVM_V2_LSTAR_GVA = 0xffffe00000000040) is
 * reachable from any guest CR3 once D.5 flips .vcpu_run.
 *
 * VA decomposition for KVM_V2_LSTAR_GVA = 0xffffe00000000040:
 *   PML4 index: (va >> 39) & 0x1ff = 0x1c0 = 448
 *               (corrected per codex --search audit at 8eb06f04e3ad;
 *                the original §D draft said "508" — derivation error.
 *                PML4[508] would correspond to 0xfffffe0000000040.)
 *   PUD  index: (va >> 30) & 0x1ff = 0
 *   PMD  index: (va >> 21) & 0x1ff = 0
 *   PTE  index: (va >> 12) & 0x1ff = 0
 *   page offset (bits 11..0):       0x40 (the LSTAR body lives at
 *                                         trampoline_kva + 0x40 — the
 *                                         5 bytes D.1 wrote; the PTE
 *                                         covers the whole page so the
 *                                         offset is implicit in
 *                                         post-translation guest VA).
 *
 * Mirrors v1's kvm_shadow_map_page pattern at
 * kvm-v1-archive/lifecycle.c:1538-1613 (callers at thread.c:2287-2365)
 * but writes REAL page-table entries — TDP walks them natively, no
 * shadow-PT machinery.
 *
 * Allocation: 3 pages from buddy (PUD + PMD + PTE). Each lives at a
 * host VA in [uml_physmem, uml_physmem+physmem_size) so its GPA =
 * __pa(kva) is in [0, physmem_size) — covered by D.4b-pre's physmem
 * memslot. KVM's TDP walk: CR3 = __pa(active_mm->pgd) → PML4[448] =
 * pud_pa | _KERNPG_TABLE (we install) → PUD[0] = pmd_pa | _KERNPG_TABLE
 * → PMD[0] = pte_pa | _KERNPG_TABLE → PTE[0] = trampoline_gpa |
 * (_PAGE_PRESENT | _PAGE_ACCESSED). All four GPAs sit in physmem and
 * resolve through D.4b-pre's memslot.
 *
 * Flags rationale:
 *   non-leaf (PUD, PMD entries pointing to PMD/PTE pages): _KERNPG_TABLE
 *     = _PAGE_PRESENT | _PAGE_RW | _PAGE_ACCESSED | _PAGE_DIRTY
 *     UML's pud_bad / pmd_bad validators (arch/um/include/asm/pgtable-
 *     4level.h:59 and pgtable.h:77) expect this exact shape:
 *       (val & (~PAGE_MASK & ~_PAGE_USER)) != _KERNPG_TABLE → bad.
 *     _PAGE_TABLE has _PAGE_USER set — wrong for kernel-half walks
 *     (would incorrectly allow CPL=3 reads of the entries themselves;
 *     defense in depth even though the leaf clears US).
 *   leaf (PTE entry pointing to the trampoline page):
 *     _PAGE_PRESENT | _PAGE_ACCESSED
 *     RO (no _PAGE_RW) — the trampoline is code, never written from
 *     guest CPL=0 either. Kernel-only (no _PAGE_USER) — guest CPL=3
 *     walking the same VA gets a page fault, not a successful access.
 *     Executable (UML has no _PAGE_NX bit defined in pgtable.h, so
 *     executable is implicit — every present page is executable from
 *     the architecture's point of view).
 *
 * Propagation: write swapper_pg_dir[448] = pud_pa | _KERNPG_TABLE.
 * UML's pgd_alloc (arch/um/kernel/mem.c:96-106) memcpy's entries
 * [USER_PTRS_PER_PGD..PTRS_PER_PGD) from swapper into every new mm
 * — automatic propagation. For pre-existing mms (init_mm primarily,
 * since this runs at subsys_initcall before userspace exists), patch
 * init_mm.pgd[448] directly. UML's mm_list is file-local in
 * arch/um/kernel/skas/mmu.c:71-74 (not exported), so iterating all
 * existing mms isn't available to backend code — but init_mm +
 * swapper_pg_dir is sufficient at subsys_initcall time (no other mms
 * exist that early; future mms inherit via pgd_alloc).
 *
 * Runtime assertion: BUG_ON(pgd_index(KVM_V2_LSTAR_GVA) != 448).
 * Catches any future VA-constant change that would silently miss the
 * install — codex --search audit at 8eb06f04e3ad specifically requested
 * this guard because the original §D draft mis-derived the PML4 index.
 *
 * Idempotent — re-invocation after a successful install short-circuits
 * via the trampoline_pud_kva sentinel. Necessary because the install
 * is reachable both from kvm_v2_trampoline_late_install (the
 * subsys_initcall lazy retry) and (defensively) from any future
 * dispatcher hook that calls in to ensure the install is up.
 *
 * v1 lifecycle reference: v1's per-task kvm_shadow_pgd_alloc allocated
 * a fresh shadow tree per mm and installed bootstrap pages on every
 * KVM_RUN entry (kvm-v1-archive/thread.c:2352-2365). v2 takes the
 * opposite approach: ONE PT chain for the whole VM, propagated via
 * swapper_pg_dir. v1 free path: kvm_shadow_pgd_free at
 * kvm-v1-archive/lifecycle.c:~1700 (per-task); v2 free path is
 * kvm_v2_kernel_half_free, called from vm_destroy.
 */
int kvm_v2_kernel_half_install(struct kvm_v2_vm *vm)
{
	void *pud_kva = NULL, *pmd_kva = NULL, *pte_kva = NULL;
	phys_addr_t pud_pa, pmd_pa, pte_pa;
	unsigned long entry;

	if (!vm)
		return -EINVAL;

	/*
	 * Idempotent short-circuit: a successful prior install means all
	 * three KVAs are non-NULL and swapper_pg_dir[448] / init_mm.pgd[448]
	 * already carry the install. Re-running the helper would leak the
	 * old PT pages and silently overwrite the PML4 entries with a new
	 * (identical-shape but different-GPA) chain — pointless and bug-
	 * inducing if any task pgd has already memcpy'd the kernel half.
	 */
	if (vm->trampoline_pud_kva)
		return 0;

	/*
	 * Prerequisites: D.1's trampoline must be installed (we need the
	 * GPA for the leaf PTE). The physmem memslot install (D.4b-pre)
	 * isn't checked here because it's a logical prerequisite — without
	 * it KVM's TDP walk fails at CR3 dereference, but that's a D.5-time
	 * surface; this helper just builds the chain.
	 */
	if (!vm->trampoline_page || !vm->trampoline_gpa) {
		pr_err("um: kvm-v2 kernel_half_install: trampoline not installed yet (page=%p gpa=%pa); D.1 must complete before D.4b\n",
		       vm->trampoline_page, &vm->trampoline_gpa);
		return -EINVAL;
	}

	/*
	 * Runtime assertion (codex --search audit requirement). Catches a
	 * future change to KVM_V2_TRAMPOLINE_GVA / KVM_V2_LSTAR_GVA that
	 * would silently land the install at the wrong PML4 slot. The
	 * BUG_ON fires before any allocation so a misconfigured constant
	 * panics deterministically rather than half-installing into a slot
	 * that the trampoline VA doesn't actually walk through.
	 */
	BUG_ON(pgd_index((unsigned long)KVM_V2_LSTAR_GVA) != 448);

	/*
	 * Allocate the three PT pages. GFP_KERNEL because subsys_initcall
	 * runs in process context (not atomic). __GFP_ZERO is essential —
	 * we only write [0] in each table; the other 511 entries MUST be
	 * zero (= not-present) so unrelated VAs don't accidentally walk
	 * into garbage. alloc_page returning NULL means the buddy
	 * allocator isn't up yet (pre-mm_init, same surface as D.0a /
	 * D.1's trampoline alloc); the caller treats it as a deferral
	 * signal — but in practice we only call this from the
	 * subsys_initcall lazy retry path AFTER mm_init, so this branch
	 * is defensive rather than expected.
	 */
	pud_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	pmd_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	pte_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!pud_kva || !pmd_kva || !pte_kva) {
		pr_info("um: kvm-v2 kernel_half_install: __get_free_page returned NULL (buddy not up?); deferring\n");
		if (pud_kva)
			free_page((unsigned long)pud_kva);
		if (pmd_kva)
			free_page((unsigned long)pmd_kva);
		if (pte_kva)
			free_page((unsigned long)pte_kva);
		return -ENOMEM;
	}

	pud_pa = __pa(pud_kva);
	pmd_pa = __pa(pmd_kva);
	pte_pa = __pa(pte_kva);

	/*
	 * E.3.5 root-cause fix (2026-04-29): UML's _KERNPG_TABLE writes
	 * software-only bits at hardware-reserved positions (UML _PAGE_RW=
	 * 0x020 = bit 5 collides with x86 A-bit; UML _PAGE_ACCESSED=0x080
	 * = bit 7 collides with x86 PS-bit; UML _PAGE_DIRTY=0x100 = bit 8).
	 * KVM's TDP MMU walks these tables as REAL x86 hardware page-table
	 * entries, so writing UML bits triggers reserved-bit faults during
	 * the page-table walk — surfaces as kvm_inj_exception #PF
	 * (error_code = PFERR_RSVD), and at the user-half walk surfaces as
	 * the EXIT_NPF storm + reinjected #PF that drove the EINTR-only
	 * boot symptom (memo 26 §E.3.5).
	 *
	 * Use raw x86 hardware bits here: P (0x1) | RW (0x2) | A (0x20).
	 * These pages are KVM-owned (not UML-managed pgtables): the
	 * trampoline kernel-half PT chain is allocated by buddy and never
	 * passes through UML's set_pte path, so writing x86 bits directly
	 * is safe — UML's tlb.c will never read them.
	 *
	 * NOTE: this fixes the kernel-half walk only (PML4[448] subtree).
	 * The user-half (PML4[0..255], walked from CR3 = __pa(active_mm->
	 * pgd)) still has UML-bit entries because UML kernel code populates
	 * the pgd via set_pte_at — every PT entry there has UML bits.
	 * KVM's TDP walk of the user RIP will continue to fail until v2
	 * grows a per-mm shadow PT (v1's approach) or UML's PTE bit layout
	 * is migrated to x86-compatible bits. This commit verifies the bit-
	 * encoding hypothesis on the kernel-half install; the user-half
	 * fix is a Phase E.4+ scope.
	 */
	#define V2_X86_P  (1ull << 0)
	#define V2_X86_RW (1ull << 1)
	#define V2_X86_A  (1ull << 5)
	#define V2_X86_KERN_NONLEAF (V2_X86_P | V2_X86_RW | V2_X86_A)
	#define V2_X86_KERN_LEAF_RO (V2_X86_P | V2_X86_A)

	((u64 *)pud_kva)[0] = (u64)(pmd_pa | V2_X86_KERN_NONLEAF);
	((u64 *)pmd_kva)[0] = (u64)(pte_pa | V2_X86_KERN_NONLEAF);
	((u64 *)pte_kva)[0] = (u64)(vm->trampoline_gpa | V2_X86_KERN_LEAF_RO);

	/*
	 * Seed swapper_pg_dir[448]. UML's pgd_alloc memcpy at
	 * arch/um/kernel/mem.c:101-103 picks this up for every future mm
	 * (it copies entries [USER_PTRS_PER_PGD..PTRS_PER_PGD) from
	 * swapper into the freshly-allocated pgd). The kernel-half
	 * install therefore propagates automatically — no per-mm hook,
	 * no arch_dup_mmap, no mm_list iteration.
	 *
	 * Direct assignment (= __pgd(...)) is correct here. set_pgd routes
	 * through set_p4d which on UML's nop4d layout boils back down to
	 * the same direct write, so set_pgd is also valid; we use direct
	 * assignment to mirror v1's approach (lifecycle.c didn't go through
	 * set_pgd either) and keep the value visible at the same address
	 * UML's mm code reads.
	 */
	/*
	 * The PML4[448] entry is also written with x86 bits — same reason
	 * as the PUD/PMD/PTE writes above. swapper_pg_dir[448] propagates
	 * to every future mm via pgd_alloc's memcpy, so every UML mm will
	 * have the kernel-half pgd entry in x86-compatible form. UML kernel
	 * code that reads pgd_val(swapper_pg_dir[448]) directly may see
	 * unexpected bits — but the only consumer of that value is the
	 * pgd_alloc memcpy itself, which doesn't interpret the bits.
	 */
	entry = (unsigned long)(pud_pa | V2_X86_KERN_NONLEAF);
	swapper_pg_dir[448] = __pgd(entry);

	/*
	 * Patch init_mm.pgd[448] explicitly. swapper_pg_dir[] is the
	 * source pgd_alloc copies from for FUTURE mms; init_mm already
	 * exists at this point (mm/init-mm.c:32 — set up before any
	 * initcall runs) and its pgd was populated before we wrote
	 * swapper, so it doesn't pick the entry up via the copy path.
	 * v1's mm_list iteration would have visited init_mm; we patch it
	 * directly because UML doesn't export mm_list (file-local in
	 * arch/um/kernel/skas/mmu.c:71-74 per memo 26 §D.4 codex audit).
	 */
	if (init_mm.pgd)
		init_mm.pgd[448] = __pgd(entry);
	else
		pr_warn("um: kvm-v2 kernel_half_install: init_mm.pgd is NULL (very early in boot?); only swapper_pg_dir[448] seeded\n");

	/*
	 * Stash the chain on the VM struct for free-time. PUD GPA goes on
	 * the struct so a future audit / introspection caller can confirm
	 * the entry value without re-walking swapper.
	 */
	vm->trampoline_pud_kva = pud_kva;
	vm->trampoline_pmd_kva = pmd_kva;
	vm->trampoline_pte_kva = pte_kva;
	vm->trampoline_pud_gpa = pud_pa;

	pr_info("um: kvm-v2 kernel_half_install: pud_gpa=%pa pmd_gpa=%pa pte_gpa=%pa leaf=trampoline_gpa=%pa gva=%#llx (swapper_pg_dir[448] + init_mm.pgd[448] = %#lx)\n",
		&pud_pa, &pmd_pa, &pte_pa, &vm->trampoline_gpa,
		(u64)KVM_V2_LSTAR_GVA, entry);
	trace_um_backend_kvm_v2_pml4_install((u64)pud_pa,
					     (u64)KVM_V2_LSTAR_GVA);
	return 0;
}

void kvm_v2_kernel_half_free(struct kvm_v2_vm *vm)
{
	if (!vm || !vm->trampoline_pud_kva)
		return;

	/*
	 * Clear swapper_pg_dir[448] and init_mm.pgd[448] BEFORE freeing
	 * the pages so any concurrent mm operation sees zero (= not
	 * present) rather than dangling. At vm_destroy time on shutdown,
	 * no mms should be operating against these entries — but
	 * defensive ordering matches v1's free path at
	 * kvm-v1-archive/lifecycle.c (per-task shadow free).
	 *
	 * Note: pgd_alloc's memcpy at arch/um/kernel/mem.c:101-103 does
	 * NOT see this clear for already-allocated mms — they retain the
	 * old PUD GPA in their pgd[448], pointing at freed memory. This
	 * is acceptable at vm_destroy because the VM fd is being torn
	 * down and KVM_RUN will not be issued again; on shutdown all mms
	 * are torn down too. If a future Phase J reinit flow lands, this
	 * dangling-entry issue must be revisited (revoke from all live
	 * mms before freeing the chain).
	 */
	swapper_pg_dir[448] = __pgd(0);
	if (init_mm.pgd)
		init_mm.pgd[448] = __pgd(0);

	free_page((unsigned long)vm->trampoline_pte_kva);
	free_page((unsigned long)vm->trampoline_pmd_kva);
	free_page((unsigned long)vm->trampoline_pud_kva);
	vm->trampoline_pte_kva = NULL;
	vm->trampoline_pmd_kva = NULL;
	vm->trampoline_pud_kva = NULL;
	vm->trampoline_pud_gpa = 0;
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

	/*
	 * D.4b-pre: piggy-back the physmem-memslot lazy retry on this
	 * initcall. vm_create's eager attempt may have returned -EAGAIN
	 * because uml_physmem / physmem_size weren't yet set when
	 * init_backend ran (arch/um/kernel/um_arch.c:372 init_backend vs
	 * 392/399 globals); subsys_initcall fires after linux_main()
	 * completes so the globals are stable here. Helper is idempotent
	 * (physmem_memslot_id sentinel) so a successful eager install
	 * short-circuits. Failure is logged inside the helper; the
	 * trampoline retry below still runs so observability lines up
	 * with the v1 ensure-memslot-then-bootstrap order at
	 * kvm-v1-archive/lifecycle.c:613-648.
	 */
	(void)kvm_v2_physmem_memslot_install(vm);

	if (!vm->trampoline_page) {
		int rc = kvm_v2_trampoline_alloc_and_install(vm);

		if (rc) {
			/*
			 * The trampoline alloc failed — without it the
			 * kernel-half install has no leaf GPA to point at.
			 * Log the deferral and return; a future caller may
			 * retry once the buddy is up. Don't propagate the
			 * error from the initcall — the substrate gate's
			 * seccomp-delegating ops still keep boot moving
			 * (D.5 is what flips .vcpu_run; until then a
			 * deferred install is observable but non-fatal).
			 */
			return 0;
		}
	}

	/*
	 * D.4b: install the kernel-half PT chain at PML4[448]. Runs AFTER
	 * the trampoline alloc above (the leaf PTE references
	 * vm->trampoline_gpa). Idempotent — the helper short-circuits if
	 * it already ran.
	 *
	 * Failure is load-bearing for D.5 (without the chain installed
	 * the trampoline VA is unreachable from any guest CR3 walk and
	 * KVM_RUN's first dispatch will #PF in the guest's GVA→GPA path).
	 * Log and return rc to surface in the initcall log; the boot
	 * itself continues because .vcpu_run is still seccomp_vcpu_run
	 * until D.5 lands. D.5's gate validation will catch a missing
	 * install via cpython-parity / single_dlopen failures.
	 */
	{
		int rc = kvm_v2_kernel_half_install(vm);

		if (rc) {
			pr_err("um: kvm-v2 kernel_half_install: failed (%d) — D.4b PT chain not installed; D.5 dispatch will fail until this is resolved\n",
			       rc);
			return rc;
		}
	}

	/*
	 * Phase E.1 (memo 26 §E.1): install IDT + handler stubs + GDT in
	 * PML4[448]/PUD[0]/PMD[0]/PTE[1..3] and re-issue KVM_SET_SREGS for
	 * every existing pool member to point idt.base / gdt.base at the
	 * new GVAs. Runs AFTER kernel_half_install above (E.1 plugs into
	 * PTE[1..3] of the chain D.4b just built); idempotent — re-runs
	 * short-circuit via vm->idt_kva.
	 *
	 * Failure is load-bearing for E.3.5's flip of .vcpu_run: without
	 * the IDT/GDT in place, any guest exception (#PF on first
	 * instruction fetch is the immediate one) cascades to triple
	 * fault. Today (.vcpu_run still seccomp) the failure is observable
	 * but non-fatal; the substrate gate's seccomp-delegating ops keep
	 * boot moving until E.3.5 lands.
	 */
	{
		int rc = kvm_v2_exception_install(vm);

		if (rc) {
			pr_err("um: kvm-v2 exception_install: failed (%d) — E.1 IDT+GDT not installed; E.3.5 dispatch will triple-fault until this is resolved\n",
			       rc);
			return rc;
		}
	}

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

/*
 * Phase D.2: KVM_EXIT_IO → handle_syscall dispatcher.
 *
 * Called from kvm_v2_vcpu_run's exit-reason switch when the guest
 * trapped via the LSTAR trampoline's `out %al, $0xf4`. The trampoline
 * lives at KVM_V2_LSTAR_GVA (D.1's bytes); the IO port (= 0xf4 =
 * UM_KVM_TRAP_SYSCALL) is the wire-level tag identifying this trap as
 * a syscall (Phase E.3 will add other ports for #PF / #GP / #UD).
 *
 * Marshal logic mirrors v1 archive's kvm_decode_syscall at
 * kvm-v1-archive/thread.c:3270-3319 (and the case-arm at 4030-4047
 * that called it). The shape is:
 *
 *   1. RAX holds the user's syscall NR (LSTAR's `out` did not clobber
 *      it; the SYSCALL instruction itself stashed user RIP into RCX
 *      and user RFLAGS into R11 before jumping to LSTAR).
 *   2. Stash NR into PT_SYSCALL_NR(regs->gp) (= HOST_ORIG_AX) and
 *      flag is_user=1 — handle_syscall reads PT_SYSCALL_NR at its top
 *      (arch/um/kernel/skas/syscall.c:25) and dispatches via
 *      __NR_syscalls.
 *   3. Propagate RCX → HOST_IP and R11 → HOST_EFLAGS so the
 *      downstream view of "where is user" reflects the user's
 *      continuation point, not the LSTAR-internal kernel state. The
 *      bug-fix block at kvm-v1-archive/thread.c:3296-3306 documents
 *      what happens otherwise: kernel-mode RFLAGS (with FMASK applied
 *      — IF/DF/TF/IOPL/NT/AC cleared by SYSCALL's mask) leaks to the
 *      syscall handler and any signal-delivery path, breaking
 *      single-step / direction-flag semantics that user code relies
 *      on.
 *   4. Defensive UPT_SYSCALL_NR(regs) = -1 mirrors seccomp's pattern
 *      at arch/um/backend/seccomp/trap_user.c:149 — handle_syscall
 *      itself overwrites it from PT_SYSCALL_NR at line 25 of
 *      syscall.c, so the assignment below is shape-matching for
 *      cross-backend symmetry rather than load-bearing.
 *
 * GPRs are already populated in regs->gp[] by C.3's
 * kvm_v2_marshal_from_kvm_regs (vcpu.c:kvm_v2_vcpu_run runs the
 * marshal BEFORE the exit-reason switch fires). No KVM_GET_REGS.
 *
 * On return from handle_syscall, regs->gp[HOST_AX] holds the syscall
 * return value. D.3 will marshal it into kvm_run->s.regs.regs.rax
 * and OR KVM_SYNC_X86_REGS into kvm_dirty_regs so the next KVM_RUN
 * delivers the value to user via the trampoline's sysretq. D.2 stops
 * one step short — the marshal-out comment block below is the seam.
 *
 * vcpu_fd is currently unread inside the helper; D.3 / D.4 may want
 * it for explicit ioctls (e.g. KVM_SET_REGS for any field
 * KVM_CAP_SYNC_REGS doesn't cover, or per-trap MSR queries). Reserved
 * here so D.3 does not have to widen the signature.
 *
 * Returns 0 on success or -ENOTSUPP on an unexpected port (Phase E
 * territory). Errors from handle_syscall are not propagated — that
 * function panics rather than returning errno on the conditions
 * relevant to the trap loop, matching the v1 archive's contract.
 */
/*
 * Phase E.3 (memo 26 §E.3): IST-frame parser + per-class dispatchers.
 *
 * Each in-guest IDT handler stub (E.1 wired the bytes; per-class slots
 * documented at syscall_trap.h::KVM_V2_HANDLER_SLOT_*) issues
 * `out %al, $port` after the CPU pushed the standard long-mode iretq
 * frame onto the per-vCPU IST stack (E.2 allocated the page; the TSS's
 * IST1 field points at ist_stack_top_gva). Layout per Intel SDM Vol.3
 * §6.13 ("Error Code") and §6.14 ("Exception and Interrupt Handling
 * in 64-bit Mode"):
 *
 *   ist_stack_top - 8:    user SS
 *   ist_stack_top - 16:   user RSP
 *   ist_stack_top - 24:   user RFLAGS
 *   ist_stack_top - 32:   user CS
 *   ist_stack_top - 40:   user RIP
 *   ist_stack_top - 48:   error_code  (only for vectors that push one)
 *
 * Vectors WITH error code (SDM Vol.3 §6.13 "Error Code"):
 *   #DF (8), #TS (10), #NP (11), #SS (12), #GP (13), #PF (14),
 *   #AC (17), #SX (30).
 * Vectors WITHOUT error code:
 *   #DE (0), #DB (1), #BP (3), #OF (4), #BR (5), #UD (6), #NM (7),
 *   #MF (16), #XM (19).
 *
 * #PF and #GP push error code; #UD, #DE, and #OF do not. The frame
 * lives in HOST kernel virtual memory (vcpu->ist_stack_kva is the kva
 * of the page we allocated in E.2; it's regular kernel memory — no
 * KVM ioctl needed to read it). After the host dispatcher consumes
 * the frame and runs UML's existing trap.c handler, we marshal the
 * (possibly mutated) regs back into the IST frame so the in-guest
 * iretq pops the post-handler RIP/RSP/RFLAGS — the marshal-back is
 * how a signal-delivered RIP (do_signal stashed a handler VA into
 * regs->gp[HOST_IP]) lands as the next user-mode instruction.
 *
 * v1 archive references (memo 27 §A.7 lift requirement):
 *   - kvm-v1-archive/thread.c:4058-4173 — #PF dispatcher (cr2 read,
 *     IST frame parse, regs marshal, fault-fix call). v1 read CR2 via
 *     KVM_GET_SREGS; v2 reads from kvm_run->s.regs.sregs.cr2 (sync-
 *     regs is enabled at vcpu_create per Phase C.3 — KVM populates
 *     cr2 in the mmap on every exit, see arch/x86/kvm/x86.c:12122
 *     and 12748 per the codex audit CLAIM E refutation).
 *   - kvm-v1-archive/thread.c:4530-4555 — #PF dispatch into UML
 *     SIGSEGV path (sig_info[SIGSEGV] → segv_handler).
 *   - kvm-v1-archive/thread.c:4572-4636 — #GP dispatcher (same shape
 *     as #PF, trap_no=13, dispatched as SIGSEGV — segv_handler's
 *     SEGV_IS_FIXABLE check at trap.c:297 returns false for trap_no
 *     != 14, so the kernel-fault path is bypassed and bad_segv runs).
 *   - kvm-v1-archive/thread.c:4637-4695 — #DE/#BP/#OF/#UD dispatch
 *     (no error code, smaller IST frame, signal mapping per port).
 *
 * Codex audit CLAIM E (CONTRADICTED): the original §E.3 prose specced
 * each handler stub doing `mov %cr2, %rax` to expose CR2 to the host.
 * That clobbers user RAX before the host captures vCPU state on
 * KVM_EXIT_IO. The audit refutation lifted v1's pattern: handler
 * stubs are pure `out %al, $port ; iretq` (4 bytes), and the host
 * reads CR2 from sync-regs sregs.cr2. v1 archive at thread.c:1297
 * documents the same removal.
 */

/*
 * Read the IRETQ frame from the IST stack page. `top` is the host
 * kernel VA of the byte one past the highest-numbered byte the CPU
 * could push (i.e. ist_stack_kva + PAGE_SIZE — stacks grow down so
 * the first push lands at top - 8). `has_error_code` selects the
 * 48-byte (with EC) vs 40-byte (without EC) frame layout.
 *
 * No bounds check on the read range — the page is 4096 bytes and we
 * read at most 48 bytes from the top, well within the allocation.
 */
struct kvm_v2_ist_frame {
	u64  user_rip;
	u64  user_cs;
	u64  user_rflags;
	u64  user_rsp;
	u64  user_ss;
	u64  error_code;	/* valid only when has_error_code */
	bool has_error_code;
};

static void kvm_v2_ist_frame_read(struct kvm_v2_vcpu *vcpu,
				  struct kvm_v2_ist_frame *f,
				  bool has_error_code)
{
	u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;

	/*
	 * Per Intel SDM Vol.3 §6.14.5, the long-mode IDT-pushed frame
	 * (with error code) lays out from bottom-of-frame upward:
	 *   +0:  error_code (only present when the vector pushes one)
	 *   +8:  RIP
	 *   +16: CS  (sign-extended u16 + 6 padding bytes)
	 *   +24: RFLAGS
	 *   +32: RSP
	 *   +40: SS  (sign-extended u16 + 6 padding bytes)
	 * Without error code, RIP is at the bottom and the rest shifts
	 * down by 8.
	 *
	 * E.5 fix: previous code used `off = has_error_code ? 48 : 40`
	 * and read RIP at `(top - off) + 0`, which lands on the
	 * error_code slot for has_error_code=true (every field shifted
	 * by 8 — RIP picked up the error code, RSP picked up RFLAGS,
	 * etc.). Boot smoke caught it: handle_io_pf saw RIP=0x10
	 * (error code) and RSP=0x10002 (RFLAGS with bit-16 RF set
	 * during #PF delivery). Use a fixed 40-byte "RIP-anchored"
	 * frame and read the error code at -8 below it when present.
	 */
	f->has_error_code = has_error_code;
	if (has_error_code)
		f->error_code = *(u64 *)(top - 48);
	else
		f->error_code = 0;

	f->user_rip    = *(u64 *)(top - 40 +  0);
	f->user_cs     = *(u64 *)(top - 40 +  8);
	f->user_rflags = *(u64 *)(top - 40 + 16);
	f->user_rsp    = *(u64 *)(top - 40 + 24);
	f->user_ss     = *(u64 *)(top - 40 + 32);
}

/*
 * Marshal (possibly mutated) regs back into the IST frame so the
 * in-guest iretq at the tail of the handler stub pops the post-
 * handler user state. We only rewrite RIP / RFLAGS / RSP — CS / SS
 * / error_code are untouched because:
 *   - CS / SS: signal delivery doesn't change the segment selector;
 *              user code stays at the same CPL.
 *   - error_code: iretq pops error_code BEFORE RIP per SDM Vol.3
 *                 §6.14.5; the value we read into the frame is what
 *                 will be popped + discarded. Rewriting it is a no-op.
 */
static void kvm_v2_ist_frame_write(struct kvm_v2_vcpu *vcpu,
				   const struct uml_pt_regs *regs,
				   bool has_error_code)
{
	u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;
	u64 cs, ss;
	struct arch_thread *a = &current->thread.arch;

	(void)has_error_code;

	/*
	 * Layout matches kvm_v2_ist_frame_read: RIP-anchored frame
	 * starts at top - 40 regardless of whether the vector pushed
	 * an error code. The error code (when present) lives at
	 * top - 48 and is popped+discarded by the in-guest iretq;
	 * we leave that slot alone.
	 */
	*(u64 *)(top - 40 +  0) = regs->gp[HOST_IP];      /* RIP */
	cs = *(u64 *)(top - 40 +  8);                     /* CS unchanged */
	*(u64 *)(top - 40 + 16) = regs->gp[HOST_EFLAGS];  /* RFLAGS */
	*(u64 *)(top - 40 + 24) = regs->gp[HOST_SP];      /* RSP */
	ss = *(u64 *)(top - 40 + 32);                     /* SS unchanged */

	/*
	 * H.1b residual fix: snapshot the IST frame into per-task
	 * storage so the next time THIS task's vcpu_run runs KVM_RUN,
	 * we can restore the IST stack to OUR frame — even if other
	 * UML tasks ran on the same vCPU in between and clobbered the
	 * shared IST stack with their own exception delivery frames.
	 *
	 * Without this, multi-thread workloads where two pthreads
	 * sharing one mm both take #PFs alternately end up with one
	 * task's iretq popping the OTHER task's CS/RIP, manifesting
	 * as "user code running at CPL=0" or wrong-task RIP jumps —
	 * the InterpreterPool repro signature (~50% flake on
	 * test_struct's test_endian_table_init_subinterpreters).
	 */
	a->kvm_v2.ist_frame[0] = (has_error_code ? *(u64 *)(top - 48) : 0);
	a->kvm_v2.ist_frame[1] = regs->gp[HOST_IP];
	a->kvm_v2.ist_frame[2] = cs;
	a->kvm_v2.ist_frame[3] = regs->gp[HOST_EFLAGS];
	a->kvm_v2.ist_frame[4] = regs->gp[HOST_SP];
	a->kvm_v2.ist_frame[5] = ss;
	a->kvm_v2.ist_pending = true;
}

/*
 * Restore per-task IST frame snapshot into the current vCPU's IST
 * stack. Called from kvm_v2_vcpu_run BEFORE KVM_RUN re-entry for
 * tasks that have a pending IST frame queued (i.e., last exit was
 * an exception class delivered via IDT IST, not a SYSCALL trap).
 *
 * Defends against the cross-task IST clobber described above.
 */
void kvm_v2_ist_frame_restore_pending(struct kvm_v2_vcpu *vcpu)
{
	struct arch_thread *a = &current->thread.arch;
	u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;

	if (!a->kvm_v2.ist_pending)
		return;

	*(u64 *)(top - 48)      = a->kvm_v2.ist_frame[0]; /* error_code */
	*(u64 *)(top - 40 +  0) = a->kvm_v2.ist_frame[1]; /* RIP */
	*(u64 *)(top - 40 +  8) = a->kvm_v2.ist_frame[2]; /* CS */
	*(u64 *)(top - 40 + 16) = a->kvm_v2.ist_frame[3]; /* RFLAGS */
	*(u64 *)(top - 40 + 24) = a->kvm_v2.ist_frame[4]; /* RSP */
	*(u64 *)(top - 40 + 32) = a->kvm_v2.ist_frame[5]; /* SS */

	a->kvm_v2.ist_pending = false;
}

/*
 * #121-D15 fix (2026-05-01): snapshot the IDT-pushed exception frame
 * verbatim from the IST stack into per-task arch_thread storage. See
 * kvm_v2_backend.h declaration for the full rationale. This is the
 * raw counterpart to kvm_v2_ist_frame_write — the latter writes a
 * UML-side-mutated RIP/RSP (post-segv_handler) for the iretq tail to
 * pop; this one preserves the hardware-pushed frame as-is for the
 * EINTR-mid-IDT-delivery case where handle_io_pf never ran and the
 * stub's iretq tail (which will run on resume) needs the original
 * IDT frame.
 *
 * Always assumes has_error_code=true (matches the #PF / #GP slots
 * that are the only handler stubs reachable mid-delivery in current
 * v2 — the no-error-code stubs at slots DE/BP/OF/UD/NM also pushed
 * a frame but with one fewer qword; treating those as has_error_code
 * for snapshot purposes leaves an extra qword in slot[0] that is
 * unused on restore; restore writes top-48 = ist_frame[0] which lands
 * at the same physical slot the CPU would have used for the next
 * frame above the no-error-code frame, harmless for those vectors).
 */
void kvm_v2_ist_frame_snapshot_raw(struct kvm_v2_vcpu *vcpu)
{
	struct arch_thread *a = &current->thread.arch;
	u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;

	a->kvm_v2.ist_frame[0] = *(u64 *)(top - 48);          /* error_code */
	a->kvm_v2.ist_frame[1] = *(u64 *)(top - 40 +  0);     /* RIP */
	a->kvm_v2.ist_frame[2] = *(u64 *)(top - 40 +  8);     /* CS */
	a->kvm_v2.ist_frame[3] = *(u64 *)(top - 40 + 16);     /* RFLAGS */
	a->kvm_v2.ist_frame[4] = *(u64 *)(top - 40 + 24);     /* RSP */
	a->kvm_v2.ist_frame[5] = *(u64 *)(top - 40 + 32);     /* SS */
	a->kvm_v2.ist_pending = true;
}

/*
 * #121-D15 SMP follow-up: PF dispatch from the EINTR path when
 * EINTR caught us mid-IDT-delivery (CPU pushed the IDT frame and
 * set RIP=stub_start, but the stub's first instruction never ran).
 * Process the PF here directly so the stub never re-runs — the
 * stub-replay path that the bare cr2-snapshot/IST-snapshot pair
 * sets up is fragile under SMP because the saved RSP points at the
 * vCPU we EINTR'd from, but on the next dispatch the task may
 * resume on a DIFFERENT vCPU with a different IST page (each vCPU
 * has its own IST GVA). Bypassing the stub avoids the cross-vCPU
 * RSP fixup entirely.
 *
 * Caller MUST be preempt-disabled (and is — kvm_v2_vcpu_run
 * preempt_disables before KVM_RUN, doesn't preempt_enable until
 * after this returns) so the IDT frame on `vcpu->ist_stack_kva` is
 * still ours. cr2 is passed in (read from eintr_sregs.cr2 by the
 * caller; the run->s.regs.sregs.cr2 mmap may have been overwritten
 * by another task between EINTR and our dispatch, so we can't read
 * it here).
 */
int kvm_v2_handle_pf_eintr_inline(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu,
				  u64 cr2)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, true /* has_error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	regs->faultinfo.error_code = (int)frame.error_code;
	regs->faultinfo.cr2        = cr2;
	regs->faultinfo.trap_no    = 14;

	trace_um_backend_kvm_v2_iotrap_pf(cr2,
					  frame.error_code,
					  frame.user_rip);

	segv_handler(SIGSEGV, NULL, regs, NULL);
	interrupt_end();

	kvm_v2_ist_frame_write(vcpu, regs, true /* has_error_code */);
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	return 0;
}

/*
 * #PF (vector 14): page-fault dispatcher. CR2 from sync-regs sregs;
 * error_code + user RIP/RSP/RFLAGS from the IST frame. Dispatches
 * via UML's segv_handler (arch/um/kernel/trap.c:292) — same path the
 * seccomp backend takes from its SIGSEGV branch
 * (arch/um/backend/seccomp/trap_user.c:154). segv_handler walks
 * SEGV_IS_FIXABLE (faultinfo.trap_no==14 → true), routes to segv()
 * which calls handle_page_fault for fault-fix or signals a SIGSEGV
 * via force_sig_fault on unfixable faults.
 *
 * regs->is_user = 1 BEFORE dispatch (codex audit independent
 * finding, mirrored from v1 archive thread.c:4553): the IDT[14] gate
 * fires on guest CPL=3 user faults; by the time the in-guest handler
 * stub runs we're at CPL=0 in ring-0 IST context. UML's segv_handler
 * reads UPT_IS_USER(regs) and panics with "Kernel tried to access
 * user memory" if it sees is_user=0 with a user-range CR2. Tag it
 * unconditionally — the FAULTING access was at CPL=3 even though
 * the in-guest delivery handler runs at CPL=0.
 */
static int kvm_v2_handle_io_pf(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;
	u64 cr2;

	kvm_v2_ist_frame_read(vcpu, &frame, true /* has_error_code */);

	/*
	 * KVM populates s.regs.sregs.cr2 on every KVM_EXIT_IO when
	 * KVM_SYNC_X86_SREGS is in kvm_valid_regs (Phase C.3 set this
	 * at vcpu_create). Codex audit CLAIM E refutation: the kernel
	 * commits cr2 to the sync-regs mmap at arch/x86/kvm/x86.c:12122
	 * + 12748 — no separate KVM_GET_SREGS ioctl needed.
	 */
	cr2 = run->s.regs.sregs.cr2;

	/*
	 * #121 instrumentation (2026-05-01): the PF stub captures CR2 to
	 * IST page byte offset PAGE_SIZE - 64 BEFORE the `out` vmexit. Read
	 * the captured value and prefer it over sregs.cr2 when sregs.cr2 is
	 * zero (rare race observed in mt-byteset N>=2: KVM exits with
	 * vcpu->arch.cr2=0 despite a real fault delivered with non-zero
	 * fault address — see exception.c kvm_v2_handler_stub_pf comment).
	 *
	 * The captured slot lives at vcpu->ist_stack_kva + PAGE_SIZE - 64,
	 * outside the iretq frame area (which occupies the top 48 bytes).
	 * This is per-vCPU (single-vCPU today; SMP needs per-CPU IST so
	 * still per-vCPU).
	 *
	 * If captured != sregs.cr2, log once-per-boot to localize the race.
	 * If sregs.cr2 == 0 but captured != 0, use captured — that's the
	 * rescue path that prevents the false NULL-deref signal.
	 */
	{
		u64 captured_cr2 = *(u64 *)((u8 *)vcpu->ist_stack_kva +
					    PAGE_SIZE - 64);
		u64 captured_rdx = *(u64 *)((u8 *)vcpu->ist_stack_kva +
					    PAGE_SIZE - 72);
		u64 sentinel = *(u64 *)((u8 *)vcpu->ist_stack_kva +
					PAGE_SIZE - 80);
		static int diag_seen;
		if (cr2 == 0 && !memcmp(current->comm, "mt-", 3) &&
		    diag_seen < 30) {
			diag_seen++;
			pr_emerg("um: kvm-v2 #PF[%d] cr2=0 captured=%llx stub_rdx=%llx regs_rdx=%lx sentinel=%llx err=%llx user_rip=%llx pid=%d\n",
				 diag_seen,
				 (unsigned long long)captured_cr2,
				 (unsigned long long)captured_rdx,
				 regs->gp[HOST_DX],
				 (unsigned long long)sentinel,
				 (unsigned long long)frame.error_code,
				 (unsigned long long)frame.user_rip,
				 current->pid);
		}
		/*
		 * G.2-fix diag: catch low-addr (1..0xff) #PFs from mt- tasks.
		 * These are the smoking-gun symptom for the T>=N stress
		 * flake. Log full register state so we can correlate with
		 * mt-mini-diag's user-side capture.
		 */
		{
			static int diag_low_pf;
			u64 effective_cr2 = (cr2 != 0) ? cr2 : captured_cr2;
			if (effective_cr2 > 0 && effective_cr2 < 0x100 &&
			    !memcmp(current->comm, "mt-", 3) &&
			    diag_low_pf < 20) {
				diag_low_pf++;
				pr_emerg("um: kvm-v2 LOW-PF[%d] pid=%d cr2=%llx err=%llx user_rip=%llx user_rsp=%llx user_rbp_or_rdx=%lx gp[BP]=%lx gp[SP]=%lx gp[DX]=%lx gp[AX]=%lx gp[DI]=%lx\n",
					 diag_low_pf, current->pid,
					 (unsigned long long)effective_cr2,
					 (unsigned long long)frame.error_code,
					 (unsigned long long)frame.user_rip,
					 (unsigned long long)frame.user_rsp,
					 regs->gp[HOST_DX],
					 regs->gp[HOST_BP],
					 regs->gp[HOST_SP],
					 regs->gp[HOST_DX],
					 regs->gp[HOST_AX],
					 regs->gp[HOST_DI]);
			}
		}
		if (cr2 == 0 && captured_cr2 != 0)
			cr2 = captured_cr2;
		/*
		 * #121 defense (2026-05-01): when CR2 is genuinely 0 (sentinel
		 * proved in-stub captured CR2 register reads as 0 — see
		 * commit f74a05655b40), the fault address is lost. For data
		 * write faults (err bit 1 = W) where the user's RDX register
		 * holds a plausible user-space pointer, prefer RDX as the
		 * fault address. Common case: slow_memset's mov %al, (%rdx)
		 * where the CPU's CR2 update was somehow lost between fault
		 * delivery and the in-guest stub running. Heuristic — only
		 * applies when:
		 *   - cr2 still 0 after captured fallback above
		 *   - error_code bit 1 (W) is set (write fault)
		 *   - captured_rdx points into typical user-space range
		 *     (>0x10000 to skip null/low addresses)
		 */
		if (cr2 == 0 && (frame.error_code & 0x2) &&
		    captured_rdx > 0x10000) {
			cr2 = captured_rdx;
		}
	}

	/*
	 * Marshal user state into regs. C.3's marshal_from_kvm_regs
	 * already populated regs->gp[] from kvm_run->s.regs.regs at
	 * the top of vcpu.c::kvm_v2_vcpu_run — but RIP/RSP/RFLAGS in
	 * there reflect the IDT-handler-mid-execution state (RIP =
	 * just-after-`out` in the handler stub; RSP = somewhere in the
	 * IST page; RFLAGS = post-IDT-gate-fired). The IST frame holds
	 * the USER state from the moment the fault fired — overwrite
	 * the GP[] view so UML's downstream handlers see "where user
	 * code was when it faulted" not "where the in-guest stub
	 * happens to be".
	 */
	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	/*
	 * faultinfo per arch/x86/um/shared/sysdep/faultinfo_64.h:
	 *   error_code — CPU-pushed bits 0..4 (P, W/R, U/S, RSVD, I/D)
	 *                per SDM Vol.3 §6.15. UML's FAULT_WRITE macro
	 *                tests bit 1 (W); handle_page_fault inside
	 *                segv() consumes is_write to gate VM_WRITE
	 *                permission checks.
	 *   cr2        — faulting linear address (the "FAULT_ADDRESS"
	 *                segv() walks down).
	 *   trap_no    — 14 (#PF). SEGV_IS_FIXABLE checks trap_no==14
	 *                so segv_handler's user-fault branch fires.
	 */
	regs->faultinfo.error_code = (int)frame.error_code;
	regs->faultinfo.cr2        = cr2;
	regs->faultinfo.trap_no    = 14;

	trace_um_backend_kvm_v2_iotrap_pf(cr2,
					  frame.error_code,
					  frame.user_rip);

	/*
	 * Dispatch into UML's existing handler. segv_handler ignores
	 * its `unused_si` argument (signature comment at trap.c:282) so
	 * NULL is correct here. Mirror v1 archive's call shape at
	 * thread.c:4554: `(*sig_info[SIGSEGV])(SIGSEGV, NULL, regs,
	 * NULL);`. We bypass sig_info[] and call segv_handler directly
	 * because the routing table maps SIGSEGV→segv_handler anyway
	 * (arch/um/os-Linux/signal.c:33) and a direct call lets the
	 * compiler inline / static-resolve.
	 */
	segv_handler(SIGSEGV, NULL, regs, NULL);

	/*
	 * Drain UML's pending signal/scheduler work BEFORE marshaling
	 * regs back to the IST frame. Without this, an unfixable
	 * SIGSEGV (e.g., user accessed an unmapped VA with no VMA, or
	 * write to RO page that copy-on-write can't satisfy) gets
	 * QUEUED on current's task_struct but never delivered to user
	 * mode — the user task re-enters at the same faulting RIP via
	 * iretq, faults again, and we infinite-loop.
	 *
	 * seccomp's equivalent path at
	 * arch/um/backend/seccomp/trap_user.c:184 calls interrupt_end()
	 * after each signal-class dispatch for exactly this reason. Per
	 * the kernel/process.c interrupt_end definition, this drains:
	 *   - resume_user_mode_work (signal delivery, including
	 *     do_signal which sets up sigframes for default-action
	 *     terminate signals like SIGSEGV / SIGBUS)
	 *   - schedule (if TIF_NEED_RESCHED is set)
	 *   - cgroup-threadgroup change end
	 *
	 * The bug surfaced under v2 boot smoke when Python's dynamic
	 * linker faulted in a not-present page that UML couldn't fix
	 * (likely a glibc CoW corner case): the SIGSEGV-loop-on-same-RIP
	 * pattern reproduced reliably with `python3 -c "import hashlib"`
	 * but not with `python3 -c "print('hi')"`.
	 *
	 * After interrupt_end, regs->gp[HOST_IP] may have been rewritten
	 * by do_signal to point at a signal-handler VA (case b in the
	 * comment below); the IST frame write then propagates that to
	 * the iretq pop-source.
	 */
	interrupt_end();

	/*
	 * After segv_handler + interrupt_end, regs->gp[HOST_IP] either:
	 *   (a) stayed at frame.user_rip — fault fixed lazily; iretq
	 *       resumes at the original faulting instruction, which
	 *       now succeeds.
	 *   (b) was rewritten to a signal handler VA — do_signal
	 *       inside interrupt_end set up a sigframe + return-to-
	 *       handler address; iretq lands at the handler, which
	 *       runs to completion and then sigreturns to (a)-style
	 *       state.
	 *   (c) the task is being terminated (default-action SIGSEGV
	 *       with no handler); on next KVM_RUN do_exit fires.
	 * Either way, marshal regs back into the IST frame so the
	 * in-guest iretq tail of the handler stub pops the right state.
	 */
	kvm_v2_ist_frame_write(vcpu, regs, true /* has_error_code */);

	/*
	 * Marshal regs->gp[] back into kvm_run->s.regs.regs and OR
	 * KVM_SYNC_X86_REGS into kvm_dirty_regs. Required because the
	 * SYSCALL-style D.2 dispatcher does the same; KVM resumes the
	 * vCPU with the dirty-bit'd state on the next KVM_RUN, and the
	 * in-guest iretq reads its pop-source from RSP (which we already
	 * patched in the IST frame above) — but downstream UML code may
	 * have updated other GPRs (e.g. signal-delivery sets RDI/RSI to
	 * sigaction args), and those need to land in the user-resume
	 * register file.
	 */
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	return 0;
}

/*
 * #GP (vector 13): general-protection-fault dispatcher. Same shape
 * as #PF (CPU pushes error code) except trap_no=13 and there's no
 * cr2 to read. Dispatches as SIGSEGV via segv_handler;
 * SEGV_IS_FIXABLE returns false (trap_no != 14) so segv_handler's
 * user-fault path falls through to bad_segv → force_sig_fault
 * (SIGSEGV). v1 archive mirror: thread.c:4572-4636.
 */
static int kvm_v2_handle_io_gp(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, true /* has_error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	regs->faultinfo.error_code = (int)frame.error_code;
	regs->faultinfo.cr2        = 0;
	regs->faultinfo.trap_no    = 13;

	trace_um_backend_kvm_v2_iotrap_gp(frame.error_code, frame.user_rip);

	/*
	 * SIGSEGV via segv_handler: same as #PF dispatch but
	 * SEGV_IS_FIXABLE check (trap.c:297) returns false because
	 * trap_no != 14, so the bad_segv path runs and force_sig_fault
	 * delivers SIGSEGV with si_code computed from the fault info.
	 */
	segv_handler(SIGSEGV, NULL, regs, NULL);

	/* Drain pending signal/scheduler work — see #PF handler comment. */
	interrupt_end();

	kvm_v2_ist_frame_write(vcpu, regs, true /* has_error_code */);
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	return 0;
}

/*
 * Common helper for #UD/#DE/#OF — vectors WITHOUT error code that
 * dispatch through relay_signal (arch/um/kernel/trap.c:418).
 * relay_signal dereferences `si->si_code` and `si->si_errno`, so we
 * construct a stack-allocated kernel_siginfo with sensible si_code
 * (which selects SIL_FAULT layout in siginfo_layout, routing to
 * force_sig_fault with FAULT_ADDRESS(faultinfo) = cr2 = 0 — the
 * trap_no field disambiguates the actual cause for downstream code
 * that examines current->thread.arch.faultinfo).
 *
 * The si_codes chosen (ILL_ILLOPN / FPE_INTOVF / SEGV_MAPERR) are
 * defensive defaults — UML's relay_signal forwards them verbatim
 * via force_sig_fault, and userspace handlers reading siginfo see
 * a plausible cause. v1 archive at thread.c:4694 passed NULL,
 * which would NULL-deref relay_signal at trap.c:456; v2 fixes
 * that latent bug by constructing a real siginfo.
 */
static void kvm_v2_dispatch_relay(struct uml_pt_regs *regs, int sig,
				  int si_code)
{
	kernel_siginfo_t info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_code  = si_code;
	info.si_errno = 0;
	relay_signal(sig, (struct siginfo *)&info, regs, NULL);
}

/*
 * #UD (vector 6): undefined-opcode dispatcher. No error code.
 * Delivers SIGILL via relay_signal. UML's userspace handler sees
 * si_code = ILL_ILLOPN. v1 archive mirror: thread.c:4659-4694
 * (port UM_KVM_UD_PORT case-arm).
 */
static int kvm_v2_handle_io_ud(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	regs->faultinfo.error_code = 0;
	regs->faultinfo.cr2        = 0;
	regs->faultinfo.trap_no    = 6;

	trace_um_backend_kvm_v2_iotrap_ud(frame.user_rip);

	kvm_v2_dispatch_relay(regs, SIGILL, ILL_ILLOPN);

	/* Drain pending signal/scheduler work — see #PF handler comment. */
	interrupt_end();

	kvm_v2_ist_frame_write(vcpu, regs, false /* no error_code */);
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	return 0;
}

/*
 * #DE (vector 0): divide-error dispatcher. No error code.
 * Delivers SIGFPE via relay_signal with si_code=FPE_INTOVF.
 * v1 archive mirror: thread.c:4659 (UM_KVM_DE_PORT case).
 */
static int kvm_v2_handle_io_de(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	regs->faultinfo.error_code = 0;
	regs->faultinfo.cr2        = 0;
	regs->faultinfo.trap_no    = 0;

	trace_um_backend_kvm_v2_iotrap_de(frame.user_rip);

	kvm_v2_dispatch_relay(regs, SIGFPE, FPE_INTOVF);

	/* Drain pending signal/scheduler work — see #PF handler comment. */
	interrupt_end();

	kvm_v2_ist_frame_write(vcpu, regs, false /* no error_code */);
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	return 0;
}

/*
 * #OF (vector 4): overflow dispatcher. No error code. Triggered by
 * the `into` instruction (legacy; rare in modern x86_64 since
 * `into` is invalid in long mode — the IDT gate is here for
 * defense-in-depth only). Delivers SIGSEGV via relay_signal —
 * matches v1 archive's mapping at thread.c:4661 (UM_KVM_OF_PORT
 * case → SIGSEGV).
 */
static int kvm_v2_handle_io_of(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	regs->faultinfo.error_code = 0;
	regs->faultinfo.cr2        = 0;
	regs->faultinfo.trap_no    = 4;

	trace_um_backend_kvm_v2_iotrap_of(frame.user_rip);

	/*
	 * #OF maps to SIGSEGV per v1 archive (thread.c:4661). Use
	 * relay_signal not segv_handler — the latter expects trap_no=14
	 * for the SEGV_IS_FIXABLE branch, and #OF has trap_no=4. Going
	 * through relay_signal lands the signal directly without the
	 * fault-fix detour.
	 */
	kvm_v2_dispatch_relay(regs, SIGSEGV, SEGV_MAPERR);

	/* Drain pending signal/scheduler work — see #PF handler comment. */
	interrupt_end();

	kvm_v2_ist_frame_write(vcpu, regs, false /* no error_code */);
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	return 0;
}

/*
 * Default-stub dispatcher: any vector E.1's IDT didn't wire to a
 * specific handler points at the panic stub which fires
 * UM_KVM_TRAP_PANIC = 0xf8. Best-effort: dump the frame contents and
 * panic. We don't know whether the firing vector pushed an error
 * code, so we read the no-error-code layout (smaller, safer if we
 * read past the actual frame we'd just see zeroes from __GFP_ZERO).
 *
 * Phase E covers #DE/#BP/#OF/#UD/#GP/#PF; E.1 wires the panic stub
 * for every other vector in the 0..255 range. Hitting this path
 * means either:
 *   (a) a vector E.1 didn't intend to deliver fired (#DF, #DB, #NM,
 *       etc.) — usually a guest-state bug or a host-side bit-rot in
 *       the trampoline / SREGS path.
 *   (b) the panic stub itself was reached via an IDT misconfiguration
 *       (gate-offset or selector wrong).
 * Either way, panic with diagnostics.
 */
static int kvm_v2_handle_io_panic(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	(void)regs;
	kvm_v2_ist_frame_read(vcpu, &frame, false /* unknown — best-effort */);

	trace_um_backend_kvm_v2_iotrap_panic(run->io.port, frame.user_rip);

	panic("kvm-v2: unhandled exception (port=%#x cpu=%d) user_rip=%#llx user_cs=%#llx user_rflags=%#llx user_rsp=%#llx",
	      run->io.port, vcpu->cpu,
	      frame.user_rip, frame.user_cs,
	      frame.user_rflags, frame.user_rsp);
}

int kvm_v2_handle_io_trap(struct uml_pt_regs *regs,
			  struct kvm_run *run,
			  struct kvm_v2_vcpu *vcpu)
{
	unsigned long syscall_nr;

	if (!vcpu)
		return -EINVAL;

	/*
	 * Phase E.3: extend the dispatch table to the per-class
	 * exception ports E.1's IDT handler stubs emit on. The
	 * SYSCALL arm is the D.2 path; PF/GP/UD/DE/OF land in
	 * the helpers above; UM_KVM_TRAP_PANIC + any other
	 * unrecognised port falls to the panic helper.
	 */
	switch (run->io.port) {
	case UM_KVM_TRAP_SYSCALL:
		break;	/* fall through to SYSCALL handler below */
	case UM_KVM_TRAP_PF:
		return kvm_v2_handle_io_pf(regs, run, vcpu);
	case UM_KVM_TRAP_GP:
		return kvm_v2_handle_io_gp(regs, run, vcpu);
	case UM_KVM_TRAP_UD:
		return kvm_v2_handle_io_ud(regs, run, vcpu);
	case UM_KVM_TRAP_DE:
		return kvm_v2_handle_io_de(regs, run, vcpu);
	case UM_KVM_TRAP_OF:
		return kvm_v2_handle_io_of(regs, run, vcpu);
	case UM_KVM_TRAP_PANIC:
	default:
		return kvm_v2_handle_io_panic(regs, run, vcpu);
	}

	/*
	 * C.3's marshal-from-kvm-regs already populated regs->gp[] from
	 * kvm_run->s.regs.regs (sync_regs path) before this helper was
	 * called — see vcpu.c:kvm_v2_vcpu_run (post-KVM_RUN marshal
	 * runs before the exit-reason switch fires). RAX holds the
	 * user's syscall NR.
	 */
	syscall_nr = regs->gp[HOST_AX];

	/*
	 * Stash NR into the SYSCALL_NR slot handle_syscall reads, and
	 * flag user-mode entry. Mirrors v1 archive's kvm_decode_syscall
	 * at kvm-v1-archive/thread.c:3284-3285.
	 */
	PT_SYSCALL_NR(regs->gp) = syscall_nr;
	regs->is_user = 1;

	/*
	 * Post-SYSCALL semantics: user RIP arrives in RCX, user RFLAGS
	 * in R11 (saved by the SYSCALL instruction itself before the
	 * CPU jumped to MSR_LSTAR). For handle_syscall + downstream
	 * signal-delivery to see the right "user is at" state, copy
	 * CX→IP and R11→EFLAGS. The bug-fix comment block at
	 * kvm-v1-archive/thread.c:3296-3306 explains the consequence of
	 * skipping this: kernel-mode RFLAGS (with FMASK applied —
	 * IF/DF/TF/IOPL/NT/AC cleared) leaks into the syscall handler's
	 * view of user state, breaking direction-flag and single-step
	 * semantics that user-mode relies on across the SYSCALL
	 * boundary.
	 */
	regs->gp[HOST_IP]     = regs->gp[HOST_CX];
	regs->gp[HOST_EFLAGS] = regs->gp[HOST_R11];

	/*
	 * Defensive default per seccomp's pattern at
	 * arch/um/backend/seccomp/trap_user.c:149.  handle_syscall
	 * re-reads PT_SYSCALL_NR into UPT_SYSCALL_NR at its top
	 * (arch/um/kernel/skas/syscall.c:25), so this assignment is
	 * overwritten immediately. We still set it for cross-backend
	 * symmetry — anyone diffing seccomp vs kvm-v2 marshal paths
	 * should see the same shape on both sides.
	 */
	UPT_SYSCALL_NR(regs) = -1;

	trace_um_backend_kvm_v2_iotrap_syscall_enter(run->io.port,
						     syscall_nr);

	handle_syscall(regs);

	/*
	 * Drain UML's pending signal/scheduler work AFTER the syscall
	 * returns. seccomp's analogous path at
	 * arch/um/backend/seccomp/trap_user.c:184 calls interrupt_end()
	 * for the same reason: handle_syscall may set the syscall return
	 * value to -ERESTARTSYS / -ERESTARTNOINTR / -ERESTARTNOHAND when
	 * the syscall was interrupted by a signal. interrupt_end runs
	 * resume_user_mode_work which fires do_signal — and do_signal's
	 * handle_signal / restart_syscall path translates the kernel-
	 * internal -ERESTART* codes into either:
	 *   (a) syscall restart (rewind RIP to the SYSCALL instruction)
	 *       if the pending signal has SA_RESTART or no handler is
	 *       installed; OR
	 *   (b) -EINTR as the user-visible return value otherwise.
	 *
	 * Without this, -ERESTARTSYS (errno 512) leaks to userspace.
	 * Caught running `dash` builtin echo after a fork+wait under v2:
	 * waitpid sets HOST_AX = -ERESTARTSYS via SIGCHLD; the next
	 * write(1, ...) in dash also returns -512 because handle_syscall
	 * preserved the sentinel through to the marshal-out path, with
	 * the user seeing every subsequent shell write fail.
	 *
	 * The reverted commit ad06c7f5164c added this same call but at
	 * that time the substrate gate showed a SYSRETQ-RIP regression.
	 * The marshal-out below explicitly overwrites run->s.regs.regs.rcx
	 * with regs->gp[HOST_IP] (and r11 with HOST_EFLAGS) so the
	 * SYSRETQ-pop-RIP-from-RCX semantics align with whatever do_signal
	 * left in HOST_IP — restart, signal-handler entry, or unchanged
	 * post-syscall RIP. With that overwrite present, the interrupt_end
	 * call is safe to re-introduce.
	 *
	 * Place BEFORE the PT_SYSCALL_NR clear below: do_signal's
	 * restart-syscall logic only fires when PT_REGS_SYSCALL_NR(regs)
	 * >= 0, which it is at this point (handle_syscall did not clear
	 * it). After interrupt_end runs, clearing PT_SYSCALL_NR ensures
	 * later exception-path interrupt_end() calls don't see a stale
	 * syscall NR + leaked -ERESTART* residual (commit a478952b8da0).
	 */
	interrupt_end();

	/*
	 * Mirror seccomp's pattern at arch/um/backend/seccomp/trap_user.c:
	 * 187-188: clear PT_SYSCALL_NR after handle_syscall returns, so
	 * that a LATER exception-path interrupt_end() (in the PF/GP/UD/DE/
	 * OF dispatchers, added at 31ba9c354063) doesn't misinterpret the
	 * stale `orig_ax + -ERESTARTSYS in HOST_AX` as an in-progress
	 * syscall needing RIP-rewind for restart.
	 *
	 * Background: bisection (memo §E.4 followup, agent
	 * `ad5cf1c09ac7c3a73`) showed fork_exec_wait's SIGILL first
	 * appears at 31ba9c354063 — the commit that added interrupt_end
	 * to the exception handlers. Mechanism:
	 *   1. SIGCHLD-interrupted waitpid sets gp[HOST_AX] = -ERESTARTSYS
	 *      and leaves PT_SYSCALL_NR(gp) = __NR_wait4.
	 *   2. Next user-mode iteration takes a fixable #PF (e.g., a CoW
	 *      page fault during execve's user-stack write).
	 *   3. v2's kvm_v2_handle_io_pf runs interrupt_end → do_signal.
	 *   4. do_signal sees PT_SYSCALL_NR >= 0 + return = -ERESTARTSYS
	 *      and runs PT_REGS_RESTART_SYSCALL, which rewinds RIP -= 2.
	 *   5. But RIP is at the faulting #PF instruction — NOT at a
	 *      SYSCALL boundary. IRETQ pops the rewound (mid-instruction)
	 *      RIP → #UD → SIGILL.
	 *
	 * This is the surgical fix: don't add interrupt_end to the syscall
	 * arm (avoiding the SYSRETQ-RCX-rewind regression of the reverted
	 * ad06c7f5164c), don't modify userspace()'s loop (avoiding the
	 * regression of the reverted bad8d61d2592). Just suppress the
	 * cross-path orig_ax leakage by mirroring exactly what seccomp
	 * does at trap_user.c:187-188.
	 *
	 * The PT_SYSCALL_NR_OFFSET vs PT_SYSCALL_RET_OFFSET guard exists
	 * because on architectures where the syscall NR slot and return
	 * slot are aliased (32-bit x86), clearing PT_SYSCALL_NR would also
	 * clobber the user-visible return value. UML/x86_64 has them
	 * separate, so the clear is unconditional in practice on this
	 * arch — but the guard preserves cross-arch correctness.
	 */
	if (PT_SYSCALL_NR_OFFSET != PT_SYSCALL_RET_OFFSET)
		PT_SYSCALL_NR(regs->gp) = -1;

	/*
	 * D.3 marshal-out: copy regs->gp[] back into kvm_run->s.regs.regs
	 * + OR KVM_SYNC_X86_REGS into kvm_dirty_regs. Critical for SYSRETQ:
	 * the trampoline's `sysretq` reads RIP from RCX and RFLAGS from R11.
	 * For normal syscall return that's the original user RIP/RFLAGS
	 * (preserved from the SYSCALL entry above where we stashed
	 * regs->gp[HOST_CX]/[HOST_R11] into HOST_IP/HOST_EFLAGS); signal-
	 * delivery (do_signal called from interrupt_end during handle_syscall)
	 * overwrites regs->gp[HOST_IP] with the signal handler VA, and the
	 * explicit lift below puts it into RCX so sysretq lands at the
	 * handler.
	 *
	 * RAX gets the return value handle_syscall stashed at gp[HOST_AX]
	 * (already covered by the marshal — the helper copies gp[HOST_AX]
	 * → dst->rax).
	 *
	 * Note: kvm_v2_marshal_to_kvm_regs at vcpu.c copies HOST_IP→rip,
	 * HOST_CX→rcx, HOST_R11→r11 — which after the marshal would leave
	 * rcx = original gp[HOST_CX] (now stale: handle_syscall did not
	 * update HOST_CX, only HOST_IP/HOST_EFLAGS — and signal-delivery
	 * may have rewritten HOST_IP). OVERWRITE rcx and r11 explicitly
	 * after the marshal so SYSRETQ sees the right user-resume RIP/
	 * RFLAGS regardless of which path handle_syscall took.
	 *
	 * v1 reference: kvm-v1-archive/thread.c documents the same RCX/R11
	 * = user-resume-RIP/RFLAGS contract for SYSRETQ at the trampoline
	 * tail (its "post-handle_syscall: marshal back to vcpu state"
	 * region around the per-task vcpu_run cleanup).
	 */
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->s.regs.regs.rcx = regs->gp[HOST_IP];
	run->s.regs.regs.r11 = regs->gp[HOST_EFLAGS];
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	trace_um_backend_kvm_v2_iotrap_syscall_exit(run->io.port,
						    regs->gp[HOST_AX]);

	return 0;
}
