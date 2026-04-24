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
#include <linux/time-internal.h>	/* time_travel_mode + tt_extra_sched_jiffies */
#include <linux/uaccess.h>	/* copy_from_user */

#include <linux/signal.h>	/* SIGSEGV for sig_info dispatch */

#include <asm/page.h>
#include <asm/processor.h>
#include <asm/prctl.h>		/* ARCH_SET_FS / ARCH_SET_GS */
#include <asm/unistd.h>		/* __NR_arch_prctl */
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
 * Force every page in every vma of `mm` to be paged-in by the
 * UML kernel. For writable vmas, do a read-then-write so
 * UML's fault handler installs a read-write PTE; read-only
 * vmas just get a byte read. Called from kvm_enter_guest
 * (pre-KVM_RUN) and again after each syscall dispatch so
 * fresh mmap/brk/mremap regions become visible to the shadow
 * PT refill that follows. Memo 09 step 3 MVP approach; a
 * #5b IDT-based lazy fault-in is the follow-on.
 *
 * Returns the number of pages successfully touched.
 */
static int kvm_touch_all_user_vmas(struct mm_struct *mm)
{
	struct vma_iterator vmi;
	struct vm_area_struct *vma;
	char probe;
	int touched = 0;

	vma_iter_init(&vmi, mm, 0);
	mmap_read_lock(mm);
	for_each_vma(vmi, vma) {
		bool writable = vma->vm_flags & VM_WRITE;
		unsigned long addr;

		for (addr = vma->vm_start; addr < vma->vm_end;
		     addr += PAGE_SIZE) {
			if (copy_from_user(&probe, (void __user *)addr, 1))
				continue;
			if (writable &&
			    copy_to_user((void __user *)addr, &probe, 1))
				continue;
			touched++;
		}
	}
	mmap_read_unlock(mm);
	return touched;
}

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

#define KVM_BOOTSTRAP_GDT_OFFSET	0x000	/* 8 entries × 8 B = 64 B */
#define KVM_BOOTSTRAP_LSTAR_OFFSET	0x040	/* 5..~150-byte trampoline / gadget */
#define KVM_BOOTSTRAP_TSS_OFFSET	0x100	/* 104-byte TSS */
#define KVM_BOOTSTRAP_IDT_OFFSET	0x180	/* 33 × 16 = 528 B */
#define KVM_BOOTSTRAP_PF_HANDLER_OFFSET	0x400	/* 11-byte #PF handler */
#define KVM_BOOTSTRAP_SYSRET_OFFSET	0x420	/* 3-byte SYSRETQ (first-entry helper) */
#define KVM_BOOTSTRAP_STACK_TOP		0x1000	/* ring-0 IST stack top */
#define KVM_BOOTSTRAP_TSS_SEL		0x30	/* GDT entry 6 (16-byte TSS desc) */
#define KVM_BOOTSTRAP_IDT_ENTRIES	33	/* covers #PF (vector 14) */

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
#ifdef CONFIG_UM_BACKEND_KVM_GADGET
/*
 * Memo 11 G4: real systrap gadget body. Entered via
 * MSR_LSTAR on every SYSCALL from ring-3. Handles the 7
 * pid-family syscalls in-guest without a VMEXIT by
 * reading per-task state via swapgs + %gs:<KVM_GADGET_
 * OFF_*>; every other syscall takes the fallback `out
 * %al, $0xf4` VMEXIT path identical to the non-gadget
 * build.
 *
 * Layout (hex offsets from LSTAR_OFFSET = +0x40 in the
 * bootstrap page):
 *
 *   +0 entry:
 *     0f 01 f8          swapgs                         (3 B)
 *
 *   +3 dispatch — 7 cmp $NR, %al; je handler_<NR>:
 *     3c 27 74 <r>      cmp $0x27 (getpid)  je pid_h    (4 B)
 *     3c ba 74 <r>      cmp $0xba (gettid)  je tid_h    (4 B)
 *     3c 6e 74 <r>      cmp $0x6e (getppid) je ppid_h   (4 B)
 *     3c 66 74 <r>      cmp $0x66 (getuid)  je uid_h    (4 B)
 *     3c 6b 74 <r>      cmp $0x6b (geteuid) je euid_h   (4 B)
 *     3c 68 74 <r>      cmp $0x68 (getgid)  je gid_h    (4 B)
 *     3c 6c 74 <r>      cmp $0x6c (getegid) je egid_h   (4 B)
 *     # offset 3 + 28 = 31
 *
 *   +31 fallback (unknown NR — VMEXIT):
 *     0f 01 f8          swapgs  (restore user GS)      (3 B)
 *     e6 f4             out %al, $0xf4                  (2 B)
 *     48 0f 07          sysretq                         (3 B)
 *     # offset 31 + 8 = 39
 *
 *   +39..+102 handlers — 7 × (mov %gs:<OFF>, %eax; jmp tail):
 *     65 8b 04 25 0c 00 00 00  mov %gs:0x08..0x20, %eax (8 B)
 *     eb <r>                   jmp tail                 (2 B)
 *
 *   +109 shared tail:
 *     0f 01 f8          swapgs  (restore user GS)      (3 B)
 *     48 0f 07          sysretq                         (3 B)
 *     # total: 115 B  (fits in +0x40..+0x100 region
 *     # that was reclaimed from the old SYSRET_OFFSET
 *     # which moved to +0x420).
 *
 * The je/jmp displacements below are computed at write
 * time because they depend on the total body layout —
 * the bytes below use placeholders (marked with
 * 0xRR) that the init path patches in. Simpler would be
 * to hardcode them, but keeping the computation
 * explicit makes it easy to add / reorder handlers
 * without a manual recount.
 *
 * Getpid returns tgid (POSIX pid). Gettid returns tid
 * (Linux thread id). See kvm_backend.h for the struct
 * field ↔ offset mapping.
 */
#define KVM_GADGET_TAIL_OFF	109	/* offset of shared tail */
#define KVM_GADGET_FALLBACK_OFF	31	/* offset of fallback `swapgs; out` */

