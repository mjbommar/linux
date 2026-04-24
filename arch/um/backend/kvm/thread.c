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

#include <linux/signal.h>	/* SIGSEGV for sig_info dispatch */

#include <asm/page.h>
#include <asm/processor.h>
#include <as-layout.h>
#include <kern_util.h>
#include <mem.h>		/* uml_physmem */
#include <os.h>
#include <registers.h>
#include <sysdep/faultinfo.h>
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
static u64   kvm_bootstrap_va;		/* kernel VA as a u64 (linear address
					 * the guest CR3 walk resolves to the
					 * bootstrap page's gpa; used for
					 * GDTR / LSTAR / RIP where the CPU
					 * expects a linear, not physical,
					 * address).
					 */

#define KVM_BOOTSTRAP_GDT_OFFSET	0x000
#define KVM_BOOTSTRAP_LSTAR_OFFSET	0x040
#define KVM_BOOTSTRAP_SYSRET_OFFSET	0x080

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

/*
 * Ring-3 bootstrap trampoline (memo 08 sub-commit #5a): 3 bytes,
 * just SYSRETQ. Placed at a dedicated offset so host-side
 * kvm_enter_guest can point vCPU RIP here on first entry to
 * transition the guest from CPL=0 (where kvm_setup_production_
 * sregs leaves it after long-mode init) into CPL=3 at the
 * caller's intended user RIP. Setup discipline:
 *
 *   - MSR_STAR[63:48] = 0x18 (bootstrap via #2a's kvm_enter_
 *     guest_program_msrs): SYSRETQ loads CS=(0x18+16)|3=0x2b
 *     (ring-3 code, GDT idx 5) and SS=(0x18+8)|3=0x23 (ring-3
 *     data, GDT idx 4).
 *   - RCX = user's intended RIP (KVM_SET_REGS).
 *   - R11 = 0x3202 (RFLAGS with IF=1, bit-1 reserved-one,
 *     IOPL=3); SYSRETQ loads RFLAGS from R11.
 *   - RIP = bootstrap_va + KVM_BOOTSTRAP_SYSRET_OFFSET.
 *
 * The 3-byte sequence is byte-identical to the LSTAR
 * trampoline's tail, but lives at a distinct offset so a fresh
 * first entry doesn't accidentally trip the OUT-before-SYSRETQ.
 * Session finding (commit 386c04a3219b diagnostic dump): this
 * gadget is what moves the guest from running init in ring-0
 * (the previously-observed CPL=0 triple-fault state) into
 * correct ring-3 execution.
 */
static const u8 kvm_bootstrap_sysret_bytes[] = {
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
	 * programmed to point at bootstrap_va +
	 * KVM_BOOTSTRAP_LSTAR_OFFSET.
	 */
	BUILD_BUG_ON(KVM_BOOTSTRAP_LSTAR_OFFSET +
		     sizeof(kvm_bootstrap_lstar_bytes) > PAGE_SIZE);
	memcpy((char *)page + KVM_BOOTSTRAP_LSTAR_OFFSET,
	       kvm_bootstrap_lstar_bytes, sizeof(kvm_bootstrap_lstar_bytes));

	/*
	 * Write the ring-3 bootstrap SYSRETQ gadget (sub-commit
	 * #5a). kvm_enter_guest points first-entry RIP at this
	 * location to transition the guest into CPL=3 at the
	 * user's chosen RIP/RFLAGS.
	 */
	BUILD_BUG_ON(KVM_BOOTSTRAP_SYSRET_OFFSET +
		     sizeof(kvm_bootstrap_sysret_bytes) > PAGE_SIZE);
	memcpy((char *)page + KVM_BOOTSTRAP_SYSRET_OFFSET,
	       kvm_bootstrap_sysret_bytes, sizeof(kvm_bootstrap_sysret_bytes));

	kvm_bootstrap_page = page;
	kvm_bootstrap_gpa  = gpa;
	kvm_bootstrap_va   = (u64)(unsigned long)page;
	spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);

	pr_info("um: kvm enter_guest: bootstrap page at va=%p gpa=0x%llx lstar=+0x%x sysret=+0x%x (%zu + %zu bytes)\n",
		page, (unsigned long long)gpa,
		KVM_BOOTSTRAP_LSTAR_OFFSET, KVM_BOOTSTRAP_SYSRET_OFFSET,
		sizeof(kvm_bootstrap_lstar_bytes),
		sizeof(kvm_bootstrap_sysret_bytes));
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
				     const struct uml_pt_regs *src);

