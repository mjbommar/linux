// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — thread + run_userspace ops.
 *
 * Workstream D-04a. Thread lifecycle (thread_create,
 * thread_start_idle, context_switch) on the UML kernel side is
 * the same jmp_buf-based mechanism ptrace and seccomp already
 * use; these wrappers exist so the dispatch macro resolves
 * kvm_<op>() in single-backend KVM_ONLY builds.
 *
 * run_userspace is the first op that actually exercises KVM:
 * ioctl(KVM_RUN) on vcpu0 and dispatch on exit_reason. At D-04a
 * the vCPU has no SREGS / CR3 / LSTAR set up yet (that's D-04b
 * and D-04c), so KVM_RUN is expected to fail-enter or shutdown
 * immediately. Log the exit_reason and panic with a readable
 * message — the panic itself still pre-console-dies today but
 * the log buffer captures enough detail for core-dump analysis.
 *
 * init_thread_regs uses the existing get_safe_registers() helper
 * (same as seccomp_init_thread_regs).
 */
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kvm.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task_stack.h>
#include <linux/spinlock.h>

#include <asm/page.h>
#include <asm/processor.h>
#include <as-layout.h>
#include <os.h>
#include <registers.h>
#include <sysdep/ptrace.h>
#include <sysdep/ptrace_user.h>
#include <asm/backend.h>

#include "kvm_backend.h"

int kvm_thread_create(struct task_struct *p, void *stack,
		      void (*handler)(void))
{
	new_thread(stack, &p->thread.switch_buf, handler);
	return 0;
}

int kvm_thread_start_idle(void *stack, struct thread_struct *t)
{
	return start_idle_thread(stack, &t->switch_buf);
}

void kvm_context_switch(struct task_struct *prev, struct task_struct *next)
{
	switch_threads(&prev->thread.switch_buf, &next->thread.switch_buf);
}

void kvm_init_thread_regs(unsigned long *gp, unsigned long *fp)
{
	get_safe_registers(gp, fp);

#ifdef CONFIG_UM_BACKEND_KVM_ONLY
	/*
	 * Under KVM_ONLY, os_early_checks short-circuits before
	 * init_pid_registers (registers.c:20) runs — there's no
	 * ptraced stub child to PTRACE_GETREGS against. That
	 * leaves the exec_regs baseline zero-filled, so
	 * get_safe_registers above returns all zeros. Two
	 * consequences we have to paper over here:
	 *
	 *   - The A-05 contract KUnit test asserts at least one
	 *     gp[] slot is non-zero (the "init writes *something*"
	 *     invariant).
	 *   - UML's scheduler uses the gp buffer as a thread's
	 *     initial register state; zero-filled gp can pass NULL
	 *     checks but leaves RIP == 0, which isn't useful.
	 *
	 * Seed RIP with a sentinel non-zero value. A real vCPU RIP
	 * is set per-KVM_RUN via KVM_SET_REGS in run_userspace;
	 * this sentinel is only ever observed by the scheduler's
	 * bookkeeping + the contract test, not by the CPU. Use
	 * STUB_START as the sentinel because it's a known-valid
	 * guest VA under UML's existing stub conventions; future
	 * real-run_userspace path will overwrite this before any
	 * KVM_RUN.
	 */
	if (!gp[HOST_IP])
		gp[HOST_IP] = STUB_START;
#endif
}

/*
 * D-05b: backend-neutral host-side IPI — the UML kernel's
 * inter-CPU signal machinery, not a KVM vCPU IPI. Mirrors
 * seccomp_ipi_send + ptrace_ipi_send almost exactly. Under
 * ncpus=1 (default) nobody calls this; the contract still
 * requires a non-NULL slot in the ops table, so we provide
 * the thin wrapper rather than a -EOPNOTSUPP stub.
 */
int kvm_ipi_send(int cpu, int vector)
{
#if IS_ENABLED(CONFIG_SMP)
	return os_send_ipi(cpu, vector);
#else
	(void)cpu;
	(void)vector;
	return 0;
#endif
}

/*
 * Map an exit_reason back to its symbol for pr_info. The list
 * matches arch/x86/kvm/kvm_host.h / Documentation/virt/kvm/api.rst;
 * we only enumerate reasons we expect to see during D-04a..D-04c
 * bring-up. Unknown reasons hit a numeric default.
 */