static const u8 kvm_bootstrap_lstar_bytes[] = {
	/* +0   entry */
	0x0f, 0x01, 0xf8,			/* swapgs */

	/*
	 * +3 dispatch. Each line is cmp imm8 + je rel8. JE
	 * rel8 is signed offset from "next instruction after
	 * je," so JE[i] at offset (5+4i) has next-insn at
	 * (7+4i); rel8 = handler_offset - (7+4i).
	 *   +3:  je +32 → pid_h  (+39)
	 *   +7:  je +38 → tid_h  (+49)
	 *   +11: je +44 → ppid_h (+59)
	 *   +15: je +50 → uid_h  (+69)
	 *   +19: je +56 → euid_h (+79)
	 *   +23: je +62 → gid_h  (+89)
	 *   +27: je +68 → egid_h (+99)
	 */
	0x3c, 0x27, 0x74, 32,	/* cmp $0x27 (getpid),  je pid_h  */
	0x3c, 0xba, 0x74, 38,	/* cmp $0xba (gettid),  je tid_h  */
	0x3c, 0x6e, 0x74, 44,	/* cmp $0x6e (getppid), je ppid_h */
	0x3c, 0x66, 0x74, 50,	/* cmp $0x66 (getuid),  je uid_h  */
	0x3c, 0x6b, 0x74, 56,	/* cmp $0x6b (geteuid), je euid_h */
	0x3c, 0x68, 0x74, 62,	/* cmp $0x68 (getgid),  je gid_h  */
	0x3c, 0x6c, 0x74, 68,	/* cmp $0x6c (getegid), je egid_h */

	/* +31  fallback — unknown NR */
	0x0f, 0x01, 0xf8,			/* swapgs (restore user GS) */
	0xe6, 0xf4,				/* out %al, $0xf4 */
	0x48, 0x0f, 0x07,			/* sysretq */

	/* +39  handler_getpid: mov %gs:KVM_GADGET_OFF_TGID, %eax */
	0x65, 0x8b, 0x04, 0x25, 0x08, 0x00, 0x00, 0x00,
	0xeb, 60,				/* jmp +60 → tail */

	/* +49  handler_gettid: mov %gs:KVM_GADGET_OFF_TID, %eax */
	0x65, 0x8b, 0x04, 0x25, 0x0c, 0x00, 0x00, 0x00,
	0xeb, 50,				/* jmp +50 → tail */

	/* +59  handler_getppid: mov %gs:KVM_GADGET_OFF_PPID, %eax */
	0x65, 0x8b, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00,
	0xeb, 40,				/* jmp +40 → tail */

	/* +69  handler_getuid: mov %gs:KVM_GADGET_OFF_UID, %eax */
	0x65, 0x8b, 0x04, 0x25, 0x14, 0x00, 0x00, 0x00,
	0xeb, 30,				/* jmp +30 → tail */

	/* +79  handler_geteuid: mov %gs:KVM_GADGET_OFF_EUID, %eax */
	0x65, 0x8b, 0x04, 0x25, 0x18, 0x00, 0x00, 0x00,
	0xeb, 20,				/* jmp +20 → tail */

	/* +89  handler_getgid: mov %gs:KVM_GADGET_OFF_GID, %eax */
	0x65, 0x8b, 0x04, 0x25, 0x1c, 0x00, 0x00, 0x00,
	0xeb, 10,				/* jmp +10 → tail */

	/* +99  handler_getegid: mov %gs:KVM_GADGET_OFF_EGID, %eax */
	0x65, 0x8b, 0x04, 0x25, 0x20, 0x00, 0x00, 0x00,
	0xeb, 0,				/* jmp +0 — tail is the next byte */

	/* +109 shared tail */
	0x0f, 0x01, 0xf8,			/* swapgs (restore user GS) */
	0x48, 0x0f, 0x07,			/* sysretq */
};
#else
static const u8 kvm_bootstrap_lstar_bytes[] = {
	0xe6, 0xf4,		/* out %al, $0xf4 */
	0x48, 0x0f, 0x07,	/* sysretq */
};
#endif

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

/*
 * #PF handler (memo 08 sub-commit #5b): 11 bytes. CPU delivers
 * #PF via IDT[14] with IST=1 → RSP loaded from TSS.IST[1], SS
 * set to null. Error code pushed on stack. Handler:
 *
 *   0f 20 d0          mov %cr2, %rax    ; read faulting VA
 *   e6 fb             out %al, $0xfb    ; VMEXIT → host fault-fill
 *   48 83 c4 08       add $8, %rsp      ; pop #PF error code
 *   48 cf             iretq             ; return to ring-3 at faulting RIP
 *
 * The `out` triggers KVM_EXIT_IO on UM_KVM_PF_PORT (0xfb). The
 * host reads CR2 via KVM_GET_SREGS, invokes UML's fault path
 * to install the backing page, refreshes the shadow PT, then
 * advances vCPU RIP past the `out` (2 bytes) so the guest
 * continues at `add $8, %rsp; iretq`. IRETQ pops the pushed
 * SS/RSP/RFLAGS/CS/RIP and restores the ring-3 context — the
 * faulting instruction retries and now finds the mapping.
 */
static const u8 kvm_bootstrap_pf_handler_bytes[] = {
	0x0f, 0x20, 0xd0,		/* mov %cr2, %rax  */
	0xe6, 0xfb,			/* out %al, $0xfb  */
	0x48, 0x83, 0xc4, 0x08,		/* add $8, %rsp    */
	0x48, 0xcf,			/* iretq           */
};

