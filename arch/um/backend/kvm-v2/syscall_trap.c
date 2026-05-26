// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase D.1+D.2: IO-port LSTAR trampoline +
 * KVM_EXIT_IO syscall-trap dispatcher.
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
 * (arch/um/backend/seccomp/trap_user.c:159).  The syscall return state
 * stays in the per-task regs; the next outer vcpu_run iteration copies
 * those regs into whichever per-host-CPU vCPU is selected for the task.
 * Do not use the shared kvm_run mmap after handle_syscall() returns:
 * blocking syscalls can schedule and another task may have reused that
 * per-CPU vCPU before the sleeping syscall resumes.
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
 *     handle_syscall, then returns to the outer per-trap loop.
 *   - The next KVM_RUN entry rebuilds the vCPU state from per-task
 *     regs. For normal syscall returns that means RIP is the user
 *     continuation from RCX and RFLAGS is the user flags from R11.
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
#include <linux/jump_label.h>	/* static_branch_unlikely — record/replay gate */
#include <linux/kvm.h>		/* struct kvm_run, KVM_EXIT_IO */
#include <linux/mm.h>
#include <linux/mm_types.h>	/* init_mm */
#include <linux/pgtable.h>	/* pgd_index, set_pgd */
#include <linux/printk.h>
#include <linux/sched/signal.h>	/* force_sig — Phase 3 strict-replay divergence */
#include <linux/set_memory.h>
#include <linux/signal.h>	/* clear_siginfo, kernel_siginfo_t */
#include <linux/string.h>
#include <linux/thread_info.h>	/* read_thread_flags, _TIF_WORK_MASK — perf-O1 gate */
#include <linux/types.h>
#include <uapi/asm-generic/siginfo.h>	/* ILL_ILLOPN, FPE_INTOVF, SEGV_MAPERR */

#include <asm/page.h>
#include <asm/pgtable.h>	/* _KERNPG_TABLE, _PAGE_PRESENT, _PAGE_ACCESSED, swapper_pg_dir */
#include <asm/processor-flags.h>	/* X86_CR0_TS */
#include <asm/trace/um_backend.h>

#include <kern_util.h>		/* segv_handler, relay_signal */
#include <os.h>			/* SMP-T26: os_drop_caching */
#include <skas.h>		/* handle_syscall */
#include <sysdep/ptrace.h>	/* uml_pt_regs, HOST_AX/CX/IP/EFLAGS/R11, UPT_SYSCALL_NR */
#include <sysdep/ptrace_user.h>	/* PT_SYSCALL_NR */

#include "kvm_v2_backend.h"
#include "state_trace.h"
#include "syscall_trap.h"

/*
 * LSTAR trampoline bodies — assembled from lstar_gadget.S and exposed
 * here as extern symbols (ftrace dynamic-trampoline pattern; see
 * arch/x86/kernel/ftrace_64.S + arch/x86/kernel/ftrace.c memcpy of
 * ftrace_caller..end into the trampoline page).
 *
 *   kvm_v2_lstar_fallback_start..._end
 *     5-byte body: out %al, $0xf4 ; sysretq. The non-gadget LSTAR
 *     target — every syscall takes the slow KVM_EXIT_IO route. Always
 *     installed first by kvm_v2_trampoline_alloc_and_install so that
 *     even if exception_install / state-page allocation fails partway,
 *     the LSTAR is functional (slow but correct).
 *
 *   kvm_v2_lstar_gadget_start..._end
 *     ~341-byte stay-in-guest dispatch tree: pid-family + getcpu +
 *     time + clock_gettime(CLOCK_MONOTONIC). Installed in-place over
 *     the fallback by kvm_v2_trampoline_upgrade_to_gadget after
 *     kvm_v2_exception_install confirms every per-vCPU gadget state
 *     page is mapped.
 *
 * The bytes are linker-defined, so `_end - _start` is a link-time
 * (not compile-time) constant. Page-fit invariants that previously
 * lived in static_assert(sizeof(array)) are now boot-time runtime
 * checks in the install functions below.
 */
extern const u8 kvm_v2_lstar_fallback_start[];
extern const u8 kvm_v2_lstar_fallback_end[];
extern const u8 kvm_v2_lstar_gadget_start[];
extern const u8 kvm_v2_lstar_gadget_end[];

/*
 * Phase H gadget body — assembled stub source: lstar_gadget.S.
 * That file documents the per-handler dispatch layout, byte-by-byte
 * displacements, and v1 cross-references (kvm-v1-archive/thread.c:
 * 873-1209). Read it for the canonical body layout.
 *
 * Stay-in-guest fast path for trivial syscalls:
 *   getpid  (NR 39  = 0x27) → KVM_V2_GADGET_OFF_TGID (0x08)
 *   gettid  (NR 186 = 0xba) → KVM_V2_GADGET_OFF_TID  (0x0c)
 *   getppid (NR 110 = 0x6e) → KVM_V2_GADGET_OFF_PPID (0x10)
 *   getuid  (NR 102 = 0x66) → KVM_V2_GADGET_OFF_UID  (0x14)
 *   geteuid (NR 107 = 0x6b) → KVM_V2_GADGET_OFF_EUID (0x18)
 *   getgid  (NR 104 = 0x68) → KVM_V2_GADGET_OFF_GID  (0x1c)
 *   getegid (NR 108 = 0x6c) → KVM_V2_GADGET_OFF_EGID (0x20)
 *   time    (NR 201 = 0xc9) → KVM_V2_GADGET_OFF_REAL_SEC (0x30)
 *                              + optional store of REAL_SEC into *tloc
 *   getcpu  (NR 309 = 0x135) → KVM_V2_GADGET_OFF_CPU_ID (0x04)
 *                              + writes 0 to *node (UML has no NUMA)
 *
 * Phase 7 (clock_gettime) backed out 2026-05-04 — gadget body verified
 * via objdump but user-visible RAX returns MONO_NSEC instead of 0.
 * Root cause not yet identified; bytes/offsets correct but somehow the
 * sysretq path returns the wrong RAX. Needs deeper diagnostics
 * (host-side instrumentation of regs.gp[HOST_AX] post-marshal). Phases
 * 4-6 ship; Phase 7 deferred.
 *
 * Any other NR falls through to the host KVM_EXIT_IO path.
 *
 * Layout (offsets relative to LSTAR body start = trampoline + 0x40):
 *
 *   +0   0f 01 f8                      swapgs                      (3 B)
 *                                        — GS_BASE := per-vCPU state page
 *                                          (run BEFORE getcpu pre-check
 *                                          so the body's %gs:OFF reads
 *                                          land on the right page).
 *
 *   +3   3d 35 01 00 00                cmp $0x135, %eax            (5 B)
 *                                        — Phase-5 getcpu pre-check.
 *                                          NR=309 doesn't fit the imm8
 *                                          dispatch chain below (which
 *                                          requires NR < 256 per the
 *                                          upper-byte guard). v1
 *                                          reference for the same
 *                                          two-step dispatch:
 *                                          kvm-v1-archive/thread.c:907-916.
 *
 *   +8   74 77                         je rel8 → h_getcpu          (2 B)
 *                                        rel8 = 119 (target +129).
 *
 *   +10  a9 00 ff ff ff                test $0xffffff00, %eax      (5 B)
 *                                        — upper-byte guard for the
 *                                          imm8 dispatch chain that
 *                                          follows. Catches NR >= 256
 *                                          aliases (e.g., preadv2
 *                                          NR=295=0x127 aliases low-
 *                                          byte 0x27 of getpid).
 *
 *   +15  75 1c                         jne +28 → fallback at +45   (2 B)
 *
 *   +17  3c 27 74 20                   cmp $0x27,%al ; je → h_tgid (4 B)
 *   +21  3c ba 74 26                   cmp $0xba,%al ; je → h_tid  (4 B)
 *   +25  3c 6e 74 2c                   cmp $0x6e,%al ; je → h_ppid (4 B)
 *   +29  3c 66 74 32                   cmp $0x66,%al ; je → h_uid  (4 B)
 *   +33  3c 6b 74 38                   cmp $0x6b,%al ; je → h_euid (4 B)
 *   +37  3c 68 74 3e                   cmp $0x68,%al ; je → h_gid  (4 B)
 *   +41  3c 6c 74 44                   cmp $0x6c,%al ; je → h_egid (4 B)
 *
 *   +45  fallback:
 *        0f 01 f8                      swapgs (restore user GS)    (3 B)
 *        e6 f4                         out %al, $0xf4              (2 B)
 *        48 0f 07                      sysretq                     (3 B)
 *
 *   handlers — each loads its slot then jmp to shared tail at +123:
 *   +53  h_tgid:  ... eb 3c                                        (10 B)
 *   +63  h_tid:   ... eb 32                                        (10 B)
 *   +73  h_ppid:  ... eb 28                                        (10 B)
 *   +83  h_uid:   ... eb 1e                                        (10 B)
 *   +93  h_euid:  ... eb 14                                        (10 B)
 *   +103 h_gid:   ... eb 0a                                        (10 B)
 *   +113 h_egid:  ... eb 00                                        (10 B)
 *
 *   +123 tail:
 *        0f 01 f8                      swapgs (restore user GS)    (3 B)
 *        48 0f 07                      sysretq                     (3 B)
 *
 *   +129 h_getcpu (56 B):
 *        Direct port of v1's handler at kvm-v1-archive/thread.c:1177-
 *        1208 with two changes:
 *          - TASK_SIZE_CAP at OFF=0x28 (v2 co-located, vs v1's separate
 *            vvar at +0x1030);
 *          - jbe-to-fallback uses rel8 instead of v1's rel32 (fallback
 *            is closer in v2's smaller body).
 *
 *        On entry: RDI = cpu*, RSI = node*, RAX = NR=309, GS=state_page.
 *        Use %edx (NOT %ecx — RCX holds user RIP for sysretq) to load
 *        CPU_ID. Use %r10d for the node-zero-write. Defer `xor %eax,%eax`
 *        until just before the tail swapgs so RAX = NR=309 survives any
 *        bounds-check fallback (handle_syscall sees the right NR).
 *
 *        The bounds checks treat user pointer == TASK_SIZE_CAP as too
 *        high (jbe goes to fallback) — TASK_SIZE_CAP is task_size - 16
 *        per v1 lifecycle.c:974, leaving 16 B headroom for the 8 B
 *        store starting at the boundary.
 *
 *   +185 total LSTAR body.
 *
 * v1 sched_yield SKIPPED with rationale: v1 archived sched_yield
 * (NR=24=0x18) at thread.c:935+ then DEMOTED it back to host-side at
 * thread.c:942-961 because the in-gadget `xor %eax,%eax ; ret`
 * skipped UML's scheduler entirely; tight sched_yield loops starved
 * co-tenant tasks for up to ~10ms (one host timer tick) before
 * SIGALRM-driven preemption fired. v2 will never gadget sched_yield —
 * the ~13µs VMEXIT cost per call is the right tradeoff for correct
 * POSIX semantics.
 *
 * jmp/je rel8 displacements are computed at write time as comments
 * but baked into the byte table for clarity.
 *
 * v1 reference: kvm-v1-archive/thread.c:873-1209 (full dispatch tree
 * including pid-family + clock_gettime + time + getcpu — Phases 6+7
 * will add the remaining clock entries).
 */
