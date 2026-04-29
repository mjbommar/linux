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
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kvm.h>		/* struct kvm_run, KVM_EXIT_IO */
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/set_memory.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/page.h>
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
