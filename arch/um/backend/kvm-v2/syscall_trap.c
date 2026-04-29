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
#include <linux/string.h>
#include <linux/types.h>

#include <asm/page.h>
#include <asm/pgtable.h>	/* _KERNPG_TABLE, _PAGE_PRESENT, _PAGE_ACCESSED, swapper_pg_dir */
#include <asm/trace/um_backend.h>

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
	 * Build the PUD page: entry [0] points at the PMD page with
	 * _KERNPG_TABLE shape (kernel non-leaf: P|RW|A|D, US=0). Other 511
	 * entries stay zero (not-present) from __GFP_ZERO. Direct u64
	 * write — these aren't UML-managed pgtable structures so we don't
	 * route through set_pud (which adds NEEDSYNC tracking that the
	 * guest TDP walker doesn't understand and KVM doesn't honour).
	 */
	((u64 *)pud_kva)[0] = (u64)(pmd_pa | _KERNPG_TABLE);

	/* PMD page: entry [0] points at the PTE page, same kernel
	 * non-leaf shape. */
	((u64 *)pmd_kva)[0] = (u64)(pte_pa | _KERNPG_TABLE);

	/*
	 * PTE page: entry [0] points at the trampoline page with the leaf
	 * flags. _PAGE_PRESENT is required for the walk to succeed.
	 * _PAGE_ACCESSED is set up-front so the CPU doesn't need to
	 * write-back an A-bit update on first access (which would fault if
	 * the leaf were RO without the A bit pre-set on some CPU
	 * generations). No _PAGE_RW (RO trampoline). No _PAGE_USER (US=0;
	 * kernel-only). UML has no _PAGE_NX so executable is implicit.
	 */
	((u64 *)pte_kva)[0] = (u64)(vm->trampoline_gpa |
				     _PAGE_PRESENT | _PAGE_ACCESSED);

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
	entry = (unsigned long)(pud_pa | _KERNPG_TABLE);
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
int kvm_v2_handle_io_trap(struct uml_pt_regs *regs,
			  struct kvm_run *run,
			  int vcpu_fd)
{
	unsigned long syscall_nr;

	(void)vcpu_fd;	/* reserved for D.3 (marshal-out path) */

	/*
	 * Only port we handle today is the SYSCALL trap. Other ports
	 * are Phase E.3's exception classes (PF/GP/UD/...). The caller
	 * (kvm_v2_vcpu_run's switch-arm) panics on unknown exit
	 * reasons; this defensive double-check keeps the helper safe to
	 * call from any future site that doesn't pre-screen the port.
	 */
	if (run->io.port != UM_KVM_TRAP_SYSCALL) {
		pr_err("um: kvm-v2 io_trap: unexpected port %#x (direction=%u size=%u)\n",
		       run->io.port, run->io.direction, run->io.size);
		return -ENOTSUPP;
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