/*
 * Reverse marshal: `struct kvm_regs` → `uml_pt_regs`. Called
 * from sub-commit #2b's KVM_RUN loop after KVM_EXIT_IO /
 * KVM_EXIT_MMIO / KVM_EXIT_INTR so UML's common syscall
 * dispatch (and any downstream fault / signal logic) sees
 * the guest's current GP register state.
 *
 * Mirrors the forward marshal exactly. HOST_ORIG_AX is NOT
 * set here — that's an UML-entry-path convention the caller
 * of kvm_decode_syscall arranges once it knows the syscall
 * bucket. Pure data-structure transform; no ioctl.
 */
static void kvm_regs_to_uml_regs(struct uml_pt_regs *dst,
				 const struct kvm_regs *src)
{
	unsigned long *gp = dst->gp;

	gp[HOST_AX]     = src->rax;
	gp[HOST_BX]     = src->rbx;
	gp[HOST_CX]     = src->rcx;
	gp[HOST_DX]     = src->rdx;
	gp[HOST_SI]     = src->rsi;
	gp[HOST_DI]     = src->rdi;
	gp[HOST_BP]     = src->rbp;
	gp[HOST_SP]     = src->rsp;
	gp[HOST_R8]     = src->r8;
	gp[HOST_R9]     = src->r9;
	gp[HOST_R10]    = src->r10;
	gp[HOST_R11]    = src->r11;
	gp[HOST_R12]    = src->r12;
	gp[HOST_R13]    = src->r13;
	gp[HOST_R14]    = src->r14;
	gp[HOST_R15]    = src->r15;
	gp[HOST_IP]     = src->rip;
	gp[HOST_EFLAGS] = src->rflags;
}

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

	/*
	 * sregs.gdt.base + LSTAR take LINEAR addresses (the CPU
	 * walks the guest CR3 to resolve them), not physical.
	 * Pass the bootstrap page's kernel VA; its walk through
	 * current->active_mm->pgd resolves to bootstrap_gpa under
	 * the Policy A identity memslot. Memo 08 sub-commit #5a
	 * session finding: previously passing a gpa here worked
	 * by accident under the harness's identity-paging setup
	 * (KVM_HARNESS_GDT_OFFSET=0x3000 happens to be a valid
	 * VA in the harness's own 2 MiB mapping); under the
	 * production CR3 (current->active_mm->pgd), that
	 * accidental identity doesn't hold.
	 */
	kvm_setup_production_sregs(&sregs, cr3_gpa, kvm_bootstrap_va);

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_SET_SREGS(cr3=0x%llx gdt_va=0x%llx) failed (%d)\n",
				    (unsigned long long)cr3_gpa,
				    (unsigned long long)kvm_bootstrap_va, rc);
		return rc;
	}

	/*
	 * Build vCPU regs from UML regs, then overlay the ring-3
	 * bootstrap shape (sub-commit #5a):
	 *
	 *   RIP = bootstrap_va + SYSRET_OFFSET  — dedicated
	 *                                          3-byte SYSRETQ
	 *                                          gadget.
	 *   RCX = user's intended RIP           — SYSRETQ loads RIP
	 *                                          from RCX.
	 *   R11 = 0x3202                        — SYSRETQ loads
	 *                                          RFLAGS from R11.
	 *                                          0x3202 = IF=1,
	 *                                          bit-1 reserved,
	 *                                          IOPL=3 so ring-3
	 *                                          can OUT on
	 *                                          debug ports.
	 *
	 * RAX / RSP / etc. pass through from UML regs — they're
	 * the user process's GP state.
	 */
	kvm_uml_regs_to_kvm_regs(&kregs, regs);
	kregs.rcx    = kregs.rip;			/* preserve user RIP */
	kregs.r11    = 0x3202;				/* user RFLAGS */
	kregs.rip    = kvm_bootstrap_va + KVM_BOOTSTRAP_SYSRET_OFFSET;
	kregs.rflags = (1UL << 1);			/* ring-0 RFLAGS */

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS, (unsigned long)&kregs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_SET_REGS(tramp_rip=0x%llx user_rip=0x%llx) failed (%d)\n",
				    (unsigned long long)kregs.rip,
				    (unsigned long long)kregs.rcx, rc);
		return rc;
	}

	/*
	 * Arm the SYSCALL trap: MSR_LSTAR at the bootstrap page's
	 * LSTAR trampoline (linear address, same rationale as
	 * GDT). MSR_STAR carries the ring-0 / ring-3 selectors;
	 * also used by the ring-3 bootstrap SYSRETQ above.
	 * Idempotent on repeat entry — KVM stores the MSRs on
	 * the vCPU.
	 */
	rc = kvm_enter_guest_program_msrs(kvm_bootstrap_va +
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

/*
 * Reverse-direction probe: exercises the kvm_regs → uml_pt_regs
 * marshal. Sub-commit #2b's KVM_RUN loop feeds KVM_GET_REGS
 * output into this helper; for the contract test today it's a
 * round-trip fidelity check.
 */
int kvm_exit_guest_probe(struct uml_pt_regs *dst, const struct kvm_regs *src)
{
	if (!dst || !src)
		return -EINVAL;
	kvm_regs_to_uml_regs(dst, src);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_exit_guest_probe);

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */

/*
 * kvm_run_userspace: backend's run_userspace op. Called in a
 * loop by arch/um/os-Linux/skas/process.c::userspace(regs).
 * Contract per Documentation/virt/uml/backend-contract.rst:
 * set up vCPU, run guest until the next trap, fill `regs`
 * with current state (+ regs->is_user=1 + HOST_ORIG_AX for
 * syscalls), dispatch the kernel-side handler, return.
 *
 * Two compile-time flavors:
 *   - CONFIG_UM_BACKEND_KVM_INTEGRATED=y: real KVM_RUN loop
 *     + exit-reason decode (memo 08 sub-commit #2b).
 *   - CONFIG_UM_BACKEND_KVM_INTEGRATED=n: D-04a scaffold
 *     panic, unchanged.
 */
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED

/*
 * Called from the syscall-exit branch of kvm_run_userspace.
 * Populates `regs` with the guest's user-mode state read back
 * from KVM_GET_REGS, advances the saved RIP past the 2-byte
 * `out %al, $0xf4` so the post-dispatch KVM_RUN resumes at
 * the trampoline's SYSRETQ, and dispatches the syscall into
 * UML's common sys_call_table path. After dispatch, the
 * caller marshals regs->gp[HOST_AX] (= syscall return) back
 * to vCPU state for the SYSRETQ to deliver to ring-3.
 */
static void kvm_decode_syscall(struct uml_pt_regs *regs,
			       struct kvm_regs *kregs, int vcpu_fd)
{
	/*
	 * `regs->gp[]` already populated by the caller's KVM_GET_REGS
	 * → kvm_regs_to_uml_regs(). RAX holds the guest's original
	 * syscall number (the LSTAR trampoline's `out` didn't
	 * clobber it — the post-out RIP/RFLAGS live in RCX/R11).
	 */
	PT_SYSCALL_NR(regs->gp) = regs->gp[HOST_AX];
	regs->is_user = 1;

	/*
	 * Advance RIP past the 2-byte `out %al, $0xf4` so the next
	 * KVM_RUN executes SYSRETQ. Mirrored into both regs and
	 * kregs — the subsequent KVM_SET_REGS picks up kregs.rip.
	 */
	kregs->rip += 2;
	regs->gp[HOST_IP] = kregs->rip;

	/*
	 * Common syscall dispatch path. Writes the return value
	 * into regs->gp[HOST_AX]; the caller marshals that back
	 * to vCPU state.
	 */
	handle_syscall(regs);

	/*
	 * Post-dispatch UML convention (lifted from seccomp_run_
	 * userspace): clear UPT_SYSCALL_NR so the caller's is_user
	 * sample doesn't re-dispatch, and reset ORIG_AX to -1 on
	 * architectures where its offset differs from syscall-RET.
	 * On x86_64 they're the same offset (HOST_AX); the guard
	 * is a no-op there but kept for symmetry with the other
	 * backends.
	 */
	UPT_SYSCALL_NR(regs) = -1;
	if (PT_SYSCALL_NR_OFFSET != PT_SYSCALL_RET_OFFSET)
		PT_SYSCALL_NR(regs->gp) = -1;

	/*
	 * Push the syscall return (now in regs->gp[HOST_AX]) plus
	 * the advanced RIP back into the vCPU state so SYSRETQ
	 * delivers the right RAX + resumes at the trampoline's
	 * SYSRETQ.
	 */
	kvm_uml_regs_to_kvm_regs(kregs, regs);
	(void)os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
			       (unsigned long)kregs);
}