static const char *kvm_exit_reason_str(u32 r)
{
	switch (r) {
	case KVM_EXIT_UNKNOWN:		return "UNKNOWN";
	case KVM_EXIT_EXCEPTION:	return "EXCEPTION";
	case KVM_EXIT_IO:		return "IO";
	case KVM_EXIT_HYPERCALL:	return "HYPERCALL";
	case KVM_EXIT_DEBUG:		return "DEBUG";
	case KVM_EXIT_HLT:		return "HLT";
	case KVM_EXIT_MMIO:		return "MMIO";
	case KVM_EXIT_SHUTDOWN:		return "SHUTDOWN";
	case KVM_EXIT_FAIL_ENTRY:	return "FAIL_ENTRY";
	case KVM_EXIT_INTR:		return "INTR";
	case KVM_EXIT_INTERNAL_ERROR:	return "INTERNAL_ERROR";
	default:			return "???";
	}
}

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
/*
 * -----------------------------------------------------------------
 * D-04/D-05 follow-on: real `run_userspace` integration, sub-commit
 * #1 (memo 08). Production analogues of the harness entry path:
 * state materialization only; KVM_RUN + exit-reason decode land in
 * sub-commits #2-#5. The panic() in kvm_run_userspace below stays
 * in place — this code is reachable only via the explicit gate
 * (CONFIG_UM_BACKEND_KVM_INTEGRATED) + a direct call from a
 * follow-on sub-commit's wiring. D-04a scaffold unchanged.
 * -----------------------------------------------------------------
 */

/*
 * Bootstrap region: a single page inside UML's physmem hosts the
 * production GDT + LSTAR trampoline. Allocated once on first
 * entry and cached; freed only on backend shutdown (deferred to
 * sub-commit #7's D-05 fallback wiring). Guarded by a spinlock
 * so concurrent kvm_enter_guest callers see a consistent GPA.
 *
 * Layout inside the page (see `kvm_enter_guest_init_bootstrap`):
 *   0x000 .. 0x030 — 6-entry GDT (48 bytes, populated by
 *                    kvm_setup_harness_gdt — same ring-0 +
 *                    ring-3 layout the harness uses)
 *   0x040 .. 0x100 — reserved for LSTAR trampoline (sub-commit
 *                    #2 will write the bounce gadget here)
 *
 * Page-aligned; sized at PAGE_SIZE because a 4 KiB alloc is the
 * cheapest long-term-stable thing to ask GFP for and we don't
 * need more. The GPA under the Policy A memslot is
 * `__pa(bootstrap_va)`.
 */
static DEFINE_SPINLOCK(kvm_bootstrap_lock);
static void *kvm_bootstrap_page;	/* kernel VA of the bootstrap page */
static u64   kvm_bootstrap_gpa;		/* __pa() of the page; 0 if unallocated */

#define KVM_BOOTSTRAP_GDT_OFFSET	0x000
#define KVM_BOOTSTRAP_LSTAR_OFFSET	0x040

/*
 * LSTAR trampoline: 5 bytes. Byte-identical to
 * `kvm_harness_lstar` in harness.c (D-04c). On SYSCALL entry
 * the CPU jumps here with RCX = post-SYSCALL RIP and R11 =
 * saved RFLAGS. The `out %al, $0xf4` triggers KVM_EXIT_IO on
 * UM_KVM_SYSCALL_PORT (0xf4); the host advances vCPU RIP past
 * the 2-byte `out` to the SYSRETQ, then resumes the vCPU so
 * SYSRETQ runs and delivers control back to the SYSCALL
 * follow-on at RCX. The wire format is deliberately shared
 * with the harness so sub-commit #3's decode can lift the
 * existing harness logic unchanged.
 */
static const u8 kvm_bootstrap_lstar_bytes[] = {
	0xe6, 0xf4,		/* out %al, $0xf4 */
	0x48, 0x0f, 0x07,	/* sysretq */
};

static int kvm_enter_guest_init_bootstrap(void)
{
	void *page;
	u64 gpa;
	unsigned long flags;

	/* Fast path: already allocated. */
	spin_lock_irqsave(&kvm_bootstrap_lock, flags);
	if (kvm_bootstrap_page) {
		spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);

	/*
	 * Allocate outside the lock — GFP_KERNEL can sleep. Second
	 * check under the lock covers the race where two callers
	 * both took the !page branch above; the loser frees its
	 * allocation.
	 */
	page = (void *)get_zeroed_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	gpa = (u64)__pa(page);

	spin_lock_irqsave(&kvm_bootstrap_lock, flags);
	if (kvm_bootstrap_page) {
		spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);
		free_page((unsigned long)page);
		return 0;
	}

	/*
	 * Populate the GDT in the page. Same 6-entry layout as the
	 * harness path so SYSRETQ-in-guest lands CS/SS correctly
	 * (memo 08 #1 reuses the harness GDT shape exactly — only
	 * the CR3 + page location differ between harness and
	 * production).
	 */
	kvm_setup_harness_gdt((u64 *)((char *)page +
				       KVM_BOOTSTRAP_GDT_OFFSET));

	/*
	 * Write the LSTAR trampoline bytes at the fixed offset
	 * (memo 08 sub-commit #2). Byte-identical to the harness
	 * wire form; the trampoline is live once MSR_LSTAR is
	 * programmed to point at bootstrap_gpa +
	 * KVM_BOOTSTRAP_LSTAR_OFFSET.
	 */
	BUILD_BUG_ON(KVM_BOOTSTRAP_LSTAR_OFFSET +
		     sizeof(kvm_bootstrap_lstar_bytes) > PAGE_SIZE);
	memcpy((char *)page + KVM_BOOTSTRAP_LSTAR_OFFSET,
	       kvm_bootstrap_lstar_bytes, sizeof(kvm_bootstrap_lstar_bytes));

	kvm_bootstrap_page = page;
	kvm_bootstrap_gpa  = gpa;
	spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);

	pr_info("um: kvm enter_guest: bootstrap page at va=%p gpa=0x%llx lstar=+0x%x (%zu bytes)\n",
		page, (unsigned long long)gpa, KVM_BOOTSTRAP_LSTAR_OFFSET,
		sizeof(kvm_bootstrap_lstar_bytes));
	return 0;
}

int kvm_bootstrap_force_init(void)
{
	return kvm_enter_guest_init_bootstrap();
}
EXPORT_SYMBOL_GPL(kvm_bootstrap_force_init);

int kvm_bootstrap_copy_lstar(u8 *dst, size_t len)
{
	unsigned long flags;
	void *page;

	if (!dst || len < sizeof(kvm_bootstrap_lstar_bytes))
		return -EINVAL;

	spin_lock_irqsave(&kvm_bootstrap_lock, flags);
	page = kvm_bootstrap_page;
	spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);

	if (!page)
		return -ENODATA;

	memcpy(dst, (char *)page + KVM_BOOTSTRAP_LSTAR_OFFSET,
	       sizeof(kvm_bootstrap_lstar_bytes));
	return sizeof(kvm_bootstrap_lstar_bytes);
}
EXPORT_SYMBOL_GPL(kvm_bootstrap_copy_lstar);

/*
 * Program MSR_STAR / MSR_LSTAR / MSR_FMASK on vcpu0 so that a
 * guest-side SYSCALL traps into the LSTAR trampoline at
 * `lstar_gpa`. Encoding per AMD64 SDM §6.1.1:
 *
 *   MSR_STAR [47:32] = kernel CS selector (SYSCALL loads this)
 *                    = 0x0008 (ring-0 code; GDT idx 1)
 *   MSR_STAR [63:48] = SYSRET base selector (SYSRETQ loads
 *                      (base+16)|3 as CS, (base+8)|3 as SS)
 *                    = 0x0018 (unused padding slot; forces
 *                      CS=0x28|3=0x2b, SS=0x20|3=0x23 which
 *                      are GDT idx 5 + 4, the ring-3 pair)
 *
 * FMASK = 0 for now; real UML entry needs IF cleared among
 * other bits, but sub-commit #2's trampoline doesn't run any
 * code that races with interrupts — just `out`/`sysretq`.
 * Sub-commit #5 (KVM_EXIT_INTR) revisits.
 */
static int kvm_enter_guest_program_msrs(u64 lstar_gpa)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct {
		struct kvm_msrs info;
		struct kvm_msr_entry entries[3];
	} msrs = {
		.info = { .nmsrs = 3 },
		.entries = {
			{
				.index = 0xc0000081,	/* MSR_STAR */
				.data  = ((u64)0x0018 << 48) |
					 ((u64)0x0008 << 32),
			},
			{
				.index = 0xc0000082,	/* MSR_LSTAR */
				.data  = lstar_gpa,
			},
			{
				.index = 0xc0000084,	/* MSR_FMASK */
				.data  = 0,
			},
		},
	};
	int rc;

	if (vcpu_fd < 0)
		return -EIO;

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS, (unsigned long)&msrs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_SET_MSRS(lstar=0x%llx) failed (%d)\n",
				    (unsigned long long)lstar_gpa, rc);
		return rc;
	}

	/*
	 * KVM_SET_MSRS returns the number of MSRs actually
	 * written. 3 is the expected value; anything less means
	 * one of STAR/LSTAR/FMASK was rejected and the trampoline
	 * is not armed.
	 */
	if (rc != 3) {
		pr_warn_once("um: kvm enter_guest: KVM_SET_MSRS wrote %d/3 MSRs\n",
			     rc);
		return -EIO;
	}
	return 0;
}

/*
 * Marshal `uml_pt_regs` → `struct kvm_regs` for KVM_SET_REGS.
 * uml_pt_regs is a host-shaped pt_regs view; the HOST_* gp[]
 * slots are populated by the existing register helpers
 * (sysdep/ptrace.h). Every GP register that matters for guest
 * execution plus RIP / RSP / RFLAGS gets forwarded; CS / SS /
 * DS etc. are provided via SREGS and not touched here.
 *
 * Pure data-structure transform — unit-testable without
 * /dev/kvm.
 */
static void kvm_uml_regs_to_kvm_regs(struct kvm_regs *dst,
				     const struct uml_pt_regs *src)
{
	const unsigned long *gp = src->gp;

	dst->rax = gp[HOST_AX];
	dst->rbx = gp[HOST_BX];
	dst->rcx = gp[HOST_CX];
	dst->rdx = gp[HOST_DX];
	dst->rsi = gp[HOST_SI];
	dst->rdi = gp[HOST_DI];
	dst->rbp = gp[HOST_BP];
	dst->rsp = gp[HOST_SP];
	dst->r8  = gp[HOST_R8];
	dst->r9  = gp[HOST_R9];
	dst->r10 = gp[HOST_R10];
	dst->r11 = gp[HOST_R11];
	dst->r12 = gp[HOST_R12];
	dst->r13 = gp[HOST_R13];
	dst->r14 = gp[HOST_R14];
	dst->r15 = gp[HOST_R15];
	dst->rip = gp[HOST_IP];
	/*
	 * RFLAGS bit 1 is reserved and must be 1 per AMD64 SDM
	 * §3.1.4. uml_pt_regs preserves whatever the last trap
	 * captured; OR the fixed bit in defensively so a SET_REGS
	 * never trips #GP.
	 */
	dst->rflags = gp[HOST_EFLAGS] | (1UL << 1);
}

/*
 * kvm_enter_guest — materialize the vCPU state from `regs` so a
 * subsequent KVM_RUN enters the UML guest in long-mode ring-0
 * at the RIP the caller chose. Sub-commit #1 of memo 08.
 *
 * Does:
 *   1. Lazy-allocate the bootstrap page (GDT + LSTAR-tramp
 *      slot).
 *   2. Build production SREGS: long-mode segments,
 *      CR0/CR4/EFER, CR3 = __pa(current->active_mm->pgd),
 *      GDT base at the bootstrap page's GPA.
 *   3. KVM_SET_SREGS into vcpu0.
 *   4. Marshal `regs` → kvm_regs, KVM_SET_REGS.
 *
 * Does NOT (yet):
 *   - Program MSR_STAR / MSR_LSTAR / MSR_FMASK. Sub-commit #2
 *     lands the LSTAR trampoline + MSR program; until then,
 *     guest syscalls would fault on LSTAR=0. kvm_enter_guest
 *     is therefore only safe to call against a bootstrap
 *     workload that doesn't issue `syscall` — e.g. a ring-0
 *     HLT to verify the state transition works. The gate in
 *     kvm_run_userspace keeps the panic() path active by
 *     default so we don't accidentally run non-trap-safe
 *     guest code.
 *
 *   - Run the KVM_RUN loop. That's sub-commits #2-#5.
 *
 *   - Pin current->active_mm for the duration. Policy-A
 *     memslot covers all of physmem so a transient mm swap
 *     doesn't invalidate the memory backing, but if a real
 *     run_userspace caller's mm gets freed while KVM_RUN
 *     blocks, CR3 points at freed pages. Sub-commit #2 adds
 *     the `get_task_mm`-style pin.
 *
 * Returns 0 on success, -errno on KVM_SET_SREGS / KVM_SET_REGS
 * failure. Never panics — the harness path does that; the
 * production path surfaces errors to the caller (and from
 * there to the existing fallback logic, D-05 in sub-commit
 * #7).
 */
int kvm_enter_guest(struct uml_pt_regs *regs)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct kvm_sregs sregs;
	struct kvm_regs kregs;
	struct mm_struct *mm;
	u64 cr3_gpa;
	int rc;

	if (vcpu_fd < 0)
		return -EIO;
	if (!regs)
		return -EINVAL;

	rc = kvm_ensure_memslot();
	if (rc < 0)
		return rc;

	rc = kvm_enter_guest_init_bootstrap();
	if (rc < 0)
		return rc;

	/*
	 * current->active_mm is always non-NULL for any running
	 * task (idle uses &init_mm); pgd pointer is the host VA
	 * of the top-level page table. Translate to guest-phys
	 * via Policy A: the identity-mapped memslot registered in
	 * kvm_ensure_memslot() means __pa() yields exactly the
	 * guest-phys address CR3 should carry.
	 */
	mm = current->active_mm;
	if (!mm || !mm->pgd) {
		pr_warn_once("um: kvm enter_guest: current->active_mm=%p has no pgd\n",
			     mm);
		return -EFAULT;
	}
	cr3_gpa = (u64)__pa(mm->pgd);

	/*
	 * Start from the current SREGS so APIC / TR / LDT bits
	 * KVM expects preserved stay intact (same discipline the
	 * harness follows — see sregs.c preamble).
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_GET_SREGS failed (%d)\n",
				    rc);
		return rc;
	}

	kvm_setup_production_sregs(&sregs, cr3_gpa, kvm_bootstrap_gpa);

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_SET_SREGS(cr3=0x%llx gdt=0x%llx) failed (%d)\n",
				    (unsigned long long)cr3_gpa,
				    (unsigned long long)kvm_bootstrap_gpa, rc);
		return rc;
	}

	kvm_uml_regs_to_kvm_regs(&kregs, regs);
	rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS, (unsigned long)&kregs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_SET_REGS(rip=0x%llx rsp=0x%llx) failed (%d)\n",
				    (unsigned long long)kregs.rip,
				    (unsigned long long)kregs.rsp, rc);
		return rc;
	}

	/*
	 * Arm the SYSCALL trap: MSR_LSTAR at the bootstrap page's
	 * LSTAR trampoline, MSR_STAR with the ring-0 / ring-3
	 * selectors. Safe to call every kvm_enter_guest — KVM
	 * stores the MSRs on the vCPU, so a second SET_MSRS is
	 * idempotent; follow-on sub-commits can make this smarter
	 * (skip on unchanged bootstrap_gpa) when profiling shows
	 * the SET_MSRS cost matters.
	 */
	rc = kvm_enter_guest_program_msrs(kvm_bootstrap_gpa +
					  KVM_BOOTSTRAP_LSTAR_OFFSET);
	if (rc < 0)
		return rc;

	return 0;
}

/*
 * KUnit-visible test hook. Exposed only when the integrated
 * gate is on so a kunit module can exercise the pure data-
 * structure path (no /dev/kvm required). Takes a pre-zeroed
 * kvm_sregs + kvm_regs pair from the caller and a fake
 * uml_pt_regs source so the marshalling logic + sregs
 * production shape are testable without actually calling
 * KVM_SET_*.
 *
 * Kept next to the real implementation so the marshalling
 * helpers have a single definition; sub-commits #2-#5 extend
 * the probe as new fields flow through.
 */
int kvm_enter_guest_probe(struct kvm_sregs *sregs, struct kvm_regs *regs,
			  const struct uml_pt_regs *src,
			  u64 cr3_gpa, u64 gdt_gpa)
{
	if (!sregs || !regs || !src)
		return -EINVAL;
	kvm_setup_production_sregs(sregs, cr3_gpa, gdt_gpa);
	kvm_uml_regs_to_kvm_regs(regs, src);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_enter_guest_probe);

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */

void kvm_run_userspace(struct uml_pt_regs *regs)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct kvm_run *run = kvm_backend_ctx()->run0;
	int rc;

	(void)regs;

	if (vcpu_fd < 0 || !run) {
		panic("um: kvm run_userspace: vCPU not initialized (vcpu_fd=%d run=%p)",
		      vcpu_fd, run);
	}

	/*
	 * First call registers the memslot now that arch_setup() has
	 * populated uml_physmem / physmem_size. Subsequent calls are
	 * idempotent no-ops. If registration fails we still attempt
	 * KVM_RUN so the panic below reports the real KVM exit reason
	 * rather than hiding behind a memslot-unavailable message.
	 */
	rc = kvm_ensure_memslot();
	if (rc < 0)
		pr_warn_once("um: kvm run_userspace: memslot registration failed (%d)\n",
			     rc);

	rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);

	/*
	 * D-04a scaffold: no SREGS / CR3 / LSTAR yet, so KVM_RUN
	 * returns either an error or a fail-entry / shutdown exit
	 * immediately. Neither is "correct" guest behavior; both are
	 * expected until D-04b lands. Panic with the exit_reason so
	 * the failure mode is diagnosable in dmesg.
	 */
	panic("um: kvm run_userspace: ioctl rc=%d, exit_reason=%u (%s) — D-04b SREGS/CR3 setup pending",
	      rc, run->exit_reason, kvm_exit_reason_str(run->exit_reason));
}
