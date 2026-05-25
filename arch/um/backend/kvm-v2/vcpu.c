// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase C.1: per-host-CPU vCPU pool + CPUID install.
 *
 * Per memo 26 §C.1. Creates `nr_cpu_ids` vCPUs at init time, each with
 * its own kvm_run mmap and the curated v1 CPUID installed. C.2 will
 * pin a pthread per vCPU and route task→vCPU dispatch by host CPU
 * index; C.1 only lands the fds + mmaps + CPUID so the dispatcher has
 * a stable per-CPU target to plug into. Until C.2 activates KVM_RUN,
 * none of these vCPUs run — ops.c still delegates `.vcpu_run` to the
 * seccomp backend and the worker-process loop drives guest execution
 * via SIGSYS as it has since R4.
 *
 * Why CPUID is installed here (not in context.c's vm_create):
 *   KVM_SET_CPUID2 is a vCPU ioctl — it requires the fd from
 *   KVM_CREATE_VCPU. Even KVM_GET_SUPPORTED_CPUID, while a KVM-fd
 *   ioctl, needs a kzalloc buffer; kvm_v2_init runs from init_backend()
 *   during linux_main(), BEFORE mm_init() brings the buddy allocator
 *   up. v1 archive's lifecycle.c documented the same constraint and
 *   deferred CPUID install entirely; v2 piggybacks on C.1 because each
 *   pool member already needs to touch its vCPU fd, and re-issuing the
 *   curated mask per vCPU is required (KVM_SET_CPUID2 is per-vCPU).
 *
 * Why the curated mask is verbatim from v1:
 *   v1 spent multiple iterations narrowing the feature surface
 *   (RDRAND/RDSEED for record-replay determinism; XSAVE/AVX/AVX2/AVX512
 *   family because v1's sregs setup never enabled CR4.OSXSAVE / set
 *   XCR0; FSGSBASE because CR4.FSGSBASE was off). v2's sregs setup is
 *   not yet written — it lives in Phase C.2/C.3. Until then matching
 *   v1's mask exactly avoids re-discovering the same #UD/#GP cliffs
 *   glibc and libcrypto fall off when those bits are advertised but
 *   not actually backed by host-side CR4/XCR0 setup. Phase C may
 *   revisit when proper XSAVE/XCR0 plumbing lands.
 */

#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/jump_label.h>	/* static_branch_unlikely — record/replay gate */
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/signal.h>	/* sigset_t / sigfillset / sigdelset / SIGALRM
				 * (D.5-fix-2 install_signal_mask) */
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/threads.h>
#include <linux/types.h>

#include <asm/msr-index.h>	/* MSR_LSTAR / MSR_STAR / MSR_SYSCALL_MASK,
				 * EFER_SCE / EFER_LME / EFER_LMA / EFER_NX */
#include <asm/page.h>
#include <asm/processor-flags.h>	/* X86_CR0_*, X86_CR4_* (D.5-fix
					 * install_production_sregs) */

#include <asm/tlbflush.h>	/* um_tlb_sync */
#include <kern_util.h>		/* interrupt_end */
#include <os.h>
#include <skas.h>		/* current_mm_sync */
#include <sysdep/ptrace.h>
#include <asm/trace/um_backend.h>

#include "kvm_v2_backend.h"
#include "state_trace.h"
#include "syscall_trap.h"

/*
 * SMP-T56 (2026-05-04): the LSTAR-EINTR carve-out in
 * kvm_v2_vcpu_run sizes its RIP-range check against the assembled
 * gadget body — same extern symbols syscall_trap.c uses to memcpy
 * the body into the trampoline page. Source of truth:
 * arch/um/backend/kvm-v2/lstar_gadget.S.
 */
extern const u8 kvm_v2_lstar_gadget_start[];
extern const u8 kvm_v2_lstar_gadget_end[];


/*
 * Round 2 Django investigation (2026-05-17): EINTR-loop-without-progress
 * threshold. The captured pre-corruption window for the Django flake
 * shows the stuck task takes ~63 consecutive same-task EINTR_PATHs
 * before SERVER_FAIL fires; 16 is a safe early-detection point that
 * still requires ~160ms of no-progress stall (well above any benign
 * single-tick EINTR blip) before the state-trace ring auto-freezes.
 *
 * Override via `kvm_v2_eintr_loop_threshold=N` on the kernel command
 * line. 0 disables the detection entirely. See struct kvm_v2_vcpu
 * eintr_run_task / eintr_run_count for the per-vCPU state.
 */
unsigned int kvm_v2_eintr_loop_threshold = 16;

static int __init kvm_v2_set_eintr_loop_threshold(char *str)
{
	unsigned int n;

	if (!str || !*str)
		return 0;
	if (kstrtouint(str, 0, &n) == 0)
		kvm_v2_eintr_loop_threshold = n;
	return 0;
}
__setup("kvm_v2_eintr_loop_threshold=", kvm_v2_set_eintr_loop_threshold);

/*
 * The pool. Sized at compile time to NR_CPUS — bounded (UML's
 * NR_CPUS_RANGE_END is 64, NR_CPUS_DEFAULT=1 without SMP) and bss-
 * resident, which sidesteps the buddy-allocator-not-up constraint
 * that init_backend() runs under (see kvm_v2_install_cpuid header
 * comment for the same rationale applied to the CPUID buffer).
 *
 * Only the first `nr_cpu_ids` slots are populated; the trailing
 * NR_CPUS - nr_cpu_ids slots stay at the .vcpu_fd = -1 sentinel below
 * so kvm_v2_vcpu_get() returns NULL for out-of-range queries.
 *
 * The `pool_initialised` flag is the "has anyone populated this yet"
 * predicate — we can't use vcpus[0].vcpu_fd == -1 as the predicate
 * because the bss-zeroed default puts the array at vcpu_fd = 0
 * (a valid fd), and rewriting bss every call would be wrong on
 * re-entry. The flag flips once when kvm_v2_vcpu_create runs the
 * first-time reset, and stays true even after destroy (destroy leaves
 * the slots at vcpu_fd = -1, the proper sentinel).
 *
 * Phase C.2 will add per-vCPU pthread + atomic_t state fields here;
 * C.1 deliberately stops at the fd + mmap + cpu-index trio because the
 * vCPU isn't actually run yet (ops.c still routes `.vcpu_run` through
 * seccomp) — adding pthread machinery before the dispatcher exists
 * would be premature per memo 27 Part B.8.
 */
static struct kvm_v2_vcpu vcpus[NR_CPUS];
static bool pool_initialised;

static void kvm_v2_vcpu_pool_reset(void)
{
	int i;

	for (i = 0; i < NR_CPUS; i++) {
		vcpus[i].vcpu_fd      = -1;
		vcpus[i].kvm_run      = NULL;
		vcpus[i].kvm_run_size = 0;
		vcpus[i].cpu          = -1;
	}
	pool_initialised = true;
}

/*
 * Match v1 archive's lifecycle.c kvm_ensure_cpuid_done — KVM_MAX_CPUID_ENTRIES
 * upstream is 256 and that's the cap v1 used. Buffer size is
 * sizeof(struct kvm_cpuid2) + max_entries * sizeof(struct kvm_cpuid_entry2).
 */
#define KVM_V2_CPUID_MAX_ENTRIES	256

/*
 * Lift the curated mask from kvm-v1-archive/lifecycle.c (kvm_ensure_cpuid_done,
 * lines 411-547). The bits below mirror v1 exactly — see that function's
 * comments for the per-bit rationale (record/replay determinism for RDRAND/
 * RDSEED; XSAVE/AVX/AVX2/AVX512 family because guest CR4.OSXSAVE is unset
 * and XCR0 is unprogrammed; FSGSBASE because CR4.FSGSBASE is off).
 *
 * Memo 26 §A.3 lists a slightly different EDX bit set (2/3/23) but
 * memo 26 itself instructs "if the archive has a function/constant for
 * this mask, lift it verbatim — better to match v1 exactly than to risk
 * a divergent mask." v1's actual EDX mask is bits 2 (AVX512_4VNNIW),
 * 3 (AVX512_4FMAPS), 8 (AVX512_VP2INTERSECT). Lift v1's choice.
 */
static void kvm_v2_curate_cpuid(struct kvm_cpuid2 *cpuid)
{
	unsigned int i;

	for (i = 0; i < cpuid->nent; i++) {
		struct kvm_cpuid_entry2 *e = &cpuid->entries[i];

		if (e->function == 1 && e->index == 0) {
			/*
			 * Leaf 1 ECX:
			 *   bit 12 = FMA, 26 = XSAVE, 27 = OSXSAVE,
			 *   28 = AVX, 29 = F16C, 30 = RDRAND.
			 *
			 * SMP-T57 (memo state-audit/25 §3.1 A.3): un-mask
			 * FMA/XSAVE/OSXSAVE/AVX/F16C — needed so glibc's
			 * IFUNC dispatch / __builtin_cpu_supports see AVX
			 * as available (it actually IS, since CR4.OSXSAVE
			 * is now set and XCR0 has YMM enabled). Keep
			 * RDRAND (bit 30) masked for record-replay
			 * determinism.
			 */
			e->ecx &= ~(1U << 30);
		}
		if (e->function == 7 && e->index == 0) {
			/*
			 * Leaf 7.0 EBX:
			 *   bit 0  = FSGSBASE, 5  = AVX2,
			 *   bit 16 = AVX512F,  17 = AVX512DQ,
			 *   bit 18 = RDSEED,   21 = AVX512IFMA,
			 *   bit 26 = AVX512PF, 27 = AVX512ER,
			 *   bit 28 = AVX512CD, 29 = SHA,
			 *   bit 30 = AVX512BW, 31 = AVX512VL.
			 *
			 * SMP-T57 (memo state-audit/25 §3.1 A.3): un-mask
			 * AVX2 (bit 5) — same rationale as Leaf 1 ECX
			 * AVX. Keep FSGSBASE (0) — separate workstream
			 * (CR4.FSGSBASE not yet enabled). Keep RDSEED (18)
			 * — replay determinism. Keep AVX-512 family
			 * (16/17/21/26-31) — Phase B candidate, gated on
			 * higher XCR0 bits.
			 */
			e->ebx &= ~((1U << 0)  | (1U << 16) |
				    (1U << 17) | (1U << 18) | (1U << 21) |
				    (1U << 26) | (1U << 27) | (1U << 28) |
				    (1U << 29) | (1U << 30) | (1U << 31));
			/*
			 * Leaf 7.0 ECX:
			 *   bit 1  = AVX512VBMI, 6  = AVX512VBMI2,
			 *   bit 8  = GFNI,       9  = VAES,
			 *   bit 10 = VPCLMULQDQ, 11 = AVX512VNNI,
			 *   bit 12 = AVX512BITALG, 14 = AVX512VPOPCNTDQ.
			 *
			 * All AVX-512 + GFNI/VAES/VPCLMULQDQ stay masked
			 * (Phase B candidates).
			 */
			e->ecx &= ~((1U << 1)  | (1U << 6)  | (1U << 8)  |
				    (1U << 9)  | (1U << 10) | (1U << 11) |
				    (1U << 12) | (1U << 14));
			/*
			 * Leaf 7.0 EDX (matching v1 archive):
			 *   bit 2 = AVX512_4VNNIW, 3 = AVX512_4FMAPS,
			 *   bit 8 = AVX512_VP2INTERSECT.
			 */
			e->edx &= ~((1U << 2) | (1U << 3) | (1U << 8));
		}
		/*
		 * Leaf 0xD is the XSAVE state-component descriptor.
		 *
		 * SMP-T57 (memo state-audit/25 §3.1 A.3, take 3):
		 * KVM_SET_XCRS validates the requested XCR0 against
		 * `vcpu->arch.guest_supported_xcr0`, which is computed
		 * from leaf 0xD sub-leaf 0 EAX (XCR0-supported mask) AND
		 * the OSXSAVE/AVX bits in leaf 1 ECX. Zeroing leaf 0xD
		 * makes that supported mask 0 → SET_XCRS rejects bits
		 * 0/1/2 with -EINVAL.
		 *
		 * Letting KVM-supplied leaf 0xD pass through means KVM's
		 * GET_SUPPORTED_CPUID returns the host's full XSAVE
		 * advertisement (AVX-512, AMX, etc.). The Phase A only-
		 * un-mask (leaf 1 ECX 26/27/28; leaf 7.0 EBX 5) keeps the
		 * higher tiers OFF in the feature-bit ABI even though
		 * leaf 0xD reports their state-component sizes — KVM's
		 * `guest_supported_xcr0` AND'd with the leaf 1 ECX bits
		 * gives the active mask. Phase B (full AVX-512/AMX) will
		 * un-mask those leaf-1/leaf-7 bits and bump XCR0
		 * accordingly.
		 *
		 * SMP-T76 (Round 14 HW-audit Q4): keep sub-leaf 0 EAX
		 * intact for SET_XCRS to accept FP|SSE|YMM, but zero out
		 * the per-component descriptors for masked features.
		 * Userspace cpuid(1) and __builtin_cpu_supports() inside
		 * the guest read leaf 0xD sub-leaves k=5,6,7 (AVX-512
		 * OPMASK / ZMM_Hi256 / Hi16_ZMM) and k=17,18 (AMX
		 * XTILECFG / XTILEDATA) for state-component size / offset.
		 * If we let the host's advertisements through, the guest
		 * sees ~8 KB XTILEDATA on Intel SPR/EMR + AMX-supported
		 * sizes on the Ryzen ZenN-with-AMX hosts that will exist,
		 * while leaf 7's AMX feature bits are masked — internally
		 * inconsistent.
		 *
		 * Zeroing the descriptors makes the advertisement self-
		 * consistent with the feature-bit mask. Sub-leaves 1, 2
		 * (XSAVES + AVX YMM upper) are kept since we DO use YMM.
		 */
		if (e->function == 0xD && (e->index == 5 || e->index == 6 ||
					   e->index == 7 || e->index == 17 ||
					   e->index == 18)) {
			e->eax = 0;
			e->ebx = 0;
			e->ecx = 0;
			e->edx = 0;
		}
	}
}

/*
 * Lazy first-run CPUID install (memo 26 §D.0a).
 *
 * History: A.3 wired this at vcpu_create time but kzalloc failed
 * (buddy allocator not yet up at init_backend time) — boot log
 * "cpuid kzalloc(%zu) failed; using KVM-default CPUID". The eager
 * call was best-effort: every error path returned 0 so vcpu_create
 * succeeded with KVM-default CPUID, leaving the curated mask
 * un-applied. Phase C never wired SET_CPUID2 elsewhere, so every
 * vCPU ran (would run, once D.5 flips .vcpu_run) without v2's
 * curated suppression of XSAVE / AVX / AVX-512 / FSGSBASE / RDRAND
 * / RDSEED. Phase D depends on the curated mask being live before
 * any guest instruction executes — otherwise an unmasked AVX bit
 * lets the guest issue a VEX encoding whose state we don't
 * snapshot under Phase C.4's legacy 512 B KVM_GET/SET_FPU path.
 *
 * D.0a fix: move the install to first KVM_RUN (vcpu_run dispatcher).
 * By that point the buddy allocator is up; kzalloc succeeds; install
 * goes through. Failure is now FATAL — the dispatcher panics on
 * negative return because running guest code without the curated
 * mask is a substrate-correctness violation, not a degradation.
 *
 * v1 archive's kvm_ensure_cpuid_done (kvm-v1-archive/lifecycle.c:
 * 411-547) used the identical "lazy install at first KVM_RUN"
 * pattern for the same buddy-allocator-not-up reason.
 *
 * The vm->cpuid stash survives the install — context.c's
 * kvm_v2_vm_destroy still owns the kfree, and snapshot / introspection
 * paths (Phase H) can re-read what's installed without re-issuing
 * the GET_SUPPORTED + curate dance.
 *
 * Returns 0 on success or -errno on any failure (kzalloc, ioctl).
 */
static int kvm_v2_install_cpuid(struct kvm_v2_vm *vm, int vcpu_fd)
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
		return -ENOMEM;
	}

	cpuid->nent = KVM_V2_CPUID_MAX_ENTRIES;
	rc = os_ioctl_generic(vm->kvm_fd, KVM_GET_SUPPORTED_CPUID,
			      (unsigned long)cpuid);
	if (rc < 0) {
		pr_err("um: kvm-v2 cpuid_install: KVM_GET_SUPPORTED_CPUID failed (%d)\n",
		       rc);
		kfree(cpuid);
		return rc;
	}

	kvm_v2_curate_cpuid(cpuid);

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_CPUID2, (unsigned long)cpuid);
	if (rc < 0) {
		pr_err("um: kvm-v2 cpuid_install: KVM_SET_CPUID2 failed (%d)\n",
		       rc);
		kfree(cpuid);
		return rc;
	}

	/*
	 * Hand the buffer to the VM context — kvm_v2_vm_destroy() owns
	 * the kfree (see context.c). The stash is per-VM (not per-vCPU)
	 * because the curated mask is identical across pool members; the
	 * second + later vCPUs to lazy-install will overwrite vm->cpuid
	 * with an identical pointer-and-contents pair. Leaking the prior
	 * buffer on overwrite is acceptable because (a) it's bounded by
	 * nr_cpu_ids and (b) D.5 lands the .vcpu_run pointer flip after
	 * which the bound is total: each pool member's first KVM_RUN
	 * overwrites once and the flag flips.
	 */
	vm->cpuid = cpuid;
	pr_info("um: kvm-v2 cpuid_install: CPUID installed (%u entries; RDRAND/RDSEED/AVX-512/FSGSBASE masked; SMP-T57 un-masked XSAVE/OSXSAVE/AVX/AVX2/FMA/F16C)\n",
		cpuid->nent);
	trace_um_backend_kvm_v2_cpuid_install(vcpu_fd, cpuid->nent);
	return 0;
}

/*
 * D.4a: program MSR_LSTAR / MSR_STAR / MSR_SYSCALL_MASK once per pool
 * member at vcpu_create_one. vCPUs are reused across tasks under v2's
 * single-VM model, so the SYSCALL MSRs are immutable across the pool's
 * lifetime — set once at create, never re-set per-dispatch (the
 * latter would pay one ioctl-and-readback per KVM_RUN for no
 * functional gain). v1 archive's kvm_enter_guest_program_msrs at
 * kvm-v1-archive/thread.c:2020-2111 used a per-vCPU msrs_primed flag to
 * gate a similar one-shot install; v2's per-host-CPU pool means the
 * eager call here is structurally equivalent without needing the flag.
 *
 * MSR_LSTAR (0xc0000082): SYSCALL entry RIP. Programmed to
 * KVM_V2_LSTAR_GVA (= 0xffffe00000000040 = trampoline GVA + 0x40),
 * which the D.1 trampoline page exposes as the 5-byte
 * `out %al,$0xf4 ; sysretq` body. The GVA is guest-walk-reachable via
 * PML4[508] — but D.4b is the commit that installs PML4[508] into
 * swapper_pg_dir (and existing mms). Until D.4b lands LSTAR points at
 * an unreachable VA; that is *expected* and harmless — D.5 hasn't
 * flipped .vcpu_run either, so no SYSCALL fires from any production
 * path. The MSR is set in D.4a so D.4b's PML4 install + D.5's pointer
 * flip both find it already armed.
 *
 * MSR_STAR (0xc0000081): high 16 bits = SYSCALL CS|SS base (kernel
 * selectors); bits 63:48 = SYSRETQ user CS|SS base. Standard pair:
 * kernel CS=0x08, user CS=0x33 (= 0x18+16|3, with the +16 and |3
 * applied by the SYSRETQ microcode). v1 used the identical pair at
 * kvm-v1-archive/thread.c:2032 — `(0x0018ULL << 48) | (0x0008ULL << 32)`.
 *
 * MSR_SYSCALL_MASK / MSR_FMASK (0xc0000084): RFLAGS bits cleared on
 * SYSCALL entry. 0x47700 = TF | IF | DF | IOPL | NT | AC. Matches Linux
 * native syscall_init's mask. Critical: DF=1 leak in masked rflags
 * caused real corruption in v1 — see the post-mortem block at
 * kvm-v1-archive/thread.c:2042-2065. Userland with DF=1 (REP MOVSB
 * backwards) leaked into LSTAR; kernel-side memcpy/memmove ran with
 * STD instead of CLD and silently corrupted whatever the kernel wrote.
 * FMASK MUST clear DF or downstream string ops execute in the wrong
 * direction.
 *
 * MSR_KERNEL_GS_BASE (0xc0000102): Phase H gadget Phase 2 (2026-05-04).
 * Programmed per-vCPU to KVM_V2_GADGET_STATE_GVA(vcpu->cpu) — the GVA
 * each vCPU's gadget state page is mapped at via PTE[
 * KVM_V2_GADGET_BASE_SLOT + cpu]. The page itself is allocated +
 * mapped later by kvm_v2_install_per_vcpu_gadget_state() at the
 * subsys_initcall lazy-install path; vcpu_create_one runs first and
 * programs the MSR with the deterministic GVA so by the time the
 * first SYSCALL fires (after subsys_initcall completes) the page IS
 * mapped behind that GVA.
 *
 * Per-vCPU isolation eliminates the writer race that forced Phase 1
 * to be UP-only: each vCPU's gadget reads from its own state page,
 * and load_user_sregs writes only to the running vCPU's page (the
 * vCPU pool's host CPU pthread is the sole writer).
 *
 * After KVM_SET_MSRS, immediately KVM_GET_MSRS and verify each value
 * round-tripped exactly. Boot-time self-check, mirrors D.1's
 * trampoline-bytes readback (commit 7e4da1dbe651). Panic on mismatch:
 * a silent KVM_SET_MSRS regression would mean SYSCALL goes to a wrong
 * RIP / wrong selectors / leaks bad RFLAGS — three of the worst
 * possible failure modes once D.5 flips .vcpu_run. One readback at
 * boot costs nothing and catches: KVM ABI shifts that silently drop a
 * write, partial-success returns the loop didn't catch, etc.
 */
static int kvm_v2_vcpu_program_msrs(int vcpu_fd, int cpu)
{
	struct {
		struct kvm_msrs hdr;
		struct kvm_msr_entry entries[4];
	} req = {
		.hdr = { .nmsrs = 4 },
		.entries = {
			{ .index = MSR_LSTAR, .data = KVM_V2_LSTAR_GVA },
			{ .index = MSR_STAR,
			  .data  = ((0x0018ULL) << 48) | ((0x0008ULL) << 32) },
			{ .index = MSR_SYSCALL_MASK, .data = 0x47700ULL },
			{ .index = MSR_KERNEL_GS_BASE,
			  .data  = KVM_V2_GADGET_STATE_GVA(cpu) },
		},
	};
	struct {
		struct kvm_msrs hdr;
		struct kvm_msr_entry entries[4];
	} readback = {
		.hdr = { .nmsrs = 4 },
		.entries = {
			{ .index = MSR_LSTAR },
			{ .index = MSR_STAR },
			{ .index = MSR_SYSCALL_MASK },
			{ .index = MSR_KERNEL_GS_BASE },
		},
	};
	int rc;
	int i;

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS, (unsigned long)&req);
	if (rc < 0) {
		pr_err("um: kvm-v2 program_msrs: KVM_SET_MSRS(vcpu_fd=%d) failed (%d)\n",
		       vcpu_fd, rc);
		return rc;
	}
	if (rc != 4) {
		/*
		 * KVM_SET_MSRS returns the count of MSRs successfully
		 * written; partial success means one of LSTAR/STAR/FMASK/
		 * KERNEL_GS_BASE was rejected and the corresponding
		 * trampoline / SYSRETQ / RFLAGS-mask / gadget GS_BASE is
		 * not armed. Treat as fatal at the caller.
		 */
		pr_err("um: kvm-v2 program_msrs: KVM_SET_MSRS wrote %d/4 MSRs (vcpu_fd=%d)\n",
		       rc, vcpu_fd);
		return -EIO;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_MSRS, (unsigned long)&readback);
	if (rc < 0 || rc != 4) {
		pr_err("um: kvm-v2 program_msrs: KVM_GET_MSRS readback failed (%d) (vcpu_fd=%d)\n",
		       rc, vcpu_fd);
		return rc < 0 ? rc : -EIO;
	}

	for (i = 0; i < 4; i++) {
		if (readback.entries[i].data != req.entries[i].data) {
			panic("kvm-v2: MSR readback MISMATCH idx=%#x: wrote %#llx got %#llx",
			      req.entries[i].index,
			      (unsigned long long)req.entries[i].data,
			      (unsigned long long)readback.entries[i].data);
		}
	}

	pr_info("um: kvm-v2 program_msrs: vcpu_fd=%d cpu=%d LSTAR=%#llx STAR=%#llx FMASK=%#llx KERNEL_GS_BASE=%#llx\n",
		vcpu_fd, cpu,
		(unsigned long long)req.entries[0].data,
		(unsigned long long)req.entries[1].data,
		(unsigned long long)req.entries[2].data,
		(unsigned long long)req.entries[3].data);
	trace_um_backend_kvm_v2_msr_program(vcpu_fd);
	return 0;
}

/*
 * D.5-fix: install full long-mode SREGS once per pool member.
 *
 * Diagnosed by opus subagent + codex --search audit (2026-04-29):
 * the original D.5 flip caused KVM_RUN to return -EINVAL with
 * exit_reason=0 because the sync-regs mmap (kvm_run->s.regs.sregs)
 * is zero-initialised on first dispatch. KVM only populates that
 * mmap via store_regs() AFTER an exit (`arch/x86/kvm/x86.c:12748-
 * 12761`), so on first KVM_RUN no exit has happened yet and CS / DS
 * / SS / TR / LDT / CR0 / CR4 all read as zero.
 *
 * load_user_sregs's KVM_SYNC_X86_SREGS dirty bit ships those zeros
 * back through __set_sregs, and `kvm_is_valid_sregs` at
 * `arch/x86/kvm/x86.c:12426-12449` rejects: with EFER.LMA=1 (we set
 * it) and CR0.PG=0 (we never set it), the validator's "Not in
 * 64-bit mode" branch fires and returns false — propagated as
 * -EINVAL up through `sync_regs` → `kvm_arch_vcpu_ioctl_run`. Note
 * `exit_reason` stays at its mmap-default 0 (KVM_EXIT_UNKNOWN)
 * because guest entry never happened.
 *
 * Mirrors v1's KVM_GET_SREGS → overlay → KVM_SET_SREGS pattern at
 * kvm-v1-archive/thread.c:2843-2913 and kvm-v1-archive/sregs.c:
 * 219-304. GET first so KVM-default TR / LDT / APIC stay (they
 * went through KVM's reset path which is valid for VMX entry under
 * unrestricted-guest mode + KVM_SET_TSS_ADDR's already-set
 * trampoline TSS); overlay long-mode CS (L=1, ring-0 selector
 * 0x08, base=0, limit=0xffffffff, type=0xb=ER+A) + DS/SS/ES/FS/GS
 * (32-bit data, ring-0 0x10, base=0, limit=0xffffffff, type=0x3=
 * RW+A) + CR0 (PE|MP|NE|WP|PG) + CR4 (PAE|OSFXSR|OSXMMEXCPT) +
 * EFER (SCE|LME|LMA|NX); SET back via real ioctl.
 *
 * Critical: also seed the sync-regs mmap with the same struct so
 * the FIRST dispatch's KVM_SYNC_X86_SREGS write doesn't ship zero
 * CR0/segments back through __set_sregs. KVM normally populates
 * the mmap on exit via store_regs; we hand-seed it here to bridge
 * the gap before the first exit happens. After the first exit,
 * store_regs naturally refreshes the mmap, and the per-dispatch
 * load_user_sregs sync-regs path becomes safe.
 *
 * Notes for future phases:
 *  - GDT/IDT base remain zero. v2's trampoline (out %al,$0xf4 ;
 *    sysretq) doesn't touch the GDT or fault into the IDT. Phase E
 *    must install a real GDT + IDT before #PF/#GP/#UD vectoring
 *    lands.
 *  - TR/LDT inherit KVM-reset defaults via KVM_GET_SREGS. KVM_SET_
 *    TSS_ADDR (already issued at vm_create) suppresses VMX's
 *    null-TR rejection under unrestricted-guest mode.
 *  - The per-dispatch EFER overwrite in kvm_v2_load_user_sregs
 *    becomes redundant with the create-time install. Harmless;
 *    Phase H can hoist it.
 */
static int kvm_v2_install_production_sregs(struct kvm_v2_vcpu *v)
{
	struct kvm_run *run;
	struct kvm_sregs sregs;
	/*
	 * E.4 (post-E.5 fork+wait validation, 2026-04-29): boot at CPL=3
	 * directly. Earlier shape used kernel CS/SS (selectors 0x08/0x10,
	 * DPL=0) and relied on the first SYSCALL→sysretq round-trip to
	 * drop the guest to CPL=3. That left a window where guest user
	 * code runs at CPL=0 — fine for trivial init binaries that
	 * SYSCALL immediately (/bin/true, /bin/echo) but catastrophic for
	 * fork's child that runs glibc post-fork bookkeeping (locks,
	 * atfork handlers, TLS reset) before any syscall: glibc's abort
	 * path at `abort+0xa9` issues `hlt` (0xf4), and `hlt` at CPL=0
	 * exits with KVM_EXIT_HLT instead of #GP'ing as it would at
	 * CPL=3. Substrate gate broke the same way for /bin/sh-spawned
	 * subprocesses.
	 *
	 * Fix: select user CS (sel 0x2b, DPL=3) and user SS (sel 0x23,
	 * DPL=3) so the guest enters at CPL=3 from the very first
	 * KVM_RUN. SYSCALL transitions to CPL=0 with CS=STAR[47:32]=0x08
	 * (kernel) for the trampoline; sysretq transitions back to CPL=3
	 * with CS=STAR[63:48]+0x10|3=0x2b. IDT-gate transitions to CPL=0
	 * with CS=gate.selector=0x08 (kernel) for handler stubs; iretq
	 * pops back to CPL=3. Existing transitions are unchanged — only
	 * the *initial* CPL is corrected.
	 *
	 * v1 archive equivalent: kvm-v1-archive/sregs.c:140-160 set
	 * user_cs/user_ss in its initial SREGS for exactly this reason.
	 * Task #87 ("port v1 bootstrap IRETQ") was an alternative solution
	 * via a guest-side IRETQ stub on first entry; this SREGS path is
	 * the simpler equivalent.
	 */
	const struct kvm_segment code = {
		.base    = 0,
		.limit   = 0xffffffff,
		.selector = 0x2b,	/* USER_CS, DPL=3 (matches GDT[5]) */
		.type    = 0xb,		/* ER + A */
		.present = 1,
		.dpl     = 3,
		.db      = 0,		/* L=1 supersedes db */
		.s       = 1,		/* code/data */
		.l       = 1,		/* 64-bit */
		.g       = 1,
	};
	const struct kvm_segment data = {
		.base    = 0,
		.limit   = 0xffffffff,
		.selector = 0x23,	/* USER_DS, DPL=3 (matches GDT[4]) */
		.type    = 0x3,		/* RW + A */
		.present = 1,
		.dpl     = 3,
		.db      = 1,
		.s       = 1,
		.l       = 0,
		.g       = 1,
	};
	int rc;

	if (!v || v->vcpu_fd < 0 || !v->kvm_run)
		return -EINVAL;

	rc = os_ioctl_generic(v->vcpu_fd, KVM_GET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_err("um: kvm-v2 install_sregs: KVM_GET_SREGS(vcpu_fd=%d) failed (%d)\n",
		       v->vcpu_fd, rc);
		return rc;
	}

	sregs.cs = code;
	sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = data;

	sregs.cr0  = X86_CR0_PE | X86_CR0_MP | X86_CR0_NE |
		     X86_CR0_WP | X86_CR0_PG;
	/*
	 * SMP-T57 (memo state-audit/25 §3.1 A.1): enable CR4.OSXSAVE so
	 * the guest can use VEX/AVX-encoded instructions (memo
	 * state-audit/24 §7.6-H found vpxor #UD-ing on Zen 4 with this
	 * bit unset). Pairs with KVM_SET_XCRS at vcpu_create_one and the
	 * AVX/XSAVE/OSXSAVE un-mask in kvm_v2_curate_cpuid.
	 */
	/*
	 * NOTE: OSXSAVE is NOT set here — KVM rejects CR4.OSXSAVE before
	 * CPUID is installed (cr4_guest_rsvd_bits treats it as reserved).
	 * The first-dispatch lazy CPUID install in vcpu_run adds OSXSAVE
	 * via SYNC_REGS once the curated CPUID is live, paired with
	 * kvm_v2_install_xcrs there. See SMP-T57 memo state-audit/25
	 * §3.1 A.1+A.2 "deferred to first KVM_RUN" and the cpuid_primed
	 * block in vcpu_run.
	 */
	sregs.cr4  = X86_CR4_PAE | X86_CR4_OSFXSR | X86_CR4_OSXMMEXCPT;
	sregs.efer = EFER_SCE | EFER_LME | EFER_LMA | EFER_NX;
	/*
	 * CR3 is set per-dispatch via load_user_sregs's sync-regs
	 * write. Leave whatever GET returned (typically 0 on a fresh
	 * vCPU). The first dispatch's load_user_sregs writes the real
	 * cr3 + dirties KVM_SYNC_X86_SREGS — validation passes because
	 * by then CR0.PG is already 1 from this install.
	 */

	rc = os_ioctl_generic(v->vcpu_fd, KVM_SET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_err("um: kvm-v2 install_sregs: KVM_SET_SREGS(vcpu_fd=%d) failed (%d)\n",
		       v->vcpu_fd, rc);
		return rc;
	}

	/*
	 * Seed the sync-regs mmap so the first dispatch's
	 * load_user_sregs's KVM_SYNC_X86_SREGS write doesn't ship zero
	 * CR0/CR4/segments back to __set_sregs. KVM populates this on
	 * exit via store_regs (`arch/x86/kvm/x86.c:12748-12761`); we
	 * hand-seed before the first entry to bridge that gap.
	 */
	run = v->kvm_run;
	run->s.regs.sregs = sregs;

	pr_info("um: kvm-v2 install_sregs: vcpu_fd=%d cr0=%#llx cr4=%#llx efer=%#llx cs.l=%u\n",
		v->vcpu_fd,
		(unsigned long long)sregs.cr0,
		(unsigned long long)sregs.cr4,
		(unsigned long long)sregs.efer,
		sregs.cs.l);
	trace_um_backend_kvm_v2_sregs_install(v->vcpu_fd);
	return 0;
}

/*
 * Phase E.1 (memo 26 §E.1): install IDT/GDT bases in SREGS for one
 * pool member.
 *
 * Why this is a separate helper from kvm_v2_install_production_sregs
 * (which ran at vcpu_create_one): production_sregs ran BEFORE E.1's
 * pages existed, so its sregs.idt / sregs.gdt fields stayed at the
 * KVM_GET_SREGS defaults (zero base, zero limit). E.1's install
 * helper iterates the pool and calls this updater after the pages
 * land. Codex --search audit finding #5 from the §E.1 spec calls
 * out this gap explicitly.
 *
 * Option B from the spec (one-helper-per-concern) — keeps
 * kvm_v2_install_production_sregs untouched (it sets CS/DS/SS/CR0/CR4
 * /EFER once at vcpu_create) and adds this descriptor-table updater
 * as the late-install seam. Cleaner separation than gating idt/gdt
 * writes inside production_sregs on a `vm->idt_kva != NULL` predicate
 * (Option A would have required the install_production_sregs caller
 * to know whether E.1 had run yet).
 *
 * SREGS round-trip via real ioctl (not sync-regs): the install needs
 * to land in KVM's vcpu state authoritatively, which the sync-regs
 * path doesn't guarantee until the next KVM_RUN exit-path store_regs
 * runs. KVM_SET_SREGS commits the descriptor-table fields immediately
 * to the vcpu's vmcs/vmcb. The mmap is then re-seeded so the next
 * KVM_RUN's load_user_sregs's KVM_SYNC_X86_SREGS write doesn't ship
 * stale (zero) idt/gdt back through __set_sregs.
 *
 * TR is intentionally left at GET_SREGS default. E.2 will issue a
 * separate descriptor-sregs update with TR pointing at the per-vCPU
 * TSS body once that lands. Keeping TR untouched here matches the
 * spec's Option B note ("E.2 will issue another descriptor sregs
 * update with TR").
 *
 * Phase E.2 update (memo 26 §E.2): the helper now ALSO writes
 * sregs.tr per vCPU — the per-vCPU TR cache holds the IST1 RSP
 * pointer (via the TSS body the helper's caller installed at
 * vcpu->tss_gva), which exception delivery dereferences to find
 * the stack to push the iretq frame onto. Without TR set, the CPU
 * uses the TR cache from KVM_GET_SREGS (typically zero / null
 * descriptor) and exception delivery would #GP-during-delivery
 * → cascade to triple fault.
 *
 * Caller responsibility: kvm_v2_install_per_vcpu_ist_tss must run
 * BEFORE this helper (so vcpu->tss_gva / ist_stack_top_gva are
 * populated). exception_install enforces that order — see the loop
 * at exception.c. Defensive: if vcpu->tss_gva is zero we fall back
 * to a null TR descriptor with a loud pr_err — KVM may reject the
 * SREGS, surfacing the misorder rather than silently installing a
 * broken TR.
 */
int kvm_v2_install_descriptors_sregs(struct kvm_v2_vm *vm,
				     struct kvm_v2_vcpu *vcpu)
{
	struct kvm_sregs sregs;
	int rc;

	if (!vm || !vcpu || vcpu->vcpu_fd < 0 || !vcpu->kvm_run)
		return -EINVAL;

	if (!vm->idt_kva || !vm->gdt_kva) {
		/*
		 * Defensive: caller (kvm_v2_exception_install) only invokes
		 * us after the pages are populated, so this branch is
		 * never expected to fire. Surface it loudly so a future
		 * out-of-order caller doesn't silently install zero
		 * descriptor-table bases.
		 */
		pr_err("um: kvm-v2 install_descriptors_sregs: vm->idt_kva or gdt_kva NULL — exception_install must complete first\n");
		return -EINVAL;
	}

	if (!vcpu->tss_gva || !vcpu->ist_stack_top_gva) {
		/*
		 * E.2: per-vCPU IST/TSS install must precede this helper.
		 * Surface loudly — the alternative is silently installing
		 * sregs.tr.base = 0, which would cause exception delivery
		 * to dereference NULL during the iretq frame push and
		 * triple-fault when E.3.5 flips .vcpu_run.
		 */
		pr_err("um: kvm-v2 install_descriptors_sregs: vcpu_fd=%d tss_gva or ist_stack_top_gva is zero — kvm_v2_install_per_vcpu_ist_tss must run first\n",
		       vcpu->vcpu_fd);
		return -EINVAL;
	}

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_SREGS,
			      (unsigned long)&sregs);
	if (rc < 0) {
		pr_err("um: kvm-v2 install_descriptors_sregs: KVM_GET_SREGS(vcpu_fd=%d) failed (%d)\n",
		       vcpu->vcpu_fd, rc);
		return rc;
	}

	/*
	 * GVAs (not GPAs) — the IDT/GDT bases must be guest VAs because
	 * KVM stores them as the VMCS GUEST_IDTR_BASE / GUEST_GDTR_BASE
	 * fields, which are CPU-architectural values the guest CPU
	 * dereferences via its current paging context. Memo 26 §E.1
	 * specifies "KVM_SET_SREGS.idt.base = the guest VA" explicitly.
	 *
	 * Limit: 256 vectors × 16 bytes - 1 = 4095 (full IDT). 8 GDT
	 * slots × 8 bytes - 1 = 63 (the 6 used + 2 TSS slots E.2 fills).
	 * Constants pulled from exception.c so a future per-VM resize
	 * stays consistent.
	 */
	sregs.idt.base  = (u64)KVM_V2_IDT_GVA;
	sregs.idt.limit = 256 * 16 - 1;
	sregs.gdt.base  = (u64)KVM_V2_GDT_GVA;
	sregs.gdt.limit = 8 * 8 - 1;

	/*
	 * Phase E.2: per-vCPU TR cache pointing at this vCPU's TSS body.
	 * The TSS body (built by kvm_v2_install_per_vcpu_ist_tss) holds
	 * IST1 = vcpu->ist_stack_top_gva at offset 36; exception delivery
	 * walks GUEST_TR_BASE → TSS body → IST[0] = stack top → push
	 * iretq frame. Bytes verbatim from v1 archive's pattern at
	 * kvm-v1-archive/thread.c:2889-2898 (sregs.tr = { base, limit=103,
	 * selector=0x30, type=11 = 64-bit busy TSS, present=1, dpl=0,
	 * s=0=system segment, g=0=byte granularity }).
	 *
	 *   type=11 (= 0xb): 64-bit BUSY TSS. KVM accepts both 9
	 *     (available) and 11 (busy) for KVM_SET_SREGS; v1 used 11
	 *     because that's what the CPU caches AFTER LTR runs (the
	 *     architectural behaviour: LTR loads the descriptor and
	 *     flips its type from 9→11). Setting 11 directly skips the
	 *     "we never run LTR in guest" gap — KVM treats the TR cache
	 *     as already-loaded.
	 *   s=0: system segment (TSS is one of the 16 system segment
	 *     types per Intel SDM Vol.3 §3.5; not a code/data segment).
	 *   l=0 / db=0: ignored for system segments.
	 *   g=0: byte granularity. Limit is 103 bytes — way under 1MB,
	 *     so byte-granular limits are correct.
	 */
	sregs.tr.base     = (u64)vcpu->tss_gva;
	sregs.tr.limit    = KVM_V2_TSS_LIMIT;
	sregs.tr.selector = KVM_V2_TSS_SEL;
	sregs.tr.type     = 0xb;	/* 64-bit busy TSS */
	sregs.tr.s        = 0;		/* system segment */
	sregs.tr.dpl      = 0;
	sregs.tr.present  = 1;
	sregs.tr.l        = 0;		/* ignored for system segments */
	sregs.tr.db       = 0;		/* ignored for system segments */
	sregs.tr.g        = 0;		/* byte granularity */
	sregs.tr.avl      = 0;
	sregs.tr.unusable = 0;

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_SREGS,
			      (unsigned long)&sregs);
	if (rc < 0) {
		pr_err("um: kvm-v2 install_descriptors_sregs: KVM_SET_SREGS(vcpu_fd=%d) failed (%d)\n",
		       vcpu->vcpu_fd, rc);
		return rc;
	}

	/*
	 * Seed the sync-regs mmap with the post-update sregs so the
	 * first dispatch's KVM_SYNC_X86_SREGS write doesn't ship stale
	 * (zero) idt/gdt/tr back. Same hand-seed pattern that
	 * install_production_sregs uses (vcpu.c:511-512); see that
	 * helper for the full rationale on why store_regs's exit-path
	 * refresh isn't enough on the very first dispatch.
	 */
	((struct kvm_run *)vcpu->kvm_run)->s.regs.sregs = sregs;

	pr_info("um: kvm-v2 install_descriptors_sregs: vcpu_fd=%d idt=%#llx (limit=%#x) gdt=%#llx (limit=%#x) tr.base=%#llx tr.limit=%u tr.sel=%#x tr.type=%u\n",
		vcpu->vcpu_fd,
		(unsigned long long)sregs.idt.base, sregs.idt.limit,
		(unsigned long long)sregs.gdt.base, sregs.gdt.limit,
		(unsigned long long)sregs.tr.base, sregs.tr.limit,
		sregs.tr.selector, sregs.tr.type);
	trace_um_backend_kvm_v2_descriptors_sregs_install(vcpu->vcpu_fd,
							  sregs.idt.base,
							  sregs.gdt.base);
	return 0;
}

/*
 * SMP-T57 (memo state-audit/25 §3.1 A.2): install XCR0 to enable
 * X87/SSE/YMM state-save. Pairs with CR4.OSXSAVE (set in
 * kvm_v2_install_production_sregs) and the AVX/XSAVE/OSXSAVE
 * un-mask in kvm_v2_curate_cpuid.
 *
 * Without XCR0 set, VEX-encoded AVX instructions raise #UD even
 * when CR4.OSXSAVE=1 and CPUID advertises AVX — XCR0 is the
 * architectural enable for the extended state-save area. Memo
 * state-audit/24 §7.6-H caught this empirically: stress-ng's
 * mscan IFUNC variant lands on `vpxor %xmm6,%xmm6,%xmm6` which
 * #UDs with XCR0=0 on Zen 4.
 *
 * Bits set:
 *   bit 0 (X87)  : always required
 *   bit 1 (SSE)  : XMM0..XMM15 (already implicit via OSFXSR but
 *                  the XCR0 ABI requires bit 1 set whenever bit 2
 *                  is set)
 *   bit 2 (YMM)  : upper 128 bits of YMM0..YMM15 (AVX/AVX2)
 *
 * Higher tiers (AVX-512 = bits 5/6/7, AMX = 17/18) stay clear
 * pending a follow-up that also un-masks the corresponding CPUID
 * bits and audits the marshal struct size.
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
	 * Read it back so we can confirm what KVM actually accepted —
	 * KVM may clamp the value against guest_supported_xcr0 (the
	 * AND of leaf 0xD sub-leaf 0 EAX with the leaf-1/leaf-7 ABI
	 * feature bits). A silent clamp to 0x3 (FP|SSE only) would
	 * leave VEX/AVX still faulting.
	 */
	{
		struct kvm_xcrs got = {0};
		int grc = os_ioctl_generic(vcpu_fd, KVM_GET_XCRS,
					   (unsigned long)&got);
		pr_info("um: kvm-v2 install_xcrs: vcpu_fd=%d set=0x7 get_rc=%d nr=%u xcr0=%#llx\n",
			vcpu_fd, grc, got.nr_xcrs,
			(unsigned long long)got.xcrs[0].value);
	}
	return 0;
}

/*
 * D.5-fix-2: install per-vCPU signal mask before first KVM_RUN.
 *
 * Symptom that drove this: under D.5-fix-1's tip, KVM_RUN succeeded
 * (no -EINVAL — SREGS install fixed that) but every dispatch
 * immediately returned -EINTR because UML's HZ=100 timer (SIGALRM
 * SI_TIMER) fires before the guest can make any progress, and v2
 * had not yet installed KVM_SET_SIGNAL_MASK to (a) keep SIGALRM
 * unblocked so it CAN preempt KVM_RUN — that's the desired
 * preemption — and (b) block every OTHER host signal so they don't
 * longjmp UML kernel code mid-KVM_RUN ioctl. Without (b) the queue
 * stays armed and we re-EINTR on every entry; with (b) only the
 * timer interrupts and the guest gets a full tick slice.
 *
 * Body verbatim from v1's pattern at kvm-v1-archive/thread.c:71-124.
 * v2 omits KVM_UM_KICK_SIGNAL — TDP + mmu_notifier handle cross-vCPU
 * coherence, so the SMP eviction primitive v1 reserved isn't needed
 * (memo 26 §F.1).
 */
static int kvm_v2_install_signal_mask(int vcpu_fd)
{
	struct {
		__u32 len;
		__u8  sigset[sizeof(sigset_t)];
	} __packed mask = {
		.len = sizeof(sigset_t),
	};
	sigset_t set;
	int rc;

	sigfillset(&set);
	sigdelset(&set, SIGALRM);  /* timer-driven preemption — see v1 */
#if IS_ENABLED(CONFIG_SMP)
	/*
	 * Phase G.2 cross-vCPU TLB shootdown (2026-05-01): also unblock
	 * IPI_SIGNAL during KVM_RUN so a remote vCPU's um_tlb_sync can
	 * kick this vCPU out of guest mode via os_send_ipi(cpu,
	 * UML_IPI_RES). The next dispatch's CR4.PGE toggle in
	 * load_user_sregs() does the actual local guest-TLB flush. The
	 * EINTR caused by the IPI returns through the existing
	 * SIGALRM-EINTR path at vcpu_run line ~1599; that path is
	 * already EINTR-tolerant.
	 *
	 * Original v2 design comment ("TDP + mmu_notifier handle cross-
	 * vCPU coherence") is correct for the TDP layer (host_PA →
	 * guest_PA) but NOT for the guest TLB layer (GVA → guest_PA);
	 * each vCPU's guest TLB caches independently, and our PTE
	 * updates (direct writes into physmem) do not fire mmu_notifier.
	 */
	sigdelset(&set, os_ipi_signum());
#endif
	memcpy(mask.sigset, &set, sizeof(sigset_t));

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_SIGNAL_MASK,
			      (unsigned long)&mask);
	if (rc < 0) {
		pr_err("um: kvm-v2 install_sigmask: KVM_SET_SIGNAL_MASK(vcpu_fd=%d) failed (%d)\n",
		       vcpu_fd, rc);
		return rc;
	}
	pr_info("um: kvm-v2 install_sigmask: vcpu_fd=%d (sigfillset minus SIGALRM%s)\n",
		vcpu_fd,
		IS_ENABLED(CONFIG_SMP) ? ", IPI_SIGNAL" : "");
	trace_um_backend_kvm_v2_sigmask_install(vcpu_fd);
	return 0;
}

/*
 * Phase G.2 cross-vCPU guest-TLB kick (2026-05-01). Called from
 * um_tlb_sync after a successful drain so all OTHER UML CPUs
 * dispatch ASAP and toggle CR4.PGE on their next vcpu_run, flushing
 * their guest TLBs.
 *
 * Mechanism: pthread_sigqueue(IPI_SIGNAL) via os_send_ipi to each
 * remote cpu_thread. With IPI_SIGNAL unblocked in the KVM signal
 * mask (kvm_v2_install_signal_mask above, SMP-only), the signal
 * interrupts KVM_RUN with -EINTR; the EINTR returns through the
 * existing path at kvm_v2_vcpu_run line ~1599 and the next
 * dispatch's CR4.PGE toggle does the actual local flush.
 *
 * Vector UML_IPI_RES (= 0, "reschedule") is repurposed — the
 * uml_ipi_handler's scheduler_ipi() is harmless when there's
 * nothing to schedule; we only need the EINTR side-effect.
 *
 * mm parameter is currently unused (we kick all online CPUs); a
 * future optimization could narrow to mm_cpumask once UML
 * populates it.
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
		 * G.2-fix narrowing #1: only kick vCPUs running THIS mm.
		 * vCPUs running other mms aren't affected by this mm's
		 * PTE changes. current_mm is updated in load_user_sregs,
		 * read here without lock — a stale read just means we
		 * send an unneeded IPI (bounded by the gen check + cmpxchg
		 * dedup below).
		 */
		if (READ_ONCE(v->current_mm) != mm)
			continue;
		/*
		 * G.2-fix narrowing #2: skip vCPUs already up-to-date.
		 * Read from the per-(mm, cpu) array so we compare against
		 * this cpu's last-seen gen for THIS mm specifically, not
		 * a cross-mm-contaminated per-vCPU counter.
		 */
		if (atomic64_read(&mm->context.tlb_gen_seen_by[cpu]) >=
		    cur_gen)
			continue;
		/*
		 * G.2-cont dedup: at most one IPI in flight per vCPU.
		 * Reset by the kicked vCPU at load_user_sregs.
		 */
		if (atomic_cmpxchg(&v->kick_pending, 0, 1) == 0)
			(void)os_send_ipi(cpu, 0 /* UML_IPI_RES */);
	}
#else
	(void)mm;
#endif
}

/*
 * Build a single pool member. Returns 0 on success or a negative errno
 * on failure; the caller (kvm_v2_vcpu_create) tears down already-built
 * entries on partial failure. KVM_CREATE_VCPU's id argument is the
 * vCPU id within the VM (0..max-1) — we use the host CPU index as the
 * id so KVM's internal numbering matches our pool keying.
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

	/*
	 * D.0a: CPUID install is deferred to first KVM_RUN (see
	 * kvm_v2_install_cpuid header). At vcpu_create time the buddy
	 * allocator is not up; the install would kzalloc-fail and ship
	 * a vCPU running with KVM-default (not v2-curated) CPUID. The
	 * dispatcher's lazy-install path runs from kvm_v2_vcpu_run()
	 * once D.5 flips .vcpu_run — until then the flag stays false
	 * and no install happens (no caller exercises the dispatcher).
	 */
	v->vcpu_fd      = vcpu_fd;
	v->kvm_run      = kvm_run;
	v->kvm_run_size = (u32)mmap_size;
	v->cpu          = cpu;
	v->cpuid_primed = false;

	/*
	 * SMP-T55: at vcpu create the vCPU's guest FPU is whatever
	 * KVM defaulted; not yet matched to any task's snapshot.
	 * Force the first post-vmexit GET by starting dirty.
	 */
	v->fpu_dirty       = true;
	v->fpu_owner_task  = NULL;

	/*
	 * Round 7 Branch B: per-vCPU dispatch-path counters start at zero.
	 * See struct comment for rationale.
	 */
	v->dispatch_heavy_count = 0;
	v->dispatch_cheap_count = 0;

	/*
	 * Phase C.3: enable KVM_CAP_SYNC_REGS for this vCPU. With
	 * kvm_valid_regs set at create time, every subsequent KVM_RUN
	 * populates kvm_run->s.regs.{regs,sregs} into the mmap'd struct
	 * on exit; kvm_v2_marshal_from_kvm_regs / kvm_v2_load_user_sregs
	 * reads/writes against that mmap directly, eliminating four
	 * ioctls per dispatch (KVM_GET_REGS / KVM_SET_REGS / KVM_GET_SREGS
	 * / KVM_SET_SREGS) in favor of `kvm_dirty_regs` flips.
	 *
	 * KVM_CAP_SYNC_REGS is required-cap in init.c (bit 0 of caps);
	 * this assignment is structural rather than conditional.
	 */
	((struct kvm_run *)kvm_run)->kvm_valid_regs =
		KVM_SYNC_X86_REGS | KVM_SYNC_X86_SREGS;

	/*
	 * D.4a: program SYSCALL MSRs once per pool member. Eager (not
	 * lazy-at-first-KVM_RUN like CPUID) because KVM_SET_MSRS / GET_MSRS
	 * don't allocate from the buddy allocator — the request and
	 * readback structs live on this stack frame. Treat failure as
	 * fatal: the helper itself panics on readback mismatch; SET_MSRS
	 * partial-success or ioctl failure is returned here as -errno and
	 * the caller (kvm_v2_vcpu_create) unwinds the partially-built pool.
	 *
	 * .vcpu_run is still routed through seccomp until D.5, so the MSRs
	 * we just wrote have no consumer today. Programming them at create
	 * time means D.4b's PML4[508] install + D.5's pointer flip both
	 * find LSTAR/STAR/FMASK already armed — no flag dance, no first-
	 * run install path to debug separately from the rest of the
	 * dispatcher.
	 */
	rc = kvm_v2_vcpu_program_msrs(vcpu_fd, cpu);
	if (rc < 0)
		goto err_unmap_kvm_run;

	/*
	 * D.5-fix: install full long-mode SREGS (CS/DS/SS/CR0/CR4/EFER)
	 * BEFORE first KVM_RUN. Without this the first dispatch returns
	 * -EINVAL because kvm_is_valid_sregs (arch/x86/kvm/x86.c:
	 * 12426-12449) rejects EFER.LMA=1 with CR0.PG=0 — the mmap is
	 * zero-init except for the four fields load_user_sregs writes,
	 * and store_regs only populates it after a successful exit.
	 * See helper header for full diagnosis.
	 */
	rc = kvm_v2_install_production_sregs(v);
	if (rc < 0)
		goto err_unmap_kvm_run;
	/*
	 * SMP-T57 (memo state-audit/25 §3.1 A.2 + A.3): the OSXSAVE bit
	 * in sregs.cr4 above + the KVM_SET_XCRS ioctl are BOTH gated on
	 * the curated CPUID being installed first. CPUID is lazy-installed
	 * at first KVM_RUN (kzalloc(GFP_KERNEL) fails at vcpu_create time
	 * — pre-mm_init in this codepath). We therefore defer the OSXSAVE
	 * arming and the SET_XCRS to right after the CPUID install in
	 * vcpu_run — see the `if (!vcpu->cpuid_primed)` block. The
	 * sregs.cr4 above leaves OSXSAVE clear for now; the first dispatch
	 * adds it via SYNC_REGS once CPUID is live.
	 */

	/*
	 * D.5-fix-2: install per-vCPU signal mask. Block every host
	 * signal except SIGALRM at KVM_RUN entry; v1 archive at
	 * kvm-v1-archive/thread.c:71-124 documents the rationale (SIGALRM
	 * MUST stay unblocked so the timer tick can preempt CPU-bound
	 * guests; everything else MUST be blocked so unrelated host
	 * signal handlers don't longjmp into UML kernel mid-ioctl).
	 *
	 * Without this, the .vcpu_run flip from D.5-fix-1 makes guest
	 * progress impossible: every dispatch EINTRs because the deferred-
	 * signal queue stays armed and re-fires on every entry.
	 */
	rc = kvm_v2_install_signal_mask(vcpu_fd);
	if (rc < 0)
		goto err_unmap_kvm_run;

	/*
	 * SMP-T74 (Round 14 HW-audit Q1): pin DR0-DR7 to zero at vCPU
	 * create time. The per-host-CPU vCPU pool means multiple UML
	 * tasks share one vCPU; without an explicit reset, any DR0-DR3
	 * (linear breakpoint address) or DR7 (enable mask) bits set by
	 * one task persist into the next task's dispatch.
	 *
	 * The UML guest does NOT use hardware debug registers — guest
	 * CPL=3 code can't MOV to/from DR (privileged at CPL>0), the
	 * curated CPUID does not advertise KVM_GUESTDBG_* capability,
	 * and no in-guest gdb / kernel watchpoint path is wired up.
	 * Therefore a one-time install at create is sufficient: with
	 * DR7 enables clear, no #DB is ever raised, so DR6 never gets
	 * hardware-updated either. If a future change ever needs the
	 * guest to use HW breakpoints, this becomes a per-task save/
	 * restore in the same shape as the iotrap_fpu pair (T73).
	 *
	 * Same architectural shape as the cross-task FPU leak T26/T27/
	 * T73 fixed; this preempts the leak before it can become a
	 * latent bug. See Documentation/virt/uml/redesign/08-future-
	 * phases/50-kvm-v2-django-flake-investigation-summary.md
	 * Round 14 HW-audit Q1.
	 */
	{
		struct kvm_debugregs zero_dr = { 0 };

		rc = os_ioctl_generic(vcpu_fd, KVM_SET_DEBUGREGS,
				      (unsigned long)&zero_dr);
		if (rc < 0) {
			pr_err("um: kvm-v2 vcpu_create_one: KVM_SET_DEBUGREGS(vcpu_fd=%d) failed (%d) — DR0-DR7 cross-task leak window remains\n",
			       vcpu_fd, rc);
			goto err_unmap_kvm_run;
		}
	}

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

	/*
	 * Round 7 Branch B: report the lifetime dispatch path split
	 * before tearing the vCPU down. The numerator/denominator is
	 * what Round 7 is trying to distinguish — does the cheap
	 * dirty-bit path dominate dispatches even on the failing
	 * kvm-v2 Django workload, and does the heavy ioctl gate fire
	 * at the rate the Round 4 tlb_gen-lag analysis predicted?
	 */
	{
		u64 heavy = v->dispatch_heavy_count;
		u64 cheap = v->dispatch_cheap_count;
		u64 total = heavy + cheap;

		pr_info("um: kvm-v2 vcpu_destroy: cpu=%d dispatches=heavy:%llu cheap:%llu total:%llu heavy_pct=%llu.%02llu\n",
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
	 * mmap of kvm_run survives the vcpu_fd close (the mapping is
	 * refcounted in the kernel), so munmap first; the v1 archive's
	 * shutdown path documents the same ordering.
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

int kvm_v2_vcpu_create(struct kvm_v2_vm *vm)
{
	int mmap_size, cpu, rc;

	if (!vm)
		return -EINVAL;

	/*
	 * Initialise the array's sentinel state on first entry. The bss-
	 * zeroed default puts every vcpu_fd at 0 (a valid fd), so we
	 * can't use that as the "already created" check until the reset
	 * has flipped it to -1. After this call the .vcpu_fd >= 0 test
	 * below is meaningful.
	 */
	if (!pool_initialised)
		kvm_v2_vcpu_pool_reset();

	if (vcpus[0].vcpu_fd >= 0) {
		pr_warn("um: kvm-v2 vcpu_create: pool already created (vcpu_fd=%d)\n",
			vcpus[0].vcpu_fd);
		return -EBUSY;
	}

	/*
	 * KVM_GET_VCPU_MMAP_SIZE is a /dev/kvm-fd ioctl and the size
	 * is constant for the host kernel's lifetime — query once and
	 * reuse for every pool member.
	 */
	mmap_size = os_ioctl_generic(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size <= 0) {
		pr_err("um: kvm-v2 vcpu_create: KVM_GET_VCPU_MMAP_SIZE failed (%d)\n",
		       mmap_size);
		return mmap_size ? mmap_size : -EIO;
	}

	/*
	 * Cap the pool at NR_CPUS to bound the static array; in practice
	 * UML's NR_CPUS_RANGE_END is 64 and nr_cpu_ids tracks the
	 * configured maximum, so the WARN below should never fire on a
	 * sanely configured build. If it does, build a smaller pool
	 * rather than overflowing the array.
	 */
	if (WARN_ON_ONCE(nr_cpu_ids > NR_CPUS)) {
		pr_warn("um: kvm-v2 vcpu_create: nr_cpu_ids=%u > NR_CPUS=%d; capping pool at NR_CPUS\n",
			nr_cpu_ids, NR_CPUS);
	}

	for (cpu = 0; cpu < min_t(int, nr_cpu_ids, NR_CPUS); cpu++) {
		rc = kvm_v2_vcpu_create_one(vm, cpu, mmap_size);
		if (rc)
			goto err_unwind;
	}

	pr_info("um: kvm-v2 vcpu_create: pool of %d vCPU(s) up (kvm_run_size=%d)\n",
		min_t(int, nr_cpu_ids, NR_CPUS), mmap_size);
	return 0;

err_unwind:
	while (--cpu >= 0)
		kvm_v2_vcpu_destroy_one(&vcpus[cpu]);
	return rc;
}

void kvm_v2_vcpu_destroy(void)
{
	int cpu;

	if (!pool_initialised)
		return;

	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		if (vcpus[cpu].vcpu_fd >= 0)
			pr_info("um: kvm-v2 vcpu_destroy: closing cpu=%d vcpu_fd=%d\n",
				cpu, vcpus[cpu].vcpu_fd);
		kvm_v2_vcpu_destroy_one(&vcpus[cpu]);
	}
}

struct kvm_v2_vcpu *kvm_v2_vcpu_get(int cpu)
{
	if (!pool_initialised)
		return NULL;
	if (cpu < 0 || cpu >= NR_CPUS)
		return NULL;
	if (vcpus[cpu].vcpu_fd < 0)
		return NULL;
	return &vcpus[cpu];
}

#if IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_KUNIT)
/**
 * kvm_v2_vcpu_prime_for_kunit - drive a pool vCPU through the lazy first-
 *                               dispatch arming sequence without entering
 *                               the guest.
 * @v: pool entry to prime (typically &vcpus[0]).
 *
 * Snapshot Phase 2 (memo 26-snapshot §Phase 2) needs a vCPU on which
 * KVM_GET_REGS / KVM_GET_SREGS / KVM_GET_XSAVE / KVM_GET_XCRS /
 * KVM_GET_VCPU_EVENTS / KVM_GET_MSRS all succeed at KUnit run-time
 * (do_basic_setup boundary). The KVM ioctls that constrain that
 * timing are CR4.OSXSAVE (rejected before KVM_SET_CPUID2 installs
 * the curated OSXSAVE bit) and KVM_SET_XCRS (rejected unless
 * guest_supported_xcr0 advertises the bits). vcpu_run handles
 * both via the lazy `if (!cpuid_primed)` block — we cannot reach
 * that block at KUnit time because no user task has dispatched yet.
 *
 * This helper extracts the same priming sequence (KVM_SET_CPUID2 +
 * GET_SREGS / SET_SREGS(cr4 |= OSXSAVE) + KVM_SET_XCRS) so the
 * KUnit fixture can drive it explicitly. After a successful return
 * the vCPU's `cpuid_primed = true`, and the snapshot capture +
 * restore paths work end-to-end.
 *
 * Build-gated on CONFIG_UM_BACKEND_KVM_V2_KUNIT so it costs zero
 * text in production builds.
 *
 * Returns 0 on success, -EINVAL if @v is unusable, -ENODEV if the VM
 * isn't initialised, -errno on the first ioctl failure.
 */
int kvm_v2_vcpu_prime_for_kunit(struct kvm_v2_vcpu *v)
{
	struct kvm_v2_vm *vm;
	struct kvm_sregs sregs2;
	struct kvm_run *run;
	int rc;

	if (!v || v->vcpu_fd < 0 || !v->kvm_run)
		return -EINVAL;
	if (v->cpuid_primed)
		return 0;
	vm = kvm_v2_vm_get();
	if (!vm)
		return -ENODEV;

	rc = kvm_v2_install_cpuid(vm, v->vcpu_fd);
	if (rc < 0) {
		pr_err("um: kvm-v2 kunit prime: install_cpuid (cpu=%d) failed: %d\n",
		       v->cpu, rc);
		return rc;
	}

	rc = os_ioctl_generic(v->vcpu_fd, KVM_GET_SREGS,
			      (unsigned long)&sregs2);
	if (rc < 0) {
		pr_err("um: kvm-v2 kunit prime: GET_SREGS pre-OSXSAVE (cpu=%d) failed: %d\n",
		       v->cpu, rc);
		return rc;
	}
	sregs2.cr4 |= X86_CR4_OSXSAVE;
	rc = os_ioctl_generic(v->vcpu_fd, KVM_SET_SREGS,
			      (unsigned long)&sregs2);
	if (rc < 0) {
		pr_err("um: kvm-v2 kunit prime: SET_SREGS+OSXSAVE (cpu=%d) failed: %d\n",
		       v->cpu, rc);
		return rc;
	}
	run = v->kvm_run;
	run->s.regs.sregs.cr4 = sregs2.cr4;

	rc = kvm_v2_install_xcrs(v->vcpu_fd);
	if (rc < 0) {
		pr_err("um: kvm-v2 kunit prime: install_xcrs (cpu=%d) failed: %d\n",
		       v->cpu, rc);
		return rc;
	}

	v->cpuid_primed = true;
	pr_info("um: kvm-v2 kunit prime: vcpu_fd=%d primed (CPUID+OSXSAVE+XCR0)\n",
		v->vcpu_fd);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_vcpu_prime_for_kunit);
#endif /* CONFIG_UM_BACKEND_KVM_V2_KUNIT */

/*
 * Phase B.5 / C.1: load guest CR3 on the supplied pool member.
 *
 * The contract (memo 26 §B.5) is:
 *   - PML4[0..255] of the guest pgd holds the user mappings (US=1).
 *   - PML4[256..511] holds the kernel direct map + kernel text + per-VM
 *     IDT/TSS page (US=0); user CPL=3 walks never reach these because
 *     the canonical-sign-extended boundary at bit 47 makes them
 *     non-canonical from user mode.
 * Setting guest CR3 = __pa(mm->pgd) therefore lets KVM's TDP walk the
 * user half cleanly without any shadow PT.
 *
 * Phase C.1 generalises the helper from A.3's file-scope-static target
 * to an explicit `vcpu` parameter so C.2's task→vCPU dispatch can pick
 * the right pool member. There is no in-tree caller yet (ops.c still
 * delegates `.vcpu_run`); the SREGS read/write plumbing lands here for
 * review.
 *
 * Caveat: the "PML4[256..511] is the kernel half" invariant requires
 * R1's high-VA layout to be active — i.e. uml_physmem in PML4[256+]
 * (vs the current default where uml_physmem == __binary_start_hva,
 * both in PML4[0]). R1 introduced the abstraction
 * (__binary_start_hva is separate from uml_physmem in
 * arch/um/kernel/um_arch.c) but did not flip the layout. The flip is
 * a separate substrate change tracked in memo 25 §R1's "v2 KVM"
 * note; Phase C/D + B.6 (mmu_notifier validation) will surface the
 * actual exit criterion when guest user mode runs via TDP.
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

/*
 * Phase C.2 helper: combine the per-iteration SREGS update into a
 * single GET_SREGS / SET_SREGS round-trip rather than calling
 * kvm_v2_load_cr3 + a separate fs.base/gs.base update (which would
 * pay 4 ioctls per dispatch instead of 2). B.5's load_cr3 stays as
 * the standalone helper for callers that only need to swap CR3
 * (e.g. context_switch when Phase D wires it). The duplication is
 * intentional: B.5's contract is "swap cr3 only", C.2's contract is
 * "establish full guest user context for one KVM_RUN", and merging
 * them would force every cr3-only caller to also re-write
 * fs.base/gs.base they don't own.
 *
 * Returns 0 on success or a negative errno on ioctl failure.
 */
/*
 * C.3: with KVM_CAP_SYNC_REGS enabled at vcpu create, the mmap'd
 * kvm_run->s.regs.sregs is authoritative on entry (KVM populated
 * it on the prior exit). Modify cr3/fs.base/gs.base in place and
 * mark KVM_SYNC_X86_SREGS in kvm_dirty_regs so KVM consumes the
 * update on the next KVM_RUN. No ioctls.
 *
 * Returns 0 always today — kept as int for forward compat with a
 * future per-CPU SYNC_REGS check should we ever want to support
 * hosts where the cap is partial.
 */
static int kvm_v2_load_user_sregs(struct kvm_v2_vcpu *vcpu,
				  unsigned long pgd_pa,
				  unsigned long fs_base,
				  unsigned long gs_base,
				  unsigned long entry_rip)
{
	struct kvm_run *run = vcpu->kvm_run;
	struct kvm_sregs *sregs = &run->s.regs.sregs;
	/*
	 * SMP-T33 (2026-05-03): capture cross-task predicate BEFORE any
	 * field of vcpu->last_task / vcpu->last_mm is updated below. Used at
	 * end of function to gate the full KVM_SET_SREGS ioctl that drops
	 * KVM's per-vCPU TDP MMU prev_roots cache.
	 *
	 * SMP-T33c (NEGATIVE): tried also firing on tlb_gen advancement to
	 * close T33b's residual. Apples-to-apples 200-boot parallel comparison
	 * REGRESSED 97.5% → 93.0%. Hypothesis was wrong — extra
	 * __set_sregs2 side effects introduce a new problem rather than
	 * closing the residual. Stick with cross_task-only gate.
	 */
	bool cross_task = (vcpu->last_task != current) ||
			  (vcpu->last_mm   != current->mm);

	sregs->cr3     = (u64)pgd_pa;
	sregs->fs.base = (u64)fs_base;
	sregs->gs.base = (u64)gs_base;

	/*
	 * SMP-T13 fix (2026-05-02): on every dispatch where rip will
	 * be a USER-half VA, reset CS/SS/DS/ES to USER selectors.
	 *
	 * Background: After a SYSCALL trap, vCPU.cs = kernel CS
	 * (=0x08, DPL=0) per MSR_STAR's SYSCALL CS half. KVM stores
	 * cs into run->s.regs.sregs at exit. The previous
	 * load_user_sregs only wrote cr3/fs/gs/cr2/cr4 — leaving cs
	 * at kernel CS. The next KVM_RUN entered the guest at user_RIP
	 * (we marshal rip = HOST_IP) but with CS=0x08 → CPU runs user
	 * code at CPL=0.
	 *
	 * Empirical proof (mt-yieldonly reproducer, 2026-05-02): T=8
	 * ncpus=4 yield-only crashes 90%+ with `segfault at 0
	 * ip=user_VA error=2`. error=2 = write-to-not-present in
	 * SUPERVISOR mode. CPL=3 user code can't generate a supervisor
	 * fault. The CPU was at CPL=0 executing user code.
	 *
	 * Conditional: only reset CS to USER when the rip we'll marshal
	 * is in user-half (< canonical kernel-half boundary). For
	 * trampoline-replay paths (EINTR-mid-trampoline, where rip is
	 * still in kernel-half = trampoline VA), keep CS at whatever
	 * KVM left it (kernel CS) so the trampoline can execute.
	 *
	 * v1 archive avoided this entirely via an IRETQ gadget that
	 * pushes [user_RIP, 0x2b, RFLAGS, user_RSP, 0x23] and iretq's
	 * atomically (kvm-v1-archive/thread.c:3088-3110). This
	 * conditional reset is the simpler v2-compatible variant.
	 */
	if (entry_rip < KVM_V2_TRAMPOLINE_GVA) {
		sregs->cs.selector = 0x2b;	/* USER_CS, DPL=3 */
		sregs->cs.type     = 0xb;
		sregs->cs.dpl      = 3;
		sregs->cs.s        = 1;
		sregs->cs.l        = 1;
		sregs->cs.db       = 0;
		sregs->cs.g        = 1;
		sregs->cs.present  = 1;
		sregs->cs.base     = 0;
		sregs->cs.limit    = 0xffffffff;
		sregs->ss.selector = 0x23;	/* USER_DS, DPL=3 */
		sregs->ss.type     = 0x3;
		sregs->ss.dpl      = 3;
		sregs->ss.s        = 1;
		sregs->ss.db       = 1;
		sregs->ss.l        = 0;
		sregs->ss.g        = 1;
		sregs->ss.present  = 1;
		sregs->ss.base     = 0;
		sregs->ss.limit    = 0xffffffff;
		sregs->ds = sregs->es = sregs->ss;
	}

	/*
	 * Defensive: cr2 in the SYNC_REGS sregs mmap reflects the LAST
	 * KVM_RUN exit's #PF address. v2 uses a per-host-CPU vCPU pool
	 * (vcpu.c:1295), so when a different task enters on the same
	 * vCPU, parent's cr2 would otherwise be re-shipped to vmcs via
	 * the dirty-bit at line 1132 below. Clear it explicitly so the
	 * dispatch starts with a clean cr2 — guest #PF handlers can't
	 * see stale parent-task fault addresses.
	 *
	 * #121-D experiment 2026-05-01: removing this line gives IDENTICAL
	 * failure rate (184/200 vs 185/200), but failing cases then show
	 * STALE cr2 values from previous tasks (cross-task leak) instead
	 * of cr2=0. So the zero is NOT the source of the cr2-loss bug —
	 * the bug is "cr2 doesn't get updated on this dispatch's fault",
	 * and our zero just makes the symptom uniform (cr2=0) rather than
	 * leaky (stale values). Keep the zero for cross-task isolation
	 * hygiene.
	 *
	 * #121-D15 fix (2026-05-01): if the previous EINTR exit caught the
	 * vCPU mid-IDT-delivery (RIP set to a stub-start by hardware, but
	 * the in-guest stub's first `push %rax` had NOT yet run), the
	 * VMCB.save.cr2 still holds the real fault address from the IDT
	 * delivery. The EINTR snapshot path saved that into per-task
	 * arch.kvm_v2.saved_cr2_at_eintr — restore it here so the resumed
	 * stub's `mov %cr2, %rax` reads the correct CR2. See
	 * arch_thread.kvm_v2.saved_cr2_at_eintr comment for full mechanism.
	 */
	{
		struct arch_thread *a = &current->thread.arch;
		if (a->kvm_v2.saved_cr2_valid) {
			sregs->cr2 = a->kvm_v2.saved_cr2_at_eintr;
			a->kvm_v2.saved_cr2_valid = false;
		} else if (vcpu->last_task != current ||
			   vcpu->last_mm   != current->mm) {
			/*
			 * SMP-T16 fix (2026-05-02): only zero cr2 on cross-task
			 * transitions. Same-task re-entry preserves whatever
			 * KVM left in sregs.cr2 — typically the hardware-
			 * architectural CR2 from the previous exit's fault. See
			 * struct kvm_v2_vcpu.last_task comment for the full
			 * mechanism (Bug A — first-KVM_RUN-of-new-exec NULL
			 * deref at e_entry under SMP, captured 2026-05-02 via
			 * state-trace ring).
			 *
			 * SMP-T23 (2026-05-02): also zero on cross-mm. After
			 * execve, `current` is unchanged but `current->mm` is a
			 * fresh mm — the original T16 gate (`last_task !=
			 * current`) is silent for this transition, leaving the
			 * pre-execve mm's stale sregs.cr2 to leak into the
			 * fresh mm's first VMRUN. Manifests as the post-T22
			 * residual on threaded-subprocess-wait.py (worker rc=-11
			 * SIGSEGV at ld-linux relocation traversal, error code
			 * patterns inconsistent with a real fault address —
			 * captured 2026-05-02 via stderr-pipe diagnostic in
			 * the reproducer + Opus subagent #2 audit). See
			 * state-audit/05 §9 cross-reference.
			 */
			sregs->cr2 = 0;
		}
		vcpu->last_task = current;
		vcpu->last_mm   = current->mm;
	}

	/*
	 * SMP-T55: cross-task arrival on this vCPU means the vCPU's
	 * guest FPU is whatever the previous task left. Mark dirty so
	 * the post-vmexit GET captures into the new task's iotrap_fpu
	 * slot (or the SET below installs the new task's snapshot if
	 * its iotrap_fpu_valid is set, which then matches and we go
	 * fpu_dirty=false again at the SET site).
	 */
	if (vcpu->fpu_owner_task != current)
		vcpu->fpu_dirty = true;

	/*
	 * Force a guest-TLB flush by toggling CR4.PGE on every dispatch.
	 *
	 * UML changes guest PTEs by writing them into physmem (the guest
	 * pgd page lives in physmem, like every other guest page). Those
	 * writes do NOT fire KVM's mmu_notifier — only host-mm operations
	 * on the spawner do. So KVM's guest-TLB cache continues to map
	 * GVA → old physmem PFN even after UML's PTE points to a fresh
	 * page (e.g. the post-CoW page after a child #PF). Subsequent
	 * guest accesses read the OLD physmem page (parent's data),
	 * which manifests as user-stack memory corruption — see
	 * `tools/testing/selftests/um/fork-tree-3level/repros/`.
	 *
	 * KVM only requests `TLB_FLUSH_GUEST` from `__set_sregs_common`
	 * when CR3 OR CR4 differ from current (arch/x86/kvm/x86.c:12474+
	 * 12487-12488 → 12529-12532). Same-CR3 dispatches (the common
	 * case — the same task re-enters after a syscall) skip the
	 * flush. v1 worked around this by toggling CR4.PGE on every
	 * same-CR3 dispatch (kvm-v1-archive/thread.c:2974-2984); the
	 * comment there documents that narrowing the toggle to
	 * "tlb_stale && same_cr3" regressed v1's gate "to ~70% pass
	 * rate vs 100%" — exactly the failure rate we observe on
	 * fork-tree-3level under v2.
	 *
	 * Implementation: alternate the PGE bit on each dispatch. KVM
	 * sees CR4 change → mmu_reset_needed → vmenter flushes guest
	 * TLB via vpid_sync_context (single-context INVVPID). The
	 * actual PGE semantic doesn't matter for UML guests — they
	 * don't use global pages.
	 *
	 * Cost: zero extra ioctls (uses the existing SYNC_REGS dirty
	 * bit). Each dispatch ships its sregs anyway; we just modify
	 * one more field. Phase H may narrow this with a per-vcpu
	 * "tlb-stale" predicate; for now correctness > performance.
	 */
	sregs->cr4 ^= X86_CR4_PGE;

	/*
	 * G.2-cont/G.2-fix (2026-05-01): ack pending kick + update
	 * per-vCPU tlb_gen tracking. The CR4.PGE toggle above IS the
	 * flush. Snapshot mm->context.tlb_gen into last_seen so the
	 * remote kicker can skip this vCPU until the gen bumps again.
	 *
	 * Also stash current_mm so the kicker can target only vCPUs
	 * running THIS mm.
	 */
	atomic_set(&vcpu->kick_pending, 0);
	WRITE_ONCE(vcpu->current_mm, current->mm);
	if (current->mm) {
		int cpu = vcpu->cpu;
		u64 cur_gen = atomic64_read(&current->mm->context.tlb_gen);
		u64 last    = atomic64_read(
				&current->mm->context.tlb_gen_seen_by[cpu]);

		if (cur_gen >= last + 3) {
			static atomic_t tlb_lag_hits = ATOMIC_INIT(0);
			if (atomic_inc_return(&tlb_lag_hits) <= 30) {
				pr_emerg("KVM_V2_TLB_LAG cpu=%d pid=%d mm=%px last=%llu cur=%llu lag=%llu\n",
					 cpu, current->pid, current->mm,
					 (unsigned long long)last,
					 (unsigned long long)cur_gen,
					 (unsigned long long)(cur_gen - last));
			}
		}

		vcpu->last_dispatch_tlb_lag =
			(cur_gen > last) ? (cur_gen - last) : 0;

		atomic64_set(
			&current->mm->context.tlb_gen_seen_by[cpu], cur_gen);

		/*
		 * TOCTOU close: another vCPU may have bumped tlb_gen
		 * between our read of cur_gen and the write above.  If
		 * so, our prev_roots[] cache is stale for the NEW gen
		 * even though we think lag==0.  Re-read and force the
		 * heavy KVM_SET_SREGS path if the gen moved.
		 */
		smp_mb();
		{
			u64 recheck = atomic64_read(
					&current->mm->context.tlb_gen);
			if (recheck != cur_gen)
				vcpu->last_dispatch_tlb_lag =
					KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD;
		}
	} else {
		vcpu->last_dispatch_tlb_lag = 0;
	}

	/*
	 * D.4a: ensure EFER.SCE is set so SYSCALL doesn't raise #UD. KVM
	 * sets the long-mode bits (LME/LMA) automatically at VMX entry,
	 * but SCE (System Call Extensions) must be explicitly enabled or
	 * the user-mode `syscall` instruction is illegal — the gate would
	 * fail on the first user-mode instruction once D.5 flips
	 * .vcpu_run. v1 set this in SREGS at kvm-v1-archive/sregs.c:264-265
	 * with mask `KVM_EFER_SCE | KVM_EFER_LME | KVM_EFER_LMA |
	 * KVM_EFER_NXE`; v2 sets the same bits via the sync-regs mmap.
	 *
	 * Use `=` (full assignment) rather than `|=` for two reasons:
	 *   1. On the first KVM_RUN before any guest exit has populated
	 *      sregs, the mmap's efer field reflects KVM's defaults — not
	 *      guaranteed to include LME/LMA/NXE. `|=` adding only SCE
	 *      would leave the others potentially unset.
	 *   2. EFER is fully owned by v2 under the single-VM model — there
	 *      are no other bits we want set, so a clean overwrite matches
	 *      v1's `sregs->efer = ...` at sregs.c:264-265 exactly.
	 *
	 * Idempotent: this fires on every dispatch (load_user_sregs is
	 * per-task), but EFER doesn't change across tasks under v2's
	 * single-VM model. Could be hoisted to vcpu_create as a one-time
	 * write — opting for per-dispatch here for symmetry with the
	 * cr3/fs.base/gs.base writes in this function. Phase H may
	 * profile and hoist if it matters; the cost is one mmap'd-field
	 * store + a dirty bit, no ioctl.
	 *
	 * EFER_NX is the kernel header's spelling of v1's KVM_EFER_NXE
	 * (NX-enable bit 11 in EFER); the bit semantics are identical.
	 */
	sregs->efer = EFER_SCE | EFER_LME | EFER_LMA | EFER_NX;

	/*
	 * Phase H.2: arm CR0.TS to lazily detect FPU usage. Any FP/SSE/AVX
	 * instruction the guest executes while TS=1 raises #NM (vec 7);
	 * our IDT[7] stub does `clts; iretq` in-guest, clearing TS so the
	 * instruction retries successfully. After vmexit, TS=0 in sregs.cr0
	 * means the guest used FPU; TS=1 means it did not — we use this
	 * post-vmexit to skip KVM_GET_FPU when no FP instruction ran.
	 *
	 * Idempotent: every dispatch re-arms; the post-exit sregs.cr0 read
	 * happens before this re-arm so no race. Existing CR0 bits (PE/MP/
	 * NE/WP/PG) are preserved — we only set the TS bit.
	 *
	 * SMP-T22 (2026-05-02) one-shot bypass: if the host-side #NM handler
	 * (kvm_v2_handle_io_nm or the EINTR variant) just cleared TS for
	 * the user's FP-instruction retry, skip the re-arm AND clear the
	 * existing TS bit so the next KVM_RUN re-enters at the user's FP
	 * instruction with TS=0. Without this, my host's TS clear is undone
	 * before the user gets a chance to retry, and the FP instruction
	 * faults again → handler clears TS → arm re-sets → infinite loop.
	 *
	 * Lazy-FPU semantics preserved: nm_ts_bypass is one-shot. After the
	 * user's FP burst completes (any non-#NM exit), the next dispatch
	 * arms TS=1 normally, so the next FP burst still trips one #NM.
	 */
	if (current->thread.arch.kvm_v2.nm_ts_bypass) {
		sregs->cr0 &= ~X86_CR0_TS;
		current->thread.arch.kvm_v2.nm_ts_bypass = false;
	} else {
		sregs->cr0 |= X86_CR0_TS;
	}

	/*
	 * SMP-T33 (2026-05-03) — KVM TDP MMU prev_roots cache invalidation
	 * on cross-task / cross-mm dispatch (state-audit memo 20).
	 *
	 * Background: KVM keeps a small LRU of recently-used TDP roots per
	 * vCPU (`vcpu->arch.mmu->prev_roots`). When v2 ships a new CR3 via
	 * the KVM_SYNC_X86_SREGS dirty-bit, KVM's `kvm_mmu_new_pgd(cr3)`
	 * fast-switches to a cached root if one exists keyed by `cr3 | pcid`,
	 * WITHOUT revalidating that the cached root's leaf SPTEs reflect
	 * current physmem PTE bytes.
	 *
	 * Under v2's per-host-CPU vCPU pool, multiple UML tasks (different
	 * mms / CR3s) share one vCPU. Their TDP roots cycle through this
	 * vCPU's prev_roots cache. mmu_notifier-triggered remote-TLB-flushes
	 * targeting one mm's *active* root on another vCPU may not fully
	 * invalidate the entry sitting in this vCPU's prev_roots[]. When
	 * this task re-dispatches, KVM fast-switches to that stale cached
	 * root → user-side reads/writes go through stale guest-VA→host-PA
	 * mappings → mt-mini's `got=0 expect=N` symptom.
	 *
	 * Why ncpus=1 PASSes 60/60: with one vCPU, prev_roots cycles only
	 * through the few mms actually running, the active root is always
	 * the most recent one, mmu_notifier flushes hit the right place.
	 * Why ncpus=4 baselines at 83%: with 4 vCPUs, an mm's root falls
	 * into prev_roots[] on the vCPUs not currently running it, then
	 * gets fast-switched-back-to without revalidation.
	 *
	 * Fix: on cross-task or cross-mm transitions, issue full KVM_SET_SREGS
	 * ioctl. The full ioctl takes the heavy `__set_sregs2 →
	 * kvm_mmu_reset_context` path which drops `prev_roots[]` and forces
	 * the next vmentry to walk fresh. Same-task same-mm re-entries (the
	 * common syscall-trap case) skip this — KVM_SYNC_X86_SREGS dirty-bit
	 * is sufficient for unchanged CR3.
	 *
	 * Empirical (state-audit/20 entry 14, 60-boot loop):
	 *   ncpus=4 baseline (no fix): 50/60 = 83%
	 *   ncpus=4 with always-fix:   59/60 = 98% (this branch's intent;
	 *                                          narrow form tested next)
	 *   ncpus=2 (control):         58/60 = 97%
	 *   ncpus=1 (control):         60/60 = 100%
	 *
	 * Cross-task gate uses the same `last_task != current || last_mm !=
	 * current->mm` predicate established by SMP-T16 (cr2 zero) and
	 * SMP-T23 (extended for cross-mm execve transitions).
	 *
	 * SMP-T56 / Round 4 (2026-05-17): testing showed forcing the
	 * heavy KVM_SET_SREGS UNCONDITIONALLY on every dispatch dropped
	 * the django-loopback-none "Executing a cache" Python flake rate
	 * from ~14% (Round 3 baseline) to ~1.1% (2/180 over a 180-iter
	 * no-trace soak), confirming that same-task same-mm dispatches
	 * with a stale per-vCPU TDP prev_roots[] cache are a real
	 * mechanism behind the flake. The unconditional variant however
	 * also caused 3/180 boot-time `swapper: page allocation failure`
	 * panics from per-dispatch __set_sregs2 → kvm_mmu_reset_context
	 * allocation pressure — so we narrow with a tlb_gen lag gate.
	 *
	 * Narrowed trigger: fire the heavy ioctl when guest tlb_gen has
	 * advanced by KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD since this
	 * vCPU last dispatched the mm. "Lag advanced" means another vCPU
	 * (or remote flush) bumped the per-mm tlb_gen multiple times
	 * without us catching up — i.e. there exist mmu_notifier-mediated
	 * physmem PTE attribute changes that the per-vCPU prev_roots[]
	 * entry for this CR3 likely hasn't observed. Same-task same-mm
	 * with no advancement falls through the cheap KVM_SYNC_X86_SREGS
	 * dirty-bit path; the heavy path runs only on dispatches that
	 * actually need it. SMP-T33c (negative) gated on *any*
	 * advancement (>=1); this gate is *>= 3*, which the Round 4
	 * evidence shows matches the failing dispatch lag range (hundreds
	 * to thousands) without firing on the normal cross-vCPU noise
	 * floor.
	 */
	if (cross_task ||
	    vcpu->last_dispatch_tlb_lag >=
		    KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD) {
		struct kvm_sregs full = *sregs;
		int rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_SREGS,
					  (unsigned long)&full);

		/*
		 * Round 7 Branch B: dispatch went through the heavy
		 * `KVM_SET_SREGS → __set_sregs → kvm_mmu_reset_context`
		 * path. Single-writer per vCPU (this dispatch thread under
		 * migrate_disable), no atomic needed.
		 */
		vcpu->dispatch_heavy_count++;

		/*
		 * SMP-T47 (mainstream-readiness audit P2 #6): if this
		 * cross-task ioctl ever fails, the heavy
		 * `__set_sregs2 → kvm_mmu_reset_context` path didn't run
		 * and the vCPU keeps stale prev_roots[] entries — exactly
		 * the SMP-T33 bug class. Silent (void)-cast made that
		 * regression invisible. Now: WARN_ON_ONCE marks the kernel
		 * tainted on first occurrence so post-mortem catches it,
		 * and a ratelimited pr_warn gives the per-vCPU context for
		 * follow-up. No perf cost on the success path (rc >= 0
		 * skips both branches).
		 */
		if (WARN_ON_ONCE(rc < 0))
			pr_warn_ratelimited("um: kvm-v2 prev_roots-drop KVM_SET_SREGS failed (rc=%d) on vcpu=%d cross_task=%d lag=%llu — stale prev_roots[] possible; SMP-T33/T56 bug class returns\n",
					    rc, vcpu->cpu, cross_task,
					    (unsigned long long)vcpu->last_dispatch_tlb_lag);
	} else {
		/*
		 * Round 7 Branch B: dispatch will rely on the cheap
		 * `KVM_SYNC_X86_SREGS` dirty-bit path inside KVM's
		 * `sync_regs() → __set_sregs` on the next KVM_RUN entry.
		 * Both heavy and cheap paths drive the same KVM helper,
		 * so per-vCPU heavy/cheap totals at shutdown distinguish
		 * "the gate fires" from "the cheap path leaks past the
		 * MMU reset" if the cheap counter grows without bound and
		 * dispatch failures still occur.
		 */
		vcpu->dispatch_cheap_count++;
	}

	run->kvm_dirty_regs |= KVM_SYNC_X86_SREGS;

	/*
	 * Phase H gadget Phase 3 (2026-05-04): refresh THIS vCPU's gadget
	 * state page so the LSTAR dispatch tree returns correct values for
	 * all five pid-family syscalls (getpid/gettid/getuid/geteuid/getgid).
	 * Each handler reads `%gs:KVM_V2_GADGET_OFF_<field>` from this
	 * vCPU's state page; we refresh all five fields here before every
	 * KVM_RUN.
	 *
	 * Per-vCPU isolation: each vCPU's MSR_KERNEL_GS_BASE points at its
	 * own state page (vcpu->gadget_state_kva), so the writer here (this
	 * host-CPU pthread for this vCPU) and the gadget reader (the same
	 * vCPU running guest code) are mutually exclusive in time without
	 * a lock. On SMP, sibling vCPUs each have their own page and
	 * refresh independently — no shared writer race.
	 *
	 * EINTR-mid-gadget robustness (audited 2026-05-04, design/stability
	 * cycle): the LSTAR gadget body is now 94 bytes and contains 3
	 * swapgs instructions; SIGALRM-driven KVM_RUN -EINTR can land
	 * anywhere in that range, including post-entry-swapgs (GS_BASE
	 * = STATE_GVA, KERNEL_GS_BASE = user_gs from the swap). Naively
	 * resuming with sregs.gs.base set to user_gs (this function's
	 * normal write) would corrupt the gadget read. The empirically-
	 * validated robustness mechanism (125M+ gadget calls clean):
	 *
	 *   1. KVM captures VMCB.save.gs.base (= STATE_GVA mid-gadget)
	 *      into eintr_sregs.gs.base on the EINTR exit.
	 *   2. kvm_v2_marshal_sregs_back (vcpu.c:1690-1691) writes
	 *      regs->gp[HOST_GS_BASE] = eintr_sregs.gs.base = STATE_GVA.
	 *      The per-task `regs` now holds the swapped value.
	 *   3. Next dispatch on this task: the caller passes
	 *      regs->gp[HOST_GS_BASE] = STATE_GVA into us as `gs_base`.
	 *      We write sregs->gs.base = STATE_GVA (above), so KVM_RUN
	 *      re-enters the gadget mid-flight with GS_BASE in the right
	 *      state. The gadget completes; tail swapgs swaps back
	 *      cleanly; KERNEL_GS_BASE returns to STATE_GVA.
	 *   4. Next vmexit captures the fully-restored gs.base and
	 *      kvm_v2_marshal_sregs_back updates HOST_GS_BASE to the
	 *      true user value.
	 *
	 * The same mechanism handles cross-vCPU resume (task A EINTRs on
	 * vCPU 0, scheduled back on vCPU 1): vCPU 1 has its own state
	 * page at STATE_GVA(1), which load_user_sregs's tgid refresh
	 * just populated for task A → mid-handler resume on vCPU 1 reads
	 * the right values from STATE_GVA(1).
	 *
	 * gadget_state_kva is allocated lazily by
	 * kvm_v2_install_per_vcpu_gadget_state (run from
	 * kvm_v2_exception_install at subsys_initcall), so before that
	 * runs (early boot, before the first SYSCALL hits the gadget) the
	 * pointer may still be NULL. The NULL check below makes the
	 * refresh a no-op in that window. The trampoline two-phase
	 * upgrade in kvm_v2_trampoline_upgrade_to_gadget guarantees the
	 * 94-byte gadget LSTAR is only installed AFTER all per-vCPU
	 * state pages exist, so by the time the gadget body executes,
	 * gadget_state_kva is non-NULL on every vCPU (refresh was a
	 * no-op only during the small startup window before the upgrade,
	 * and during that window the LSTAR was the safe 5-byte fallback
	 * that doesn't read the state page).
	 *
	 * Cost: 5 namespace lookups + 5 × 4-byte stores. No ioctl, no
	 * lock. Trivial vs the rest of load_user_sregs.
	 *
	 * Field semantics (POSIX):
	 *   getpid  → tgid via task_tgid_vnr (process ID)
	 *   gettid  → pid  via task_pid_vnr  (thread ID — Linux extension)
	 *   getuid  → real uid (current_cred->uid, ns-mapped)
	 *   geteuid → effective uid (current_cred->euid, ns-mapped)
	 *   getgid  → real gid (current_cred->gid, ns-mapped)
	 * v1 reference: kvm-v1-archive/lifecycle.c:904-922's
	 * kvm_gadget_state_refresh — same field set + same accessor
	 * choices.
	 */
	if (vcpu->gadget_state_kva) {
		u8 *p = (u8 *)vcpu->gadget_state_kva;
		const struct cred *c = current_cred();
		struct timespec64 real_ts;

		WRITE_ONCE(*(u32 *)(p + KVM_V2_GADGET_OFF_CPU_ID),
			   (u32)vcpu->cpu);
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

		/*
		 * Phase 6: refresh REAL_SEC for the time(2) gadget. NO
		 * seqlock needed — 1-second resolution makes a torn read
		 * at-worst off-by-one (matches v1 archive thread.c:1110-
		 * 1115 and the native vDSO's behavior).
		 */
		ktime_get_real_ts64(&real_ts);
		WRITE_ONCE(*(s64 *)(p + KVM_V2_GADGET_OFF_REAL_SEC),
			   real_ts.tv_sec);

		/*
		 * Phase 7: seqlock-protected MONO_SEC/MONO_NSEC + BUDGET reset
		 * for clock_gettime(CLOCK_MONOTONIC). v1 reference:
		 * kvm-v1-archive/lifecycle.c:1030-1066.
		 */
		{
			u32 *seq = (u32 *)(p + KVM_V2_GADGET_OFF_SEQ);
			u64 mono_ns = ktime_get_ns();

			WRITE_ONCE(*seq, *seq + 1);	/* even → odd */
			smp_wmb();
			WRITE_ONCE(*(s64 *)(p + KVM_V2_GADGET_OFF_MONO_SEC),
				   (s64)(mono_ns / NSEC_PER_SEC));
			WRITE_ONCE(*(s64 *)(p + KVM_V2_GADGET_OFF_MONO_NSEC),
				   (s64)(mono_ns % NSEC_PER_SEC));
			WRITE_ONCE(*(s32 *)(p + KVM_V2_GADGET_OFF_BUDGET),
				   KVM_V2_VVAR_BUDGET_INITIAL);
			smp_wmb();
			WRITE_ONCE(*seq, *seq + 1);	/* odd → even */
		}
	}

	return 0;
}

/*
 * Marshal uml_pt_regs.gp[] → struct kvm_regs for KVM_SET_REGS.
 * Mirrors the v1 archive's kvm_uml_regs_to_kvm_regs (thread.c
 * around line 2164). RFLAGS bit 1 is reserved-must-be-1 per AMD64
 * SDM §3.1.4 — OR it in defensively so a stale uml_pt_regs never
 * trips #GP on KVM_SET_REGS. Pure data-shape transform; C.3 will
 * replace this whole call site with KVM_CAP_SYNC_REGS writes
 * directly into vcpu->kvm_run->s.regs.regs.
 */
/* Non-static so D.3's marshal-out path in syscall_trap.c can reuse it.
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
 * Reverse marshal: struct kvm_regs → uml_pt_regs.gp[]. Called after
 * KVM_RUN returns so UML's syscall / fault / signal dispatch sees
 * the guest's post-exit GPRs. HOST_ORIG_AX is intentionally NOT
 * written here — that's an UML entry-path convention the syscall
 * dispatcher arranges once it knows the bucket (matches v1's
 * kvm_regs_to_uml_regs). C.3 replaces this with
 * KVM_CAP_SYNC_REGS reads from vcpu->kvm_run->s.regs.regs.
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
 * in those gp[] slots — `arch/x86/um/syscalls_64.c:22` writes them
 * synchronously when user-mode arch_prctl(ARCH_SET_FS,...) traps,
 * and `kvm_v2_load_user_sregs` (above) reads them back into
 * `sregs->fs.base`/`gs.base` on every dispatch. Without this
 * symmetric read-back, intra-guest FS/GS updates from any path
 * other than arch_prctl (e.g. a future direct-wrfsbase guest, or
 * a sibling task's per-CPU sregs write landing on the same vCPU
 * before the next task's load_user_sregs) are silently lost on the
 * next dispatch — we re-install the stale gp[HOST_FS_BASE],
 * clobbering live MSR_FS_BASE.
 *
 * Today this is largely defensive: empirically tracing with
 * `pr_emerg("DIAG-FS: sregs_back ...")` against the fork-tree-3level
 * gate at tip aaced3ce4924 shows fs.base = 0x0 throughout the run
 * (static-glibc binaries don't issue arch_prctl(SET_FS) — the canary
 * lives at a globally-mapped __stack_chk_guard, not a TLS slot). So
 * this read-back is correct but does NOT resolve #95 / #96 / the
 * fork-tree-3level FAIL signature; that bug is a different family
 * (memo §E.4: stack-smashing-detected from glibc FORTIFY checks,
 * not TLS canary).
 *
 * v1 reference: kvm-v1-archive/thread.c:1918 `kvm_propagate_fs_gs_
 * base` did the equivalent via KVM_SET_MSRS on every arch_prctl;
 * v2 lifts that into per-dispatch SYNC_REGS, so the read-back must
 * also run per-dispatch. SYNC_REGS guarantees sregs is coherent on
 * every KVM_RUN exit (including EINTR), so reading from the mmap'd
 * page is zero-ioctl. This commit closes the v1→v2 regression in
 * FS/GS round-trip; the fork-tree-3level investigation continues.
 */
void kvm_v2_marshal_sregs_back(struct uml_pt_regs *dst,
			       const struct kvm_sregs *src)
{
	dst->gp[HOST_FS_BASE] = (unsigned long)src->fs.base;
	dst->gp[HOST_GS_BASE] = (unsigned long)src->gs.base;
}

/*
 * C.4 forward decl: kvm_v2_fpu_install_on_first_run lives at the
 * bottom of the file alongside kvm_v2_fpu_capture_for_fork (the two
 * are a logical pair). The dispatcher below references it before its
 * definition.
 */
static int kvm_v2_fpu_install_on_first_run(struct kvm_v2_vcpu *vcpu);

/*
 * Phase C.2: KVM_RUN dispatcher — task→vCPU dispatch helper that
 * mirrors seccomp_vcpu_run's "one round-trip" shape:
 *
 *   1. Pick the per-host-CPU vCPU (preempt_disable so the pick
 *      stays valid across KVM_RUN; v1's archive enforced the same
 *      invariant via kvm_vcpu_for_current).
 *   2. Load the user-mode CPU state for `current`:
 *      CR3 = __pa(active_mm->pgd), fs.base / gs.base from
 *      regs->gp[HOST_FS_BASE / HOST_GS_BASE].
 *   3. Marshal regs->gp[] → kvm_regs, KVM_SET_REGS.
 *   4. KVM_RUN.
 *   5. Marshal kvm_regs → regs->gp[].
 *   6. Dispatch on kvm_run->exit_reason.
 *
 * At C.2 the helper has NO production caller — ops.c still routes
 * `.vcpu_run` to seccomp_vcpu_run; Phase D flips the pointer (D.5).
 * The panic placeholders for HLT / FAIL_ENTRY / INTERNAL_ERROR /
 * SHUTDOWN remain EXPECTED — Phase E adds MMIO + exception classes;
 * until those land any non-IO return from KVM_RUN under v2 is by
 * definition a bug we want to surface loudly. D.2 added the
 * KVM_EXIT_IO arm (kvm_v2_handle_io_trap → handle_syscall) so the
 * SYSCALL trap path no longer panics; it's still unreachable today
 * because nobody calls kvm_v2_vcpu_run.
 *
 * The pool_initialised fallback to seccomp_vcpu_run is defensive:
 * today init_backend always populates the pool before any caller
 * could plumb to here, but Phase D's pointer-flip might race with
 * shutdown teardown, and the seccomp fallback keeps the dispatch
 * contract intact (.vcpu_run is HOT — never NULL).
 */
void kvm_v2_vcpu_run(struct uml_pt_regs *regs)
{
	struct kvm_v2_vcpu *vcpu;
	struct kvm_run *run;
	int cpu, rc;
	u32 exit_reason;

	KVMV2_TRACE(KVMV2_OP_VCPU_RUN_ENTRY, regs, NULL, NULL);

	/*
	 * SMP-T13 fix (2026-05-02): use migrate_disable() instead of
	 * preempt_disable().
	 *
	 * Background: UML builds with CONFIG_PREEMPT_VOLUNTARY (no
	 * CONFIG_PREEMPT_COUNT), which makes preempt_disable() a NO-OP
	 * — it does NOT prevent the task from being migrated to another
	 * host CPU when handle_syscall calls schedule()  (e.g. from
	 * sched_yield's call chain).
	 *
	 * Empirical proof (UM_CPU_MIGRATED probe at syscall_trap.c
	 * exit, 2026-05-02): mt-yieldonly T=8 ncpus=4 hits >30
	 * cross-CPU migrations within handle_syscall(__NR_sched_yield)
	 * — the migrated task then continues with a STALE `vcpu` ptr
	 * pointing at the original host CPU's vCPU. marshal_to_kvm
	 * writes to the wrong run mmap; KVM_RUN ioctl runs against
	 * the wrong vCPU. Cross-task contamination follows.
	 *
	 * migrate_disable() pins the running task to the current host
	 * CPU regardless of preempt-count semantics. Voluntary schedule
	 * is still allowed, but when we resume we're on the SAME CPU,
	 * so `cpu`, `vcpu`, and `run` remain valid.
	 *
	 * v1 archive used `kvm_vcpu_for_current()` which dereferenced
	 * a per-current pointer — implicitly correct because the
	 * current task struct moves with the task. v2's per-host-CPU
	 * pool requires explicit CPU pinning instead.
	 */
	migrate_disable();

	cpu = smp_processor_id();
	vcpu = kvm_v2_vcpu_get(cpu);
	if (!vcpu) {
		/*
		 * Pool not up — should not happen post init_backend, but
		 * keep the dispatch contract intact. migrate_enable
		 * before delegating because seccomp_vcpu_run does its
		 * own scheduling-sensitive work (turnstile, futex).
		 */
		migrate_enable();
		seccomp_vcpu_run(regs);
		return;
	}

	run = vcpu->kvm_run;

	/*
	 * Round 6 Deliverable D (2026-05-18): record the dispatch
	 * (host_cpu, vcpu, pid, mm) tuple BEFORE any per-dispatch state
	 * fixup. This trace point lets post-processing correlate
	 * "Executing a cache" Python flakes with the preceding ~50
	 * dispatches' pool-slot reuse / cross-host-CPU migration window
	 * for the same guest task. The existing state-trace fields
	 * cpu/pid/task_mm_ptr/vcpu_current_mm already carry the data;
	 * KVMV2_OP_DISPATCH_LOCATION is just a labelled hook so dump
	 * post-processors can filter for "dispatch arrival" entries
	 * cheaply (op==20) without re-deriving "is this a vcpu_run
	 * entry" from the broader VCPU_RUN_ENTRY op (which is captured
	 * before vcpu is picked and therefore lacks vcpu.cpu / vcpu's
	 * current_mm).
	 */
	KVMV2_TRACE(KVMV2_OP_DISPATCH_LOCATION, regs, run, vcpu);

	/*
	 * D.0a: lazy first-run CPUID install. The eager install at
	 * vcpu_create_one was removed (kzalloc fails at init_backend
	 * time before the buddy allocator is up). By first KVM_RUN the
	 * buddy allocator is up and the install goes through. Sticky:
	 * one install per pool member for the lifetime of the VM.
	 *
	 * Failure here is fatal — the curated mask suppresses XSAVE /
	 * AVX / AVX-512 / FSGSBASE; without it the guest can issue VEX
	 * encodings whose state we don't snapshot under Phase C.4's
	 * legacy 512 B KVM_GET/SET_FPU path. Running on with KVM-default
	 * CPUID is a substrate-correctness violation, not a degradation.
	 */
	if (!vcpu->cpuid_primed) {
		struct kvm_v2_vm *vm = kvm_v2_vm_get();

		if (!vm)
			panic("kvm-v2: cpuid lazy install (cpu=%d): VM not initialised",
			      cpu);
		rc = kvm_v2_install_cpuid(vm, vcpu->vcpu_fd);
		if (rc < 0)
			panic("kvm-v2: cpuid lazy install (cpu=%d) failed: %d",
			      cpu, rc);
		/*
		 * SMP-T57 (memo state-audit/25 §3.1): with the curated
		 * CPUID now advertising XSAVE/OSXSAVE/AVX, arm CR4.OSXSAVE
		 * via a full GET+SET_SREGS pair (must be SYNCHRONOUS so
		 * KVM's vcpu->arch.cr4 has OSXSAVE live before the
		 * subsequent KVM_SET_XCRS validates against it — the
		 * SYNC_REGS path is deferred to next KVM_RUN, too late).
		 * Then install XCR0=FP|SSE|YMM. Do both inside the
		 * cpuid_primed=false window so the cost is paid once per
		 * vCPU.
		 */
		{
			struct kvm_sregs sregs2;

			rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_SREGS,
					      (unsigned long)&sregs2);
			if (rc < 0)
				panic("kvm-v2: SMP-T57 GET_SREGS pre-OSXSAVE (cpu=%d) failed: %d",
				      cpu, rc);
			sregs2.cr4 |= X86_CR4_OSXSAVE;
			rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_SREGS,
					      (unsigned long)&sregs2);
			if (rc < 0)
				panic("kvm-v2: SMP-T57 SET_SREGS+OSXSAVE (cpu=%d) failed: %d",
				      cpu, rc);
			/*
			 * Mirror the live cr4 into the SYNC_REGS mmap so the
			 * next dispatch's load_user_sregs's PGE-toggle XOR
			 * starts from the correct base.
			 */
			run->s.regs.sregs.cr4 = sregs2.cr4;
		}
		rc = kvm_v2_install_xcrs(vcpu->vcpu_fd);
		if (rc < 0)
			panic("kvm-v2: install_xcrs lazy (cpu=%d) failed: %d",
			      cpu, rc);
		vcpu->cpuid_primed = true;
	}

	/*
	 * Drain any pending UML-side TLB invalidations into the
	 * spawner mm BEFORE entering the guest. UML kernel updates to
	 * guest PTEs (CoW resolution, mremap, mprotect, etc.) are just
	 * writes into physmem from KVM's perspective — they do NOT
	 * fire kvm_arch_mmu_notifier callbacks because no host-mm
	 * mapping changed. So KVM's TDP cache will keep mapping the
	 * guest VA → old physmem page even after UML's PTE points to
	 * a fresh page.
	 *
	 * `current_mm_sync()` calls `um_tlb_sync(current->mm)` which
	 * walks the deferred-flush queue and applies each pending
	 * mremap/mprotect/munmap to the spawner mm via host syscalls.
	 * Those host-syscall mm operations are visible to KVM's
	 * mmu_notifier and force a TDP/EPT invalidation for the
	 * affected GPA range.
	 *
	 * seccomp's userspace() loop calls this at
	 * arch/um/backend/seccomp/trap_user.c:68 before set_stub_state.
	 * v1 called it at kvm-v1-archive/thread.c:3851 before its
	 * KVM_RUN. v2 was missing the equivalent — codex-gpt5.5-xhigh
	 * audit (memo §E.4 follow-up, 2026-04-30) traced this to the
	 * fork-tree-3level CHILD-side stack-corruption symptom: each
	 * un-flushed CoW left KVM mapping the child's stack VA to the
	 * pre-CoW (parent's) physmem page, so child's stack writes
	 * landed on parent's stack page, eventually clobbering the
	 * saved canary.
	 */
	{
		/*
		 * H.1b residual hardening: panic on um_tlb_sync failure
		 * mirrors v1 archive's pattern at kvm-v1-archive/
		 * thread.c:3851-3856 ("if (rc < 0) panic"). Codex audit
		 * flagged that current_mm_sync() (which v2 calls below)
		 * discards the return value at arch/um/kernel/skas/
		 * process.c:63-69 — a silent sync failure leaves stale
		 * TLB queue entries that the next dispatch's CR4.PGE
		 * toggle won't catch (it flushes guest TLB but doesn't
		 * apply UML's pending PTE updates).
		 *
		 * Wire the explicit um_tlb_sync + panic-on-error here so
		 * any silent sync regression surfaces loudly.
		 */
		if (current->mm) {
			int sync_rc = um_tlb_sync(current->mm);
			if (sync_rc < 0)
				panic("um: kvm-v2 vcpu_run: um_tlb_sync(mm=%p) failed (%d)",
				      current->mm, sync_rc);
		}
	}

	KVMV2_TRACE(KVMV2_OP_POST_TLB_SYNC, regs, run, vcpu);

	(void)kvm_v2_load_user_sregs(vcpu,
				     __pa(current->active_mm->pgd),
				     regs->gp[HOST_FS_BASE],
				     regs->gp[HOST_GS_BASE],
				     regs->gp[HOST_IP]);

	KVMV2_TRACE(KVMV2_OP_POST_LOAD_SREGS, regs, run, vcpu);

	/*
	 * C.4: install per-task FPU snapshot (set at fork by
	 * kvm_v2_fpu_capture_for_fork) or arch-reset values for a fresh
	 * task. Failure is fatal: running the guest with arbitrary FPU
	 * state is worse than aborting.
	 */
	rc = kvm_v2_fpu_install_on_first_run(vcpu);
	if (rc < 0)
		panic("kvm-v2: fpu install (cpu=%d) failed: %d", cpu, rc);

	/*
	 * H.1b residual fix: if last exit for THIS task was an exception
	 * delivered via IDT IST (handle_io_pf etc. snapshotted the frame),
	 * restore the IST stack from the per-task snapshot. Defends
	 * against cross-task IST clobber when multiple UML tasks share
	 * one per-host-CPU vCPU and each takes its own #PF — without
	 * this, the trampoline's iretq pops whichever frame was last
	 * pushed by ANY task on this vCPU, possibly jumping to the wrong
	 * task's CS:RIP.
	 */
	kvm_v2_ist_frame_restore_pending(vcpu);

	KVMV2_TRACE(KVMV2_OP_POST_IST_RESTORE, regs, run, vcpu);

	/*
	 * Memo §H.1b SMOKING-GUN fix (2026-04-30): restore the FPU
	 * snapshot taken IMMEDIATELY after the previous KVM_RUN exit.
	 * Pairs with the KVM_GET_FPU below at line ~1474. Without this,
	 * XMM/x87 state set by guest user code is clobbered with kernel-
	 * struct data while host code runs between dispatches —
	 * mt-xmmprobe.c captures `02020202...` → `16000000ffffffff
	 * 0000000000000000` drift directly.
	 */
	if (current->thread.arch.kvm_v2.iotrap_fpu_valid) {
		/*
		 * SMP-T73 (2026-05-18): KVM_SET_XSAVE (4 KB struct
		 * kvm_xsave), not KVM_SET_FPU (legacy 512 B FXSAVE).
		 * The legacy ioctl only restores x87 + XMM low 128;
		 * post-SMP-T57-Phase-A the guest uses AVX-256 ymm
		 * registers whose upper 128 lives in the extended
		 * XSAVE state. See processor_64.h header comment +
		 * Round 14 doc addendum.
		 */
		(void)os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_XSAVE,
				       (unsigned long)&current->thread.arch.kvm_v2.iotrap_fpu);
		current->thread.arch.kvm_v2.iotrap_fpu_valid = false;
		/*
		 * SMP-T55: vCPU's guest FPU now bit-identical to current's
		 * iotrap_fpu. Mark clean and record ownership; the post-
		 * vmexit GET below can skip if the guest didn't touch FPU.
		 */
		vcpu->fpu_dirty       = false;
		vcpu->fpu_owner_task  = current;
	}

	/*
	 * SMP-T75 (Round 14 HW-audit Q2): restore per-task pending-event
	 * snapshot before re-entering the guest. Mirrors the iotrap_fpu
	 * SET above. Without this, KVM's in-kernel pending-exception /
	 * interrupt-shadow / NMI / SMI queue for the previous task on
	 * this per-host-CPU vCPU bleeds into the current task's first
	 * vmentry. Dormant on the django soak workload (canonical #PF +
	 * #UD via gadget mostly inject before vmexit returns) but #DB /
	 * async #MC NMIs / SVM intercept-injects would fire it.
	 */
	if (current->thread.arch.kvm_v2.iotrap_events_valid) {
		(void)os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_VCPU_EVENTS,
				       (unsigned long)&current->thread.arch.kvm_v2.iotrap_events);
		current->thread.arch.kvm_v2.iotrap_events_valid = false;
	}

	KVMV2_TRACE(KVMV2_OP_POST_FPU_INSTALL, regs, run, vcpu);

	/*
	 * C.3: write GPRs into the mmap'd kvm_run->s.regs.regs and mark
	 * KVM_SYNC_X86_REGS in kvm_dirty_regs. KVM consumes both
	 * (kvm_dirty_regs and the dirty s.regs fields) on entry.
	 */
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	trace_um_backend_kvm_v2_vcpu_enter(cpu, run);

	KVMV2_TRACE(KVMV2_OP_PRE_KVM_RUN, regs, run, vcpu);

	/*
	 * SMP-T80 (state-audit/29): mark this host thread as "inside
	 * ioctl(KVM_RUN)" so sig_handler_common credits any timer tick
	 * that interrupts the ioctl to the guest task's utime (rather
	 * than the default stime).  Without this bracket, ticks default
	 * to stime, guest utime never advances, ITIMER_VIRTUAL never
	 * delivers SIGVTALRM.
	 *
	 * The first iteration of this fix called account_user_time()
	 * directly with an rdtsc delta — but the host tick handler was
	 * ALSO crediting stime via the unflipped is_user=0 default, so
	 * cumulative CPU time was double-counted (utime + stime ≈ 2 ×
	 * wall-clock).  This bracket is single-credit: the tick lands
	 * on utime once.  Sub-tick precision is lost (HZ=100 → 10 ms
	 * granularity); ITIMER_VIRTUAL workloads with thresholds ≥
	 * 100 ms still work, sub-tick callers do not.  The trade is
	 * worth the correct rusage accounting.
	 *
	 * Zero hot-path cost: os_kvm_run_enter/exit are two writes to
	 * a __thread variable.
	 */
#if IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_ITIMER_VIRTUAL)
	os_kvm_run_enter();
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_RUN, 0);
	os_kvm_run_exit();