#define UM_KVM_PF_PORT	0xfb	/* sub-commit #5b #PF-handler VMEXIT */

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

	/*
	 * Install the #PF handler bytes (sub-commit #5b). The
	 * handler only runs if IDT + TSS are armed via SREGS in
	 * kvm_enter_guest; until that wires up the bytes sit dormant
	 * in the page and cost nothing.
	 */
	BUILD_BUG_ON(KVM_BOOTSTRAP_PF_HANDLER_OFFSET +
		     sizeof(kvm_bootstrap_pf_handler_bytes) > PAGE_SIZE);
	memcpy((char *)page + KVM_BOOTSTRAP_PF_HANDLER_OFFSET,
	       kvm_bootstrap_pf_handler_bytes,
	       sizeof(kvm_bootstrap_pf_handler_bytes));

	/*
	 * Extend the GDT to 8 entries: entries 0-5 were populated
	 * by kvm_setup_harness_gdt above (null, ring-0 code, ring-0
	 * data, padding anchor, ring-3 data, ring-3 code).
	 * Entries 6+7 together form the 16-byte TSS descriptor
	 * selected by KVM_BOOTSTRAP_TSS_SEL (0x30) when SREGS loads
	 * TR. AMD64 SDM vol 3 §4.8.3 describes the long-mode system
	 * segment descriptor layout:
	 *
	 *   bits  0..15  limit[0:15]
	 *   bits 16..31  base[0:15]
	 *   bits 32..39  base[16:23]
	 *   bits 40..47  type=9 | S=0 | DPL=0 | P=1  (0x89)
	 *   bits 48..55  limit[16:19] | AVL | G
	 *   bits 56..63  base[24:31]
	 *   desc[1] bits 0..31   base[32:63]
	 *   desc[1] bits 32..63  reserved (0)
	 */
	{
		u64 tss_base = (u64)(unsigned long)page +
				KVM_BOOTSTRAP_TSS_OFFSET;
		u32 tss_limit = 104 - 1;	/* TSS is 104 bytes */
		u64 *gdt = (u64 *)((char *)page + KVM_BOOTSTRAP_GDT_OFFSET);
		u64 low;

		low  = (u64)(tss_limit & 0xffff);
		low |= ((u64)(tss_base & 0xffff)) << 16;
		low |= ((u64)((tss_base >> 16) & 0xff)) << 32;
		low |= ((u64)0x89) << 40;		/* type=9, P=1 */
		low |= ((u64)((tss_limit >> 16) & 0xf)) << 48;
		low |= ((u64)((tss_base >> 24) & 0xff)) << 56;

		gdt[6] = low;
		gdt[7] = (tss_base >> 32) & 0xffffffffULL;
	}

	/*
	 * Zero the TSS + populate IST[1] only. RSP0/RSP1/RSP2 are
	 * not used (all our cross-CPL transitions go through IDT
	 * entries whose IST field points here). IST[1] top-of-
	 * stack is at bootstrap_va + KVM_BOOTSTRAP_STACK_TOP; stack
	 * grows down into 0xe00-0xfff (256 bytes — plenty for a
	 * single interrupt frame + a handful of temporaries).
	 *
	 * TSS layout (AMD64 SDM vol 3 §10.8.2):
	 *   bytes  0..3   reserved
	 *   bytes  4..11  RSP0
	 *   bytes 12..19  RSP1
	 *   bytes 20..27  RSP2
	 *   bytes 28..35  reserved
	 *   bytes 36..43  IST1   ← populated
	 *   bytes 44..51  IST2
	 *   ...
	 *   bytes 96..99  reserved
	 *   bytes 100..103 I/O map base (set past limit → no I/O bitmap)
	 */
	{
		char *tss = (char *)page + KVM_BOOTSTRAP_TSS_OFFSET;
		u64 ist1 = (u64)(unsigned long)page +
				KVM_BOOTSTRAP_STACK_TOP;

		memset(tss, 0, 104);
		*(u64 *)(tss + 36) = ist1;
		*(u16 *)(tss + 102) = 104;	/* IOPB off-of-limit */
	}

	/*
	 * Populate IDT[14] (#PF). Other vectors stay zero — a hit
	 * is a contract-violation that should fail loudly. Long-
	 * mode IDT entry format (Intel SDM vol 3 §6.14.1):
	 *
	 *   bytes  0..1   offset[0:15]
	 *   bytes  2..3   segment selector (0x08 = ring-0 code)
	 *   byte   4      IST (low 3 bits)
	 *   byte   5      type_attr: P|DPL|0|type
	 *                 0x8E = P=1, DPL=0, 0, type=0xE (interrupt
	 *                        gate, 64-bit)
	 *   bytes  6..7   offset[16:31]
	 *   bytes  8..11  offset[32:63]
	 *   bytes 12..15  reserved (0)
	 */
	{
		u64 handler_va = (u64)(unsigned long)page +
				  KVM_BOOTSTRAP_PF_HANDLER_OFFSET;
		u8 *idt = (u8 *)page + KVM_BOOTSTRAP_IDT_OFFSET;
		u8 *e;

		memset(idt, 0, KVM_BOOTSTRAP_IDT_ENTRIES * 16);
		e = idt + 14 * 16;
		e[0]  = (u8)(handler_va & 0xff);
		e[1]  = (u8)((handler_va >> 8) & 0xff);
		e[2]  = 0x08;			/* ring-0 code selector */
		e[3]  = 0x00;
		e[4]  = 0x01;			/* IST=1 */
		e[5]  = 0x8e;			/* P|DPL0|int-gate */
		e[6]  = (u8)((handler_va >> 16) & 0xff);
		e[7]  = (u8)((handler_va >> 24) & 0xff);
		e[8]  = (u8)((handler_va >> 32) & 0xff);
		e[9]  = (u8)((handler_va >> 40) & 0xff);
		e[10] = (u8)((handler_va >> 48) & 0xff);
		e[11] = (u8)((handler_va >> 56) & 0xff);
		/* bytes 12-15 stay zero from memset. */
	}

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
/*
 * Push MSR_FS_BASE / MSR_GS_BASE into the vCPU. Used by both
 * the initial `kvm_enter_guest` path (seeding from UML's per-
 * task gp[HOST_FS_BASE]) and the post-arch_prctl path in
 * `kvm_decode_syscall` (class B per memo 10). Guest glibc's
 * `_start` issues `arch_prctl(ARCH_SET_FS, tls_addr)` before
 * its first FS-relative load; without this propagation,
 * `fs:0x10` would fault at guest VA 0x10 and spin the
 * bootstrap #PF handler (the cr2=0x10 loop blocking task
 * #192). See Documentation/virt/uml/redesign/02-workstreams/
 * D-kvm-backend/10-syscall-classification.md §"Class B".
 *
 * Idempotent: writing the same FS_BASE twice is a no-op in
 * the vCPU. Called on every arch_prctl regardless of the
 * option (GET_* reads already come from UML's gp[] and
 * pushing them back is harmless).
 */
static int kvm_propagate_fs_gs_base(int vcpu_fd, u64 fs_base, u64 gs_base)
{
	struct {
		struct kvm_msrs info;
		struct kvm_msr_entry entries[2];
	} msrs = {
		.info = { .nmsrs = 2 },
		.entries = {
			{
				.index = 0xc0000100,	/* MSR_FS_BASE */
				.data  = fs_base,
			},
			{
				.index = 0xc0000101,	/* MSR_GS_BASE */
				.data  = gs_base,
			},
		},
	};
	int rc;

	if (vcpu_fd < 0)
		return -EIO;

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS, (unsigned long)&msrs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm: KVM_SET_MSRS(fs=0x%llx gs=0x%llx) failed (%d)\n",
				    (unsigned long long)fs_base,
				    (unsigned long long)gs_base, rc);
		return rc;
	}
	/* Writing 2 MSRs; anything less is a silent reject. */
	if (rc != 2) {
		pr_warn_once("um: kvm: KVM_SET_MSRS(fs/gs) wrote %d/2 MSRs\n",
			     rc);
		return -EIO;
	}
	return 0;
}

/*
 * Memo 11 G3: program MSR_KERNEL_GS_BASE to point at the
 * gadget state page. The gadget's LSTAR handler runs in
 * ring-0, where MSR_GS_BASE holds the USER's %gs (possibly
 * zero, possibly something glibc / arch_prctl(ARCH_SET_GS)
 * set). A `swapgs` at handler entry swaps MSR_GS_BASE
 * with MSR_KERNEL_GS_BASE, making %gs:off resolve to
 * the gadget state page; a second `swapgs` before
 * SYSRETQ restores the user's GS.
 *
 * Separating the gadget state channel from the user's GS
 * keeps userspace arch_prctl(ARCH_SET_GS) semantics
 * intact (memo 11 §"Per-vCPU state channel"); the ~4 cyc
 * cost of two swapgs per gadget call is accounted for in
 * the D71 / memo 11 post-G2 cost model.
 */
static int kvm_enter_guest_program_kernel_gs_base(u64 gadget_state_va)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct {
		struct kvm_msrs info;
		struct kvm_msr_entry entries[1];
	} msrs = {
		.info = { .nmsrs = 1 },
		.entries = {
			{
				.index = 0xc0000102,	/* MSR_KERNEL_GS_BASE */
				.data  = gadget_state_va,
			},
		},
	};
	int rc;

	if (vcpu_fd < 0)
		return -EIO;
	if (!gadget_state_va)
		return 0;	/* unmapped; nothing to program */

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS, (unsigned long)&msrs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_SET_MSRS(kernel_gs_base=0x%llx) failed (%d)\n",
				    (unsigned long long)gadget_state_va, rc);
		return rc;
	}
	if (rc != 1) {
		pr_warn_once("um: kvm enter_guest: KVM_SET_MSRS wrote %d/1 gadget-MSRs\n",
			     rc);
		return -EIO;
	}
	return 0;
}

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
	 * Memo 09 step 2: shadow PT is the guest CR3. Allocate on
	 * first use (kvm_shadow_pgd_alloc is idempotent; lazy per
	 * memo 09 step 1's finding that kvm_init fires before
	 * mm_init) + map the bootstrap page so the guest can fetch
	 * the SYSRETQ gadget + GDT + LSTAR trampoline. Without this
	 * step, D66's triple-fault fires; with it, the guest walks
	 * shadow PT → reaches the bootstrap page → begins executing.
	 *
	 * current->active_mm is still used by kvm_decode_mmio
	 * (memo 09 step 3) to locate the faulting process's
	 * logical pgd for bit-translation; this block no longer
	 * hands its pgd to the CPU as CR3.
	 */
	rc = kvm_shadow_pgd_alloc();
	if (rc < 0)
		return rc;

	rc = kvm_shadow_map_page(kvm_bootstrap_va,
				 (u64)__pa(kvm_bootstrap_page),
				 KVM_X86_PTE_P | KVM_X86_PTE_RW |
				 KVM_X86_PTE_US);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: shadow_map_page(bootstrap) failed (%d)\n",
				    rc);
		return rc;
	}

	/*
	 * Memo 11 G3 — per-vCPU gadget state channel. Allocate
	 * the page lazily (same shape as shadow_pgd_alloc), map
	 * it one page above the bootstrap page (guest VA =
	 * kvm_bootstrap_va + 0x1000), and refresh it from
	 * `current` right before KVM_RUN so gadget handlers
	 * read up-to-date pid/tgid/uid/gid via %gs:<off>. Maps
	 * read-only from ring-3 (P | US) — only the host writes
	 * via the kernel VA alias.
	 */
	rc = kvm_gadget_state_alloc();
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: gadget_state_alloc failed (%d)\n",
				    rc);
		return rc;
	}
	{
		u64 gstate_va = kvm_bootstrap_va + PAGE_SIZE;

		rc = kvm_shadow_map_page(gstate_va,
					 kvm_gadget_state_gpa(),
					 KVM_X86_PTE_P | KVM_X86_PTE_US);
		if (rc < 0) {
			pr_warn_ratelimited("um: kvm enter_guest: shadow_map_page(gadget_state) failed (%d)\n",
					    rc);
			return rc;
		}
		kvm_backend_ctx()->gadget_state_va = gstate_va;
	}
	kvm_gadget_state_refresh();

	cr3_gpa = kvm_shadow_pgd_gpa();
	if (!cr3_gpa) {
		pr_warn_once("um: kvm enter_guest: shadow_pgd_gpa is zero after alloc\n");
		return -EIO;
	}

	/*
	 * Memo 09 step 3: eager-fill the shadow PT from the current
	 * process's logical pgd. Every present UML PTE gets a
	 * matching x86-encoded entry via kvm_um_pte_to_x86. Bounded
	 * by actually-mapped pages (typical /bin/true: 20-50 pages;
	 * larger processes scale linearly).
	 *
	 * Called every kvm_enter_guest today — wasteful on repeat
	 * entries but correct. Optimization (skip when UML pgd
	 * hasn't changed since last fill) is a follow-on lift once
	 * D-06 perf measurements quantify the cost.
	 */
	mm = current->active_mm;
	if (mm && mm->pgd) {
		int touched;
		int filled;

		touched = kvm_touch_all_user_vmas(mm);
		filled = kvm_shadow_fill_from_uml_pgd(mm->pgd);
		if (filled < 0) {
			pr_warn_ratelimited("um: kvm enter_guest: shadow fill failed (%d)\n",
					    filled);
			return filled;
		}
		pr_info_ratelimited("um: kvm enter_guest: touched %d user pages, filled %d shadow PTEs\n",
				    touched, filled);
	}

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

	/*
	 * Sub-commit #5b: arm the IDT + TSS so guest-side #PF gets
	 * routed to the bootstrap #PF handler instead of triple-
	 * faulting. Both base fields are linear addresses (guest
	 * CR3 resolves them through the shadow PT to the bootstrap
	 * page). TR descriptor lives at GDT[6] (selector 0x30);
	 * `type=11` (0xb) marks it as a busy 64-bit TSS after load
	 * — KVM's KVM_SET_SREGS accepts the available-TSS form and
	 * flips the busy bit on load. The kvm_segment structure
	 * mirrors what the CPU caches after an LTR instruction.
	 */
	sregs.idt.base  = kvm_bootstrap_va + KVM_BOOTSTRAP_IDT_OFFSET;
	sregs.idt.limit = KVM_BOOTSTRAP_IDT_ENTRIES * 16 - 1;
	sregs.tr = (struct kvm_segment){
		.base     = kvm_bootstrap_va + KVM_BOOTSTRAP_TSS_OFFSET,
		.limit    = 104 - 1,
		.selector = KVM_BOOTSTRAP_TSS_SEL,
		.type     = 11,		/* 64-bit busy TSS */
		.present  = 1,
		.dpl      = 0,
		.s        = 0,		/* system segment */
		.g        = 0,
	};

	/*
	 * Seed FS/GS base from the task's stored values (memo 10
	 * class B, sub-commit #5c). UML's sys_arch_prctl stashes
	 * ARCH_SET_FS/GS into gp[HOST_FS_BASE] / gp[HOST_GS_BASE];
	 * the outer userspace() loop may have entered kvm_run_
	 * userspace after a reschedule where the previous
	 * SET_MSRS state was lost. Seed via sregs.fs.base /
	 * gs.base so the first KVM_RUN inherits the right TLS
	 * pointer without a separate KVM_SET_MSRS call. In long
	 * mode KVM keeps the segment cache base and
	 * MSR_{FS,GS}_BASE in sync; writing one writes the other.
	 */
	sregs.fs.base = regs->gp[HOST_FS_BASE];
	sregs.gs.base = regs->gp[HOST_GS_BASE];

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

	/*
	 * Memo 11 G3: MSR_KERNEL_GS_BASE -> gadget state VA.
	 * Programmed only when the gadget state page has been
	 * mapped (kvm_gadget_state_va() returns 0 otherwise).
	 * Gadget handlers `swapgs` at entry to swap in this
	 * base; user's MSR_GS_BASE stays whatever sub-commit
	 * #5c set for them.
	 */
	rc = kvm_enter_guest_program_kernel_gs_base(kvm_gadget_state_va());
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
	/*
	 * Mirror the real kvm_enter_guest path's FS/GS base seeding
	 * (memo 10 sub-commit #5c) so contract tests catch any drift
	 * between probe and production.
	 */
	sregs->fs.base = src->gp[HOST_FS_BASE];
	sregs->gs.base = src->gp[HOST_GS_BASE];
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
	 * Cache the number up front so handle_syscall clobbering
	 * HOST_AX (with the return value) doesn't hide it from the
	 * post-dispatch class-B propagation below.
	 */
	unsigned long syscall_nr = regs->gp[HOST_AX];

	PT_SYSCALL_NR(regs->gp) = syscall_nr;
	regs->is_user = 1;

	/*
	 * SYSCALL saves post-instruction RIP into RCX (and
	 * RFLAGS into R11) before jumping to MSR_LSTAR. For the
	 * break-out-and-re-enter pattern (audit A1), the next
	 * call to kvm_enter_guest's bootstrap SYSRETQ dance
	 * resumes ring-3 at whatever HOST_IP holds, so stash
	 * the user's continuation RIP there.
	 *
	 * Historical note: before A1 an inner `for (;;)` loop
	 * stayed in the same KVM_RUN and advanced kregs->rip
	 * past the LSTAR's 2-byte `out` so the trampoline's
	 * sysretq at +0x42 ran in ring-0 and returned to RCX in
	 * ring-3. That worked because the vCPU state carried
	 * through to the next KVM_RUN iteration. Under A1's
	 * per-trap re-entry model, kvm_enter_guest rebuilds all
	 * the vCPU state from `regs`, so HOST_IP must be the
	 * user RIP, not the LSTAR-internal address.
	 */
	regs->gp[HOST_IP] = regs->gp[HOST_CX];

	/*
	 * Memo 10 class-D short-circuit: syscalls that would be
	 * semantically wrong for a bare-userspace guest (ptrace,
	 * reboot, kexec_load / kexec_file_load, init/finit/
	 * delete_module, bpf) are trapped at the dispatcher
	 * before reaching handle_syscall. Returning -EPERM is the
	 * accurate "we saw the syscall, and we refuse it at this
	 * layer" answer — unlike -ENOSYS (which specifically
	 * means "no such syscall NR in this kernel"), -EPERM is
	 * the established convention for permission-style denials
	 * (see capable() / ns_capable() failure paths throughout
	 * the kernel). Future tightening can promote this to
	 * SIGSYS via signal delivery for audit visibility.
	 */
	if (kvm_classify_syscall(syscall_nr) == KVM_SYSCALL_CLASS_TRAP) {
		pr_info_ratelimited("um: kvm: trapping class-D syscall nr=%lu (memo 10)\n",
				    syscall_nr);
		regs->gp[HOST_AX] = -EPERM;
		goto skip_dispatch;
	}

	/*
	 * Common syscall dispatch path. Writes the return value
	 * into regs->gp[HOST_AX]; the caller marshals that back
	 * to vCPU state.
	 */
	pr_info_ratelimited("um: kvm: dispatching handle_syscall nr=%lu (via LSTAR trampoline)\n",
			    PT_SYSCALL_NR(regs->gp));
	handle_syscall(regs);

	/*
	 * mmap/brk/mremap/mprotect syscalls can extend the user
	 * mm with new mappings that are still lazy (present in
	 * vmas but not yet in the logical pgd). Before the next
	 * KVM_RUN, walk vmas + touch each page to force fault-in,
	 * then re-fill the shadow PT. Expensive but correct;
	 * sub-commit #5b's IDT #PF handler replaces this with
	 * targeted on-demand installation.
	 */
	{
		struct mm_struct *mm2 = current->active_mm;

		if (mm2 && mm2->pgd) {
			(void)kvm_touch_all_user_vmas(mm2);
			(void)kvm_shadow_fill_from_uml_pgd(mm2->pgd);
		}
	}