void kvm_run_userspace(struct uml_pt_regs *regs)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct kvm_run *run = kvm_backend_ctx()->run0;
	struct kvm_regs kregs;
	int rc;

	if (vcpu_fd < 0 || !run) {
		panic("um: kvm run_userspace: vCPU not initialized (vcpu_fd=%d run=%p)",
		      vcpu_fd, run);
	}

	rc = kvm_enter_guest(regs);
	if (rc < 0)
		panic("um: kvm run_userspace: enter_guest failed (%d)", rc);

	/*
	 * Inner loop: drive the vCPU through as many KVM_RUNs as it
	 * takes to reach a point where the caller's outer userspace()
	 * loop should regain control — HLT (rescheduling) or
	 * KVM_EXIT_INTR (host signal). Syscall exits dispatch
	 * in-line and immediately re-enter KVM_RUN for the SYSRETQ
	 * back to ring-3; this keeps the host-side trap-loop shape
	 * gVisor-compatible (one call into run_userspace = one
	 * logical "resume until something interesting happens").
	 */
	for (;;) {
		rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
		if (rc < 0) {
			/*
			 * -EINTR is the kernel's normal "host signal
			 * interrupted KVM_RUN" path; treat it the same
			 * as KVM_EXIT_INTR — bubble out so the outer
			 * loop's interrupt_end() runs.
			 */
			if (rc == -EINTR)
				goto out_read_regs;
			panic("um: kvm run_userspace: KVM_RUN failed (%d)", rc);
		}

		rc = os_ioctl_generic(vcpu_fd, KVM_GET_REGS,
				      (unsigned long)&kregs);
		if (rc < 0)
			panic("um: kvm run_userspace: KVM_GET_REGS failed (%d)",
			      rc);
		kvm_regs_to_uml_regs(regs, &kregs);

		switch (run->exit_reason) {
		case KVM_EXIT_IO:
			if (run->io.port == UM_KVM_SYSCALL_PORT) {
				kvm_decode_syscall(regs, &kregs, vcpu_fd);
				/*
				 * Next KVM_RUN consumes the trampoline's
				 * SYSRETQ and resumes ring-3.
				 */
				continue;
			}
			if (run->io.port == UM_KVM_SYSRETQ_PORT) {
				/*
				 * Phase III Lift #1b-style ring-3 fallback
				 * emit. Not a normal production flow;
				 * surfaces as a panic so a confused guest
				 * state is caught loudly rather than
				 * silently consumed.
				 */
				panic("um: kvm run_userspace: unexpected ring-3 port 0xf5 exit\n");
			}
			panic("um: kvm run_userspace: KVM_EXIT_IO port=0x%x (unknown)",
			      run->io.port);

		case KVM_EXIT_HLT:
			/*
			 * Guest HLT. Return to the outer userspace()
			 * loop so UML's scheduler can dispatch. is_user
			 * stays 1; HOST_IP points past the HLT.
			 */
			regs->is_user = 1;
			return;

		case KVM_EXIT_INTR:
			goto out_read_regs;

		case KVM_EXIT_MMIO: {
			/*
			 * EPT-level fault: the guest walked its pgd to
			 * a valid gpa, but no memslot backs that gpa.
			 * Route through UML's common fault handler so
			 * mmap-on-demand / swap-in / SIGSEGV delivery
			 * all use the same code path as the ptrace +
			 * seccomp backends.
			 *
			 * Faultinfo mapping (memo 08 sub-commit #3):
			 *   trap_no = 14  (X86 #PF — SEGV_IS_FIXABLE)
			 *   error_code = bit 1 set if write
			 *                bit 2 set if user-mode access
			 *                (regs->is_user already true
			 *                 once the inner loop ran once)
			 *   cr2 = gpa + uml_physmem — the host-VA
			 *         equivalent under the Policy A
			 *         identity memslot. In UML this is
			 *         the address `segv` interprets as the
			 *         faulting-VA; vma lookup uses it.
			 *
			 * Phase III Lift #1d's harness decode template
			 * landed the same shape; this is that logic
			 * lifted into production with regs + fault
			 * handler hooked up.
			 */
			struct faultinfo *fi = UPT_FAULTINFO(regs);

			fi->trap_no    = 14;
			fi->error_code = (run->mmio.is_write ? 2 : 0) |
					 (regs->is_user ? 4 : 0);
			fi->cr2        = (unsigned long)run->mmio.phys_addr +
					 uml_physmem;

			pr_debug("um: kvm run_userspace: KVM_EXIT_MMIO gpa=0x%llx cr2=0x%lx len=%u write=%u\n",
				 (unsigned long long)run->mmio.phys_addr,
				 fi->cr2, run->mmio.len, run->mmio.is_write);

			/*
			 * Dispatch through the same sig_info[SIGSEGV]
			 * table-entry seccomp + ptrace use. siginfo is
			 * NULL here — segv_handler only consumes the
			 * faultinfo we just populated.
			 */
			(*sig_info[SIGSEGV])(SIGSEGV, NULL, regs, NULL);

			/*
			 * If the handler returned, the VMA was found +
			 * the fault was serviced (or queued as a deferred
			 * SIGSEGV in the guest). Re-enter KVM_RUN to let
			 * the guest retry the faulting access.
			 */
			continue;
		}

		case KVM_EXIT_SHUTDOWN:
		case KVM_EXIT_FAIL_ENTRY:
		case KVM_EXIT_INTERNAL_ERROR:
		case KVM_EXIT_EXCEPTION: {
			struct kvm_sregs dump_sregs;
			int dump_rc;
			u64 guest_cr3 = 0, guest_rip = 0;

			/*
			 * Best-effort state snapshot on the failing
			 * vCPU. Helps diagnose bring-up issues
			 * (triple-fault on first instruction,
			 * unmapped CR3, etc.) from a single failing
			 * boot rather than requiring a kgdb dance.
			 * Unused by normal operation since the
			 * calling path already panics.
			 */
			dump_rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
						   (unsigned long)&dump_sregs);
			if (dump_rc >= 0)
				guest_cr3 = dump_sregs.cr3;
			guest_rip = kregs.rip;

			pr_err("um: kvm run_userspace: unrecoverable exit %u (%s)\n",
			       run->exit_reason,
			       kvm_exit_reason_str(run->exit_reason));
			pr_err("um: kvm: guest RIP=0x%llx CR3=0x%llx CS=0x%x CPL=%u is_user=%d\n",
			       (unsigned long long)guest_rip,
			       (unsigned long long)guest_cr3,
			       dump_rc >= 0 ? dump_sregs.cs.selector : 0,
			       dump_rc >= 0 ? dump_sregs.cs.dpl : 0,
			       regs->is_user);
			if (dump_rc >= 0) {
				pr_err("um: kvm: CR0=0x%llx CR4=0x%llx EFER=0x%llx GDTR base=0x%llx limit=0x%x\n",
				       (unsigned long long)dump_sregs.cr0,
				       (unsigned long long)dump_sregs.cr4,
				       (unsigned long long)dump_sregs.efer,
				       (unsigned long long)dump_sregs.gdt.base,
				       dump_sregs.gdt.limit);
			}
			pr_err("um: kvm: bootstrap page va=0x%llx gpa=0x%llx lstar=0x%llx sysret=0x%llx uml_physmem=0x%lx\n",
			       (unsigned long long)kvm_bootstrap_va,
			       (unsigned long long)kvm_bootstrap_gpa,
			       (unsigned long long)(kvm_bootstrap_va +
						    KVM_BOOTSTRAP_LSTAR_OFFSET),
			       (unsigned long long)(kvm_bootstrap_va +
						    KVM_BOOTSTRAP_SYSRET_OFFSET),
			       uml_physmem);
			/*
			 * Walk the guest CR3 by hand for
			 * kvm_bootstrap_va so we can see whether
			 * init's pgd actually covers the bootstrap
			 * page. Under Policy A memslot, gpa = hostva -
			 * uml_physmem, so each pgd/pud/pmd/pte physical
			 * address in the tables is a gpa we can
			 * re-translate via __va() to a host VA we can
			 * safely read. Print the entry values + the
			 * resolved host VA at each level. A zero entry
			 * on the path means the VA is unmapped in this
			 * CR3 — the smoking gun for the CPL=0 triple-
			 * fault. Safe to do here because we already
			 * panic next; no re-entry concern.
			 */
			{
				u64 va  = kvm_bootstrap_va;
				u64 cr3 = guest_cr3 & ~0xfffULL;
				u64 *pgd_va, *pud_va, *pmd_va, *pte_va;
				u64 pgde = 0, pude = 0, pmde = 0, pte = 0;
				unsigned int pgd_i, pud_i, pmd_i, pte_i;

				pgd_i = (va >> 39) & 0x1ff;
				pud_i = (va >> 30) & 0x1ff;
				pmd_i = (va >> 21) & 0x1ff;
				pte_i = (va >> 12) & 0x1ff;

				if (cr3 && cr3 < physmem_size) {
					pgd_va = (u64 *)__va(cr3);
					pgde = pgd_va[pgd_i];
					pr_err("um: kvm: pgd[%u]@0x%llx = 0x%llx\n",
					       pgd_i, (unsigned long long)cr3,
					       (unsigned long long)pgde);
				} else {
					pr_err("um: kvm: CR3=0x%llx out of physmem (size=0x%llx); cannot walk\n",
					       (unsigned long long)cr3,
					       (unsigned long long)physmem_size);
				}
				if ((pgde & 1) && !(pgde & (1ULL << 7))) {
					u64 pud_pa = pgde & 0x000ffffffffff000ULL;

					pud_va = (u64 *)__va(pud_pa);
					pude = pud_va[pud_i];
					pr_err("um: kvm: pud[%u]@0x%llx = 0x%llx\n",
					       pud_i,
					       (unsigned long long)pud_pa,
					       (unsigned long long)pude);
				}
				if ((pude & 1) && !(pude & (1ULL << 7))) {
					u64 pmd_pa = pude & 0x000ffffffffff000ULL;

					pmd_va = (u64 *)__va(pmd_pa);
					pmde = pmd_va[pmd_i];
					pr_err("um: kvm: pmd[%u]@0x%llx = 0x%llx\n",
					       pmd_i,
					       (unsigned long long)pmd_pa,
					       (unsigned long long)pmde);
				}
				if ((pmde & 1) && !(pmde & (1ULL << 7))) {
					u64 pte_pa = pmde & 0x000ffffffffff000ULL;

					pte_va = (u64 *)__va(pte_pa);
					pte = pte_va[pte_i];
					pr_err("um: kvm: pte[%u]@0x%llx = 0x%llx%s\n",
					       pte_i,
					       (unsigned long long)pte_pa,
					       (unsigned long long)pte,
					       (pte & 1) ? "" : " (NOT PRESENT)");
				} else if ((pmde & 1) && (pmde & (1ULL << 7))) {
					pr_err("um: kvm: 2MB huge page at pmd level\n");
				}
			}
			if (run->exit_reason == KVM_EXIT_FAIL_ENTRY) {
				u64 hw = run->fail_entry.hardware_entry_failure_reason;

				pr_err("um: kvm: FAIL_ENTRY hw_reason=0x%llx\n",
				       (unsigned long long)hw);
			}
			if (run->exit_reason == KVM_EXIT_INTERNAL_ERROR)
				pr_err("um: kvm: INTERNAL_ERROR suberror=%u\n",
				       run->internal.suberror);
			panic("um: kvm run_userspace: unrecoverable exit %u (%s)",
			      run->exit_reason,
			      kvm_exit_reason_str(run->exit_reason));
		}

		default:
			panic("um: kvm run_userspace: unknown exit reason %u (%s)",
			      run->exit_reason,
			      kvm_exit_reason_str(run->exit_reason));
		}
	}

out_read_regs:
	/*
	 * Host-side INTR / EINTR path: the outer userspace() loop
	 * calls interrupt_end() on re-entry. regs already reflects
	 * the last successful KVM_GET_REGS (or the pre-KVM_RUN
	 * state on -EINTR, in which case kregs is stale — re-read).
	 */
	if (rc == -EINTR) {
		rc = os_ioctl_generic(vcpu_fd, KVM_GET_REGS,
				      (unsigned long)&kregs);
		if (rc >= 0)
			kvm_regs_to_uml_regs(regs, &kregs);
	}
	regs->is_user = 1;
}

#else /* !CONFIG_UM_BACKEND_KVM_INTEGRATED */

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

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */
