// SPDX-License-Identifier: GPL-2.0
/*
 * UML KVM backend vCPU pool and KVM_RUN dispatcher.
 *
 * The backend creates one vCPU per host CPU used by UML. Each pool member
 * owns its vCPU fd, kvm_run mmap, signal mask, descriptor state, and
 * per-vCPU trampoline pages. Tasks are marshalled into the selected pool
 * member immediately before KVM_RUN and marshalled back to per-task UML
 * register state after every exit.
 *
 * CPUID is installed lazily on first run. KVM_SET_CPUID2 is a vCPU ioctl,
 * and the supported-CPUID buffer requires the buddy allocator, so early VM
 * creation cannot finish the install reliably. The curated CPUID mask is
 * required before guest entry: the guest must not see features whose
 * architectural state is not saved, restored, or made deterministic by
 * this backend.
 */

#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/signal.h>	/* sigset_t, sigfillset, sigdelset, SIGALRM */
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/threads.h>
#include <linux/types.h>

#include <asm/msr-index.h>
#include <asm/page.h>
#include <asm/processor-flags.h>	/* X86_CR0_*, X86_CR4_* */

#include <asm/tlbflush.h>	/* um_tlb_sync */
#include <kern_util.h>		/* interrupt_end */
#include <os.h>
#include <skas.h>		/* um_tlb_sync */
#include <sysdep/ptrace.h>
#include <asm/trace/um_backend.h>

#include "kvm_v2_backend.h"
#include "state_trace.h"
#include "syscall_trap.h"

#define KVM_V2_USER_SEG_LIMIT		0xffffffff
#define KVM_V2_SYSCALL_FMASK		(X86_EFLAGS_TF | X86_EFLAGS_IF | \
					 X86_EFLAGS_DF | X86_EFLAGS_IOPL | \
					 X86_EFLAGS_NT | X86_EFLAGS_AC)
#define KVM_V2_SYSCALL_MSR_COUNT	4

/*
 * Layout-compatible with struct kvm_msrs plus fixed storage for the syscall
 * MSR set programmed at vCPU creation.
 */
struct kvm_v2_msr_batch {
	__u32 nmsrs;
	__u32 pad;
	struct kvm_msr_entry entries[KVM_V2_SYSCALL_MSR_COUNT];
};

/*
 * The pool. Sized at compile time to KVM_V2_MAX_VCPUS, which follows
 * UML's configured CPU capacity (64 at most, 1 by default without SMP),
 * and BSS-resident to avoid allocating before the buddy allocator is up.
 *
 * Only the first nr_cpu_ids slots are populated; trailing capacity
 * slots stay at the .vcpu_fd = -1 sentinel below so kvm_v2_vcpu_get()
 * returns NULL for out-of-range queries.
 *
 * The pool_initialised flag is the population predicate. The code cannot
 * use vcpus[0].vcpu_fd == -1 because the BSS-zeroed default puts the array
 * at vcpu_fd = 0 (a valid fd), and rewriting the array every call would be
 * wrong on re-entry. The flag flips once when kvm_v2_vcpu_create runs the
 * first-time reset, and stays true even after destroy (destroy leaves
 * the slots at vcpu_fd = -1, the proper sentinel).
 */
static struct kvm_v2_vcpu vcpus[KVM_V2_MAX_VCPUS];
static bool pool_initialised;

static void kvm_v2_vcpu_pool_reset(void)
{
	int i;

	for (i = 0; i < KVM_V2_MAX_VCPUS; i++) {
		vcpus[i].vcpu_fd      = -1;
		vcpus[i].kvm_run      = NULL;
		vcpus[i].kvm_run_size = 0;
		vcpus[i].cpu          = -1;
		vcpus[i].signal_mask_installed = false;
		vcpus[i].signal_mask_blocks_timer = false;
		memset(&vcpus[i].run_regs, 0, sizeof(vcpus[i].run_regs));
		memset(&vcpus[i].run_sregs, 0, sizeof(vcpus[i].run_sregs));
		vcpus[i].run_exit_reason = 0;
	}
	pool_initialised = true;
}

static struct kvm_segment kvm_v2_user_code_segment(void)
{
	return (struct kvm_segment) {
		.base    = 0,
		.limit   = KVM_V2_USER_SEG_LIMIT,
		.selector = KVM_V2_USER_CS_SEL,
		.type    = 0xb,		/* ER + A */
		.present = 1,
		.dpl     = 3,
		.db      = 0,		/* L=1 supersedes db */
		.s       = 1,		/* code/data */
		.l       = 1,		/* 64-bit */
		.g       = 1,
	};
}

static struct kvm_segment kvm_v2_user_data_segment(void)
{
	return (struct kvm_segment) {
		.base    = 0,
		.limit   = KVM_V2_USER_SEG_LIMIT,
		.selector = KVM_V2_USER_DS_SEL,
		.type    = 0x3,		/* RW + A */
		.present = 1,
		.dpl     = 3,
		.db      = 1,
		.s       = 1,
		.l       = 0,
		.g       = 1,
	};
}

static void kvm_v2_apply_user_segments(struct kvm_sregs *sregs)
{
	const struct kvm_segment code = kvm_v2_user_code_segment();
	const struct kvm_segment data = kvm_v2_user_data_segment();

	sregs->cs = code;
	sregs->ds = data;
	sregs->es = data;
	sregs->fs = data;
	sregs->gs = data;
	sregs->ss = data;
}

static void kvm_v2_apply_descriptor_sregs(struct kvm_sregs *sregs,
					  const struct kvm_v2_vcpu *vcpu)
{
	sregs->idt.base  = (u64)KVM_V2_IDT_GVA;
	sregs->idt.limit = KVM_V2_IDT_LIMIT;
	sregs->gdt.base  = (u64)KVM_V2_GDT_GVA;
	sregs->gdt.limit = KVM_V2_GDT_LIMIT;

	sregs->tr.base     = (u64)vcpu->tss_gva;
	sregs->tr.limit    = KVM_V2_TSS_LIMIT;
	sregs->tr.selector = KVM_V2_TSS_SEL;
	sregs->tr.type     = 0xb;	/* 64-bit busy TSS */
	sregs->tr.s        = 0;		/* system segment */
	sregs->tr.dpl      = 0;
	sregs->tr.present  = 1;
	sregs->tr.l        = 0;		/* ignored for system segments */
	sregs->tr.db       = 0;		/* ignored for system segments */
	sregs->tr.g        = 0;		/* byte granularity */
	sregs->tr.avl      = 0;
	sregs->tr.unusable = 0;
}

/*
 * KVM_MAX_CPUID_ENTRIES is 256 upstream. Buffer size is sizeof(struct
 * kvm_cpuid2) plus one struct kvm_cpuid_entry2 per entry.
 */
#define KVM_V2_CPUID_MAX_ENTRIES	256

/*
 * Curate the host-supported CPUID surface before exposing it to the
 * guest. Keep nondeterministic RNG instructions hidden,
 * hide features whose state is not fully saved/restored, and only expose
 * XSAVE-family features after the matching CR4 and XCR0 state is installed.
 */
static void kvm_v2_curate_cpuid_leaf1(struct kvm_cpuid_entry2 *e)
{
	/*
	 * Leaf 1 ECX:
	 *   bit 12 = FMA, 26 = XSAVE, 27 = OSXSAVE,
	 *   28 = AVX, 29 = F16C, 30 = RDRAND.
	 *
	 * FMA/XSAVE/OSXSAVE/AVX/F16C are available because CR4.OSXSAVE is set
	 * and XCR0 enables FP, SSE, and YMM. Keep RDRAND masked because the
	 * backend does not virtualize the instruction's entropy source.
	 */
	e->ecx &= ~(1U << 30);
}

static void kvm_v2_curate_cpuid_leaf7(struct kvm_cpuid_entry2 *e)
{
	/*
	 * Leaf 7.0 EBX:
	 *   bit 0  = FSGSBASE, 5  = AVX2,
	 *   bit 16 = AVX512F,  17 = AVX512DQ,
	 *   bit 18 = RDSEED,   21 = AVX512IFMA,
	 *   bit 26 = AVX512PF, 27 = AVX512ER,
	 *   bit 28 = AVX512CD, 29 = SHA,
	 *   bit 30 = AVX512BW, 31 = AVX512VL.
	 *
	 * AVX2 is available with the same CR4/XCR0 contract as AVX. Keep
	 * FSGSBASE masked while CR4.FSGSBASE is off, keep RDSEED masked for
	 * deterministic execution, and keep AVX-512 masked until the wider XCR0
	 * state is enabled.
	 */
	e->ebx &= ~((1U << 0)  | (1U << 16) | (1U << 17) |
		    (1U << 18) | (1U << 21) | (1U << 26) |
		    (1U << 27) | (1U << 28) | (1U << 29) |
		    (1U << 30) | (1U << 31));

	/*
	 * Leaf 7.0 ECX:
	 *   bit 1  = AVX512VBMI, 6  = AVX512VBMI2,
	 *   bit 8  = GFNI,       9  = VAES,
	 *   bit 10 = VPCLMULQDQ, 11 = AVX512VNNI,
	 *   bit 12 = AVX512BITALG, 14 = AVX512VPOPCNTDQ.
	 *
	 * These features remain masked because the backend does not expose their
	 * full architectural state.
	 */
	e->ecx &= ~((1U << 1)  | (1U << 6)  | (1U << 8) |
		    (1U << 9)  | (1U << 10) | (1U << 11) |
		    (1U << 12) | (1U << 14));

	/*
	 * Leaf 7.0 EDX:
	 *   bit 2 = AVX512_4VNNIW, 3 = AVX512_4FMAPS,
	 *   bit 8 = AVX512_VP2INTERSECT.
	 */
	e->edx &= ~((1U << 2) | (1U << 3) | (1U << 8));
}

static bool kvm_v2_cpuid_hide_xsave_subleaf(u32 index)
{
	return index == 5 || index == 6 || index == 7 ||
	       index == 17 || index == 18;
}

static void kvm_v2_curate_cpuid_xsave(struct kvm_cpuid_entry2 *e)
{
	/*
	 * Leaf 0xD is the XSAVE state-component descriptor.
	 *
	 * KVM_SET_XCRS validates the requested XCR0 against
	 * vcpu->arch.guest_supported_xcr0, which is computed from leaf 0xD
	 * sub-leaf 0 EAX (XCR0-supported mask) AND the OSXSAVE/AVX bits in leaf
	 * 1 ECX. Zeroing leaf 0xD makes that supported mask 0, so SET_XCRS
	 * rejects bits 0/1/2 with -EINVAL.
	 *
	 * Keep sub-leaf 0 EAX intact for SET_XCRS to accept FP|SSE|YMM, but
	 * zero the per-component descriptors for masked AVX-512 and AMX features.
	 * Sub-leaves 1 and 2 are kept because the backend uses XSAVES and AVX YMM
	 * state.
	 */
	if (kvm_v2_cpuid_hide_xsave_subleaf(e->index)) {
		e->eax = 0;
		e->ebx = 0;
		e->ecx = 0;
		e->edx = 0;
	}
}

static void kvm_v2_curate_cpuid(struct kvm_cpuid2 *cpuid)
{
	unsigned int i;

	for (i = 0; i < cpuid->nent; i++) {
		struct kvm_cpuid_entry2 *e = &cpuid->entries[i];

		if (e->function == 1 && e->index == 0)
			kvm_v2_curate_cpuid_leaf1(e);
		else if (e->function == 7 && e->index == 0)
			kvm_v2_curate_cpuid_leaf7(e);
		else if (e->function == 0xD)
			kvm_v2_curate_cpuid_xsave(e);
	}
}

static struct kvm_cpuid2 *kvm_v2_get_supported_cpuid(struct kvm_v2_vm *vm)
{
	struct kvm_cpuid2 *cpuid;
	size_t buf_sz;
	int rc;

	buf_sz = sizeof(*cpuid) +
		 KVM_V2_CPUID_MAX_ENTRIES * sizeof(struct kvm_cpuid_entry2);
	cpuid = kzalloc(buf_sz, GFP_KERNEL);
	if (!cpuid) {
		pr_err("um: kvm-v2 cpuid_install: kzalloc(%zu) failed\n",
		       buf_sz);
		return ERR_PTR(-ENOMEM);
	}