skip_dispatch:
	/*
	 * Class-B post-dispatch propagation (memo 10 sub-commit #5c):
	 * arch_prctl(ARCH_SET_FS/GS) updated gp[HOST_FS_BASE] /
	 * gp[HOST_GS_BASE] inside sys_arch_prctl — propagate those
	 * values into the vCPU's MSR_FS_BASE / MSR_GS_BASE so the
	 * next SYSRETQ-to-ring-3 sees the right TLS pointer. GET
	 * options don't modify gp[] so a SET_MSRS here is harmless
	 * (same values round-tripped). Non-arch_prctl syscalls skip
	 * this path entirely; FS/GS are preserved by KVM across
	 * VMEXITs so there's nothing to do for them.
	 */
	if (syscall_nr == __NR_arch_prctl) {
		int prc = kvm_propagate_fs_gs_base(vcpu_fd,
						   regs->gp[HOST_FS_BASE],
						   regs->gp[HOST_GS_BASE]);
		if (prc < 0) {
			/*
			 * Audit finding A4: UML's sys_arch_prctl
			 * already updated task_struct FS/GS state;
			 * if we can't push that into the vCPU, the
			 * guest's next fs:-relative load will fault
			 * at the wrong address + spin in the #PF
			 * handler. Kill the guest task rather than
			 * continue with a mismatched
			 * task_struct / vCPU view.
			 */
			pr_warn_ratelimited("um: kvm: arch_prctl FS/GS propagate failed (%d); killing guest task\n",
					    prc);
			fatal_sigsegv();
		}
	}

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
	 * Push the syscall return (now in regs->gp[HOST_AX])
	 * back into the vCPU state so the next KVM_RUN (via
	 * kvm_enter_guest's bootstrap SYSRETQ, which picks up
	 * RAX from kregs) delivers the right return value to
	 * ring-3. Failure here leaves handle_syscall's work
	 * stranded — guest resumes with stale RAX, likely
	 * triggering an infinite loop in the glibc error-
	 * check path. Audit A4: fatal rather than silent.
	 */
	kvm_uml_regs_to_kvm_regs(kregs, regs);
	if (os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
			     (unsigned long)kregs) < 0) {
		pr_warn_ratelimited("um: kvm: post-syscall KVM_SET_REGS failed; killing guest task\n");
		fatal_sigsegv();
	}
}

/*
 * Forward-declared + used by run_userspace's time-travel
 * bookkeeping below. Defined in arch/um/os-Linux/skas/process.c,
 * shared across all run_userspace impls and reset by
 * switch_threads on every context switch. Matches the pattern
 * already landed in seccomp/trap_user.c + ptrace/trap_user.c.
 */
extern unsigned int unscheduled_userspace_iterations;

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

	/*
	 * Time-travel bookkeeping (sub-commit #4 from memo 08).
	 * Under TT_MODE_INFCPU / TT_MODE_EXTERNAL, UML's virtual
	 * clock doesn't advance unless something forces it; a
	 * userspace task spinning in a tight trap loop would freeze
	 * simulated time. Bump tt_extra_sched_jiffies after
	 * CONFIG_UML_MAX_USERSPACE_ITERATIONS unyielded iterations
	 * so the scheduler gets a chance to tick. Matches the
	 * identical guard in seccomp_run_userspace /
	 * ptrace_run_userspace; the counter resets on
	 * switch_threads.
	 */
	if (time_travel_mode == TT_MODE_INFCPU ||
	    time_travel_mode == TT_MODE_EXTERNAL) {
#ifdef CONFIG_UML_MAX_USERSPACE_ITERATIONS
		if (CONFIG_UML_MAX_USERSPACE_ITERATIONS &&
		    unscheduled_userspace_iterations++ >
		    CONFIG_UML_MAX_USERSPACE_ITERATIONS) {
			tt_extra_sched_jiffies += 1;
			unscheduled_userspace_iterations = 0;
		}
#endif
	}

	/*
	 * mm pinning note (sub-commit #4): UML's outer userspace()
	 * loop runs in the context of the task owning
	 * current->active_mm; KVM_RUN executes synchronously in
	 * that same task's context so the mm can't be freed under
	 * us without the task itself being destroyed first. No
	 * explicit mmget/mmput needed here — matches the seccomp
	 * + ptrace backends which also don't pin. If SMP KVM ever
	 * adds cross-task vCPU dispatch, this invariant needs
	 * revisiting.
	 */

	rc = kvm_enter_guest(regs);
	if (rc < 0)
		panic("um: kvm run_userspace: enter_guest failed (%d)", rc);

	/*
	 * Per-trap shape (audit A1 finalized 2026-04-24 round-3):
	 * each kvm_run_userspace call drives ONE KVM_RUN, ONE
	 * dispatch, then interrupt_end() + return. Matches the
	 * contract ptrace / seccomp already honor (trap_user.c
	 * :254 + :157). An earlier revision kept a for (;;) inner
	 * loop that batched SYSCALL + PF "continuation" traps
	 * across multiple KVM_RUN iterations — correct for the
	 * sysretq/iretq completion but silently delayed
	 * interrupt_end() across syscall/fault bursts, up to the
	 * next host timer interrupt (~10ms). The fix for SYSCALL
	 * is kvm_decode_syscall stashing HOST_IP = HOST_CX (the
	 * user's post-SYSCALL RIP); the next kvm_enter_guest's
	 * bootstrap SYSRETQ dance resumes there. The fix for PF
	 * extracts user RIP + RSP from the IDT-pushed iretq
	 * frame on the IST stack and does the same re-entry
	 * (see the PF case below).
	 *
	 * No scheduler-drift concern: with per-trap exit,
	 * interrupt_end() runs once per trap, scheduling can
	 * happen inside it, and the next kvm_run_userspace
	 * invocation rebuilds the vCPU from scratch — so a
	 * task that scheduled away and comes back sees a fresh
	 * kvm_enter_guest before the next KVM_RUN.
	 *
	 * Cost: ~5 extra ioctls per non-gadget syscall vs the
	 * old inner-loop model (KVM_GET/SET_SREGS + KVM_SET_
	 * REGS + KVM_SET_MSRS in each kvm_enter_guest).
	 * Borne only by non-gadget paths; gadget-handled
	 * syscalls never VMEXIT so they're unaffected.
	 */
	{
		struct kvm_sregs exit_sregs;
		bool sregs_valid = false;

		rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
		if (rc < 0) {
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

		/*
		 * Audit finding A2 (memo D70): derive is_user from
		 * the observed CPL at VMEXIT instead of hardcoding
		 * it per-case. CPL = cs.selector & 3 (lowest 2
		 * bits of the ring-3 CS selector are the RPL, which
		 * equals CPL for any user-mode selector). seccomp /
		 * ptrace backends only trap from ring-3 so they
		 * always set is_user = 1, but the KVM backend can
		 * also VMEXIT mid-ring-0 (e.g. MMIO fault while
		 * executing the bootstrap #PF handler or an
		 * exception decoder). Misclassifying a ring-0 exit
		 * as is_user = 1 sends the fault through
		 * arch/um/kernel/trap.c's user-signal path at
		 * trap.c:321 instead of the panic path at
		 * trap.c:292 — guest kernel code that faults would
		 * silently signal a phantom user task instead of
		 * panicking the host kernel, making KVM crash
		 * diagnostics untrustworthy.
		 *
		 * Read SREGS best-effort: a failure here isn't
		 * fatal because the per-case defaults below are
		 * correct for their specific exit reasons (SYSCALL
		 * is always CPL=3, the #PF handler entry is always
		 * from CPL=3). The sregs_valid flag tells the MMIO
		 * case (the one that actually needs runtime CPL)
		 * whether to trust the fresh read or fall back.
		 */
		if (os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
				     (unsigned long)&exit_sregs) >= 0) {
			sregs_valid = true;
			regs->is_user = (exit_sregs.cs.selector & 3) != 0;
		}

		switch (run->exit_reason) {
		case KVM_EXIT_IO:
			if (run->io.port == UM_KVM_SYSCALL_PORT) {
				kvm_decode_syscall(regs, &kregs, vcpu_fd);
				/*
				 * Audit A1 (2026-04-24 round-3):
				 * exit to interrupt_end() per trap, not per
				 * trap-burst. kvm_decode_syscall has already
				 * stashed the user's continuation RIP into
				 * regs->gp[HOST_IP] (= HOST_CX, the post-
				 * SYSCALL user RIP). The next kvm_run_
				 * userspace iteration rebuilds the vCPU from
				 * regs via kvm_enter_guest's bootstrap
				 * SYSRETQ dance, resuming ring-3 at that
				 * RIP. Matches ptrace / seccomp's per-trap
				 * interrupt_end contract.
				 */
				goto out_read_regs;
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
			if (run->io.port == UM_KVM_PF_PORT) {
				/*
				 * Sub-commit #5b: guest-side #PF handler
				 * trapped in. Read CR2, touch the page to
				 * force UML fault-in, refresh shadow PT.
				 * If the touch fails (cr2 outside any vma,
				 * NULL-deref, etc.) dispatch through
				 * sig_info[SIGSEGV] — same SIGSEGV path the
				 * MMIO decode uses. That either fixes-up
				 * via on-demand vma expansion OR signals
				 * the guest task so the loop doesn't spin
				 * on a permanently-unresolvable fault.
				 */
				struct kvm_sregs dump_sregs;
				unsigned long cr2;
				char probe;
				bool touched = false;

				if (os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
						     (unsigned long)&dump_sregs) < 0) {
					panic("um: kvm PF handler: KVM_GET_SREGS failed");
				}
				cr2 = dump_sregs.cr2;
				pr_info_ratelimited("um: kvm #PF: cr2=0x%lx; fault-in + shadow refill\n",
						    cr2);

				if (cr2 && current->active_mm &&
				    current->active_mm->pgd) {
					struct mm_struct *m2 =
						current->active_mm;

					if (!copy_from_user(&probe,
							    (void __user *)cr2, 1)) {
						(void)copy_to_user((void __user *)cr2,
								   &probe, 1);
						touched = true;
					}
					(void)kvm_shadow_fill_from_uml_pgd(m2->pgd);
				}

				/*
				 * If the touch couldn't reach cr2 (NULL
				 * deref, unmapped-beyond-vma, protection
				 * violation), treat it as a real fault:
				 * populate faultinfo + dispatch SIGSEGV.
				 * Same code path KVM_EXIT_MMIO uses above.
				 * After SIGSEGV dispatch we return to the
				 * outer userspace() loop rather than re-
				 * entering KVM_RUN — the faulting insn
				 * would just fault again + spin us. UML's
				 * signal-delivery + scheduler machinery in
				 * the outer loop notices the queued SIGSEGV
				 * and either terminates the task or handles
				 * it; on the next run_userspace iteration
				 * regs reflect the post-signal state.
				 */
				if (!touched) {
					struct faultinfo *fi =
						UPT_FAULTINFO(regs);

					fi->trap_no    = 14;
					fi->error_code = 4; /* user-mode */
					fi->cr2        = cr2;
					(*sig_info[SIGSEGV])(SIGSEGV, NULL,
							     regs, NULL);
					regs->is_user = 1;
					/*
					 * Drop out to interrupt_end so the
					 * queued SIGSEGV drains before the
					 * outer userspace() loop re-enters.
					 */
					goto out_read_regs;
				}

				/*
				 * Audit A1 round-3: extract user
				 * continuation regs from the IDT-pushed
				 * iretq frame on the IST stack, stash
				 * them in `regs`, and exit to
				 * interrupt_end() per trap (matching
				 * the SYSCALL path above and ptrace /
				 * seccomp's contract). Prior revisions
				 * advanced kregs.rip+2 + KVM_SET_REGS
				 * and `continue`d the for-loop to let
				 * the handler's `iretq` run — that
				 * worked but silently batched PF
				 * events across the inner loop, which
				 * delayed resched + signal drain.
				 *
				 * IST frame layout at `out %al, $0xfb`
				 * VMEXIT (kregs.rsp points at offset
				 * 0):
				 *   +0:  error_code
				 *   +8:  RIP      (user's faulting VA)
				 *   +16: CS
				 *   +24: RFLAGS   (user's, at fault)
				 *   +32: RSP      (user's stack ptr)
				 *   +40: SS
				 *
				 * Read via the bootstrap page's kernel-
				 * VA alias — the IST stack lives in
				 * the bootstrap page (RX-mapped for the
				 * guest, kernel-VA-directly-accessible
				 * for the host). Next kvm_enter_guest's
				 * bootstrap SYSRETQ dance resumes ring-3
				 * at HOST_IP (= user RIP), with HOST_SP
				 * restored to user RSP. User RFLAGS is
				 * approximated by the hardcoded 0x3202
				 * (IF=1, IOPL=3, reserved bit-1); a
				 * faulting instruction almost never
				 * depends on entry RFLAGS being bit-
				 * exact, so the retry proceeds
				 * correctly. Tracked as a v2 refinement
				 * in memo 11 §"Known limitations".
				 */
				{
					unsigned long off =
						(unsigned long)(kregs.rsp -
								kvm_bootstrap_va);
					u8 *ist = (u8 *)kvm_bootstrap_page + off;
					u64 user_rip, user_rsp;

					if (off >= PAGE_SIZE) {
						pr_warn_ratelimited("um: kvm: PF IST out of range\n");
						fatal_sigsegv();
					}
					user_rip = *(u64 *)(ist + 8);
					user_rsp = *(u64 *)(ist + 32);
					regs->gp[HOST_IP] = user_rip;
					regs->gp[HOST_SP] = user_rsp;
				}
				goto out_read_regs;
			}
			panic("um: kvm run_userspace: KVM_EXIT_IO port=0x%x (unknown)",
			      run->io.port);

		case KVM_EXIT_HLT:
			/*
			 * Guest HLT. Jump to out_read_regs so
			 * interrupt_end() drains pending resched +
			 * signals before the next kvm_run_userspace call
			 * re-enters the guest. A plain `break;` would
			 * only exit the switch (not the for-loop) and
			 * silently loop back into KVM_RUN, skipping
			 * interrupt_end entirely — that was the A1
			 * partial-fix defect flagged in the 2026-04-24
			 * audit round. is_user was set from the observed
			 * CPL above (A2); leave it. HOST_IP points past
			 * the HLT.
			 */
			goto out_read_regs;

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
			bool is_user;

			/*
			 * Audit A2 follow-up: pick is_user for the
			 * fault error code from the live CPL read at
			 * the top of the loop when we have it
			 * (sregs_valid), not from a stale `regs->
			 * is_user` that a prior exit may have left
			 * behind. A KVM_GET_SREGS failure (sregs_
			 * valid = false, rare) falls back to the
			 * pre-fault regs->is_user; harmless for
			 * ring-3 userspace workloads because sregs
			 * rarely fails, but the explicit branch
			 * documents the intent and the fallback
			 * matches every other backend's "always
			 * user-mode" assumption on EPT faults.
			 */
			if (sregs_valid)
				is_user = (exit_sregs.cs.selector & 3) != 0;
			else
				is_user = regs->is_user;

			fi->trap_no    = 14;
			fi->error_code = (run->mmio.is_write ? 2 : 0) |
					 (is_user ? 4 : 0);
			fi->cr2        = (unsigned long)run->mmio.phys_addr +
					 uml_physmem;

			pr_info_ratelimited("um: kvm run_userspace: KVM_EXIT_MMIO gpa=0x%llx cr2=0x%lx len=%u write=%u\n",
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
			 * Re-fill the shadow PT: segv_handler may have
			 * installed a new mapping via mm_map, but the
			 * shadow PT still reflects the pre-fault state.
			 * Walking current->active_mm->pgd again picks up
			 * the new entry. Memo 09 step 3 follow-on:
			 * targeted single-page invalidate is an
			 * optimisation; for MVP the full refill works.
			 */
			if (current->active_mm && current->active_mm->pgd)
				(void)kvm_shadow_fill_from_uml_pgd(
					current->active_mm->pgd);
			/*
			 * MMIO is a clean ring-3 boundary: the SEGV
			 * either got handled (page installed, guest
			 * retries) or got signaled. Drop to
			 * interrupt_end so resched + signals drain
			 * before the outer loop re-enters. Same
			 * for-loop-vs-switch trap as the HLT case;
			 * goto, not break.
			 */
			goto out_read_regs;
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
			/*
			 * Dump 16 bytes of guest instruction bytes at the
			 * failing RIP. RIP is a user VA; copy_from_user
			 * reads it. Useful for deciding whether the fault
			 * was at a syscall instruction, a mov-from-memory,
			 * or a computed jump into nothing.
			 */
			{
				unsigned char insn_bytes[16] = { 0 };
				long cr_rc;

				cr_rc = copy_from_user(insn_bytes,
						       (void __user *)(unsigned long)guest_rip,
						       sizeof(insn_bytes));
				if (cr_rc == 0) {
					pr_err("um: kvm: guest insn @ RIP=0x%llx: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
					       (unsigned long long)guest_rip,
					       insn_bytes[0], insn_bytes[1],
					       insn_bytes[2], insn_bytes[3],
					       insn_bytes[4], insn_bytes[5],
					       insn_bytes[6], insn_bytes[7],
					       insn_bytes[8], insn_bytes[9],
					       insn_bytes[10], insn_bytes[11],
					       insn_bytes[12], insn_bytes[13],
					       insn_bytes[14], insn_bytes[15]);
				} else {
					pr_err("um: kvm: guest insn @ RIP=0x%llx unreadable (copy_from_user=%ld)\n",
					       (unsigned long long)guest_rip, cr_rc);
				}
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
	 * Host-side INTR / EINTR path: kregs may be stale (KVM_RUN
	 * returned before a clean KVM_GET_REGS ran). Pull fresh
	 * guest state so interrupt_end() + the next kvm_enter_guest
	 * operate on accurate regs.
	 */
	if (rc == -EINTR) {
		struct kvm_sregs sregs;

		rc = os_ioctl_generic(vcpu_fd, KVM_GET_REGS,
				      (unsigned long)&kregs);
		if (rc >= 0)
			kvm_regs_to_uml_regs(regs, &kregs);
		/*
		 * Audit A2: derive is_user from CPL here too, so
		 * a host-signal interrupt mid-ring-0 (e.g. inside
		 * the LSTAR trampoline) doesn't mask as a user-
		 * mode exit. Best-effort — if KVM_GET_SREGS
		 * fails, fall back to user-mode since host
		 * signals during normal workload almost always
		 * fire while the guest is in ring-3.
		 */
		if (os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
				     (unsigned long)&sregs) >= 0)
			regs->is_user = (sregs.cs.selector & 3) != 0;
		else
			regs->is_user = 1;
	}

	/*
	 * Drain resched + pending signals + resume work, matching
	 * the contract seccomp/ptrace backends honor (audit A1 /
	 * decisions-log D70). Each kvm_run_userspace call = one
	 * trap + one interrupt_end, same shape as
	 * seccomp_run_userspace line 157 +
	 * ptrace_run_userspace line 254.
	 */
	interrupt_end();
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