#else
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_RUN, 0);
#endif

	KVMV2_TRACE(KVMV2_OP_POST_KVM_RUN, regs, run, vcpu);

	/*
	 * Memo §H.1b H.1b SMOKING-GUN fix (2026-04-30): KVM_GET_FPU
	 * IMMEDIATELY after KVM_RUN exits, before any host-kernel code
	 * could touch FPU/XMM and clobber the guest's state.
	 *
	 * Phase H.2 (2026-05-01): skip the GET_FPU when guest CR0.TS is
	 * still 1 — that means the in-guest #NM stub never fired, the
	 * guest didn't execute any FP/SSE/AVX instruction, and KVM's
	 * vcpu->arch.guest_fpu is unchanged from what we restored at the
	 * SET_FPU before run. iotrap_fpu remains valid from the previous
	 * non-skip dispatch.
	 *
	 * mt-xmmprobe.c proved XMM0 is corrupted across the #PF cycle when
	 * FPU was actively in use: pattern in XMM0 changes from
	 * `02020202...02` to `16000000ffff...` between fault and resume.
	 * For dispatches that DON'T touch FPU (the common syscall case),
	 * the GET is wasted work — this skip recovers that cost.
	 */
	{
		/*
		 * SMP-T26 fix (2026-05-02): always KVM_GET_FPU after KVM_RUN.
		 *
		 * The original Phase H.2 optimization at this site SKIPPED the
		 * KVM_GET_FPU when guest CR0.TS=1 (FPU not touched), preserving
		 * the per-task `iotrap_fpu`/`iotrap_fpu_valid` from the prior
		 * dispatch. This is correct in a SINGLE-task vCPU model — but
		 * v2 uses a per-host-CPU vCPU pool, where multiple UML tasks
		 * share one vCPU.
		 *
		 * Bug captured by SMP-T26 trace (run-with-bug-t26.log):
		 *   1. Task X enters KVM_RUN, install at line 1772 consumes
		 *      iotrap_fpu_valid (set to false).
		 *   2. KVM_RUN exits with TS=1 (no FPU) → KVM_GET_FPU SKIPPED
		 *      → iotrap_fpu_valid stays false.
		 *   3. Task Y context-switches in, runs FPU-touching code,
		 *      modifies the per-host-CPU vCPU's FPU state.
		 *   4. Task X re-dispatches: install_on_first_run finds both
		 *      fpu_valid=false AND iotrap_fpu_valid=false → no-op.
		 *      vCPU FPU now has Task Y's data.
		 *   5. Task X executes XMM op via #NM → handler clears TS
		 *      (kvm_v2_handle_io_nm) but does NOT restore FPU.
		 *   6. Task X re-executes XMM op with Task Y's leftover XMM —
		 *      e.g., glibc's MOVUPS at _int_malloc 0x41646a writes
		 *      partial-stale data to chunk fd+bk → bk=0 →
		 *      deterministic SIGSEGV at _int_malloc bin walk.
		 *
		 * Fix: always capture FPU after every KVM_RUN exit. The cost
		 * is one ioctl per dispatch (~1µs); the benefit is preventing
		 * cross-task XMM corruption that drives the 0.15%/fork
		 * threaded-fork-malloc residual. Pair with kvm_v2_handle_io_nm
		 * also restoring per-task FPU when iotrap_fpu_valid is true
		 * (separate fix in syscall_trap.c).
		 */
		/*
		 * SMP-T55 (2026-05-07): gate the GET on the per-vCPU
		 * dirty epoch. Skip only when (a) the vCPU's FPU has
		 * not been written since our last SET/GET (fpu_dirty=false)
		 * AND (b) the slot we'd write into still belongs to current
		 * (fpu_owner_task==current). Both arms must hold; the
		 * owner_task check is belt-and-suspenders against any
		 * path that mutates guest_fpu without setting fpu_dirty.
		 *
		 * Mark dirty before deciding: if KVM_RUN exited with
		 * CR0.TS=0 the guest issued an FP/SSE/AVX instruction;
		 * the GET must run to capture the resulting state.
		 *
		 * Phase 2 record/replay extension (memo 27 §3.8(i)): when
		 * `um_kvm_v2_record_enabled` is on, force the GET on every
		 * vmexit regardless of the dirty epoch. Rationale: replay
		 * needs the post-vmexit XSAVE to be bit-identical to record;
		 * the SMP-T55 skip optimization preserves the vCPU's FPU
		 * across dispatches in a way the replay machine doesn't
		 * model (the per-task iotrap_fpu lifecycle interacts with
		 * the snapshot/replay state machine in ways SMP-T55 didn't
		 * contemplate — see memo 27 §3.8). The static_branch gate
		 * makes this zero-cost when record is off (the common case);
		 * when on, every vmexit pays the ~1µs KVM_GET_FPU. Phase 2-3
		 * round-trip acceptance requires this; Phases 2.5/5/6 may
		 * revisit if the cost becomes prohibitive at scale.
		 */
		bool fpu_was_used = !(run->s.regs.sregs.cr0 & X86_CR0_TS);

		if (fpu_was_used)
			vcpu->fpu_dirty = true;

		if (vcpu->fpu_dirty || vcpu->fpu_owner_task != current ||
		    static_branch_unlikely(&um_kvm_v2_record_enabled)) {
			/*
			 * SMP-T73 (2026-05-18): KVM_GET_XSAVE (4 KB struct
			 * kvm_xsave), not KVM_GET_FPU (legacy 512 B FXSAVE).
			 * Per-dispatch save must include YMM upper 128 to
			 * prevent cross-task XMM-upper leak through the
			 * per-host-CPU vCPU pool. This is the cache-flake
			 * root cause; see processor_64.h header comment.
			 */
			struct kvm_xsave *iotrap = &current->thread.arch.kvm_v2.iotrap_fpu;
			int fpu_rc;

			fpu_rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_XSAVE,
						  (unsigned long)iotrap);
			current->thread.arch.kvm_v2.iotrap_fpu_valid = (fpu_rc == 0);
			if (fpu_rc == 0) {
				vcpu->fpu_dirty       = false;
				vcpu->fpu_owner_task  = current;
			}
		}
		/*
		 * else: skip — the vCPU's guest FPU is still bit-identical
		 * to current->thread.arch.kvm_v2.iotrap_fpu (set by the
		 * pre-run KVM_SET_FPU above, or by the previous dispatch's
		 * post-vmexit GET, or — if the very first dispatch — never
		 * because we start fpu_dirty=true and force the first GET).
		 * iotrap_fpu_valid carries over from the last successful GET,
		 * so the next dispatch's pre-run SET still finds it true and
		 * re-installs.
		 */
	}

	/*
	 * SMP-T75 (Round 14 HW-audit Q2): capture per-task pending-event
	 * snapshot AFTER KVM_RUN exits, mirroring iotrap_fpu. Storing
	 * the GET result in arch_thread lets the next dispatch's
	 * KVM_SET_VCPU_EVENTS re-install it before the next KVM_RUN —
	 * preventing the per-host-CPU vCPU pool from leaking pending
	 * exceptions / NMI / SMI / interrupt-shadow state across tasks.
	 */
	{
		struct kvm_vcpu_events *iotrap_ev =
			&current->thread.arch.kvm_v2.iotrap_events;
		int ev_rc;

		ev_rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_VCPU_EVENTS,
					 (unsigned long)iotrap_ev);
		current->thread.arch.kvm_v2.iotrap_events_valid = (ev_rc == 0);
	}

	/*
	 * SNAPSHOT the kvm_run mmap state IMMEDIATELY after KVM_RUN
	 * returns, BEFORE unblock_signals(). Codex (gpt-5.5 xhigh)
	 * audit (memo §E.4 followup, 2026-04-30) flagged this as the
	 * residual fork-tree-3level / child_delay bug after the TLB-
	 * flush fix at 11102c8176fb landed.
	 *
	 * Sequence v2 was using:
	 *   KVM_RUN → unblock_signals → read run->{exit_reason,s.regs}
	 *
	 * The window between unblock_signals and the read can host:
	 *   1. SIGALRM-driven timer tick (timer_real_alarm_handler at
	 *      arch/um/os-Linux/signal.c:373 → timer IRQ at
	 *      arch/um/kernel/time.c:770) which can wake other tasks
	 *      and set TIF_NEED_RESCHED.
	 *   2. preempt_schedule_irq on the way out of unblock_signals
	 *      may switch `current` to a different UML task on the
	 *      same per-host-CPU vCPU.
	 *   3. Another KVM_RUN exit on the same vCPU mmap may occur
	 *      via the new task's userspace() → vcpu_run, overwriting
	 *      the mmap before we read it.
	 *
	 * v1 handled this by snapshotting kvm_regs / kvm_sregs into
	 * locals before its own unblock_signals (kvm-v1-archive/
	 * thread.c:3937-3979). Mirror the same shape here.
	 */
	{
		struct kvm_regs eintr_regs = run->s.regs.regs;
		struct kvm_sregs eintr_sregs = run->s.regs.sregs;
		u32 exit_reason_snap = run->exit_reason;

		/*
		 * D.5-fix-2: drain UML's deferred-signal queue. v1 archive
		 * does this at kvm-v1-archive/thread.c:3979 — without it,
		 * SIGALRM-handler-deferred work (timer tick processing,
		 * scheduler yields, etc.) stays queued and we re-EINTR on
		 * every dispatch.
		 *
		 * Symmetric: signals are blocked at the host-thread level
		 * during KVM_RUN via KVM_SET_SIGNAL_MASK; SIGALRM is the
		 * one exception (timer preemption). On EINTR-from-SIGALRM,
		 * the host signal handler queued work via UML's irqflags
		 * machinery; unblock_signals here drains it.
		 */
		unblock_signals();

		exit_reason = exit_reason_snap;
		trace_um_backend_kvm_v2_vcpu_exit(cpu, exit_reason);

		if (rc < 0) {
			if (rc == -EINTR) {
				KVMV2_TRACE(KVMV2_OP_EINTR_PATH, regs, run, vcpu);
				kvm_v2_marshal_from_kvm_regs(regs, &eintr_regs);
				kvm_v2_marshal_sregs_back(regs, &eintr_sregs);

				/*
				 * Round 2 Django investigation (2026-05-17):
				 * detect stuck-in-EINTR-loop. A same-task
				 * repeating EINTR with no intervening
				 * KVM_EXIT_IO means the in-guest CPU is making
				 * zero forward progress — either spinning at a
				 * legitimate RIP (unlikely; SIGALRM ticks would
				 * normally interrupt forward-progress code at
				 * different RIPs) or faulting at a VA whose
				 * IDT-delivery pathway is broken (the captured
				 * Django failure window from memo §44 Round 2).
				 *
				 * Bump the counter on same-task EINTR; reset on
				 * cross-task or on the non-EINTR exit path
				 * below. Freeze the state-trace ring once the
				 * counter crosses the threshold so the dump
				 * captures the moment progress stopped, not
				 * SERVER_FAIL's flood that comes 30+ seconds
				 * later.
				 *
				 * One-shot via kvm_v2_state_trace_freeze's
				 * cmpxchg; subsequent EINTR loops on the same
				 * boot just bump the counter without re-firing.
				 */
				if (vcpu->eintr_run_task == current) {
					vcpu->eintr_run_count++;
				} else {
					vcpu->eintr_run_task  = current;
					vcpu->eintr_run_count = 1;
				}
				if (kvm_v2_eintr_loop_threshold &&
				    vcpu->eintr_run_count ==
				    kvm_v2_eintr_loop_threshold) {
					char reason[128];

					snprintf(reason, sizeof(reason),
						 "eintr-loop pid=%d count=%u rip=%llx cr2=%llx cr3=%llx",
						 current ? current->pid : 0,
						 vcpu->eintr_run_count,
						 (unsigned long long)eintr_regs.rip,
						 (unsigned long long)eintr_sregs.cr2,
						 (unsigned long long)eintr_sregs.cr3);
					kvm_v2_state_trace_freeze(reason);
				}
				/*
				 * #121-D15 fix (2026-05-01): if EINTR caught us
				 * with RIP pointing at an in-guest exception
				 * handler stub (CPU IDT-delivered the fault and
				 * set RIP=KVM_V2_HANDLERS_GVA+slot*64, but the
				 * stub's first instruction hasn't run yet, OR
				 * the stub is mid-execution), preserve sregs.cr2
				 * — VMCB.save.cr2 holds the real fault address
				 * from the IDT delivery and the next dispatch's
				 * load_user_sregs would otherwise zero it, making
				 * the stub's `mov %cr2, %rax` capture 0. See the
				 * arch_thread.kvm_v2.saved_cr2_at_eintr comment
				 * for the full mechanism.
				 */
				/*
				 * #121-D15 SMP fix (2026-05-01): if EINTR
				 * caught us mid-IDT-delivery for a #PF (stub
				 * is at slot 5 = HANDLERS_GVA + 0x140; stride
				 * 64 bytes so the in-stub PC is in
				 * [HANDLERS_GVA+0x140, +0x180)), process the
				 * PF inline NOW — while we're guaranteed to be
				 * preempt-disabled on the same vCPU whose IST
				 * stack holds the freshly-pushed IDT frame and
				 * whose VMCB.save.cr2 was just snapshotted into
				 * eintr_sregs.cr2. Bypassing the stub avoids
				 * the cross-vCPU RSP/IST headache that the
				 * original snapshot-and-replay approach had
				 * under SMP (the saved RSP pointed at the
				 * vmexit-vCPU's IST page, but on the next
				 * dispatch the task could land on a different
				 * vCPU with a different IST page).
				 *
				 * For other handler stubs (DE/BP/OF/UD/GP/NM),
				 * fall back to the snapshot/replay path — those
				 * are uncommon in practice (mt-byteset only
				 * trips PF) and the snapshot still gives
				 * correct contents on the next dispatch's IST
				 * stack; the RSP-points-at-old-vCPU concern is
				 * theoretical until a workload demonstrates a
				 * mid-stub EINTR on those vectors.
				 */
				if (eintr_regs.rip >= KVM_V2_LSTAR_GVA &&
				    eintr_regs.rip <  KVM_V2_LSTAR_GVA +
				                      (kvm_v2_lstar_gadget_end -
				                       kvm_v2_lstar_gadget_start)) {
					/*
					 * SMP-T25 (2026-05-02) + SMP-T56
					 * (2026-05-04) Bug B residual fix:
					 * EINTR caught the guest somewhere in
					 * the LSTAR body. There are two regimes:
					 *
					 * (a) RIP in [LSTAR, LSTAR+3): pre-entry
					 *     -swapgs. Same shape as the original
					 *     SMP-T25 fix for the 5-byte fallback
					 *     (`out + sysretq`) — HOST_IP was not
					 *     redirected, so on the next dispatch
					 *     marshal_to_kvm_regs would write
					 *     kvm_run.rip = LSTAR with USER CS,
					 *     causing an instruction-fetch fault
					 *     on the kernel-only LSTAR page
					 *     (BUG_B err=0x15 class). Rewind
					 *     HOST_IP to user SYSCALL retry
					 *     (HOST_CX - 2; SYSCALL is 2 bytes,
					 *     opcode 0F 05). MSR_KERNEL_GS_BASE
					 *     is unchanged here (entry-swapgs
					 *     hasn't run).
					 *
					 * (b) RIP >= LSTAR+3: post-entry-swapgs.
					 *     The CPU has hardware-swapped
					 *     GS_BASE ↔ MSR_KERNEL_GS_BASE: now
					 *     GS_BASE = STATE_GVA(cpu) and
					 *     MSR_KERNEL_GS_BASE = user_gs (≈0
					 *     for typical x86_64 userspace).
					 *     Without recovery, the *next* task
					 *     to dispatch on this vCPU enters
					 *     SYSCALL with KERNEL_GS_BASE=0; its
					 *     swapgs lands GS_BASE=0; the
					 *     gadget's `mov %rdx, %gs:0x50` then
					 *     faults at virtual address 0x50
					 *     with err=2 (P=0/W=1/U=0). That is
					 *     the BUG_B regression observed
					 *     post-gadget-revival (mt-mini SMP
					 *     N=400: 10/400 SIGSEGV-via-bad-GS).
					 *     Recovery: KVM_SET_MSRS to restore
					 *     MSR_KERNEL_GS_BASE = STATE_GVA(cpu)
					 *     so the vCPU is left in a clean
					 *     state regardless of which task
					 *     dispatches next, then rewind RIP
					 *     to user SYSCALL retry.
					 *
					 * In both regimes we rewind to user
					 * SYSCALL retry — the gadget body is
					 * idempotent (rerunning is fine), and
					 * the MSR fix paired with the rewind
					 * means the retry restarts in a known
					 * good state on whichever vCPU/task
					 * runs next.
					 */
					KVMV2_TRACE(KVMV2_OP_EINTR_INLINE_LSTAR,
						    regs, run, vcpu);
					if (eintr_regs.rip >= KVM_V2_LSTAR_GVA + 3) {
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
						(void)os_ioctl_generic(vcpu->vcpu_fd,
								       KVM_SET_MSRS,
								       (unsigned long)&req);
					}
					/*
					 * SMP-T58 (Round 9, 2026-05-18): recover
					 * user RDX/R8/R10 from gadget state-page
					 * SAVE slots when EINTR caught the body
					 * AFTER the corresponding entry-save
					 * instruction completed.
					 *
					 * Without this, scratch writes inside
					 * h_time / h_getcpu / h_clock_gettime
					 * (MONO_SEC into R10, MONO_NSEC into R8,
					 * REAL_SEC into RDX) leak into eintr_regs
					 * and propagate as "user" GPRs on the
					 * next dispatch, violating Linux x86_64
					 * syscall ABI (RDX/R8/R10 preserved
					 * across SYSCALL). Same shape as T41's
					 * PF-stub RAX recovery from IST top-56
					 * (commit af659ad4297d).
					 *
					 * Entry-save byte offsets are locked by
					 * test_byteshape.c::
					 * test_lstar_gadget_entry_sequence:
					 *   swapgs           ends at LSTAR + 3
					 *   SAVE_RDX (movq)  ends at LSTAR + 12
					 *   SAVE_R8  (movq)  ends at LSTAR + 21
					 *   SAVE_R10 (movq)  ends at LSTAR + 30
					 */
					if (vcpu->gadget_state_kva) {
						u8 *page = (u8 *)vcpu->gadget_state_kva;

						if (eintr_regs.rip >= KVM_V2_LSTAR_GVA + 12)
							regs->gp[HOST_DX] =
								*(u64 *)(page + KVM_V2_GADGET_OFF_SAVE_RDX);
						if (eintr_regs.rip >= KVM_V2_LSTAR_GVA + 21)
							regs->gp[HOST_R8] =
								*(u64 *)(page + KVM_V2_GADGET_OFF_SAVE_R8);
						if (eintr_regs.rip >= KVM_V2_LSTAR_GVA + 30)
							regs->gp[HOST_R10] =
								*(u64 *)(page + KVM_V2_GADGET_OFF_SAVE_R10);
					}
					{
						static atomic64_t lstar_eintr_count;
						long n = atomic64_inc_return(&lstar_eintr_count);
						if (n == 1 || (n & 0xff) == 0)
							pr_info("um: kvm-v2 SMP-T25/T56 LSTAR-EINTR rewind #%ld pid=%d comm=%s rip=%llx rcx=%llx (%s-swapgs)\n",
								n, current->pid, current->comm,
								(unsigned long long)eintr_regs.rip,
								(unsigned long long)eintr_regs.rcx,
								eintr_regs.rip >= KVM_V2_LSTAR_GVA + 3 ? "post" : "pre");
					}
					regs->gp[HOST_IP] = regs->gp[HOST_CX] - 2;
				} else if (eintr_regs.rip >= KVM_V2_HANDLERS_GVA + 0x140 &&
				    eintr_regs.rip <  KVM_V2_HANDLERS_GVA + 0x180) {
					/* Re-marshal regs from eintr snapshot
					 * so the inline handler sees consistent
					 * GP register state (gp[HOST_DX] etc.
					 * for the segv_handler dispatch).
					 */
					KVMV2_TRACE(KVMV2_OP_EINTR_INLINE_PF,
						    regs, run, vcpu);
					(void)kvm_v2_handle_pf_eintr_inline(regs, run, vcpu,
									    eintr_sregs.cr2);
				} else if (eintr_regs.rip >= KVM_V2_HANDLERS_GVA + 0x1c0 &&
					   eintr_regs.rip <  KVM_V2_HANDLERS_GVA + 0x200) {
					/*
					 * SMP-T17 (2026-05-02) Bug B fix: slot 7 = #NM
					 * stub (clts; iretq). EINTR caught us before the
					 * stub's iretq pop. The stub-replay path is
					 * structurally fragile: between snapshot and
					 * resume, OTHER tasks on this vCPU push their
					 * own iretq frames into IST top-40..top, and
					 * restore_pending's writeback can land on a CR3
					 * (new task's mm) where the IST GVA aliases a
					 * different physical page than at snapshot time.
					 * The stub's iretq then pops bytes that include
					 * the kernel-half NM_stub address in the RIP
					 * slot, transitions to CPL=3, and immediately
					 * faults trying to fetch from US=0 (Bug B).
					 *
					 * Process #NM inline: clear CR0.TS in sregs
					 * (= what `clts` would do) and restore user
					 * regs from the IST frame (5 qwords, no error
					 * code). The next KVM_RUN re-enters at the user
					 * RIP — NOT at the stub.
					 */
					KVMV2_TRACE(KVMV2_OP_EINTR_INLINE_PF,
						    regs, run, vcpu);
					(void)kvm_v2_handle_nm_eintr_inline(regs, run, vcpu);
				} else if (eintr_regs.rip >= KVM_V2_HANDLERS_GVA &&
					   eintr_regs.rip <  KVM_V2_HANDLERS_GVA + 0x200) {
					struct arch_thread *a = &current->thread.arch;
					a->kvm_v2.saved_cr2_at_eintr = eintr_sregs.cr2;
					a->kvm_v2.saved_cr2_valid = true;
					kvm_v2_ist_frame_snapshot_raw(vcpu);
					KVMV2_TRACE(KVMV2_OP_EINTR_RAW_SNAPSHOT,
						    regs, run, vcpu);
				}
				trace_um_backend_kvm_v2_vcpu_eintr(cpu);
				/*
				 * Memo §H.1b: even on EINTR, KVM_RUN entry
				 * executed the CR4.PGE flush. Pages deferred
				 * during prior dispatches' exit handling are
				 * safe to release. See comment on the post-
				 * marshal drain below for the full timing
				 * argument.
				 */
				if (current->mm)
					um_mmu_gather_drain(current->mm);
				migrate_enable();
				/*
				 * v1 archive (kvm-v1-archive/thread.c:5169-5177)
				 * called interrupt_end() after the EINTR marshal-
				 * back to drain pending scheduler/signal work.
				 * Without it, accumulated SIGALRM-driven scheduler
				 * ticks build up unfulfilled — after ~50
				 * sched_yield-style EINTR cycles the test fails
				 * with rc=255 even though the user-mode code
				 * called exit_group(0). Codex (gpt-5.5 xhigh)
				 * audit, memo §E.4 follow-up.
				 */
				interrupt_end();
				return;
			}
			panic("kvm-v2: KVM_RUN(cpu=%d) failed: %d (exit_reason=%u)",
			      cpu, rc, exit_reason);
		}

		/*
		 * C.3: post-exit, KVM populated kvm_run->s.regs.{regs,
		 * sregs} because kvm_valid_regs was set at vcpu_create.
		 * Marshal GPRs back from the SNAPSHOT (which preceded
		 * unblock_signals), not from the live mmap which may have
		 * been overwritten by a context-switched task's KVM_RUN.
		 */
		kvm_v2_marshal_from_kvm_regs(regs, &eintr_regs);
		kvm_v2_marshal_sregs_back(regs, &eintr_sregs);

		/*
		 * Round 2 Django investigation (2026-05-17): a non-EINTR
		 * KVM_RUN return path means the in-guest CPU made forward
		 * progress (KVM_EXIT_IO is the only other exit reason
		 * dispatched below; HLT / FAIL_ENTRY / INTERNAL_ERROR all
		 * panic). Reset the EINTR-loop counter — the captured
		 * stuck-task signal we look for is "consecutive EINTR with
		 * no intervening vmexit on the same task", not "EINTR ever".
		 */
		vcpu->eintr_run_task  = NULL;
		vcpu->eintr_run_count = 0;
	}

	/*
	 * Memo §H.1b residual fix: drain the per-mm deferred-free
	 * queue. Pages added to this queue by mm/mmu_gather.c during
	 * a previous dispatch's exit handling have, by now, seen at
	 * least one CR4.PGE-toggled KVM_RUN entry — namely the one
	 * that just exited above. So the guest CPU's TLB no longer
	 * caches stale translations to those PFNs; safe to release
	 * back to buddy.
	 *
	 * Pages added to the queue DURING this dispatch's exit
	 * handling (the switch (exit_reason) block below) wait for
	 * the NEXT dispatch's KVM_RUN to flush — they get drained at
	 * THAT dispatch's drain call here.
	 */
	if (current->mm)
		um_mmu_gather_drain(current->mm);

	switch (exit_reason) {
	case KVM_EXIT_IO:
		/*
		 * D.2 + E.3: IO-port exit. The LSTAR trampoline (D.1)
		 * issues `out %al, $0xf4` from CPL=0 for SYSCALL; E.1's
		 * IDT handler stubs issue `out %al, $port` for each
		 * exception class (port table at syscall_trap.h's
		 * enum um_kvm_iotrap). Phase E.3 widened the helper to
		 * dispatch by port: SYSCALL → handle_syscall;
		 * #PF/#GP/#UD/#DE/#OF → segv_handler/relay_signal per
		 * class; UM_KVM_TRAP_PANIC + any unknown port → panic.
		 *
		 * Pass the full vCPU struct (not just the fd) so the
		 * exception dispatchers can read vcpu->ist_stack_kva
		 * to extract the iretq frame the CPU pushed onto the
		 * IST stack at exception delivery time.
		 */
		rc = kvm_v2_handle_io_trap(regs, run, vcpu);
		if (rc < 0)
			panic("kvm-v2: io_trap (cpu=%d port=%#x) failed: %d",
			      cpu, run->io.port, rc);

		/*
		 * SMP-T78: kvm_v2_handle_io_trap releases migrate_disable
		 * around handle_syscall (so sched_setaffinity can succeed).
		 * On return, the task may be on a different host CPU.
		 * Re-fetch cpu/vcpu/run so the post-trap KVMV2_TRACE below
		 * and the matching migrate_enable record correct values.
		 * Re-fetch is safe even when no migration happened (idempotent
		 * on the same CPU).
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
		panic("kvm-v2: unexpected exit %u from KVM_RUN; Phase D/E provides the dispatch",
		      exit_reason);
	default:
		pr_warn_ratelimited("kvm-v2: unhandled exit_reason=%u (cpu=%d)\n",
				    exit_reason, cpu);
		panic("kvm-v2: unhandled exit %u from KVM_RUN; Phase D/E provides the dispatch",
		      exit_reason);
	}

	KVMV2_TRACE(KVMV2_OP_VCPU_RUN_EXIT, regs, run, vcpu);

	migrate_enable();
}

/*
 * Phase C.4: fork-time FPU capture. arch_copy_thread (processor_64.h /
 * processor_32.h) calls this on every task fork once
 * CONFIG_UM_BACKEND_KVM_V2=y. Snapshots the parent's per-host-CPU vCPU
 * FPU state into to->kvm_v2.fpu so the child's first KVM_RUN restores
 * it via kvm_v2_fpu_install_on_first_run — POSIX fork() requires FPU
 * inheritance.
 *
 * Reshape vs v1 archive (kvm_fpu_capture_for_fork in
 * kvm-v1-archive/thread.c:275): v1 keyed on `from->kvm.vcpu->fd` (the
 * parent task's per-task vcpu_fd, whose lifetime matched the task).
 * v2's per-host-CPU pool means the parent's "current vCPU" is whichever
 * pool entry was running when fork fires; we therefore preempt_disable
 * around smp_processor_id() + kvm_v2_vcpu_get() so the pick stays
 * coherent with the FD we issue KVM_GET_FPU against. arch_copy_thread
 * runs from the fork path on the parent's kernel stack with preempt
 * enabled, so the disable here is the helper's own — not nested.
 *
 * NULL guard: if the pool isn't up yet (pre-init_backend forks during
 * kthreadd bring-up, or fallback after backend = seccomp), or the
 * per-CPU slot is at the .vcpu_fd = -1 sentinel, leave fpu_valid=false
 * and return 0. Failure is non-fatal — child will get the
 * architectural reset values on first run, mirroring v1's behaviour
 * (KVM_GET_FPU failure → "child gets KVM-default FPU" not panic).
 */
int kvm_v2_fpu_capture_for_fork(struct arch_thread *from,
				struct arch_thread *to)
{
	struct kvm_v2_vcpu *vcpu;
	int cpu, rc;

	(void)from;	/* parent's snapshot lives on the per-CPU vCPU, not in `from` */

	/* SMP-T13 (2026-05-02): migrate_disable() — preempt_disable is
	 * a no-op without CONFIG_PREEMPT_COUNT. See vcpu_run for full
	 * background. The KVM_GET_FPU ioctl is short and unlikely to
	 * yield, but use migrate_disable for symmetry + safety. */
	migrate_disable();
	cpu  = smp_processor_id();
	vcpu = kvm_v2_vcpu_get(cpu);
	if (!vcpu || vcpu->vcpu_fd < 0) {
		/* No parent vCPU to snapshot — child starts from arch defaults. */
		to->kvm_v2.fpu_valid = false;
		trace_um_backend_kvm_v2_fpu_capture(cpu, 0);
		migrate_enable();
		return 0;
	}

	/*
	 * SMP-T73 (2026-05-18): KVM_GET_XSAVE (4 KB struct kvm_xsave),
	 * not KVM_GET_FPU (legacy 512 B FXSAVE). Fork-side capture must
	 * include YMM upper 128 so the child's first dispatch's
	 * KVM_SET_XSAVE restores the full parent FPU state, including
	 * any AVX-256 register contents glibc's IFUNC variants depend on.
	 */
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_XSAVE,
			      (unsigned long)&to->kvm_v2.fpu);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm-v2 fpu_capture_for_fork: KVM_GET_XSAVE(cpu=%d vcpu_fd=%d) failed (%d) — child gets arch-default FPU\n",
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
 * Phase D.3: per-task FPU capture on context-switch-out. C.4 only
 * captures at fork (arch_copy_thread). Tasks migrating between host
 * CPUs via the UML scheduler need their FPU snapshot to follow them
 * — otherwise the destination per-CPU vCPU has whichever FPU the last
 * task on that vCPU left behind. Even on a UP build (NR_CPUS_DEFAULT=1)
 * the capture is correct: it snapshots the outgoing task's per-CPU vCPU
 * FPU into the task's arch_thread so the task's NEXT first-run installs
 * the snapshot via kvm_v2_fpu_install_on_first_run (C.4); the intervening
 * task on the same vCPU runs through its own first-run install path.
 *
 * Hooked from kvm_v2_context_switch (ops.c) BEFORE seccomp's
 * context_switch runs. We snapshot FROM's per-CPU vCPU into
 * from->thread.arch.kvm_v2.fpu so its first run on its next destination
 * CPU restores the snapshot via kvm_v2_fpu_install_on_first_run (C.4).
 *
 * Mirrors kvm_v2_fpu_capture_for_fork's preempt_disable + per-CPU pool
 * pick + NULL-or-sentinel guard. Failure is non-fatal: the warning
 * lands in dmesg ratelimited; the task migrates without an FPU snapshot
 * (its first run on the destination CPU will install architectural
 * reset values via kvm_v2_fpu_install_on_first_run's else-branch).
 *
 * v1 archive's equivalent: kvm-v1-archive/thread.c:240-339 documents
 * the per-task FPU-capture-and-install pair. v1 keyed off `from->kvm.
 * vcpu->fd` (per-task vCPU model) — v2's per-host-CPU pool means we
 * capture from whichever pool entry was running when context_switch
 * fires, and the task carries its snapshot to whatever destination CPU
 * it lands on next.
 */
void kvm_v2_fpu_capture_for_switch_out(struct task_struct *from)
{
	struct kvm_v2_vcpu *vcpu;
	int cpu, rc;

	if (!from)
		return;

	/* SMP-T13 (2026-05-02): migrate_disable() — see vcpu_run. */
	migrate_disable();
	cpu  = smp_processor_id();
	vcpu = kvm_v2_vcpu_get(cpu);
	if (!vcpu || vcpu->vcpu_fd < 0) {
		/*
		 * SMP-T29 (2026-05-03): do NOT clear from->fpu_valid here.
		 * If pool isn't up, leave any prior snapshot intact (e.g.
		 * from kvm_v2_fpu_capture_for_fork on a freshly-fork'd task
		 * whose snapshot is still authoritative).
		 *
		 * Original code cleared fpu_valid=false here, which would
		 * have been correct under the assumption that from has run
		 * since fork. For a never-yet-dispatched freshly-fork'd
		 * task, that assumption is wrong and the parent-snapshot
		 * is the right state to install on first run.
		 */
		migrate_enable();
		return;
	}

	/*
	 * SMP-T29 (2026-05-03) ROOT-CAUSE FIX: only capture when the
	 * per-host-CPU vCPU's FPU state actually belongs to `from`.
	 *
	 * Bug captured by 30-boot soak: a freshly-fork'd task carrying
	 * `kvm_v2_fpu_capture_for_fork`'s parent-snapshot
	 * (fpu_valid=true) can be context-switched OUT before its very
	 * first KVM_RUN dispatch (e.g., SIGALRM preempt while inside
	 * fork_handler). Without this gate, the unconditional KVM_GET_FPU
	 * below would overwrite the pristine parent-snapshot with
	 * whatever the per-CPU vCPU's FPU happens to hold (= some other
	 * task's state). On the eventual first dispatch,
	 * kvm_v2_fpu_install_on_first_run would then restore that
	 * garbage as if it were the parent's FPU. The freshly-fork'd
	 * task runs with corrupted XMM, glibc's __libc_fork
	 * post-fork-cleanup MOVAPS writes the wrong stack values, and
	 * the next iteration of glibc's _dl_stack_used walk faults at
	 * `__fork+0x11c (mov 0x3d8(%r15),%rax)` with cr2=0x3d8 (r15
	 * dereferenced as NULL+0x3d8). Once the first child cascades,
	 * the parent's worker pthread's own _dl_stack_used walks corrupt
	 * the same way → 2675 sequential CHILD_FAIL events in one boot.
	 *
	 * Gate: vcpu->last_task is set in load_user_sregs at every
	 * dispatch entry. "last_task == from" ⇔ "the most recent KVM_RUN
	 * on this per-host-CPU vCPU was from's, and KVM_GET_FPU here
	 * reads from's actual FPU." If false, leave any existing
	 * snapshot intact.
	 */
	if (vcpu->last_task != from) {
		/* Per-CPU vCPU's FPU does NOT reflect `from`. Preserve
		 * any existing fpu_valid=true (set by capture_for_fork or
		 * an earlier valid capture). */
		migrate_enable();
		return;
	}

	/*
	 * SMP-T73 (2026-05-18): KVM_GET_XSAVE (4 KB struct kvm_xsave),
	 * not KVM_GET_FPU (legacy 512 B FXSAVE). Switch-out capture
	 * must include YMM upper 128 to preserve cross-vCPU task
	 * migration of full AVX state. See processor_64.h header.
	 */
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_XSAVE,
			      (unsigned long)&from->thread.arch.kvm_v2.fpu);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm-v2 fpu_capture_for_switch_out: KVM_GET_XSAVE(cpu=%d vcpu_fd=%d) failed (%d) — preserving prior snapshot if any\n",
				    cpu, vcpu->vcpu_fd, rc);
		/* SMP-T29: do NOT clear fpu_valid here either — preserve
		 * any prior valid snapshot. */
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
 * Phase D.3: v2's context_switch op wraps seccomp's. Capture parent's
 * FPU into arch_thread before delegating to seccomp (which does the
 * stub-child swap and turnstile work via switch_threads on the
 * per-process jmp_buf). After D.5's pointer flip, the child's first
 * kvm_v2_vcpu_run installs the snapshot via
 * kvm_v2_fpu_install_on_first_run (C.4 already wires this).
 *
 * Today (D.3) the FPU capture runs at every UML context switch under
 * v2 backend selection, regardless of whether the child runs through
 * KVM (.vcpu_run is still seccomp_vcpu_run until D.5). That is
 * intentional per memo 26 §D.3: D.3 ships with a working observable
 * behaviour — `trace-cmd -e um_backend:um_backend_kvm_v2_fpu_capture`
 * shows events firing during normal scheduling. The seccomp delegation
 * still drives actual guest progress.
 */
void kvm_v2_context_switch(struct task_struct *from, struct task_struct *to)
{
	kvm_v2_fpu_capture_for_switch_out(from);

	/*
	 * Drain `from->active_mm` BEFORE delegating to seccomp so any
	 * deferred TLB syncs from `from`'s last user-mode session land
	 * in the spawner mm now (visible to KVM's mmu_notifier) rather
	 * than carrying over to `to`'s first dispatch on the same vCPU.
	 *
	 * v1 archive (kvm-v1-archive/thread.c:412-423) did the same
	 * before its switch_threads. Without it, accumulated NEEDSYNC
	 * PTEs on `from`'s mm leak into `to`'s view of physmem on the
	 * shared per-host-CPU vCPU. After many context switches (e.g.
	 * sched_yield-heavy workloads), the next exec()'d binary sees
	 * stale guest pgd entries and ld.so segfaults during
	 * relocation processing — see fork-tree-3level repros for
	 * the canonical reproducer (codex audit, memo §E.4).
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
 * Phase C.4: pre-KVM_RUN FPU install. Called from kvm_v2_vcpu_run
 * between SREGS load and the KVM_RUN ioctl. Two cases:
 *
 *   - fpu_valid=true: capture_for_fork populated current's snapshot at
 *     fork. KVM_SET_FPU it into the per-CPU vCPU and clear fpu_valid
 *     (one-shot — subsequent runs use the vCPU's own running FPU
 *     state, no need to re-restore).
 *
 *   - fpu_valid=false: fresh task or post-execve via arch_flush_thread.
 *     Install architectural reset values per AMD64 SDM §11.5.1
 *     (fcw=0x037f, mxcsr=0x1f80; everything else zero). Built as a
 *     dynamic stack init rather than a static const so the struct's
 *     trailing FXSAVE area lands deterministically zeroed without
 *     pulling a 512 B rodata blob into kernel text.
 *
 * Returns 0 on success, -errno on KVM_SET_FPU failure. Dispatcher
 * panics on negative return: an FPU install failure means the guest
 * would run with arbitrary FPU state, which crashes downstream in
 * unpredictable ways — fail loudly here.
 */
static int kvm_v2_fpu_install_on_first_run(struct kvm_v2_vcpu *vcpu)
{
	struct arch_thread *a = &current->thread.arch;
	struct kvm_xsave init_fpu;
	int rc, was_valid;

	if (a->kvm_v2.fpu_valid) {
		/*
		 * SMP-T73 (2026-05-18): KVM_SET_XSAVE (4 KB struct
		 * kvm_xsave), not KVM_SET_FPU. Restoring only the legacy
		 * 512 B FXSAVE area leaves YMM upper 128 in whatever
		 * state the per-host-CPU vCPU last held — i.e., another
		 * task's leftover AVX state. See processor_64.h header.
		 */
		rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_XSAVE,
				      (unsigned long)&a->kvm_v2.fpu);
		if (rc < 0)
			return rc;
		a->kvm_v2.fpu_valid = false;	/* one-shot */
		was_valid = 1;
		/*
		 * SMP-T55: we just installed a fresh fork/switch-in FPU
		 * snapshot. The vCPU's guest FPU now matches `a->kvm_v2.fpu`,
		 * but the per-task `iotrap_fpu` slot does NOT yet — the post-
		 * vmexit GET must run this dispatch to populate iotrap_fpu so
		 * the next pre-run SET (and any cross-task arrival on this
		 * vCPU) finds an authoritative per-task slot. Force dirty;
		 * also record ownership so cross-task arrival logic is happy.
		 */
		vcpu->fpu_dirty       = true;
		vcpu->fpu_owner_task  = current;
	} else {
		/*
		 * E.4 hypothesis test (2026-04-29): leave per-vCPU FPU
		 * untouched on re-entry. Earlier shape unconditionally
		 * KVM_SET_FPU'd architectural reset values on every dispatch
		 * where fpu_valid=false — which destroyed XMM/x87 state
		 * mid-instruction whenever a task re-entered after a #PF.
		 *
		 * SMP-T28 (2026-05-02) tested adding a one-shot
		 * fpu_arch_reset_needed flag (set by arch_flush_thread) to
		 * install architectural FPU on the very first dispatch of
		 * freshly-execve'd tasks. Result: rate REGRESSED from
		 * ~0.003%/fork to 0.046%/fork. Hypothesis was wrong; the
		 * residual after T26/T27 is something else. Reverted.
		 */
		(void)init_fpu;
		was_valid = 0;
	}

	trace_um_backend_kvm_v2_fpu_install(vcpu->cpu, was_valid);
	return 0;
}