	cpuid->nent = KVM_V2_CPUID_MAX_ENTRIES;
	rc = os_ioctl_generic(vm->kvm_fd, KVM_GET_SUPPORTED_CPUID,
			      (unsigned long)cpuid);
	if (rc < 0) {
		pr_err("um: kvm-v2 cpuid_install: KVM_GET_SUPPORTED_CPUID failed (%d)\n",
		       rc);
		kfree(cpuid);
		return ERR_PTR(rc);
	}

	return cpuid;
}

static int kvm_v2_set_curated_cpuid(int vcpu_fd, struct kvm_cpuid2 *cpuid)
{
	int rc;

	kvm_v2_curate_cpuid(cpuid);

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_CPUID2, (unsigned long)cpuid);
	if (rc < 0)
		pr_err("um: kvm-v2 cpuid_install: KVM_SET_CPUID2 failed (%d)\n",
		       rc);

	return rc;
}

static void kvm_v2_save_cpuid(struct kvm_v2_vm *vm, struct kvm_cpuid2 *cpuid)
{
	struct kvm_cpuid2 *prev_cpuid;

	/*
	 * Keep the most recent curated mask for VM teardown and inspection.
	 * Later vCPUs install an identical mask; replace the saved pointer
	 * rather than retaining one buffer per pool member.
	 */
	spin_lock(&vm->lock);
	prev_cpuid = vm->cpuid;
	vm->cpuid = cpuid;
	spin_unlock(&vm->lock);
	kfree(prev_cpuid);
}

/*
 * Lazy CPUID install before guest entry. By the first KVM_RUN the buddy
 * allocator is available, so GET_SUPPORTED_CPUID can allocate its buffer and
 * the curated mask can be installed before guest code executes. Failure is
 * required before guest entry because running with KVM-default CPUID would
 * expose unsupported or nondeterministic architectural state.
 *
 * The vm->cpuid stash survives the install. context.c owns the final
 * kfree, and introspection code can inspect the installed mask without
 * repeating GET_SUPPORTED_CPUID.
 *
 * Returns 0 on success or -errno on any failure (kzalloc, ioctl).
 */
static int kvm_v2_install_cpuid(struct kvm_v2_vm *vm, int vcpu_fd)
{
	struct kvm_cpuid2 *cpuid;
	u32 nent;
	int rc;

	cpuid = kvm_v2_get_supported_cpuid(vm);
	if (IS_ERR(cpuid))
		return PTR_ERR(cpuid);

	rc = kvm_v2_set_curated_cpuid(vcpu_fd, cpuid);
	if (rc < 0) {
		kfree(cpuid);
		return rc;
	}

	nent = cpuid->nent;
	kvm_v2_save_cpuid(vm, cpuid);

	pr_debug("um: kvm-v2 cpuid_install: installed %u curated entries\n",
		 nent);
	trace_um_backend_kvm_v2_cpuid_install(vcpu_fd, nent);
	return 0;
}

/*
 * Program MSR_LSTAR, MSR_STAR, MSR_SYSCALL_MASK, and MSR_KERNEL_GS_BASE
 * once per pool member at vcpu_create_one. vCPUs are reused across tasks
 * under the single-VM model, so the SYSCALL MSRs are immutable across the
 * pool's lifetime; set once at create, never re-set per-dispatch.
 *
 * MSR_LSTAR (0xc0000082): SYSCALL entry RIP. Programmed to
 * KVM_V2_LSTAR_GVA (= 0xffffe00000000040 = trampoline GVA + 0x40),
 * which the trampoline page exposes as the 5-byte
 * out %al,$0xf4 ; sysretq body. The GVA is guest-walk-reachable via
 * the kernel-half mapping installed in swapper_pg_dir and inherited by
 * task page tables.
 *
 * MSR_STAR (0xc0000081): high 16 bits = SYSCALL CS|SS base (kernel
 * selectors); bits 63:48 = SYSRETQ user CS|SS base. Standard pair:
 * kernel CS=0x08, user CS=0x33 (= 0x18+16|3, with the +16 and |3
 * applied by the SYSRETQ microcode).
 *
 * MSR_SYSCALL_MASK / MSR_FMASK (0xc0000084): RFLAGS bits cleared on
 * SYSCALL entry. 0x47700 = TF | IF | DF | IOPL | NT | AC. Matches Linux
 * native syscall_init's mask. FMASK must clear DF so user direction-flag
 * state cannot leak into kernel string operations.
 *
 * MSR_KERNEL_GS_BASE (0xc0000102): programmed per-vCPU to
 * KVM_V2_GADGET_STATE_GVA(vcpu->cpu), the GVA each vCPU's gadget state
 * page is mapped at via PTE[KVM_V2_GADGET_BASE_SLOT + cpu]. The page itself
 * is allocated and mapped by kvm_v2_install_per_vcpu_gadget_state();
 * vcpu_create_one programs the deterministic GVA so the MSR is ready when
 * the page exists.
 *
 * Per-vCPU isolation keeps each gadget reading its own state page, and
 * load_user_sregs writes only to the running vCPU's page.
 *
 * After KVM_SET_MSRS, immediately KVM_GET_MSRS and verify each value
 * round-tripped exactly. A silent KVM_SET_MSRS failure would send SYSCALL
 * to the wrong RIP, use the wrong SYSRET selectors, or leak bad RFLAGS.
 * One readback before guest entry catches partial writes or unexpected KVM
 * ABI behavior before guest code executes.
 */
static void kvm_v2_init_syscall_msrs(struct kvm_v2_msr_batch *msrs, int cpu)
{
	*msrs = (struct kvm_v2_msr_batch) {
		.nmsrs = KVM_V2_SYSCALL_MSR_COUNT,
		.entries = {
			{ .index = MSR_LSTAR, .data = KVM_V2_LSTAR_GVA },
			{ .index = MSR_STAR,
			  .data  = ((u64)KVM_V2_SYSRET_BASE_SEL << 48) |
				   ((u64)KVM_V2_KERNEL_CS_SEL << 32) },
			{ .index = MSR_SYSCALL_MASK,
			  .data = KVM_V2_SYSCALL_FMASK },
			{ .index = MSR_KERNEL_GS_BASE,
			  .data  = KVM_V2_GADGET_STATE_GVA(cpu) },
		},
	};
}

static void kvm_v2_init_syscall_msr_readback(struct kvm_v2_msr_batch *msrs)
{
	*msrs = (struct kvm_v2_msr_batch) {
		.nmsrs = KVM_V2_SYSCALL_MSR_COUNT,
		.entries = {
			{ .index = MSR_LSTAR },
			{ .index = MSR_STAR },
			{ .index = MSR_SYSCALL_MASK },
			{ .index = MSR_KERNEL_GS_BASE },
		},
	};
}

static int kvm_v2_write_syscall_msrs(int vcpu_fd,
				     struct kvm_v2_msr_batch *req)
{
	int rc;

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS, (unsigned long)req);
	if (rc < 0) {
		pr_err("um: kvm-v2 program_msrs: KVM_SET_MSRS(vcpu_fd=%d) failed (%d)\n",
		       vcpu_fd, rc);
		return rc;
	}
	if (rc != 4) {
		/*
		 * KVM_SET_MSRS returns the count of MSRs successfully
		 * written; partial success means one of LSTAR/STAR/FMASK/
		 * KERNEL_GS_BASE was rejected, leaving the corresponding
		 * trampoline / SYSRETQ / RFLAGS-mask / gadget GS_BASE
		 * setup incomplete. Return failure to the caller.
		 */
		pr_err("um: kvm-v2 program_msrs: KVM_SET_MSRS wrote %d/4 MSRs (vcpu_fd=%d)\n",
		       rc, vcpu_fd);
		return -EIO;
	}
	return 0;
}

static int kvm_v2_read_syscall_msrs(int vcpu_fd,
				    struct kvm_v2_msr_batch *readback)
{
	int rc;

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_MSRS, (unsigned long)readback);
	if (rc < 0 || rc != 4) {
		pr_err("um: kvm-v2 program_msrs: KVM_GET_MSRS readback failed (%d) (vcpu_fd=%d)\n",
		       rc, vcpu_fd);
		return rc < 0 ? rc : -EIO;
	}
	return 0;
}

static void kvm_v2_verify_syscall_msrs(const struct kvm_v2_msr_batch *req,
				       const struct kvm_v2_msr_batch *readback)
{
	int i;

	for (i = 0; i < KVM_V2_SYSCALL_MSR_COUNT; i++) {
		if (readback->entries[i].data != req->entries[i].data) {
			panic("kvm-v2: MSR readback MISMATCH idx=%#x: wrote %#llx got %#llx",
			      req->entries[i].index,
			      (unsigned long long)req->entries[i].data,
			      (unsigned long long)readback->entries[i].data);
		}
	}
}

static void kvm_v2_log_syscall_msrs(int vcpu_fd, int cpu,
				    const struct kvm_v2_msr_batch *req)
{
	pr_debug("um: kvm-v2 program_msrs: vcpu_fd=%d cpu=%d LSTAR=%#llx STAR=%#llx FMASK=%#llx KERNEL_GS_BASE=%#llx\n",
		 vcpu_fd, cpu,
		 (unsigned long long)req->entries[0].data,
		 (unsigned long long)req->entries[1].data,
		 (unsigned long long)req->entries[2].data,
		 (unsigned long long)req->entries[3].data);
	trace_um_backend_kvm_v2_msr_program(vcpu_fd);
}

static int kvm_v2_vcpu_program_msrs(int vcpu_fd, int cpu)
{
	struct kvm_v2_msr_batch req;
	struct kvm_v2_msr_batch readback;
	int rc;

	kvm_v2_init_syscall_msrs(&req, cpu);
	rc = kvm_v2_write_syscall_msrs(vcpu_fd, &req);
	if (rc)
		return rc;

	kvm_v2_init_syscall_msr_readback(&readback);
	rc = kvm_v2_read_syscall_msrs(vcpu_fd, &readback);
	if (rc)
		return rc;

	kvm_v2_verify_syscall_msrs(&req, &readback);
	kvm_v2_log_syscall_msrs(vcpu_fd, cpu, &req);
	return 0;
}

static void kvm_v2_apply_baseline_sregs(struct kvm_sregs *sregs)
{
	/*
	 * Start directly at CPL=3. Select user CS (sel 0x2b, DPL=3) and user
	 * SS (sel 0x23, DPL=3) so the guest enters at CPL=3 from the first
	 * KVM_RUN. SYSCALL transitions to CPL=0 with CS=STAR[47:32]=0x08 for
	 * the trampoline; sysretq transitions back to CPL=3 with
	 * CS=STAR[63:48]+0x10|3=0x2b. IDT-gate transitions to CPL=0 with
	 * CS=gate.selector=0x08 for handler stubs; iretq pops back to CPL=3.
	 */
	kvm_v2_apply_user_segments(sregs);

	sregs->cr0  = X86_CR0_PE | X86_CR0_MP | X86_CR0_NE |
		      X86_CR0_WP | X86_CR0_PG;
	/*
	 * OSXSAVE is not set here. KVM rejects CR4.OSXSAVE before CPUID is
	 * installed because cr4_guest_rsvd_bits treats it as reserved. The
	 * first-dispatch lazy CPUID install in vcpu_run adds OSXSAVE and
	 * pairs it with kvm_v2_install_xcrs().
	 */
	sregs->cr4  = X86_CR4_PAE | X86_CR4_OSFXSR | X86_CR4_OSXMMEXCPT;
	sregs->efer = EFER_SCE | EFER_LME | EFER_LMA | EFER_NX;
	/*
	 * CR3 is set per dispatch by load_user_sregs(). Leave the reset value
	 * from KVM_GET_SREGS here; by first dispatch CR0.PG is already valid.
	 */
}