/*
 * The hand-coded byte table previously here was migrated to
 * lstar_gadget.S (Phase I mainstream-readiness item #2 — see
 * commit messages). The fallback and gadget bytes now ship as
 * extern .rodata symbols declared above.
 *
 * Page-fit invariant — gadget body lives at
 * KVM_V2_TRAMPOLINE_LSTAR_OFFSET (0x40) within the trampoline
 * page; budget is PAGE_SIZE - 0x40 = 4032 B. Linker-defined
 * sizes are not compile-time constants in C, so this is
 * checked once at boot in kvm_v2_trampoline_alloc_and_install
 * instead of via static_assert.
 */

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
	 * Two-phase LSTAR install (2026-05-04, Phase H gadget Phase 3):
	 *
	 * Step 1 (here): write the SAFE 5-byte fallback LSTAR (out + sysretq).
	 * Step 2 (kvm_v2_trampoline_upgrade_to_gadget, called from
	 *        kvm_v2_exception_install AFTER all per-vCPU state pages
	 *        are confirmed installed): overwrite with the 94-byte
	 *        gadget LSTAR.
	 *
	 * Why two-phase: the 94-byte gadget body's `mov %gs:OFF, %eax`
	 * dereferences MSR_KERNEL_GS_BASE = KVM_V2_GADGET_STATE_GVA(cpu),
	 * which is only mapped after kvm_v2_install_per_vcpu_gadget_state
	 * runs (during exception_install). Pre-Phase-3 the LSTAR was 5
	 * bytes that didn't reach into a separate page — atomic. Now the
	 * gadget bytes have a hard data dependency on a separately-installed
	 * page chain, so we must guarantee the install ordering is
	 * `state page → gadget bytes`, never `gadget bytes → state page`.
	 *
	 * If exception_install fails partway (alloc_page OOM, PTE index
	 * overflow, etc.), the 5-byte LSTAR stays in place and SYSCALLs
	 * route through the slow KVM_EXIT_IO path — same as pre-Phase-3.
	 * No silent half-broken state where gadget bytes execute but state
	 * pages aren't mapped (would SIGSEGV on first user SYSCALL after
	 * .vcpu_run flip).
	 *
	 * Use memcpy not __builtin_memcpy_inline (FORTIFY_SOURCE noise on
	 * small fixed copies on some toolchains).
	 *
	 * Page-fit invariant — checked once at first install. The fallback
	 * is 5 bytes and the gadget body is bounded by the per-vCPU state
	 * region offset (KVM_V2_TRAMPOLINE_STATE_OFFSET = 0x800), so any
	 * link-time growth past 0x7c0 bytes panics here rather than silently
	 * scribbling into the state page area.
	 */
	{
		const size_t fb_len = kvm_v2_lstar_fallback_end -
				      kvm_v2_lstar_fallback_start;
		const size_t gd_len = kvm_v2_lstar_gadget_end -
				      kvm_v2_lstar_gadget_start;
		const size_t budget = PAGE_SIZE -
				      KVM_V2_TRAMPOLINE_LSTAR_OFFSET;

		if (fb_len > budget || gd_len > budget) {
			panic("um: kvm-v2 trampoline_install: LSTAR body overflow — fallback=%zu gadget=%zu budget=%zu (offset 0x%x..PAGE_SIZE)",
			      fb_len, gd_len, budget,
			      KVM_V2_TRAMPOLINE_LSTAR_OFFSET);
		}

		memcpy((u8 *)kva + KVM_V2_TRAMPOLINE_LSTAR_OFFSET,
		       kvm_v2_lstar_fallback_start, fb_len);

		/*
		 * Boot-time self-check: read back the bytes we just wrote
		 * and panic on any mismatch. The trampoline is on the hot
		 * path post-D.5; a corrupt LSTAR is one of the worst
		 * possible failure modes (guest jumps to garbage at CPL=0).
		 * Catches: byte-table corruption between compile and load,
		 * write-protected page silently dropping the memcpy, etc.
		 */
		if (memcmp((const u8 *)kva + KVM_V2_TRAMPOLINE_LSTAR_OFFSET,
			   kvm_v2_lstar_fallback_start, fb_len) != 0) {
			const u8 *got = (const u8 *)kva +
					KVM_V2_TRAMPOLINE_LSTAR_OFFSET;

			panic("um: kvm-v2 trampoline_install: fallback LSTAR readback MISMATCH at kva=%p+%#x — got %02x %02x %02x %02x %02x, want e6 f4 48 0f 07",
			      kva, KVM_V2_TRAMPOLINE_LSTAR_OFFSET,
			      got[0], got[1], got[2], got[3], got[4]);
		}
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

	pr_info("um: kvm-v2 trampoline_install: kva=%p gpa=%pa gva=%#llx (5-byte fallback LSTAR at +%#x; gadget upgrade pending exception_install)\n",
		kva, &gpa, (u64)KVM_V2_LSTAR_GVA,
		KVM_V2_TRAMPOLINE_LSTAR_OFFSET);
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

/*
 * Phase H gadget Phase 3 (2026-05-04): in-place upgrade of the LSTAR
 * body from the 5-byte fallback (out + sysretq) to the 94-byte
 * stay-in-guest gadget. Caller (kvm_v2_exception_install) guarantees
 * every per-vCPU gadget state page is mapped before this runs, so the
 * gadget's `mov %gs:OFF, %eax` always finds backing.
 *
 * Idempotent — a second invocation re-writes the same bytes; readback
 * still passes. No locking required because kvm_v2_exception_install
 * runs once per VM at subsys_initcall, before any user task exists.
 *
 * On failure path semantics: if any caller decides not to upgrade
 * (e.g., per-vCPU install short-circuited because a previous
 * exception_install already ran), the LSTAR stays at the 5-byte
 * fallback. SYSCALLs work via the slow KVM_EXIT_IO route — no
 * functional regression vs pre-Phase-3, just slower.
 */
int kvm_v2_trampoline_upgrade_to_gadget(struct kvm_v2_vm *vm)
{
	void *kva;
	const u8 *body = kvm_v2_lstar_gadget_start;
	const size_t body_len = kvm_v2_lstar_gadget_end -
				kvm_v2_lstar_gadget_start;

	if (!vm || !vm->trampoline_page) {
		pr_warn("um: kvm-v2 trampoline_upgrade: vm or trampoline_page NULL — leaving 5-byte fallback in place\n");
		return -EINVAL;
	}

	kva = vm->trampoline_page;

	memcpy((u8 *)kva + KVM_V2_TRAMPOLINE_LSTAR_OFFSET, body, body_len);

	if (memcmp((const u8 *)kva + KVM_V2_TRAMPOLINE_LSTAR_OFFSET,
		   body, body_len) != 0) {
		const u8 *got = (const u8 *)kva + KVM_V2_TRAMPOLINE_LSTAR_OFFSET;

		panic("um: kvm-v2 trampoline_upgrade: gadget LSTAR readback MISMATCH at kva=%p+%#x len=%zu — got %02x %02x %02x %02x %02x...",
		      kva, KVM_V2_TRAMPOLINE_LSTAR_OFFSET, body_len,
		      got[0], got[1], got[2], got[3], got[4]);
	}

	pr_info("um: kvm-v2 trampoline_upgrade: LSTAR body upgraded to %zu-byte gadget at +%#x (per-vCPU state pages confirmed installed)\n",
		body_len, KVM_V2_TRAMPOLINE_LSTAR_OFFSET);
	return 0;
}

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
 * return value.  This helper must not write it back through @run after
 * handle_syscall returns: blocking syscalls can schedule, and another
 * UML task may reuse the same per-host-CPU vCPU before this task
 * resumes.  The next outer kvm_v2_vcpu_run() iteration marshals the
 * per-task regs into the selected vCPU immediately before KVM_RUN.
 *
 * vcpu_fd is currently unread inside the helper; later phases may want
 * it for explicit ioctls (e.g. KVM_SET_REGS for any field
 * KVM_CAP_SYNC_REGS doesn't cover, or per-trap MSR queries).
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

	KVMV2_TRACE(KVMV2_OP_IST_FRAME_WRITE_PRE,
		    (struct uml_pt_regs *)regs, NULL, vcpu);

	/*
	 * SMP-T19 sanity guard (2026-05-02): refuse to write a kernel-half
	 * RIP to IST top-40. The IST iretq frame's RIP slot must hold a
	 * legitimate USER RIP (where the iretq tail will land in CPL=3).
	 * If regs->gp[HOST_IP] is in the kernel-half handlers range, then
	 * the regs structure carries a stub-internal-fault address — likely
	 * because some upstream path (segv_handler under unusual conditions,
	 * or a stale regs view) didn't update HOST_IP from the frame's
	 * real user_rip. Writing this would cause the next iretq to pop
	 * the stub address as the user RIP, transition to CPL=3 with a
	 * kernel-half RIP, and surface as the NM_stub+2 P=0 segfault.
	 * Drop the write — leave IST contents intact (the previous valid
	 * frame is still there) and emit a one-shot diagnostic so we can
	 * triangulate the upstream caller.
	 */
	if (regs->gp[HOST_IP] >= KVM_V2_HANDLERS_GVA &&
	    regs->gp[HOST_IP] < KVM_V2_HANDLERS_GVA + 0x1000) {
		static int diag_seen;
		if (diag_seen < 30) {
			diag_seen++;
			pr_emerg("um: kvm-v2 IST_FRAME_WRITE_KERNEL_RIP[%d] pid=%d comm=%s host_ip=%lx host_sp=%lx\n",
				 diag_seen, current->pid, current->comm,
				 regs->gp[HOST_IP], regs->gp[HOST_SP]);
		}
		KVMV2_TRACE(KVMV2_OP_IST_FRAME_WRITE_POST,
			    (struct uml_pt_regs *)regs, NULL, vcpu);
		return;
	}

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

	KVMV2_TRACE(KVMV2_OP_IST_FRAME_WRITE_POST,
		    (struct uml_pt_regs *)regs, NULL, vcpu);
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

	/*
	 * SMP-T19 sanity guard (2026-05-02): NEVER write a kernel-half RIP
	 * or kernel-CPL CS into the IST iretq frame. Any such pending entry
	 * is corruption — the snapshot must describe legitimate USER state
	 * (CS RPL=3, RIP outside the kernel-half handlers page). Kernel-half
	 * RIPs slip into ist_frame[1] via:
	 *   - kvm_v2_ist_frame_write being called after a CPL=0 stub-internal
	 *     fault left regs->gp[HOST_IP] pointing at a stub address
	 *   - kvm_v2_ist_frame_snapshot_raw capturing IST top-40 before any
	 *     legitimate user-mode hardware push overwrote previous CPL=0
	 *     frame contents
	 *   - dup_task_struct memcpy inheriting parent's poisoned snapshot
	 *     across fork (the dominant vector — see arch_copy_thread)
	 * Drop instead of replay.
	 */
	if ((a->kvm_v2.ist_frame[1] >= KVM_V2_HANDLERS_GVA &&
	     a->kvm_v2.ist_frame[1] <  KVM_V2_HANDLERS_GVA + 0x1000) ||
	    (a->kvm_v2.ist_frame[2] & 3) != 3) {
		a->kvm_v2.ist_pending = false;
		return;
	}

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
	u64 rip = *(u64 *)(top - 40 +  0);
	u64 cs  = *(u64 *)(top - 40 +  8);

	/*
	 * SMP-T19 sanity guard (2026-05-02): the IDT-pushed iretq frame must
	 * describe USER state (CS.RPL=3, RIP outside kernel-half handlers
	 * range). If we observe a CPL=0 frame here, the IST page contents
	 * we'd capture are a stub-internal fault frame (e.g., from a faulty
	 * iretq pop, or from snapshotting before any legitimate hardware
	 * push happened). Capturing & later replaying that into the iretq
	 * pop-source would inject kernel-half RIP into user mode CPL=3 →
	 * Bug B-class fault. Refuse to capture; clear ist_pending so
	 * restore_pending becomes a no-op for this task.
	 */
	if ((rip >= KVM_V2_HANDLERS_GVA &&
	     rip <  KVM_V2_HANDLERS_GVA + 0x1000) ||
	    (cs & 3) != 3) {
		a->kvm_v2.ist_pending = false;
		return;
	}

	a->kvm_v2.ist_frame[0] = *(u64 *)(top - 48);          /* error_code */
	a->kvm_v2.ist_frame[1] = rip;                         /* RIP */
	a->kvm_v2.ist_frame[2] = cs;                          /* CS */
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
/*
 * SMP-T17 (2026-05-02): inline #NM handler for the EINTR-mid-NM-stub
 * case. Bug B mechanism: when SIGALRM EINTRs the guest with RIP at
 * NM stub start (HANDLERS_GVA + 0x1c0), the snapshot/replay path saves
 * a frame and re-enters at NM_stub on next dispatch. Re-entry means
 * `clts; iretq` runs again, but the iretq pops from a stale IST top-40
 * because the in-between dispatches of OTHER tasks on the same vCPU
 * overwrite/corrupt those slots in ways the snapshot/restore_pending
 * pair doesn't fully repair (especially the RIP pop slot when CR3
 * differs between the saved-snapshot task and the IST-page-aliased
 * physical page seen at iretq time on the new mm).
 *
 * The fix: NEVER replay the NM stub. Process the #NM inline:
 *   1. Read the IDT-pushed iretq frame from IST (5 qwords; no err code)
 *   2. Restore user state into regs->gp[] from that frame
 *   3. Clear CR0.TS in sregs (= what `clts` would do)
 *   4. Marshal regs back; the next KVM_RUN re-enters directly at user RIP
 *      (NOT at the stub) — no iretq pop required.
 *
 * Mirrors the pattern of the inline #PF handler shipped at e5977806fd14.
 * No segv_handler / interrupt_end calls — #NM has no UML-side
 * processing (just FPU-on for the guest).
 */
int kvm_v2_handle_nm_eintr_inline(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	/* #NM has NO error code. */
	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	/* Emulate `clts`: clear CR0.TS so the next KVM_RUN re-enters
	 * with TS=0. The user's faulting FP instruction will succeed
	 * on retry (we resume at frame.user_rip = the FP instruction).
	 */
	run->s.regs.sregs.cr0 &= ~X86_CR0_TS;
	run->kvm_dirty_regs |= KVM_SYNC_X86_SREGS;

	/* SMP-T22: one-shot bypass — kvm_v2_load_user_sregs at the
	 * next dispatch would otherwise unconditionally re-arm TS,
	 * undoing the clear above. The flag tells it to skip the
	 * arming + clear the existing TS bit. Pairs with the same
	 * mechanism in kvm_v2_handle_io_nm (the non-EINTR variant). */
	current->thread.arch.kvm_v2.nm_ts_bypass = true;

	/* No ist_frame_write — we're not preparing a stub iretq tail.
	 * The next dispatch's marshal_to_kvm_regs will write user_rip
	 * directly into KVM's regs.
	 */

	return 0;
}

int kvm_v2_handle_pf_eintr_inline(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu,
				  u64 cr2)
{
	struct kvm_v2_ist_frame frame;
	u64 stub_rip_at_eintr = regs->gp[HOST_IP];
	u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;

	kvm_v2_ist_frame_read(vcpu, &frame, true /* has_error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	/*
	 * SMP-T41 fix (2026-05-03): recover user RAX from the IST stack.
	 *
	 * The PF stub (exception.c kvm_v2_handler_stub_pf) starts with
	 * `push %rax` (1-byte opcode 0x50 at stub_start+0). That push
	 * stores user RAX at IST top-56 (the IDT iretq frame occupies
	 * top-48..top-8 with the error code at top-48; push %rax
	 * decrements RSP by 8 and writes user RAX one slot below the
	 * iretq frame).
	 *
	 * After the push, the stub executes `mov %cr2, %rax` (3 bytes,
	 * opcode `0f 20 d0` at stub_start+0xa) which OVERWRITES RAX with
	 * CR2. If EINTR catches the guest with RIP in [stub_start+0xd,
	 * stub_start+0x18) — i.e., between the cr2-into-RAX move and the
	 * pop %rax that restores it — current RAX in eintr_regs is
	 * actually CR2, not user's RAX.
	 *
	 * marshal_to_kvm_regs below writes ALL GPRs into kvm_run.s.regs,
	 * so without recovery the next KVM_RUN resumes at frame.user_rip
	 * with RAX = CR2. For mt-mini's `mov %al, (%rdx)` write
	 * instruction, AL is the low byte of RAX; CR2 is page-aligned, so
	 * AL = 0; the user's first store on the freshly-installed page
	 * lands as 0 instead of the intended val. strict_memset's
	 * read-back returns 0, surfacing as STRICT_MEMSET_FAIL with
	 * page-aligned offset (the byte[0]=0 signature isolated by T40).
	 *
	 * Recovery: if EINTR caught us PAST the push (stub_rip > stub
	 * start), user RAX is at IST top-56. Read it back into
	 * regs->gp[HOST_AX] before marshal. Subsequent stub instructions
	 * (`movq sentinel`, `mov %cr2, %rax`, `mov %rax, -8(%rsp)`,
	 * `mov %rdx, -16(%rsp)`) write to top-80 / top-64 / top-72 — none
	 * of them touch top-56 — so the original push value is intact for
	 * the entire EINTR-able window.
	 *
	 * For RIP == stub_start+0 (push hasn't executed yet), eintr_regs
	 * already carries user RAX; do nothing.
	 *
	 * Confirmed via state-trace dump on STRICT_MEMSET_FAIL: failing
	 * cr2's last PF event is consistently EINTR_INLINE_PF, with RAX
	 * at EINTR == cr2 (page-aligned), i.e., low byte 0.
	 */
	if (stub_rip_at_eintr > KVM_V2_HANDLERS_GVA + 0x140)
		regs->gp[HOST_AX] = *(u64 *)(top - 56);

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

	KVMV2_TRACE(KVMV2_OP_HANDLE_IO_PF_PRE, regs, run, vcpu);

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

		/*
		 * Bug B trigger (2026-05-02): when user_rip falls in the
		 * KERNEL-HALF VA range (user task running CPL=3 with RIP
		 * pointing at kernel-only memory), the user is trying to
		 * execute kernel code at CPL=3. This means SOMETHING
		 * (iretq, sysret, kvm_run regs marshal) loaded a kernel-half
		 * RIP into the user-mode return frame.
		 *
		 * Originally checked only HANDLERS_GVA (0xffffe00000002000).
		 * SMP-T24 follow-up (2026-05-02): widened to the full
		 * trampoline-PT chain (0xffffe00000000000 to +0x4000) to
		 * catch the LSTAR variant — `python3[N]: segfault at
		 * ffffe00000000040 ip ffffe00000000040 error 15` (= 0x15,
		 * P=1+U=1+I/D=1, classic kernel-page-from-CPL=3 #PF).
		 *
		 * Auto-freeze state-trace so the full sequence of events
		 * leading up to the fault is preserved for post-mortem.
		 */
		if (frame.user_rip >= KVM_V2_TRAMPOLINE_GVA &&
		    frame.user_rip <  KVM_V2_TRAMPOLINE_GVA + 0x4000) {
			KVMV2_TRACE(KVMV2_OP_TRACE_TRIGGER, regs, run, vcpu);
			kvm_v2_state_trace_dump("Bug B: user_rip in kernel-half VA");
			pr_emerg("um: kvm-v2 BUG_B user_rip=%llx err=%llx pid=%d comm=%s sp=%llx\n",
				 (unsigned long long)frame.user_rip,
				 (unsigned long long)frame.error_code,
				 current->pid, current->comm,
				 (unsigned long long)frame.user_rsp);
		}

		/*
		 * Bug class — RSV=1 (reserved-bit violation) trigger
		 * (SMP-T23 follow-up, 2026-05-02): #PF error code bit 3
		 * (RSV) means the page-table walk encountered a reserved
		 * bit set in some level. Indicates page-table corruption.
		 * Captured as a residual on threaded-subprocess-wait.py
		 * post-T22 (e.g. `segfault at ffffe00000000040 ip
		 * ffffe00000000040 error 15` — write to LSTAR with RSV=1).
		 * One-shot trace dump per boot so we capture a fresh
		 * smoking-gun without log-flooding.
		 */
		if (frame.error_code & 0x8 /* RSV bit */) {
			static int diag_seen;
			if (diag_seen < 5) {
				diag_seen++;
				KVMV2_TRACE(KVMV2_OP_TRACE_TRIGGER, regs, run, vcpu);
				kvm_v2_state_trace_dump("Bug C: PF with RSV=1");
				pr_emerg("um: kvm-v2 BUG_C[%d] PF-RSV cr2=%llx user_rip=%llx err=%llx pid=%d comm=%s\n",
					 diag_seen,
					 (unsigned long long)cr2,
					 (unsigned long long)frame.user_rip,
					 (unsigned long long)frame.error_code,
					 current->pid, current->comm);
			}
		}

		/*
		 * Bug class — high-cr2 with non-trivial signature (page-
		 * recycling-class trigger; SMP-T23 follow-up): user-half
		 * cr2 above the typical TLS/heap region (>0x100000000 = 4GB)
		 * with P=0 and U=1. Indicates user accessed a stale-TLB-
		 * mapped freed-and-recycled physical page. Doesn't fire on
		 * legitimate high-VA accesses (most workloads stay below
		 * 0x10000000000 = 1TB). One-shot per boot.
		 */
		if (cr2 >= 0x100000000ULL &&
		    cr2 <  0x800000000000ULL &&
		    !(frame.error_code & 0x1) /* P=0 */ &&
		    (frame.error_code & 0x4) /* U=1 */) {
			static int diag_seen;
			if (diag_seen < 5) {
				diag_seen++;
				/*
				 * SMP-T24 (2026-05-02): do NOT auto-freeze
				 * the state-trace ring on BUG_PR — the high-
				 * cr2 user-fault trigger fires on legitimate
				 * demand-paging events at init.sh boot
				 * (cr2 in 0x55... = bash heap, P=0 + U=1 =
				 * normal kernel-fault-in path). Freezing on
				 * those misses the actual BUG_B / BUG_C
				 * captures that come later. Print only.
				 */
				pr_emerg("um: kvm-v2 BUG_PR[%d] high-cr2 cr2=%llx user_rip=%llx err=%llx pid=%d comm=%s\n",
					 diag_seen,
					 (unsigned long long)cr2,
					 (unsigned long long)frame.user_rip,
					 (unsigned long long)frame.error_code,
					 current->pid, current->comm);
			}
		}

		/*
		 * SMP-T26 trigger (2026-05-02): NULL-class user fault from
		 * malloc-stress-c (or threaded-fork-malloc child). Captures
		 * the deterministic glibc heap-corruption signature
		 * (cr2=0x10, ip=user-text, error=4 = read of not-present)
		 * for state-trace post-mortem.
		 *
		 * Filter:
		 *   - cr2 < 0x1000 (small offset from NULL)
		 *   - error 4 (P=0, U=1, R=0)
		 *   - comm prefix "malloc-stress" (the diag-binary's name,
		 *     which gets truncated to "malloc-stress-c" by 16-char
		 *     TASK_COMM_LEN)
		 * One-shot per boot; freezes the trace ring so we capture
		 * the FULL ~5000-entry dispatch history of this child.
		 */
		if (cr2 < 0x1000 &&
		    frame.error_code == 0x4 &&
		    !memcmp(current->comm, "malloc-stress", 13)) {
			static int diag_t26;
			if (diag_t26 == 0) {
				diag_t26 = 1;
				KVMV2_TRACE(KVMV2_OP_TRACE_TRIGGER, regs, run, vcpu);
				kvm_v2_state_trace_dump("BUG_T26: NULL-class user fault in malloc-stress");
				pr_emerg("um: kvm-v2 BUG_T26 cr2=%llx user_rip=%llx err=%llx pid=%d comm=%s sp=%llx\n",
					 (unsigned long long)cr2,
					 (unsigned long long)frame.user_rip,
					 (unsigned long long)frame.error_code,
					 current->pid, current->comm,
					 (unsigned long long)frame.user_rsp);
			}
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
	 * Round 2 Django silent-crash hook (2026-05-17): if segv_handler
	 * just called force_sig_fault(SIGSEGV/SIGBUS) — i.e., the fault
	 * was unfixable and handle_page_fault returned -EFAULT / -EACCES —
	 * the terminate-class signal is now queued on current. This is the
	 * silent-crash path: the user task will be killed by the next
	 * interrupt_end()'s do_signal without writing anything to stderr,
	 * bypassing the user-mode tgkill+SIGABRT trigger in
	 * kvm_v2_state_trace_capture()'s HANDLE_SYSCALL_PRE hook.
	 *
	 * Freeze the trace ring NOW, before interrupt_end() consumes the
	 * signal and the task starts exiting. One-shot via cmpxchg inside
	 * kvm_v2_state_trace_freeze — first hit wins. Captured pid/cr2/
	 * user_rip in the dmesg log together with KVMV2T_DUMP entries from
	 * the soak harness's debugfs dump pins down the moment of failure.
	 *
	 * We test sigismember (not fatal_signal_pending — that only checks
	 * SIGKILL) on SIGSEGV and SIGBUS only; do not include SIGABRT
	 * because the kernel-side path here never queues SIGABRT.
	 */
	if (current && current->sighand) {
		unsigned long irqflags;
		bool queued = false;

		spin_lock_irqsave(&current->sighand->siglock, irqflags);
		queued = sigismember(&current->pending.signal, SIGSEGV) ||
			 sigismember(&current->pending.signal, SIGBUS) ||
			 sigismember(&current->signal->shared_pending.signal,
				     SIGSEGV) ||
			 sigismember(&current->signal->shared_pending.signal,
				     SIGBUS);
		spin_unlock_irqrestore(&current->sighand->siglock, irqflags);
		if (queued) {
			char reason[96];

			snprintf(reason, sizeof(reason),
				 "fatal-segv cr2=%llx user_rip=%llx err=%llx",
				 (unsigned long long)cr2,
				 (unsigned long long)frame.user_rip,
				 (unsigned long long)frame.error_code);
			kvm_v2_state_trace_freeze(reason);
		}
	}

	/*
	 * SMP-T26 H_E experiment was tested here (2026-05-02): forced
	 * TDP/EPT cache invalidation via madvise(MADV_DONTNEED) on the
	 * spawner mm range corresponding to the freshly-mapped guest PA
	 * after segv_handler. Did NOT change the threaded-fork-malloc
	 * fail rate (6/6 boots × ~8 child SIGSEGVs each, identical or
	 * slightly worse than the 0.15%/fork baseline). Hypothesis E
	 * (TDP cache aliasing for the faulted page) is wrong as
	 * implemented — the corruption may be on an ADJACENT page that
	 * wasn't faulted, or the mechanism isn't TDP-cache at all.
	 *
	 * See state-audit Layer 14 for hypothesis status. Code reverted
	 * (only this comment remains as a marker).
	 */

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

	KVMV2_TRACE(KVMV2_OP_HANDLE_IO_PF_POST, regs, run, vcpu);

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
 * SMP-T22 (2026-05-02) — #NM (vec 7) host-side handler.
 *
 * The in-guest stub `out %al, $UM_KVM_TRAP_NM ; iretq` traps to here
 * via KVM_EXIT_IO. We:
 *   1. Read the IDT-pushed iretq frame (no error_code) from IST.
 *   2. Marshal user state into regs (HOST_IP/SP/EFLAGS).
 *   3. Clear sregs.cr0.TS = host-side `clts` emulation.
 *   4. Set arch_thread.kvm_v2.nm_ts_bypass = true so the next
 *      kvm_v2_load_user_sregs SKIPS the unconditional TS arming
 *      AND clears the existing TS bit. Without that, the user's
 *      FP instruction would fault again on retry → infinite loop.
 *   5. Drain pending signal/scheduler work (interrupt_end).
 *   6. Marshal regs back into kvm_run->s.regs.regs.
 *   7. Return; next KVM_RUN re-enters at frame.user_rip with TS=0.
 *
 * The in-guest iretq tail is UNREACHABLE — we marshal user state
 * directly via SYNC_REGS, bypassing any iretq from IST. This
 * eliminates the NM_stub+2 iretq-pops-kernel-half-RIP surface
 * (Bug B class — memos 10, SMP-T17, SMP-T19).
 */
static int kvm_v2_handle_io_nm(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	/* Host-side `clts` emulation. */
	run->s.regs.sregs.cr0 &= ~X86_CR0_TS;
	run->kvm_dirty_regs   |= KVM_SYNC_X86_SREGS;

	/* One-shot bypass: load_user_sregs at the next dispatch will see
	 * this flag, skip the TS re-arm, and clear the flag. */
	current->thread.arch.kvm_v2.nm_ts_bypass = true;

	/*
	 * SMP-T55: the next KVM_RUN will execute the user FPU instruction
	 * the #NM handler is unblocking (we cleared TS above). That touches
	 * the vCPU's guest FPU. Force the post-vmexit GET regardless of
	 * the post-run TS readback so iotrap_fpu captures the resulting
	 * state.
	 */
	vcpu->fpu_dirty = true;

	/* Drain pending signal/scheduler work — same pattern as peers. */
	interrupt_end();

	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs   |= KVM_SYNC_X86_REGS;

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

	kvm_v2_ist_frame_read(vcpu, &frame, false /* unknown — best-effort */);

	trace_um_backend_kvm_v2_iotrap_panic(run->io.port, frame.user_rip);

	/*
	 * Deliver SIGSEGV to the faulting process instead of panicking
	 * the entire kernel.  The panic stub fires for any IDT vector
	 * without a dedicated handler (#DB, #AC, #XM, etc.).  These
	 * are user-mode faults that should kill the offending process,
	 * not the kernel.  Follow the #GP handler pattern.
	 */
	{
		/*
		 * Dump raw IST stack top bytes — the CPU pushes the
		 * exception frame growing downward from ist_stack_top.
		 * Read 48 bytes below the top to cover the error-code
		 * layout (6 u64: err, RIP, CS, RFLAGS, RSP, SS).
		 */
		u64 ist_raw[6] = {0};

		if (vcpu->ist_stack_kva) {
			u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;

			memcpy(ist_raw, top - 48, sizeof(ist_raw));
		}
		{
			u64 pml4_508 = 0;

			if (current->mm && current->mm->pgd)
				pml4_508 = ((u64 *)current->mm->pgd)[448];
			{
				u8 io_al = 0;
				u64 data_off = run->io.data_offset;
				u8 stub_bytes[4] = {0};
				struct kvm_v2_vm *vm = kvm_v2_vm_get();

				if (data_off && data_off < 4096)
					io_al = *((u8 *)run + data_off);
				if (vm && vm->handlers_kva) {
					u64 rip = run->s.regs.regs.rip;
					u64 off = rip - KVM_V2_HANDLERS_GVA;

					if (off < PAGE_SIZE && off + 4 <= PAGE_SIZE)
						memcpy(stub_bytes,
						       (u8 *)vm->handlers_kva + off,
						       4);
				}
				pr_warn_ratelimited("kvm-v2: unhandled vec (port=%#x cpu=%d pid=%d) "
						    "run_rip=%#llx stub=[%02x %02x %02x %02x] "
						    "ist=[%#llx %#llx] rip=%#llx\n",
						    run->io.port,
						    vcpu->cpu, current->pid,
						    (u64)run->s.regs.regs.rip,
						    stub_bytes[0], stub_bytes[1],
						    stub_bytes[2], stub_bytes[3],
						    ist_raw[0], ist_raw[1],
						    frame.user_rip);
			}
		}
	}

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	regs->faultinfo.error_code = 0;
	regs->faultinfo.cr2        = 0;
	regs->faultinfo.trap_no    = 0;

	segv_handler(SIGSEGV, NULL, regs, NULL);

	interrupt_end();

	kvm_v2_ist_frame_write(vcpu, regs, false /* no error code */);
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	return 0;
}

int kvm_v2_handle_io_trap(struct uml_pt_regs *regs,
			  struct kvm_run *run,
			  struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame_scratch;
	unsigned long syscall_nr;
	u16 io_port;

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
		/*
		 * SMP-T60 GADGET-SAVE-AUDIT: at gadget-fallback IO trap, verify
		 * the per-vCPU state-page SAVE_RDX/R8/R10 slots actually got
		 * written with the user's pre-SYSCALL RDX/R8/R10. The gadget's
		 * entry-save block writes them via `movq %rdx, %gs:0x50` etc.
		 * If GS_BASE was wrong at entry-swapgs (e.g., MSR_KERNEL_GS_BASE
		 * was corrupted), those writes land OUTSIDE the state page and
		 * the slots retain whatever was there before. Since we're now
		 * back in host mode with the user RDX/R8/R10 in run->s.regs.regs
		 * (post-fallback-restore = same as entry values), compare the
		 * two. Mismatch = corrupted GS_BASE captured.
		 *
		 * One-shot per kernel boot (rate-limited atomic), trace-ring
		 * freeze on mismatch for post-mortem. Cheap on success path:
		 * three 8-byte reads + three compares + atomic_read.
		 */
		if (vcpu->gadget_state_kva) {
			u8 *page = (u8 *)vcpu->gadget_state_kva;
			u64 slot_rdx = *(u64 *)(page + KVM_V2_GADGET_OFF_SAVE_RDX);
			u64 slot_r8  = *(u64 *)(page + KVM_V2_GADGET_OFF_SAVE_R8);
			u64 slot_r10 = *(u64 *)(page + KVM_V2_GADGET_OFF_SAVE_R10);
			u64 user_rdx = run->s.regs.regs.rdx;
			u64 user_r8  = run->s.regs.regs.r8;
			u64 user_r10 = run->s.regs.regs.r10;

			if (slot_rdx != user_rdx || slot_r8 != user_r8 ||
			    slot_r10 != user_r10) {
				static atomic_t saw_mismatch = ATOMIC_INIT(0);
				if (atomic_inc_return(&saw_mismatch) <= 10) {
					struct kvm_sregs sregs_now;
					int rc;

					(void)kvm_v2_state_trace_freeze("BUG_T60: gadget SAVE slot != user reg");
					rc = os_ioctl_generic(vcpu->vcpu_fd,
							      KVM_GET_SREGS,
							      (unsigned long)&sregs_now);
					pr_emerg("um: kvm-v2 BUG_T60 GADGET-SAVE-MISMATCH cpu=%d pid=%d comm=%s rip=%llx\n",
						 vcpu->cpu, current->pid, current->comm,
						 (unsigned long long)run->s.regs.regs.rip);
					pr_emerg("  slot_rdx=%#llx user_rdx=%#llx %s\n",
						 (unsigned long long)slot_rdx,
						 (unsigned long long)user_rdx,
						 slot_rdx == user_rdx ? "MATCH" : "MISMATCH");
					pr_emerg("  slot_r8 =%#llx user_r8 =%#llx %s\n",
						 (unsigned long long)slot_r8,
						 (unsigned long long)user_r8,
						 slot_r8 == user_r8 ? "MATCH" : "MISMATCH");
					pr_emerg("  slot_r10=%#llx user_r10=%#llx %s\n",
						 (unsigned long long)slot_r10,
						 (unsigned long long)user_r10,
						 slot_r10 == user_r10 ? "MATCH" : "MISMATCH");
					if (rc == 0)
						pr_emerg("  gs.base=%#llx (expected STATE_GVA=%#llx) %s\n",
							 (unsigned long long)sregs_now.gs.base,
							 (unsigned long long)KVM_V2_GADGET_STATE_GVA(vcpu->cpu),
							 sregs_now.gs.base == KVM_V2_GADGET_STATE_GVA(vcpu->cpu) ? "MATCH" : "MISMATCH");
				}
			}
		}
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
	case UM_KVM_TRAP_NM:
		return kvm_v2_handle_io_nm(regs, run, vcpu);
	case UM_KVM_TRAP_BP:
		/* #BP (vector 3, INT3) — no error code. Deliver SIGTRAP. */
		kvm_v2_ist_frame_read(vcpu, &frame_scratch, false);
		regs->gp[HOST_IP]     = frame_scratch.user_rip;
		regs->gp[HOST_SP]     = frame_scratch.user_rsp;
		regs->gp[HOST_EFLAGS] = frame_scratch.user_rflags;
		regs->is_user         = 1;
		regs->faultinfo.error_code = 0;
		regs->faultinfo.cr2        = 0;
		regs->faultinfo.trap_no    = 3;
		kvm_v2_dispatch_relay(regs, SIGTRAP, TRAP_BRKPT);
		interrupt_end();
		kvm_v2_ist_frame_write(vcpu, regs, false);
		kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
		run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;
		return 0;
	case UM_KVM_TRAP_DB:
		/* #DB (vector 1) — no error code. Deliver SIGTRAP. */
		kvm_v2_ist_frame_read(vcpu, &frame_scratch, false);
		regs->gp[HOST_IP]     = frame_scratch.user_rip;
		regs->gp[HOST_SP]     = frame_scratch.user_rsp;
		regs->gp[HOST_EFLAGS] = frame_scratch.user_rflags;
		regs->is_user         = 1;
		regs->faultinfo.error_code = 0;
		regs->faultinfo.cr2        = 0;
		regs->faultinfo.trap_no    = 1;
		kvm_v2_dispatch_relay(regs, SIGTRAP, TRAP_TRACE);
		interrupt_end();
		kvm_v2_ist_frame_write(vcpu, regs, false);
		kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
		run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;
		return 0;
	case UM_KVM_TRAP_SS:
		/* #SS (vector 12) — has error code. Deliver SIGSEGV. */
		kvm_v2_ist_frame_read(vcpu, &frame_scratch, true);
		regs->gp[HOST_IP]     = frame_scratch.user_rip;
		regs->gp[HOST_SP]     = frame_scratch.user_rsp;
		regs->gp[HOST_EFLAGS] = frame_scratch.user_rflags;
		regs->is_user         = 1;
		regs->faultinfo.error_code = (int)frame_scratch.error_code;
		regs->faultinfo.cr2        = 0;
		regs->faultinfo.trap_no    = 12;
		segv_handler(SIGSEGV, NULL, regs, NULL);
		interrupt_end();
		kvm_v2_ist_frame_write(vcpu, regs, true);
		kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
		run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;
		return 0;
	case UM_KVM_TRAP_AC:
		/* #AC (vector 17) — has error code (always 0). Deliver SIGBUS. */
		kvm_v2_ist_frame_read(vcpu, &frame_scratch, true);
		regs->gp[HOST_IP]     = frame_scratch.user_rip;
		regs->gp[HOST_SP]     = frame_scratch.user_rsp;
		regs->gp[HOST_EFLAGS] = frame_scratch.user_rflags;
		regs->is_user         = 1;
		regs->faultinfo.error_code = (int)frame_scratch.error_code;
		regs->faultinfo.cr2        = 0;
		regs->faultinfo.trap_no    = 17;
		segv_handler(SIGSEGV, NULL, regs, NULL);
		interrupt_end();
		kvm_v2_ist_frame_write(vcpu, regs, true);
		kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
		run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;
		return 0;
	case UM_KVM_TRAP_DF:
		/* #DF (vector 8) — has error code (always 0). Deliver SIGSEGV. */
		kvm_v2_ist_frame_read(vcpu, &frame_scratch, true);
		regs->gp[HOST_IP]     = frame_scratch.user_rip;
		regs->gp[HOST_SP]     = frame_scratch.user_rsp;
		regs->gp[HOST_EFLAGS] = frame_scratch.user_rflags;
		regs->is_user         = 1;
		regs->faultinfo.error_code = 0;
		regs->faultinfo.cr2        = 0;
		regs->faultinfo.trap_no    = 8;
		pr_warn_ratelimited("kvm-v2: #DF double fault cpu=%d pid=%d rip=%#llx\n",
				    vcpu->cpu, current->pid, frame_scratch.user_rip);
		segv_handler(SIGSEGV, NULL, regs, NULL);
		interrupt_end();
		kvm_v2_ist_frame_write(vcpu, regs, true);
		kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
		run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;
		return 0;
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

	io_port = run->io.port;

	trace_um_backend_kvm_v2_iotrap_syscall_enter(io_port, syscall_nr);

	KVMV2_TRACE(KVMV2_OP_HANDLE_SYSCALL_PRE, regs, run, vcpu);

	/*
	 * Record/replay v2 — replay consume hook (memo 27 §Phase 3, §3.2).
	 *
	 * Slot: BEFORE handle_syscall. Symmetric to the Phase 2 observe
	 * site below (which runs AFTER handle_syscall to capture the
	 * post-call retval). When a recorded log was previously captured
	 * (rec->state == REPLAYING after kvm_v2_record_replay), this
	 * block walks the next entry, validates kind == SYSCALL +
	 * NR equality, and on a match writes the recorded retval into
	 * regs->gp[HOST_AX] then jumps past handle_syscall to the
	 * common syscall-return cleanup. The live syscall is NOT issued —
	 * replay's
	 * whole contract is "reproduce the recorded side effects without
	 * re-issuing them against the host."
	 *
	 * Three rc paths:
	 *
	 *   rc > 0:   entry served. Stuff served_ret into HOST_AX, clear
	 *             PT_SYSCALL_NR (defensive — the Phase 2 hook below
	 *             expects the syscall_nr-already-cleared shape we'd
	 *             normally hit AFTER handle_syscall), and goto past
	 *             handle_syscall to the post-call cleanup.
	 *
	 *   rc == 0:  not REPLAYING (or rec NULL race against _stop).
	 *             Fall through to live handle_syscall.
	 *
	 *   rc < 0:   -ENODATA (end-of-log) or -EILSEQ (kind/NR mismatch).
	 *             In strict_replay mode, deliver SIGSEGV to current —
	 *             divergence is a hard fault. In loose mode, fall
	 *             through to live handle_syscall (and pr_info the
	 *             miss for the operator).
	 *
	 * The strict-mode `force_sig(SIGSEGV)` path is the one new
	 * task-visible signal Phase 3 introduces. SIGSEGV mirrors v1
	 * (kvm-v1-archive/syscall_class.c's strict-replay arms used the
	 * same signal) and matches the operator mental model: divergence
	 * == corrupt-program == segfault. The force_sig delivery uses
	 * the existing #include <linux/sched/signal.h> machinery; we
	 * still goto past handle_syscall so the common cleanup runs
	 * normally — the kernel will deliver the queued signal on the
	 * task's next return-to-userspace check.
	 *
	 * Zero hot-path cost when off: the static-key gate compiles to
	 * a 5-byte NOP patched out at boot; the new hook block adds zero
	 * cycles to non-record runtime (same shape as the Phase 2 site
	 * above the return cleanup).
	 */
	if (static_branch_unlikely(&um_kvm_v2_record_enabled)) {
		struct kvm_v2_record *rec = kvm_v2_record_active();

		if (rec && rec->state == KVM_V2_RECORD_REPLAYING) {
			long served_ret = 0;
			int rc;

			rc = kvm_v2_record_consume_syscall(rec, syscall_nr,
							   &served_ret);
			if (rc > 0) {
				regs->gp[HOST_AX] = (unsigned long)served_ret;
				goto skip_handle_syscall;
			}
			if (rc < 0 && rec->strict_replay) {
				pr_info_ratelimited(
					"kvm-v2 record: strict replay divergence nr=%lu rc=%d entries_replayed=%llu\n",
					syscall_nr, rc, rec->entries_replayed);
				force_sig(SIGSEGV);
				goto skip_handle_syscall;
			}
			/*
			 * rc == 0 (not REPLAYING / racing _stop) or
			 * (rc < 0 and !strict_replay): drop through to
			 * live handle_syscall below.
			 */
		}
	}

	/*
	 * SMP-T78 (state-audit/27): the outer kvm_v2_vcpu_run holds
	 * migrate_disable() for the whole dispatch (the SMP-T13 fix that
	 * pins @vcpu/@run validity across schedule()).  Linux's
	 * __set_cpus_allowed_ptr_locked refuses sched_setaffinity()
	 * requests that exclude the current CPU when migration_disabled
	 * is held — see kernel/sched/core.c:3179 — and triggers a
	 * WARN_ON_ONCE + -EBUSY.  That breaks any guest workload that
	 * legitimately wants to migrate (CPython's
	 * test_process_cpu_count_affinity is the canonical reproducer).
	 *
	 * Release the pin around handle_syscall(): the syscall path
	 * touches only @regs (per-task, migration-safe), not @vcpu/@run.
	 * After handle_syscall returns, the calling task may be on a
	 * different host CPU; the outer kvm_v2_vcpu_run loop's NEXT
	 * iteration will re-fetch vcpu/run from smp_processor_id().
	 * The kvm_v2_handle_io_trap caller MUST NOT touch @run or @vcpu
	 * after we return — see the post-syscall block below for the
	 * existing matching invariant.
	 */
	migrate_enable();
	handle_syscall(regs);
	migrate_disable();

	/*
	 * Do not pass @run or @vcpu after handle_syscall().  A blocking
	 * syscall can sleep, and v2's per-host-CPU vCPU pool lets another
	 * UML task reuse this vCPU before the sleeping syscall resumes.
	 * At this point the only authoritative state is per-task regs.
	 */
	KVMV2_TRACE(KVMV2_OP_HANDLE_SYSCALL_POST, regs, NULL, NULL);

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
	 * preserved the sentinel through to the syscall-return path, with
	 * the user seeing every subsequent shell write fail.
	 *
	 * The reverted commit ad06c7f5164c added this same call but at
	 * that time the substrate gate showed a SYSRETQ-RIP regression.
	 * v2 no longer marshals back into @run after handle_syscall();
	 * the next outer vcpu_run iteration rebuilds the vCPU from
	 * per-task regs, so HOST_IP/HOST_EFLAGS still carry the restart,
	 * signal-handler entry, or unchanged post-syscall RIP/RFLAGS.
	 *
	 * Place BEFORE the PT_SYSCALL_NR clear below: do_signal's
	 * restart-syscall logic only fires when PT_REGS_SYSCALL_NR(regs)
	 * >= 0, which it is at this point (handle_syscall did not clear
	 * it). After interrupt_end runs, clearing PT_SYSCALL_NR ensures
	 * later exception-path interrupt_end() calls don't see a stale
	 * syscall NR + leaked -ERESTART* residual (commit a478952b8da0).
	 *
	 * perf-O1 (2026-05-04): inline the _TIF_WORK_MASK gate that
	 * interrupt_end() does internally, AND also gate on the
	 * syscall return being in the -ERESTART* range. The function
	 * call has overhead (~30-50 cyc per dispatch); skipping it on
	 * the common fast path (syscall returned cleanly, no signals
	 * pending) wins back ~50-200 cyc on tight syscall loops like
	 * Python startup.
	 *
	 * Conditions to call interrupt_end:
	 *   (a) handle_syscall returned -ERESTART* (-512..-516) —
	 *       do_signal MUST run to translate -ERESTART → restart-RIP
	 *       or -EINTR. Closes the dash bug (#107).
	 *   (b) any TIF_WORK_MASK bit set — must drain pending sched/
	 *       signal work. Substrate parity required.
	 *
	 * If neither, interrupt_end()'s body is a no-op anyway —
	 * skipping the call eliminates the function-call cost.
	 */
	{
		long _ret = (long)regs->gp[HOST_AX];
		if (unlikely((_ret <= -512 && _ret >= -516) ||
			     (read_thread_flags() & _TIF_WORK_MASK)))
			interrupt_end();
	}

	/*
	 * Record/replay v2 — observation hook (memo 27 §Phase 2, §3.1).
	 *
	 * Slot: between interrupt_end() (above) and the PT_SYSCALL_NR
	 * clear (below). Order matters:
	 *
	 *   - AFTER handle_syscall: regs->gp[HOST_AX] holds the return
	 *     value the syscall produced; this is what replay has to
	 *     reproduce.
	 *   - AFTER interrupt_end: do_signal may rewrite regs->gp[HOST_IP]
	 *     when a signal is delivered. The observe entry doesn't
	 *     capture HOST_IP, so the relative ordering is moot, BUT
	 *     placing the hook AFTER interrupt_end means signal-delivery
	 *     side effects (e.g. -ERESTARTSYS → -EINTR translation) are
	 *     already settled in HOST_AX before we capture retval.
	 *   - BEFORE the PT_SYSCALL_NR clear: not strictly required (we
	 *     pass the cached @syscall_nr from line 2153 directly), but
	 *     keeps the hook's view of regs in a "syscall NR slot still
	 *     populated" state — minimizes surprise for the inevitable
	 *     Phase 5/6 hooks that may want to read PT_SYSCALL_NR.
	 *   - BEFORE returning to the outer vcpu_run loop:
	 *     regs->gp[HOST_DI/SI/DX/R10/R8/R9] still hold the original
	 *     syscall arg values that
	 *     C.3's marshal-from-kvm-regs populated at entry —
	 *     handle_syscall is supposed to read but not mutate these.
	 *     If a future regression made handle_syscall clobber args,
	 *     the observe entry would capture the post-clobber value;
	 *     Phase 2 accepts that risk (no in-tree path clobbers args
	 *     today) since fixing it would require stashing args on the
	 *     stack at entry — Phase 2 cost-benefit doesn't justify the
	 *     extra copy.
	 *
	 * Zero hot-path cost when off: the static-key gate compiles to
	 * a 5-byte NOP that the kernel patches out at boot;
	 * static_branch_enable() flips it to a `jmp` only when
	 * kvm_v2_record_start arms a container. Non-record runtime
	 * (every shipped UML profile) pays one skipped jne per vmexit.
	 *
	 * v1 reference: kvm-v1-archive/syscall_class.c (the per-NR
	 * dispatcher had the same gate at the tail of each arm). v2
	 * lifts the gate up to the unified dispatcher per memo 27 §3.1.
	 */
	if (static_branch_unlikely(&um_kvm_v2_record_enabled)) {
		struct kvm_v2_record *rec = kvm_v2_record_active();

		if (rec)
			kvm_v2_record_observe_syscall(rec, syscall_nr,
						      (long)regs->gp[HOST_AX],
						      regs);
	}

	/*
	 * Phase 3 (memo 27 §3.2) replay-served path rejoins here: skip
	 * the live handle_syscall + the interrupt_end -ERESTART* block +
	 * the Phase 2 observe hook (we just emitted the recorded retval
	 * into HOST_AX from the log). The PT_SYSCALL_NR clear below still
	 * runs so the cross-path orig_ax-leakage protection
	 * (commit a478952b8da0) stays armed for any subsequent exception
	 * dispatcher hits.
	 */
skip_handle_syscall:
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
	 * There is deliberately no post-syscall marshal into @run here.
	 * The next kvm_v2_vcpu_run() iteration copies regs->gp[] into the
	 * selected vCPU immediately before KVM_RUN.  Avoiding @run here is
	 * required for sleeping syscalls: while this task sleeps, another
	 * task can reuse the same per-host-CPU vCPU and overwrite the
	 * shared kvm_run mmap.
	 */
	/*
	 * SMP-T12 diagnostic: log the syscall return value we're about
	 * to ship to user. mmap (syscall_nr was 9 BEFORE we cleared
	 * orig_ax) returning errno-range or zero is the MMAP_NULL bug
	 * signature. Bounded to 30 hits.
	 */
	{
		long ax_val = (long)regs->gp[HOST_AX];

		if (syscall_nr == 9 &&
		    ((unsigned long)ax_val >= -4096UL || ax_val == 0)) {
			static atomic_t marshal_diag = ATOMIC_INIT(0);
			if (atomic_inc_return(&marshal_diag) <= 30)
				pr_emerg("UM_MARSHAL_DIAG pid=%d cpu=%d "
					 "syscall_nr=%lu ship.rax=%#lx hax=%#lx\n",
					 current->pid, raw_smp_processor_id(),
					 syscall_nr,
					 (unsigned long)regs->gp[HOST_AX],
					 (unsigned long)regs->gp[HOST_AX]);
		}
	}

	trace_um_backend_kvm_v2_iotrap_syscall_exit(io_port, regs->gp[HOST_AX]);

	return 0;
}