static void kvm_v2_seed_sregs_mmap(struct kvm_v2_vcpu *v,
				   const struct kvm_sregs *sregs)
{
	struct kvm_run *run = v->kvm_run;

	/*
	 * Seed the sync-regs mmap so the first dispatch's KVM_SYNC_X86_SREGS
	 * write does not ship zero CR0/CR4/segments back to __set_sregs. KVM
	 * normally populates this area on exit; hand-seed it before first entry
	 * to bridge that gap.
	 */
	run->s.regs.sregs = *sregs;
}

static void kvm_v2_log_baseline_sregs(struct kvm_v2_vcpu *v,
				      const struct kvm_sregs *sregs)
{
	pr_debug("um: kvm-v2 install_sregs: vcpu_fd=%d cr0=%#llx cr4=%#llx efer=%#llx cs.l=%u\n",
		 v->vcpu_fd,
		 (unsigned long long)sregs->cr0,
		 (unsigned long long)sregs->cr4,
		 (unsigned long long)sregs->efer,
		 sregs->cs.l);
	trace_um_backend_kvm_v2_sregs_install(v->vcpu_fd);
}

static void kvm_v2_log_descriptor_sregs(struct kvm_v2_vcpu *vcpu,
					const struct kvm_sregs *sregs)
{
	pr_debug("um: kvm-v2 install_descriptors_sregs: vcpu_fd=%d idt=%#llx gdt=%#llx tr=%#llx\n",
		 vcpu->vcpu_fd,
		 (unsigned long long)sregs->idt.base,
		 (unsigned long long)sregs->gdt.base,
		 (unsigned long long)sregs->tr.base);
	trace_um_backend_kvm_v2_descriptors_sregs_install(vcpu->vcpu_fd,
							  sregs->idt.base,
								  sregs->gdt.base);
}

static int kvm_v2_validate_descriptor_sregs(struct kvm_v2_vm *vm,
					    struct kvm_v2_vcpu *vcpu)
{
	if (!vm || !vcpu || vcpu->vcpu_fd < 0 || !vcpu->kvm_run)
		return -EINVAL;

	if (!vm->idt_kva || !vm->gdt_kva) {
		/*
		 * Callers must populate descriptor pages before installing
		 * SREGS. Reject out-of-order calls instead of installing zero
		 * descriptor-table bases.
		 */
		pr_err("um: kvm-v2 install_descriptors_sregs: descriptor pages missing\n");
		return -EINVAL;
	}

	if (!vcpu->tss_gva || !vcpu->ist_stack_top_gva) {
		/*
		 * Per-vCPU IST/TSS install must precede this helper. Surface
		 * loudly; the alternative is silently installing sregs.tr.base =
		 * 0, which would cause exception delivery to dereference NULL
		 * during the iretq frame push and triple-fault.
		 */
		pr_err("um: kvm-v2 install_descriptors_sregs: vcpu_fd=%d missing IST/TSS state\n",
		       vcpu->vcpu_fd);
		return -EINVAL;
	}

	return 0;
}

static int kvm_v2_commit_descriptor_sregs(struct kvm_v2_vcpu *vcpu,
					  struct kvm_sregs *sregs)
{
	int rc;

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_SREGS,
			      (unsigned long)sregs);
	if (rc < 0)
		pr_err("um: kvm-v2 install_descriptors_sregs: KVM_SET_SREGS(vcpu_fd=%d) failed (%d)\n",
		       vcpu->vcpu_fd, rc);

	return rc;
}

/*
 * Install the baseline long-mode SREGS once per pool member.
 *
 * KVM's sync-regs mmap is zero-initialised before the first KVM_RUN and
 * is normally populated only after an exit. GET_SREGS first to preserve
 * KVM reset defaults for fields this backend does not own, overlay the
 * long-mode segment and control-register state, then seed the sync-regs
 * mmap with the same structure so the first KVM_SYNC_X86_SREGS write
 * cannot ship zero segments or CR0 back into KVM.
 */
static int kvm_v2_install_baseline_sregs(struct kvm_v2_vcpu *v)
{
	struct kvm_sregs sregs;
	int rc;

	if (!v || v->vcpu_fd < 0 || !v->kvm_run)
		return -EINVAL;

	rc = os_ioctl_generic(v->vcpu_fd, KVM_GET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_err("um: kvm-v2 install_sregs: KVM_GET_SREGS(vcpu_fd=%d) failed (%d)\n",
		       v->vcpu_fd, rc);
		return rc;
	}

	kvm_v2_apply_baseline_sregs(&sregs);

	rc = os_ioctl_generic(v->vcpu_fd, KVM_SET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_err("um: kvm-v2 install_sregs: KVM_SET_SREGS(vcpu_fd=%d) failed (%d)\n",
		       v->vcpu_fd, rc);
		return rc;
	}

	kvm_v2_seed_sregs_mmap(v, &sregs);
	kvm_v2_log_baseline_sregs(v, &sregs);
	return 0;
}

/*
 * Install IDT, GDT, and TR bases in SREGS for one pool member.
 *
 * Descriptor pages are allocated after the baseline long-mode state, so this
 * helper commits them with KVM_GET_SREGS/KVM_SET_SREGS and then re-seeds the
 * sync-regs mmap. KVM_SET_SREGS updates the vCPU state immediately; sync-regs
 * alone would not refresh the mmap before the next KVM_RUN exit.
 *
 * The per-vCPU TR cache points at that vCPU's TSS body. The TSS carries IST1,
 * which exception delivery uses for the iretq frame push. Callers must install
 * per-vCPU IST/TSS state before this helper.
 */
int kvm_v2_install_descriptors_sregs(struct kvm_v2_vm *vm,
				     struct kvm_v2_vcpu *vcpu)
{
	struct kvm_sregs sregs;
	int rc;

	rc = kvm_v2_validate_descriptor_sregs(vm, vcpu);
	if (rc)
		return rc;

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_SREGS,
			      (unsigned long)&sregs);
	if (rc < 0) {
		pr_err("um: kvm-v2 install_descriptors_sregs: KVM_GET_SREGS(vcpu_fd=%d) failed (%d)\n",
		       vcpu->vcpu_fd, rc);
		return rc;
	}

	kvm_v2_apply_descriptor_sregs(&sregs, vcpu);

	rc = kvm_v2_commit_descriptor_sregs(vcpu, &sregs);
	if (rc < 0)
		return rc;

	kvm_v2_seed_sregs_mmap(vcpu, &sregs);
	kvm_v2_log_descriptor_sregs(vcpu, &sregs);
	return 0;
}

/*
 * Install XCR0 to enable x87, SSE, and YMM state-save. Pairs with
 * CR4.OSXSAVE and the AVX/XSAVE/OSXSAVE CPUID bits.
 *
 * Without XCR0 set, VEX-encoded AVX instructions raise #UD even
 * when CR4.OSXSAVE=1 and CPUID advertises AVX; XCR0 is the
 * architectural enable for the extended state-save area.
 *
 * Bits set:
 *   bit 0 (X87)  : always required
 *   bit 1 (SSE)  : XMM0..XMM15 (already implicit via OSFXSR but
 *                  the XCR0 ABI requires bit 1 set whenever bit 2
 *                  is set)
 *   bit 2 (YMM)  : upper 128 bits of YMM0..YMM15 (AVX/AVX2)
 *
 * Higher tiers (AVX-512 = bits 5/6/7, AMX = 17/18) stay clear
 * until the corresponding CPUID bits and state marshaling are ready.
 */
static int kvm_v2_install_xcrs(int vcpu_fd)
{
	struct kvm_xcrs xcrs = {
		.nr_xcrs = 1,
		.xcrs[0] = { .xcr = 0, .value = 0x7 },
	};
	int rc;

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_XCRS, (unsigned long)&xcrs);
	if (rc < 0) {
		pr_err("um: kvm-v2 install_xcrs: KVM_SET_XCRS(vcpu_fd=%d) failed (%d)\n",
		       vcpu_fd, rc);
		return rc;
	}
	/*
	 * Read back XCR0 to confirm what KVM accepted. KVM may clamp the
	 * value against guest_supported_xcr0, the AND of leaf 0xD sub-leaf 0
	 * EAX with the leaf-1/leaf-7 ABI feature bits. A silent clamp to
	 * 0x3 (FP|SSE only) would leave VEX/AVX still faulting.
	 */
	{
		struct kvm_xcrs got = {0};
		int grc = os_ioctl_generic(vcpu_fd, KVM_GET_XCRS,
					   (unsigned long)&got);
		pr_debug("um: kvm-v2 install_xcrs: vcpu_fd=%d set=0x7 get_rc=%d nr=%u xcr0=%#llx\n",
			 vcpu_fd, grc, got.nr_xcrs,
			 (unsigned long long)got.xcrs[0].value);
	}
	return 0;
}

/*
 * Install or update the per-vCPU signal mask before KVM_RUN.
 *
 * Normal execution keeps SIGALRM unblocked so UML's timer can preempt
 * KVM_RUN. Replay blocks SIGALRM so timer delivery cannot create an
 * unrecorded in-guest EINTR point. Block other host signals in both modes so
 * they are delivered through UML's normal return-to-user handling rather than
 * interrupting arbitrary kernel code mid-ioctl. On SMP, leave the UML IPI
 * signal unblocked so remote TLB kicks can force KVM_RUN to return.
 */
static int kvm_v2_set_signal_mask(struct kvm_v2_vcpu *vcpu, bool block_timer)
{
	struct {
		__u32 len;
		__u8  sigset[sizeof(sigset_t)];
	} __packed mask = {
		.len = sizeof(sigset_t),
	};
	sigset_t set;
	int rc;

	if (vcpu->signal_mask_installed &&
	    vcpu->signal_mask_blocks_timer == block_timer)
		return 0;

	sigfillset(&set);
	if (!block_timer)
		sigdelset(&set, SIGALRM);  /* timer-driven preemption */
#if IS_ENABLED(CONFIG_SMP)
	/*
	 * Also unblock IPI_SIGNAL during KVM_RUN so a remote vCPU's
	 * um_tlb_sync can kick this vCPU out of guest mode. The next
	 * dispatch toggles CR4.PGE to flush the local guest TLB.
	 */
	sigdelset(&set, os_ipi_signum());
#endif
	memcpy(mask.sigset, &set, sizeof(sigset_t));

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_SIGNAL_MASK,
			      (unsigned long)&mask);
	if (rc < 0) {
		pr_err("um: kvm-v2 install_sigmask: KVM_SET_SIGNAL_MASK(vcpu_fd=%d) failed (%d)\n",
		       vcpu->vcpu_fd, rc);
		return rc;
	}
	vcpu->signal_mask_installed = true;
	vcpu->signal_mask_blocks_timer = block_timer;
	pr_debug("um: kvm-v2 install_sigmask: vcpu_fd=%d (SIGALRM %s%s)\n",
		 vcpu->vcpu_fd, block_timer ? "blocked" : "unblocked",
		 IS_ENABLED(CONFIG_SMP) ? ", IPI_SIGNAL unblocked" : "");
	trace_um_backend_kvm_v2_sigmask_install(vcpu->vcpu_fd, block_timer);
	return 0;
}

/*
 * Cross-vCPU guest-TLB kick. Called after um_tlb_sync drains pending host-mm
 * changes so other UML CPUs promptly leave KVM_RUN and toggle CR4.PGE on their
 * next dispatch.
 *
 * The IPI uses UML_IPI_RES because scheduler_ipi() is harmless when no
 * reschedule is pending; the required side effect is the EINTR from KVM_RUN.
 * The kick targets all online CPUs; an mm_cpumask filter can narrow it once
 * UML tracks that mask.
 */
void kvm_v2_tlb_kick_others(struct mm_struct *mm)
{
#if IS_ENABLED(CONFIG_SMP)
	int my_cpu, cpu;
	u64 cur_gen;

	if (!mm)
		return;
	cur_gen = atomic64_read(&mm->context.tlb_gen);
	my_cpu = raw_smp_processor_id();
	for_each_online_cpu(cpu) {
		struct kvm_v2_vcpu *v;

		if (cpu == my_cpu)
			continue;
		v = kvm_v2_vcpu_get(cpu);
		if (!v)
			continue;
		/*
		 * Only kick vCPUs running this mm. vCPUs running other mms are
		 * not affected by this mm's PTE changes. current_mm is updated
		 * in load_user_sregs and read locklessly here; an outdated value
		 * can at worst send an unneeded IPI, bounded by the generation
		 * check and cmpxchg dedup below.
		 */
		if (READ_ONCE(v->current_mm) != mm)
			continue;
		/*
		 * Skip vCPUs already up-to-date.
		 * Read from the per-(mm, cpu) array to compare against
		 * this cpu's last-seen gen for THIS mm specifically, not
		 * a cross-mm-contaminated per-vCPU counter.
		 */
		if (atomic64_read(&mm->context.tlb_gen_seen_by[cpu]) >=
		    cur_gen)
			continue;
		/*
		 * At most one IPI in flight per vCPU. Reset by the kicked vCPU
		 * at load_user_sregs.
		 */
		if (atomic_cmpxchg(&v->kick_pending, 0, 1) == 0)
			(void)os_send_ipi(cpu, 0 /* UML_IPI_RES */);
	}
#else
	(void)mm;
#endif
}

static void kvm_v2_vcpu_init_slot(struct kvm_v2_vcpu *v, int cpu, int vcpu_fd,
				  void *kvm_run, int mmap_size)
{
	v->vcpu_fd      = vcpu_fd;
	v->kvm_run      = kvm_run;
	v->kvm_run_size = (u32)mmap_size;
	v->cpu          = cpu;
	v->signal_mask_installed = false;
	v->signal_mask_blocks_timer = false;

	/*
	 * CPUID install is deferred to first KVM_RUN because the supported CPUID
	 * buffer needs the buddy allocator. Until the lazy install succeeds, the
	 * dispatcher must not enter guest code.
	 */
	v->cpuid_primed = false;

	/*
	 * At vCPU create the guest FPU contains KVM reset state, not any task's
	 * snapshot. Force the first post-vmexit GET by starting dirty.
	 */
	v->fpu_dirty      = true;
	v->fpu_owner_task = NULL;

	v->dispatch_heavy_count = 0;
	v->dispatch_cheap_count = 0;
}

static void kvm_v2_vcpu_enable_sync_regs(void *kvm_run)
{
	/*
	 * KVM_CAP_SYNC_REGS is required-cap in init.c. With kvm_valid_regs set
	 * at create time, KVM_RUN populates kvm_run->s.regs.{regs,sregs} on
	 * exit, and the dispatcher writes updates back through kvm_dirty_regs.
	 */
	((struct kvm_run *)kvm_run)->kvm_valid_regs =
		KVM_SYNC_X86_REGS | KVM_SYNC_X86_SREGS;
}

static int kvm_v2_vcpu_reset_debugregs(int vcpu_fd)
{
	struct kvm_debugregs zero_dr = { 0 };
	int rc;

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_DEBUGREGS,
			      (unsigned long)&zero_dr);
	if (rc < 0)
		pr_err("um: kvm-v2 vcpu_create_one: KVM_SET_DEBUGREGS(vcpu_fd=%d) failed (%d)\n",
		       vcpu_fd, rc);

	return rc;
}

static int kvm_v2_vcpu_finish_setup(struct kvm_v2_vcpu *v)
{
	int rc;

	rc = kvm_v2_vcpu_program_msrs(v->vcpu_fd, v->cpu);
	if (rc < 0)
		return rc;

	rc = kvm_v2_install_baseline_sregs(v);
	if (rc < 0)
		return rc;

	rc = kvm_v2_set_signal_mask(v, false);
	if (rc < 0)
		return rc;

	return kvm_v2_vcpu_reset_debugregs(v->vcpu_fd);
}

/*
 * Build a single pool member. Returns 0 on success or a negative errno
 * on failure; the caller (kvm_v2_vcpu_create) tears down already-built
 * entries on partial failure. KVM_CREATE_VCPU's id argument is the vCPU id
 * within the VM (0..max-1); using the host CPU index keeps KVM's internal
 * numbering aligned with the pool key.
 */
static int kvm_v2_vcpu_create_one(struct kvm_v2_vm *vm, int cpu, int mmap_size)
{
	struct kvm_v2_vcpu *v = &vcpus[cpu];
	int vcpu_fd, rc;
	void *kvm_run;

	vcpu_fd = os_ioctl_generic(vm->vm_fd, KVM_CREATE_VCPU,
				   (unsigned long)cpu);
	if (vcpu_fd < 0) {
		pr_err("um: kvm-v2 vcpu_create: KVM_CREATE_VCPU(id=%d) failed (%d)\n",
		       cpu, vcpu_fd);
		return vcpu_fd;
	}

	kvm_run = os_mmap_rw_shared(vcpu_fd, mmap_size);
	if (!kvm_run) {
		pr_err("um: kvm-v2 vcpu_create: mmap of kvm_run (cpu=%d size %d) failed\n",
		       cpu, mmap_size);
		rc = -ENOMEM;
		goto err_close_vcpu;
	}

	kvm_v2_vcpu_init_slot(v, cpu, vcpu_fd, kvm_run, mmap_size);
	kvm_v2_vcpu_enable_sync_regs(kvm_run);

	rc = kvm_v2_vcpu_finish_setup(v);
	if (rc < 0)
		goto err_unmap_kvm_run;

	trace_um_backend_kvm_v2_vcpu_create(vcpu_fd, v->kvm_run_size);
	return 0;

err_unmap_kvm_run:
	os_unmap_memory(kvm_run, mmap_size);
	v->kvm_run      = NULL;
	v->kvm_run_size = 0;
err_close_vcpu:
	os_close_file(vcpu_fd);
	v->vcpu_fd      = -1;
	v->cpu          = -1;
	return rc;
}

static void kvm_v2_vcpu_destroy_one(struct kvm_v2_vcpu *v)
{
	if (v->vcpu_fd < 0)
		return;

	/* Report the lifetime dispatch path split before tearing the vCPU down. */
	{
		u64 heavy = v->dispatch_heavy_count;
		u64 cheap = v->dispatch_cheap_count;
		u64 total = heavy + cheap;

		pr_debug("um: kvm-v2 vcpu_destroy: cpu=%d dispatches=heavy:%llu cheap:%llu total:%llu heavy_pct=%llu.%02llu\n",
			 v->cpu,
			 (unsigned long long)heavy,
			 (unsigned long long)cheap,
			 (unsigned long long)total,
			 (unsigned long long)(total ? heavy * 100 / total : 0),
			 (unsigned long long)(total
				 ? (heavy * 10000 / total) % 100
				 : 0));
	}

	/*
	 * The kvm_run mmap survives vcpu_fd close because the mapping is
	 * refcounted in the kernel, so unmap before closing the fd.
	 */
	if (v->kvm_run) {
		os_unmap_memory(v->kvm_run, (int)v->kvm_run_size);
		v->kvm_run = NULL;
	}

	os_close_file(v->vcpu_fd);
	v->vcpu_fd      = -1;
	v->kvm_run_size = 0;
	v->cpu          = -1;
}

static bool kvm_v2_vcpu_pool_busy(void)
{
	if (vcpus[0].vcpu_fd < 0)
		return false;

	pr_warn("um: kvm-v2 vcpu_create: pool already created (vcpu_fd=%d)\n",
		vcpus[0].vcpu_fd);
	return true;
}

static int kvm_v2_get_vcpu_mmap_size(struct kvm_v2_vm *vm)
{
	int mmap_size;

	mmap_size = os_ioctl_generic(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size <= 0) {
		pr_err("um: kvm-v2 vcpu_create: KVM_GET_VCPU_MMAP_SIZE failed (%d)\n",
		       mmap_size);
		return mmap_size ? mmap_size : -EIO;
	}

	return mmap_size;
}

static unsigned int kvm_v2_vcpu_pool_size(void)
{
	if (WARN_ON_ONCE(nr_cpu_ids > KVM_V2_MAX_VCPUS))
		pr_warn("um: kvm-v2 vcpu_create: nr_cpu_ids=%u > max_vcpus=%u; capping pool\n",
			nr_cpu_ids, (unsigned int)KVM_V2_MAX_VCPUS);

	return min_t(unsigned int, nr_cpu_ids, KVM_V2_MAX_VCPUS);
}

static int kvm_v2_create_pool_members(struct kvm_v2_vm *vm,
				      unsigned int pool_size,
				      int mmap_size)
{
	unsigned int cpu;
	int rc;

	for (cpu = 0; cpu < pool_size; cpu++) {
		rc = kvm_v2_vcpu_create_one(vm, cpu, mmap_size);
		if (rc)
			goto err_unwind;
	}

	return 0;

err_unwind:
	while (cpu > 0)
		kvm_v2_vcpu_destroy_one(&vcpus[--cpu]);
	return rc;
}

int kvm_v2_vcpu_create(struct kvm_v2_vm *vm)
{
	int mmap_size;
	int rc;
	unsigned int pool_size;

	if (!vm)
		return -EINVAL;

	/*
	 * Initialise the array's sentinel state on first entry. The BSS-zeroed
	 * default puts every vcpu_fd at 0, a valid fd, so that value cannot be
	 * used as the "already created" check until reset flips it to -1.
	 * After this call the .vcpu_fd >= 0 condition below is meaningful.
	 */
	if (!pool_initialised)
		kvm_v2_vcpu_pool_reset();

	if (kvm_v2_vcpu_pool_busy())
		return -EBUSY;

	/*
	 * KVM_GET_VCPU_MMAP_SIZE is a /dev/kvm-fd ioctl and the size
	 * is constant for the host kernel's lifetime -- query once and
	 * reuse for every pool member.
	 */
	mmap_size = kvm_v2_get_vcpu_mmap_size(vm);
	if (mmap_size < 0)
		return mmap_size;

	/*
	 * Cap the pool at KVM_V2_MAX_VCPUS to bound the static array. In
	 * practice nr_cpu_ids tracks the configured maximum, so the WARN
	 * below should never fire. If it does, build a smaller pool rather
	 * than overflowing the array.
	 */
	pool_size = kvm_v2_vcpu_pool_size();
	rc = kvm_v2_create_pool_members(vm, pool_size, mmap_size);
	if (rc)
		return rc;

	pr_debug("um: kvm-v2 vcpu_create: pool of %u vCPU(s) up (kvm_run_size=%d)\n",
		 pool_size, mmap_size);
	return 0;
}

void kvm_v2_vcpu_destroy(void)
{
	int cpu;

	if (!pool_initialised)
		return;

	for (cpu = 0; cpu < KVM_V2_MAX_VCPUS; cpu++) {
		if (vcpus[cpu].vcpu_fd >= 0)
			pr_debug("um: kvm-v2 vcpu_destroy: closing cpu=%d vcpu_fd=%d\n",
				 cpu, vcpus[cpu].vcpu_fd);
		kvm_v2_vcpu_destroy_one(&vcpus[cpu]);
	}
}

struct kvm_v2_vcpu *kvm_v2_vcpu_get(int cpu)
{
	if (!pool_initialised)
		return NULL;
	if (cpu < 0 || cpu >= KVM_V2_MAX_VCPUS)
		return NULL;
	if (vcpus[cpu].vcpu_fd < 0)
		return NULL;
	return &vcpus[cpu];
}

/*
 * Load guest CR3 on the supplied pool member. The caller passes the
 * physical address of the current mm's pgd, giving KVM a page table whose
 * user half is user-accessible and whose kernel half is supervisor-only.
 */
int kvm_v2_load_cr3(struct kvm_v2_vcpu *vcpu, unsigned long pgd)
{
	struct kvm_sregs sregs;
	int rc;

	if (!vcpu || vcpu->vcpu_fd < 0)
		return -ENODEV;

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_SREGS,
			      (unsigned long)&sregs);
	if (rc < 0)
		return rc;

	sregs.cr3 = (u64)pgd;	/* caller passes __pa(mm->pgd) */

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_SREGS,
			      (unsigned long)&sregs);
	if (rc < 0)
		return rc;

	return 0;
}

static void kvm_v2_load_user_segments(struct kvm_sregs *sregs,
				      unsigned long entry_rip)
{
	/*
	 * If the next RIP is a user-half VA, reset CS/SS/DS/ES to user
	 * selectors. SYSCALL exits leave CS as the kernel selector; entering
	 * user RIP with kernel CS would run user code at CPL=0. Keep the
	 * existing selector only for trampoline-replay paths where RIP is
	 * still in the kernel-half trampoline range.
	 */
	if (entry_rip >= KVM_V2_TRAMPOLINE_GVA)
		return;

	kvm_v2_apply_user_segments(sregs);
}

static void kvm_v2_restore_or_clear_cr2(struct kvm_v2_vcpu *vcpu,
					struct kvm_sregs *sregs)
{
	struct arch_thread *a = &current->thread.arch;

	/*
	 * cr2 in the sync-regs mmap reflects the last KVM_RUN exit. Preserve
	 * a saved CR2 when resuming an interrupted page-fault stub; otherwise
	 * clear CR2 when ownership crosses task or mm so stale fault addresses
	 * cannot leak between UML tasks sharing one vCPU.
	 */
	if (a->kvm_v2.saved_cr2_valid) {
		sregs->cr2 = a->kvm_v2.saved_cr2_at_eintr;
		a->kvm_v2.saved_cr2_valid = false;
	} else if (vcpu->last_task != current || vcpu->last_mm != current->mm) {
		sregs->cr2 = 0;
	}

	vcpu->last_task = current;
	vcpu->last_mm   = current->mm;
}

static void kvm_v2_update_dispatch_tlb_state(struct kvm_v2_vcpu *vcpu)
{
	/*
	 * Ack any pending remote TLB kick and update per-vCPU tlb_gen
	 * tracking. The caller's CR4.PGE toggle is the local flush. Stash
	 * current_mm so the remote kicker can target only vCPUs running this
	 * mm.
	 */
	atomic_set(&vcpu->kick_pending, 0);
	WRITE_ONCE(vcpu->current_mm, current->mm);
	if (current->mm) {
		int cpu = vcpu->cpu;
		u64 cur_gen = atomic64_read(&current->mm->context.tlb_gen);
		u64 last = atomic64_read(&current->mm->context.tlb_gen_seen_by[cpu]);

		vcpu->last_dispatch_tlb_lag =
			(cur_gen > last) ? (cur_gen - last) : 0;

		atomic64_set(&current->mm->context.tlb_gen_seen_by[cpu],
			     cur_gen);

		/*
		 * Another vCPU may have bumped tlb_gen between the read and the
		 * write above. Re-read and force the heavy KVM_SET_SREGS path if
		 * the generation moved.
		 */
		smp_mb();
		if (atomic64_read(&current->mm->context.tlb_gen) != cur_gen)
			vcpu->last_dispatch_tlb_lag =
				KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD;
	} else {
		vcpu->last_dispatch_tlb_lag = 0;
	}
}

static void kvm_v2_arm_lazy_fpu_trap(struct kvm_sregs *sregs)
{
	/*
	 * Arm CR0.TS to lazily detect FPU usage. Any FP/SSE/AVX instruction
	 * the guest executes while TS=1 raises #NM; the in-guest handler clears
	 * TS and retries the instruction. After vmexit, TS=0 means the guest
	 * used FPU and the post-vmexit path must capture it.
	 */
	if (current->thread.arch.kvm_v2.nm_ts_bypass) {
		sregs->cr0 &= ~X86_CR0_TS;
		current->thread.arch.kvm_v2.nm_ts_bypass = false;
	} else {
		sregs->cr0 |= X86_CR0_TS;
	}
}

static void kvm_v2_maybe_drop_prev_roots(struct kvm_v2_vcpu *vcpu,
					 struct kvm_sregs *sregs,
					 bool cross_task)
{
	/*
	 * KVM keeps a small LRU of recently used TDP roots per vCPU. When a
	 * task/mm changes, or when this vCPU has fallen far behind the mm's
	 * guest-TLB generation, use a full KVM_SET_SREGS ioctl to force KVM's
	 * __set_sregs2 path through kvm_mmu_reset_context and drop cached
	 * prev_roots[] entries. Same-task same-mm re-entry can use the cheap
	 * KVM_SYNC_X86_SREGS dirty-bit path.
	 */
	if (cross_task ||
	    vcpu->last_dispatch_tlb_lag >=
		    KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD) {
		struct kvm_sregs full = *sregs;
		int rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_SREGS,
					  (unsigned long)&full);

		vcpu->dispatch_heavy_count++;

		if (WARN_ON_ONCE(rc < 0))
			pr_warn_ratelimited("um: kvm-v2 prev_roots drop failed rc=%d cpu=%d cross=%d lag=%llu\n",
					    rc, vcpu->cpu, cross_task,
					    (unsigned long long)vcpu->last_dispatch_tlb_lag);
	} else {
		vcpu->dispatch_cheap_count++;
	}
}

static bool kvm_v2_cross_task_or_mm(const struct kvm_v2_vcpu *vcpu)
{
	return vcpu->last_task != current || vcpu->last_mm != current->mm;
}

static void kvm_v2_load_user_address_state(struct kvm_sregs *sregs,
					   unsigned long pgd_pa,
					   unsigned long fs_base,
					   unsigned long gs_base)
{
	sregs->cr3     = (u64)pgd_pa;
	sregs->fs.base = (u64)fs_base;
	sregs->gs.base = (u64)gs_base;
}

void kvm_v2_load_user_address_sregs(struct kvm_sregs *sregs,
				    unsigned long pgd_pa,
				    unsigned long fs_base,
				    unsigned long gs_base,
				    unsigned long entry_rip)
{
	/*
	 * Refresh selectors before FS/GS bases. kvm_v2_apply_user_segments()
	 * replaces the whole FS/GS segment cache with flat data descriptors,
	 * whose base is zero. arch_prctl() stores the guest TLS base in
	 * regs->gp[HOST_FS_BASE/GS_BASE]; losing it here makes dynamic
	 * loaders fault on their next %fs access.
	 */
	kvm_v2_load_user_segments(sregs, entry_rip);
	kvm_v2_load_user_address_state(sregs, pgd_pa, fs_base, gs_base);
}

static void kvm_v2_note_fpu_owner_change(struct kvm_v2_vcpu *vcpu)
{
	if (vcpu->fpu_owner_task != current)
		vcpu->fpu_dirty = true;
}

static void kvm_v2_force_guest_tlb_flush(struct kvm_sregs *sregs)
{
	/*
	 * UML mutates guest PTEs directly in physmem, which does not notify
	 * KVM's MMU. Toggle CR4.PGE on each dispatch so KVM observes a CR4
	 * change and flushes the guest TLB on entry. UML guests do not use
	 * global pages, so the PGE value itself is not semantically important.
	 */
	sregs->cr4 ^= X86_CR4_PGE;
}

static void kvm_v2_apply_record_time_policy(struct kvm_sregs *sregs)
{
#ifdef CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL
	/*
	 * Direct user RDTSC/RDTSCP observes host time outside the replay log.
	 * CR4.TSD makes those instructions fault at CPL=3 while strict replay
	 * is active; syscall time sources still reach the strict replay gate.
	 */
	if (static_branch_unlikely(&um_kvm_v2_record_enabled) &&
	    kvm_v2_record_replay_active())
		sregs->cr4 |= X86_CR4_TSD;
	else
		sregs->cr4 &= ~X86_CR4_TSD;
#endif
}

static void kvm_v2_apply_record_signal_policy(struct kvm_v2_vcpu *vcpu)
{
#ifdef CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL
	bool replay = static_branch_unlikely(&um_kvm_v2_record_enabled) &&
		      kvm_v2_record_replay_active();
	int rc;

	rc = kvm_v2_set_signal_mask(vcpu, replay);
	if (rc < 0)
		panic("kvm-v2: record signal policy (cpu=%d replay=%d) failed: %d",
		      vcpu->cpu, replay, rc);
#else
	(void)vcpu;
#endif
}

static void kvm_v2_load_user_efer(struct kvm_sregs *sregs)
{
	/*
	 * EFER.SCE is required for SYSCALL. The backend owns EFER under the
	 * single-VM model, so write the complete long-mode value.
	 */
	sregs->efer = EFER_SCE | EFER_LME | EFER_LMA | EFER_NX;
}

static void kvm_v2_refresh_gadget_identity(struct kvm_v2_vcpu *vcpu, u8 *p)
{
	const struct cred *c;

	c = current_cred();
	WRITE_ONCE(*(u32 *)(p + KVM_V2_GADGET_OFF_CPU_ID), (u32)vcpu->cpu);
	WRITE_ONCE(*(u32 *)(p + KVM_V2_GADGET_OFF_TGID),
		   task_tgid_vnr(current));
	WRITE_ONCE(*(u32 *)(p + KVM_V2_GADGET_OFF_TID),
		   task_pid_vnr(current));
	WRITE_ONCE(*(u32 *)(p + KVM_V2_GADGET_OFF_PPID),
		   task_ppid_nr(current));
	WRITE_ONCE(*(u32 *)(p + KVM_V2_GADGET_OFF_UID),
		   from_kuid_munged(current_user_ns(), c->uid));
	WRITE_ONCE(*(u32 *)(p + KVM_V2_GADGET_OFF_EUID),
		   from_kuid_munged(current_user_ns(), c->euid));
	WRITE_ONCE(*(u32 *)(p + KVM_V2_GADGET_OFF_GID),
		   from_kgid_munged(current_user_ns(), c->gid));
	WRITE_ONCE(*(u32 *)(p + KVM_V2_GADGET_OFF_EGID),
		   from_kgid_munged(current_user_ns(), c->egid));
}

static void kvm_v2_refresh_gadget_realtime(u8 *p)
{
	struct timespec64 real_ts;

	ktime_get_real_ts64(&real_ts);
	WRITE_ONCE(*(s64 *)(p + KVM_V2_GADGET_OFF_REAL_SEC), real_ts.tv_sec);
}

static void kvm_v2_refresh_gadget_monotonic(u8 *p)
{
	u32 *seq = (u32 *)(p + KVM_V2_GADGET_OFF_SEQ);
	u64 mono_ns = ktime_get_ns();

	WRITE_ONCE(*seq, *seq + 1);
	smp_wmb();	/* publish odd seq before payload */
	WRITE_ONCE(*(s64 *)(p + KVM_V2_GADGET_OFF_MONO_SEC),
		   (s64)(mono_ns / NSEC_PER_SEC));
	WRITE_ONCE(*(s64 *)(p + KVM_V2_GADGET_OFF_MONO_NSEC),
		   (s64)(mono_ns % NSEC_PER_SEC));
	WRITE_ONCE(*(s32 *)(p + KVM_V2_GADGET_OFF_BUDGET),
		   KVM_V2_VVAR_BUDGET_INITIAL);
	smp_wmb();	/* publish payload before even seq */
	WRITE_ONCE(*seq, *seq + 1);
}

static void kvm_v2_refresh_gadget_state(struct kvm_v2_vcpu *vcpu)
{
	u8 *p;

	/*
	 * Refresh this vCPU's gadget state page before every KVM_RUN. The LSTAR
	 * gadget reads pid, credential, and vvar-style clock data from this
	 * page through MSR_KERNEL_GS_BASE. Each vCPU has its own page, so the
	 * dispatch thread and guest reader are mutually exclusive in time.
	 */
	if (!vcpu->gadget_state_kva)
		return;

	p = (u8 *)vcpu->gadget_state_kva;
	kvm_v2_refresh_gadget_identity(vcpu, p);
	kvm_v2_refresh_gadget_realtime(p);
	kvm_v2_refresh_gadget_monotonic(p);
}

/*
 * Establish the guest user SREGS for one KVM_RUN. With KVM_CAP_SYNC_REGS
 * enabled, the mmap'd kvm_run->s.regs.sregs is authoritative on entry.
 * Modify cr3/fs.base/gs.base in place and mark KVM_SYNC_X86_SREGS so KVM
 * consumes the update on the next KVM_RUN. A full KVM_SET_SREGS ioctl is
 * also issued when task/mm ownership changes or the TLB-generation lag is
 * high enough to force KVM to drop cached TDP roots.
 *
 * Returns 0; kept as int so callers can surface a per-CPU SYNC_REGS
 * check if a host ever exposes a partial cap.
 */
static int kvm_v2_load_user_sregs(struct kvm_v2_vcpu *vcpu,
				  unsigned long pgd_pa,
				  unsigned long fs_base,
				  unsigned long gs_base,
				  unsigned long entry_rip)
{
	struct kvm_run *run = vcpu->kvm_run;
	struct kvm_sregs *sregs = &run->s.regs.sregs;
	bool cross_task = kvm_v2_cross_task_or_mm(vcpu);

	kvm_v2_load_user_address_sregs(sregs, pgd_pa, fs_base, gs_base,
				       entry_rip);
	kvm_v2_restore_or_clear_cr2(vcpu, sregs);
	kvm_v2_note_fpu_owner_change(vcpu);
	kvm_v2_force_guest_tlb_flush(sregs);
	kvm_v2_apply_record_time_policy(sregs);
	kvm_v2_update_dispatch_tlb_state(vcpu);
	kvm_v2_load_user_efer(sregs);
	kvm_v2_arm_lazy_fpu_trap(sregs);
	kvm_v2_maybe_drop_prev_roots(vcpu, sregs, cross_task);
	run->kvm_dirty_regs |= KVM_SYNC_X86_SREGS;
	kvm_v2_refresh_gadget_state(vcpu);

	return 0;
}

/*
 * Marshal uml_pt_regs.gp[] to struct kvm_regs. RFLAGS bit 1 is reserved
 * and must be one, so set it before KVM consumes the state.
 */
/*
 * Non-static so syscall_trap.c can reuse it.
 * Declared in kvm_v2_backend.h.
 */
void kvm_v2_marshal_to_kvm_regs(struct kvm_regs *dst,
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
	dst->rflags = gp[HOST_EFLAGS] | (1UL << 1);
}

/*
 * Reverse marshal: struct kvm_regs to uml_pt_regs.gp[]. Called after
 * KVM_RUN returns so UML's syscall / fault / signal dispatch sees
 * the guest's post-exit GPRs. HOST_ORIG_AX is intentionally NOT
 * written here -- that's an UML entry-path convention the syscall
 * dispatcher arranges once it knows the trap class.
 */
void kvm_v2_marshal_from_kvm_regs(struct uml_pt_regs *dst,
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

/*
 * Reverse-marshal sregs.fs.base / gs.base back into the per-task
 * gp[HOST_FS_BASE/GS_BASE] slots. UML's canonical FS/GS state lives
 * in those gp[] slots; arch_prctl(ARCH_SET_FS, ...) writes them
 * synchronously, and kvm_v2_load_user_sregs() reads them back into
 * sregs on every dispatch.
 *
 * Without this symmetric read-back, intra-guest FS/GS updates from paths
 * such as wrfsbase can be lost on the next dispatch when stale gp[] values
 * are reinstalled.
 *
 * SYNC_REGS guarantees sregs is coherent on every KVM_RUN exit,
 * including EINTR, so this is a zero-ioctl read from the mmap'd page.
 */
void kvm_v2_marshal_sregs_back(struct uml_pt_regs *dst,
			       const struct kvm_sregs *src)
{
	dst->gp[HOST_FS_BASE] = (unsigned long)src->fs.base;
	dst->gp[HOST_GS_BASE] = (unsigned long)src->gs.base;
}

/*
 * Forward declaration: kvm_v2_fpu_install_on_first_run lives at the
 * bottom of the file alongside kvm_v2_fpu_capture_for_fork (the two
 * are a logical pair). The dispatcher below references it before its
 * definition.
 */
static int kvm_v2_fpu_install_on_first_run(struct kvm_v2_vcpu *vcpu);

static bool kvm_v2_rip_in_lstar_gadget(u64 rip)
{
	unsigned long len = kvm_v2_lstar_gadget_end -
			    kvm_v2_lstar_gadget_start;

	return rip >= KVM_V2_LSTAR_GVA && rip < KVM_V2_LSTAR_GVA + len;
}

static bool kvm_v2_rip_in_handler_slot(u64 rip, unsigned int slot)
{
	u64 start = KVM_V2_HANDLERS_GVA + slot * KVM_V2_HANDLER_SLOT_STRIDE;

	return rip >= start && rip < start + KVM_V2_HANDLER_SLOT_STRIDE;
}

static bool kvm_v2_rip_in_handler_area(u64 rip)
{
	return rip >= KVM_V2_HANDLERS_GVA &&
	       rip < KVM_V2_HANDLERS_GVA +
		     KVM_V2_HANDLER_NR_SLOTS * KVM_V2_HANDLER_SLOT_STRIDE;
}

static void kvm_v2_restore_lstar_kernel_gs(struct kvm_v2_vcpu *vcpu, u64 rip)
{
	int rc;

	if (rip < KVM_V2_LSTAR_GVA + 3)
		return;

	{
		struct {
			struct kvm_msrs hdr;
			struct kvm_msr_entry e[1];
		} req = {
			.hdr = { .nmsrs = 1 },
			.e = {{
				.index = MSR_KERNEL_GS_BASE,
				.data = KVM_V2_GADGET_STATE_GVA(vcpu->cpu),
			}},
		};

		rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_MSRS,
				      (unsigned long)&req);
		if (rc < 0)
			panic("kvm-v2: restore MSR_KERNEL_GS_BASE after LSTAR EINTR (cpu=%d) failed: %d",
			      vcpu->cpu, rc);
	}
}

static void kvm_v2_recover_lstar_saved_gprs(struct uml_pt_regs *regs,
					    struct kvm_v2_vcpu *vcpu, u64 rip)
{
	u8 *page;

	if (!vcpu->gadget_state_kva)
		return;

	page = (u8 *)vcpu->gadget_state_kva;
	if (rip >= KVM_V2_LSTAR_GVA + 12)
		regs->gp[HOST_DX] =
			*(u64 *)(page + KVM_V2_GADGET_OFF_SAVE_RDX);
	if (rip >= KVM_V2_LSTAR_GVA + 21)
		regs->gp[HOST_R8] =
			*(u64 *)(page + KVM_V2_GADGET_OFF_SAVE_R8);
	if (rip >= KVM_V2_LSTAR_GVA + 30)
		regs->gp[HOST_R10] =
			*(u64 *)(page + KVM_V2_GADGET_OFF_SAVE_R10);
}

static void kvm_v2_recover_lstar_eintr(struct uml_pt_regs *regs,
				       const struct kvm_regs *eintr_regs,
				       struct kvm_v2_vcpu *vcpu)
{
	u64 rip = eintr_regs->rip;

	/*
	 * EINTR caught the guest somewhere in the LSTAR body. There are two
	 * regimes:
	 *
	 * (a) RIP in [LSTAR, LSTAR+3): before entry swapgs. Rewind HOST_IP to
	 *     the user SYSCALL instruction so the next dispatch retries from
	 *     user mode.
	 *
	 * (b) RIP >= LSTAR+3: after entry swapgs. The CPU has hardware-swapped
	 *     GS_BASE <-> MSR_KERNEL_GS_BASE, so restore MSR_KERNEL_GS_BASE to
	 *     this vCPU's state page before rewinding.
	 *
	 * In both regimes the gadget body is idempotent and the retry restarts
	 * from user SYSCALL in a known swapgs state.
	 */
	kvm_v2_restore_lstar_kernel_gs(vcpu, rip);

	/*
	 * Recover user RDX/R8/R10 from gadget state-page SAVE slots when EINTR
	 * caught the body after the corresponding entry-save instruction
	 * completed. Without this, scratch writes inside fast syscall handlers
	 * can leak into the next dispatch as user GPRs.
	 */
	kvm_v2_recover_lstar_saved_gprs(regs, vcpu, rip);
	regs->gp[HOST_IP] = regs->gp[HOST_CX] - 2;
}

static void kvm_v2_prime_cpuid(struct kvm_v2_vcpu *vcpu, int cpu)
{
	struct kvm_v2_vm *vm;
	int rc;

	vm = kvm_v2_vm_get();
	if (!vm)
		panic("kvm-v2: cpuid lazy install (cpu=%d): VM not initialised",
		      cpu);

	rc = kvm_v2_install_cpuid(vm, vcpu->vcpu_fd);
	if (rc < 0)
		panic("kvm-v2: cpuid lazy install (cpu=%d) failed: %d",
		      cpu, rc);
}

static void kvm_v2_enable_xsave_sregs(struct kvm_v2_vcpu *vcpu, int cpu)
{
	struct kvm_run *run = vcpu->kvm_run;
	struct kvm_sregs sregs;
	int rc;

	/*
	 * Curated CPUID now advertises XSAVE/OSXSAVE/AVX. Arm CR4.OSXSAVE
	 * with a synchronous GET+SET_SREGS pair before KVM_SET_XCRS
	 * validates XCR0 against KVM's live CR4 state.
	 */
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_SREGS,
			      (unsigned long)&sregs);
	if (rc < 0)
		panic("kvm-v2: GET_SREGS pre-OSXSAVE (cpu=%d) failed: %d",
		      cpu, rc);

	sregs.cr4 |= X86_CR4_OSXSAVE;
#ifdef CONFIG_UM_BACKEND_KVM_V2_RDPMC
	/*
	 * CR4.PCE makes rdpmc accessible at CPL=3. Combined with KVM's vPMU
	 * this lets guest userspace read hardware performance counters
	 * directly when the host KVM module emulates them.
	 */
	sregs.cr4 |= X86_CR4_PCE;
#endif
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_SREGS,
			      (unsigned long)&sregs);
	if (rc < 0)
		panic("kvm-v2: SET_SREGS+OSXSAVE (cpu=%d) failed: %d",
		      cpu, rc);

	/*
	 * Mirror the live cr4 into the SYNC_REGS mmap so the next dispatch's
	 * PGE-toggle XOR starts from the correct base.
	 */
	run->s.regs.sregs.cr4 = sregs.cr4;
}

static void kvm_v2_prime_xcrs(struct kvm_v2_vcpu *vcpu, int cpu)
{
	int rc;

	rc = kvm_v2_install_xcrs(vcpu->vcpu_fd);
	if (rc < 0)
		panic("kvm-v2: install_xcrs lazy (cpu=%d) failed: %d",
		      cpu, rc);
}

static void kvm_v2_prime_vcpu_for_run(struct kvm_v2_vcpu *vcpu, int cpu)
{
	/*
	 * Lazy CPUID install before guest entry. By the first KVM_RUN the
	 * buddy allocator is available, and the install is sticky for the
	 * lifetime of this pool member. KVM-default CPUID can expose features
	 * this backend does not save, restore, or make deterministic.
	 */
	if (vcpu->cpuid_primed)
		return;

	kvm_v2_prime_cpuid(vcpu, cpu);
	kvm_v2_enable_xsave_sregs(vcpu, cpu);
	kvm_v2_prime_xcrs(vcpu, cpu);
	vcpu->cpuid_primed = true;
}

#if IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_KUNIT)
int kvm_v2_vcpu_prime_for_kunit(struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_vm *vm;
	struct kvm_run *run;
	struct kvm_sregs sregs;
	int rc;

	if (!vcpu || vcpu->vcpu_fd < 0 || !vcpu->kvm_run)
		return -EINVAL;
	if (vcpu->cpuid_primed)
		return 0;

	vm = kvm_v2_vm_get();
	if (!vm)
		return -ENODEV;

	rc = kvm_v2_install_cpuid(vm, vcpu->vcpu_fd);
	if (rc < 0)
		return rc;

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_SREGS,
			      (unsigned long)&sregs);
	if (rc < 0)
		return rc;

	sregs.cr4 |= X86_CR4_OSXSAVE;
#ifdef CONFIG_UM_BACKEND_KVM_V2_RDPMC
	sregs.cr4 |= X86_CR4_PCE;
#endif
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_SREGS,
			      (unsigned long)&sregs);
	if (rc < 0)
		return rc;

	run = vcpu->kvm_run;
	run->s.regs.sregs.cr4 = sregs.cr4;

	rc = kvm_v2_install_xcrs(vcpu->vcpu_fd);
	if (rc < 0)
		return rc;

	vcpu->cpuid_primed = true;
	return 0;
}
#endif

static void kvm_v2_sync_current_mm_for_run(void)
{
	int rc;

	if (!current->mm)
		return;

	/*
	 * Drain pending UML-side TLB invalidations into the spawner mm before
	 * entering the guest. UML kernel updates to guest PTEs are writes into
	 * physmem from KVM's perspective; they do not fire KVM mmu_notifier
	 * callbacks because no host-mm mapping changed. Without this sync, KVM's
	 * TDP cache can retain translations for guest PTEs that have changed.
	 */
	rc = um_tlb_sync(current->mm);
	if (rc < 0)
		panic("um: kvm-v2 vcpu_run: um_tlb_sync(mm=%p) failed (%d)",
		      current->mm, rc);
}

static void kvm_v2_restore_task_state_for_run(struct kvm_v2_vcpu *vcpu,
					      int cpu)
{
	int rc;

	/*
	 * Install per-task FPU snapshot or architectural reset values before
	 * guest entry. Entering the guest with arbitrary FPU state is worse
	 * than stopping here.
	 */
	rc = kvm_v2_fpu_install_on_first_run(vcpu);
	if (rc < 0)
		panic("kvm-v2: fpu install (cpu=%d) failed: %d", cpu, rc);

	/*
	 * Restore the per-task IST stack snapshot before re-entering the guest.
	 * This prevents one task from consuming another task's exception frame
	 * when multiple UML tasks share one per-host-CPU vCPU.
	 */
	kvm_v2_ist_frame_restore_pending(vcpu);

	if (current->thread.arch.kvm_v2.iotrap_fpu_valid) {
		/*
		 * Use KVM_SET_XSAVE, not legacy KVM_SET_FPU. The guest can use
		 * AVX YMM state whose upper half lives only in extended XSAVE
		 * state.
		 */
		rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_XSAVE,
				      (unsigned long)&current->thread.arch.kvm_v2.iotrap_fpu);
		if (rc < 0)
			panic("kvm-v2: restore iotrap XSAVE (cpu=%d) failed: %d",
			      cpu, rc);
		current->thread.arch.kvm_v2.iotrap_fpu_valid = false;
		vcpu->fpu_dirty      = false;
		vcpu->fpu_owner_task = current;
	}

	if (current->thread.arch.kvm_v2.iotrap_events_valid) {
		/*
		 * Restore pending exceptions, interrupt shadow, NMI, and SMI
		 * state before the task reuses this pool vCPU.
		 */
		rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_VCPU_EVENTS,
				      (unsigned long)&current->thread.arch.kvm_v2.iotrap_events);
		if (rc < 0)
			panic("kvm-v2: restore iotrap VCPU events (cpu=%d) failed: %d",
			      cpu, rc);
		current->thread.arch.kvm_v2.iotrap_events_valid = false;
	}
}

static int kvm_v2_run_ioctl(struct kvm_v2_vcpu *vcpu)
{
	/*
	 * Mark this host thread as inside KVM_RUN so timer ticks that interrupt
	 * the ioctl are charged to guest user time.
	 */
#if IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_ITIMER_VIRTUAL)
	int rc;

	os_kvm_run_enter();
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_RUN, 0);
	os_kvm_run_exit();
	return rc;
#else
	return os_ioctl_generic(vcpu->vcpu_fd, KVM_RUN, 0);
#endif
}

static void kvm_v2_capture_task_state_after_run(struct kvm_v2_vcpu *vcpu)
{
	struct kvm_run *run = vcpu->kvm_run;
	bool fpu_was_used = !(run->s.regs.sregs.cr0 & X86_CR0_TS);
	struct kvm_vcpu_events *events =
		&current->thread.arch.kvm_v2.iotrap_events;
	int rc;

	/*
	 * Gate KVM_GET_XSAVE on the per-vCPU dirty epoch. Skip only when the
	 * vCPU FPU has not changed since the last SET/GET and the cached owner
	 * is still current. Both checks must hold.
	 */
	if (fpu_was_used)
		vcpu->fpu_dirty = true;

	if (vcpu->fpu_dirty || vcpu->fpu_owner_task != current) {
		struct kvm_xsave *fpu = &current->thread.arch.kvm_v2.iotrap_fpu;

		rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_XSAVE,
				      (unsigned long)fpu);
		current->thread.arch.kvm_v2.iotrap_fpu_valid = (rc == 0);
		if (rc == 0) {
			vcpu->fpu_dirty      = false;
			vcpu->fpu_owner_task = current;
		}
	}

	/*
	 * Capture pending-event state so the next dispatch can restore it before
	 * KVM_RUN and keep exception/NMI/interrupt-shadow state per task.
	 */
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_VCPU_EVENTS,
			      (unsigned long)events);
	current->thread.arch.kvm_v2.iotrap_events_valid = (rc == 0);
}

static void kvm_v2_handle_interrupted_run(struct uml_pt_regs *regs,
					  struct kvm_run *run,
					  struct kvm_v2_vcpu *vcpu,
					  const struct kvm_regs *eintr_regs,
					  const struct kvm_sregs *eintr_sregs,
					  int cpu)
{
	kvm_v2_marshal_from_kvm_regs(regs, eintr_regs);
	kvm_v2_marshal_sregs_back(regs, eintr_sregs);

	if (kvm_v2_rip_in_lstar_gadget(eintr_regs->rip)) {
		kvm_v2_recover_lstar_eintr(regs, eintr_regs, vcpu);
	} else if (kvm_v2_rip_in_handler_slot(eintr_regs->rip,
					      KVM_V2_HANDLER_SLOT_PF)) {
		/*
		 * Page-fault handler: process the fault inline while this task
		 * still owns the vCPU and its IST stack.
		 */
		(void)kvm_v2_handle_pf_eintr_inline(regs, run, vcpu,
						    eintr_sregs->cr2);
	} else if (kvm_v2_rip_in_handler_slot(eintr_regs->rip,
					      KVM_V2_HANDLER_SLOT_NM)) {
		/*
		 * #NM handler: clear CR0.TS in sregs and restore user regs
		 * from the IST frame. The next KVM_RUN re-enters at the user
		 * RIP, not the handler stub.
		 */
		(void)kvm_v2_handle_nm_eintr_inline(regs, run, vcpu);
	} else if (kvm_v2_rip_in_handler_area(eintr_regs->rip)) {
		struct arch_thread *a = &current->thread.arch;

		a->kvm_v2.saved_cr2_at_eintr = eintr_sregs->cr2;
		a->kvm_v2.saved_cr2_valid = true;
		kvm_v2_ist_frame_snapshot_raw(vcpu);
	}

	trace_um_backend_kvm_v2_vcpu_eintr(cpu);

	/*
	 * Even on EINTR, KVM_RUN entry executed the CR4.PGE flush. Pages
	 * deferred during prior dispatches are safe to release.
	 */
	if (current->mm)
		um_mmu_gather_drain(current->mm);
}

static void kvm_v2_prepare_vcpu_entry(struct uml_pt_regs *regs,
				      struct kvm_v2_vcpu *vcpu, int cpu)
{
	struct kvm_run *run = vcpu->kvm_run;

	kvm_v2_prime_vcpu_for_run(vcpu, cpu);
	kvm_v2_sync_current_mm_for_run();
	kvm_v2_apply_record_signal_policy(vcpu);

	(void)kvm_v2_load_user_sregs(vcpu,
				     __pa(current->active_mm->pgd),
				     regs->gp[HOST_FS_BASE],
				     regs->gp[HOST_GS_BASE],
				     regs->gp[HOST_IP]);

	kvm_v2_restore_task_state_for_run(vcpu, cpu);

	/*
	 * Write GPRs into the mmap'd kvm_run->s.regs.regs and mark
	 * KVM_SYNC_X86_REGS in kvm_dirty_regs. KVM consumes both on entry.
	 */
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	trace_um_backend_kvm_v2_vcpu_enter(cpu, run);
	kvm_v2_state_trace(KVM_V2_STATE_TRACE_RUN_ENTER, cpu, 0, 0,
			   run->s.regs.regs.rip, run->s.regs.regs.rsp,
			   run->s.regs.regs.rax);
}

static void kvm_v2_snapshot_run_exit(struct kvm_run *run,
				     struct kvm_v2_vcpu *vcpu)
{
	/*
	 * Snapshot the kvm_run mmap state immediately after KVM_RUN returns and
	 * before unblock_signals(). Unblocking can run timer and scheduler work;
	 * if this task is switched out, another task can reuse the same pool vCPU
	 * and overwrite the mmap before it is snapshotted.
	 */
	vcpu->run_regs = run->s.regs.regs;
	vcpu->run_sregs = run->s.regs.sregs;
	vcpu->run_exit_reason = run->exit_reason;
}

static bool kvm_v2_handle_run_failure(struct uml_pt_regs *regs,
				      struct kvm_run *run,
				      struct kvm_v2_vcpu *vcpu,
				      int cpu, int rc)
{
	if (rc >= 0)
		return false;

	if (rc != -EINTR)
		panic("kvm-v2: KVM_RUN(cpu=%d) failed: %d (exit_reason=%u)",
		      cpu, rc, vcpu->run_exit_reason);

	kvm_v2_state_trace(KVM_V2_STATE_TRACE_RUN_EINTR, cpu,
			   vcpu->run_exit_reason, 0, regs->gp[HOST_IP],
			   regs->gp[HOST_SP], regs->gp[HOST_AX]);
	kvm_v2_handle_interrupted_run(regs, run, vcpu, &vcpu->run_regs,
				      &vcpu->run_sregs, cpu);
	return true;
}

static void kvm_v2_dispatch_exit_reason(struct uml_pt_regs *regs,
					struct kvm_run **runp,
					struct kvm_v2_vcpu **vcpup,
					int *cpu_p, u32 exit_reason)
{
	struct kvm_run *run = *runp;
	struct kvm_v2_vcpu *vcpu = *vcpup;
	int cpu = *cpu_p;
	int rc;

	switch (exit_reason) {
	case KVM_EXIT_IO:
		/*
		 * IO-port exit from the LSTAR trampoline or an IDT handler
		 * stub. Pass the full vCPU struct so exception dispatchers can
		 * read the IST frame the CPU pushed during exception delivery.
		 */
		rc = kvm_v2_handle_io_trap(regs, run, vcpu);
		if (rc < 0)
			panic("kvm-v2: io_trap (cpu=%d port=%#x) failed: %d",
			      cpu, run->io.port, rc);

		/*
		 * kvm_v2_handle_io_trap may temporarily release migration
		 * pinning around handle_syscall. Re-fetch cpu/vcpu/run before
		 * the matching migrate_enable.
		 */
		cpu = smp_processor_id();
		vcpu = kvm_v2_vcpu_get(cpu);
		if (vcpu)
			run = vcpu->kvm_run;
		break;
	case KVM_EXIT_HLT:
	case KVM_EXIT_FAIL_ENTRY:
	case KVM_EXIT_INTERNAL_ERROR:
	case KVM_EXIT_SHUTDOWN:
		panic("kvm-v2: unexpected exit %u from KVM_RUN", exit_reason);
	default:
		pr_warn_ratelimited("kvm-v2: unhandled exit_reason=%u (cpu=%d)\n",
				    exit_reason, cpu);
		panic("kvm-v2: unhandled exit %u from KVM_RUN", exit_reason);
	}

	*runp = run;
	*vcpup = vcpu;
	*cpu_p = cpu;
}

static bool kvm_v2_select_vcpu_or_fallback(struct uml_pt_regs *regs,
					   struct kvm_v2_vcpu **vcpup,
					   struct kvm_run **runp,
					   int *cpu_p)
{
	struct kvm_v2_vcpu *vcpu;
	int cpu;

	cpu = smp_processor_id();
	vcpu = kvm_v2_vcpu_get(cpu);
	if (!vcpu) {
		migrate_enable();
		seccomp_vcpu_run(regs);
		return false;
	}

	*vcpup = vcpu;
	*runp = vcpu->kvm_run;
	*cpu_p = cpu;
	return true;
}

static int kvm_v2_enter_and_snapshot(struct uml_pt_regs *regs,
				     struct kvm_v2_vcpu *vcpu, int cpu)
{
	struct kvm_run *run = vcpu->kvm_run;
	int rc;

	kvm_v2_prepare_vcpu_entry(regs, vcpu, cpu);
	rc = kvm_v2_run_ioctl(vcpu);
	kvm_v2_capture_task_state_after_run(vcpu);
	kvm_v2_snapshot_run_exit(run, vcpu);
	kvm_v2_state_trace(KVM_V2_STATE_TRACE_RUN_EXIT, cpu,
			   vcpu->run_exit_reason,
			   vcpu->run_exit_reason == KVM_EXIT_IO ?
				   run->io.port : 0,
			   vcpu->run_regs.rip, vcpu->run_regs.rsp,
			   vcpu->run_regs.rax);

	/*
	 * Drain UML's deferred-signal queue. Signals are blocked at the
	 * host-thread level during KVM_RUN except for SIGALRM, and the timer
	 * handler queues work through UML's irqflags machinery.
	 */
	unblock_signals();
	trace_um_backend_kvm_v2_vcpu_exit(cpu, vcpu->run_exit_reason);
	return rc;
}

static bool kvm_v2_finish_failed_run(struct uml_pt_regs *regs,
				     struct kvm_run *run,
				     struct kvm_v2_vcpu *vcpu,
				     int cpu, int rc)
{
	if (!kvm_v2_handle_run_failure(regs, run, vcpu, cpu, rc))
		return false;

	migrate_enable();
	interrupt_end();
	return true;
}

static void kvm_v2_marshal_successful_exit(struct uml_pt_regs *regs,
					   struct kvm_v2_vcpu *vcpu)
{
	/*
	 * KVM populated kvm_run->s.regs.{regs,sregs} because kvm_valid_regs was
	 * set at vcpu_create. Marshal back from the snapshot taken before
	 * unblock_signals(), not from the live mmap that another task may have
	 * reused.
	 */
	kvm_v2_marshal_from_kvm_regs(regs, &vcpu->run_regs);
	kvm_v2_marshal_sregs_back(regs, &vcpu->run_sregs);
}

static void kvm_v2_drain_deferred_frees(void)
{
	/*
	 * Pages queued during a prior dispatch have now seen at least one
	 * CR4.PGE-toggled KVM_RUN entry, so guest TLBs no longer cache
	 * translations to those PFNs. Pages queued by exit handling wait for the
	 * next dispatch.
	 */
	if (current->mm)
		um_mmu_gather_drain(current->mm);
}

/*
 * KVM_RUN dispatcher: task-to-vCPU dispatch helper that mirrors
 * seccomp_vcpu_run's one-round-trip shape:
 *
 *   1. Pick the per-host-CPU vCPU and pin migration so the pick stays
 *      valid across KVM_RUN.
 *   2. Load the user-mode CPU state for current:
 *      CR3 = __pa(active_mm->pgd), fs.base / gs.base from
 *      regs->gp[HOST_FS_BASE / HOST_GS_BASE].
 *   3. Marshal regs->gp[] to kvm_regs.
 *   4. KVM_RUN.
 *   5. Marshal kvm_regs back to regs->gp[].
 *   6. Dispatch on kvm_run->exit_reason.
 *
 * The seccomp fallback preserves the dispatch contract if this path is
 * reached before the KVM pool is available.
 */
void kvm_v2_vcpu_run(struct uml_pt_regs *regs)
{
	struct kvm_v2_vcpu *vcpu;
	struct kvm_run *run;
	u32 exit_reason;
	int cpu;
	int rc;

	/*
	 * Pin the task to its current host CPU while it owns a pool vCPU.
	 * Voluntary scheduling is still allowed, but when execution resumes
	 * cpu, vcpu, and run still refer to the same pool member.
	 */
	migrate_disable();

	if (!kvm_v2_select_vcpu_or_fallback(regs, &vcpu, &run, &cpu))
		return;

	rc = kvm_v2_enter_and_snapshot(regs, vcpu, cpu);
	exit_reason = vcpu->run_exit_reason;
	if (kvm_v2_finish_failed_run(regs, run, vcpu, cpu, rc))
		return;

	kvm_v2_marshal_successful_exit(regs, vcpu);
	kvm_v2_drain_deferred_frees();
	kvm_v2_dispatch_exit_reason(regs, &run, &vcpu, &cpu, exit_reason);
	migrate_enable();
}

/*
 * Fork-time FPU capture. arch_copy_thread() calls this on every task fork
 * once CONFIG_UM_BACKEND_KVM_V2=y. Snapshot the parent's per-host-CPU vCPU
 * FPU state into to->kvm_v2.fpu so the child's first KVM_RUN restores
 * it via kvm_v2_fpu_install_on_first_run; POSIX fork() requires FPU
 * inheritance.
 *
 * The current vCPU is whichever pool entry was running when fork fires,
 * so migration is disabled around smp_processor_id() and
 * kvm_v2_vcpu_get() to keep the pool pick coherent with the queried fd.
 *
 * If the pool is not up yet, or the per-CPU slot is at the
 * .vcpu_fd = -1 sentinel, leave fpu_valid=false and return 0. The child
 * will get architectural reset values on first run.
 */
int kvm_v2_fpu_capture_for_fork(struct arch_thread *from,
				struct arch_thread *to)
{
	struct kvm_v2_vcpu *vcpu;
	int cpu, rc;

	(void)from;	/* parent's snapshot lives on the per-CPU vCPU, not in from */

	/* Keep the pool vCPU selection stable across the XSAVE ioctl. */
	migrate_disable();
	cpu  = smp_processor_id();
	vcpu = kvm_v2_vcpu_get(cpu);
	if (!vcpu || vcpu->vcpu_fd < 0) {
		/* No parent vCPU to snapshot; child starts from arch defaults. */
		to->kvm_v2.fpu_valid = false;
		trace_um_backend_kvm_v2_fpu_capture(cpu, 0);
		migrate_enable();
		return 0;
	}

	/*
	 * Use KVM_GET_XSAVE, not legacy KVM_GET_FPU. Fork-side capture must
	 * include YMM upper halves so the child's first dispatch restores the
	 * full parent FPU state.
	 */
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_XSAVE,
			      (unsigned long)&to->kvm_v2.fpu);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm-v2 fpu_capture_for_fork: KVM_GET_XSAVE cpu=%d fd=%d failed (%d)\n",
				    cpu, vcpu->vcpu_fd, rc);
		to->kvm_v2.fpu_valid = false;
		trace_um_backend_kvm_v2_fpu_capture(cpu, 0);
		migrate_enable();
		return 0;
	}

	to->kvm_v2.fpu_valid = true;
	trace_um_backend_kvm_v2_fpu_capture(cpu, 1);
	migrate_enable();
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_fpu_capture_for_fork);

/*
 * Per-task FPU capture on context-switch-out. Tasks moving between UML
 * scheduler contexts need their FPU snapshot to follow them; otherwise the
 * destination pool vCPU may contain another task's FPU state. Failure
 * preserves any prior valid snapshot.
 */
void kvm_v2_fpu_capture_for_switch_out(struct task_struct *from)
{
	struct kvm_v2_vcpu *vcpu;
	int cpu, rc;

	if (!from)
		return;

	/* Keep the pool vCPU selection stable across the XSAVE ioctl. */
	migrate_disable();
	cpu  = smp_processor_id();
	vcpu = kvm_v2_vcpu_get(cpu);
	if (!vcpu || vcpu->vcpu_fd < 0) {
		/* Pool unavailable; preserve any prior valid snapshot. */
		migrate_enable();
		return;
	}

	/*
	 * Capture only when this pool vCPU's FPU state actually belongs to
	 * from. A freshly forked but never dispatched task may already carry
	 * a valid parent snapshot; do not overwrite it with another task's
	 * current pool-vCPU state.
	 */
	if (vcpu->last_task != from) {
		/* Pool vCPU's FPU does not reflect from; preserve snapshot. */
		migrate_enable();
		return;
	}

	/*
	 * Use KVM_GET_XSAVE, not legacy KVM_GET_FPU. Switch-out capture must
	 * include YMM upper halves to preserve full AVX state.
	 */
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_XSAVE,
			      (unsigned long)&from->thread.arch.kvm_v2.fpu);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm-v2 fpu_capture_for_switch_out: KVM_GET_XSAVE cpu=%d fd=%d failed (%d)\n",
				    cpu, vcpu->vcpu_fd, rc);
		/* Preserve any prior valid snapshot. */
		trace_um_backend_kvm_v2_fpu_capture(cpu, 0);
		migrate_enable();
		return;
	}

	from->thread.arch.kvm_v2.fpu_valid = true;
	trace_um_backend_kvm_v2_fpu_capture(cpu, 1);
	migrate_enable();
}
EXPORT_SYMBOL_GPL(kvm_v2_fpu_capture_for_switch_out);

/*
 * v2 context_switch op wraps seccomp's. Capture the outgoing task's FPU
 * into arch_thread before delegating to seccomp, which handles the
 * process-switch mechanics.
 */
void kvm_v2_context_switch(struct task_struct *from, struct task_struct *to)
{
	kvm_v2_fpu_capture_for_switch_out(from);

	/*
	 * Drain from->active_mm before delegating to seccomp so any
	 * deferred TLB syncs from from's last user-mode session land
	 * in the spawner mm now (visible to KVM's mmu_notifier) rather
	 * than carrying over to to's first dispatch on the same vCPU.
	 */
	if (from && from->active_mm) {
		int sync_rc = um_tlb_sync(from->active_mm);

		if (sync_rc < 0)
			panic("um: kvm-v2 context_switch: um_tlb_sync(from=%p active_mm=%p) failed (%d)",
			      from, from->active_mm, sync_rc);
	}

	seccomp_context_switch(from, to);
}
EXPORT_SYMBOL_GPL(kvm_v2_context_switch);

/*
 * Pre-KVM_RUN FPU install. Called from kvm_v2_vcpu_run between SREGS load
 * and the KVM_RUN ioctl. Two cases:
 *
 *   - fpu_valid=true: capture_for_fork or switch-out capture populated
 *     current's snapshot. KVM_SET_XSAVE it into the pool vCPU and clear
 *     fpu_valid as a one-shot.
 *
 *   - fpu_valid=false: fresh task or post-execve via arch_flush_thread.
 *     Install architectural reset values per AMD64 SDM section 11.5.1
 *     (fcw=0x037f, mxcsr=0x1f80; everything else zero). Built as a
 *     dynamic stack init rather than a static const so the struct's
 *     trailing FXSAVE area lands deterministically zeroed without
 *     pulling a 512 B rodata blob into kernel text.
 *
 * Returns 0 on success, -errno on KVM_SET_FPU failure. Dispatcher
 * panics on negative return: an FPU install failure means the guest would
 * run with arbitrary FPU state.
 */
static int kvm_v2_fpu_install_on_first_run(struct kvm_v2_vcpu *vcpu)
{
	struct arch_thread *a = &current->thread.arch;
	struct kvm_xsave init_fpu;
	int rc, was_valid;

	if (a->kvm_v2.fpu_valid) {
		/*
		 * Use KVM_SET_XSAVE, not KVM_SET_FPU. Restoring only the
		 * legacy FXSAVE area would leave YMM upper halves in whatever
		 * state the pool vCPU last held.
		 */
		rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_XSAVE,
				      (unsigned long)&a->kvm_v2.fpu);
		if (rc < 0)
			return rc;
		a->kvm_v2.fpu_valid = false;	/* one-shot */
		was_valid = 1;
		/*
		 * The vCPU guest FPU now matches the installed snapshot, but
		 * the per-task iotrap_fpu slot is not authoritative until the
		 * next post-vmexit GET. Force that GET and update ownership.
		 */
		vcpu->fpu_dirty       = true;
		vcpu->fpu_owner_task  = current;
	} else {
		/*
		 * No pending snapshot: leave the pool vCPU's FPU untouched.
		 * Re-installing architectural reset values on every dispatch
		 * would destroy live FPU state after faults or signals.
		 */
		(void)init_fpu;
		was_valid = 0;
	}

	trace_um_backend_kvm_v2_fpu_install(vcpu->cpu, was_valid);
	return 0;
}
