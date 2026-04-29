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
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/threads.h>
#include <linux/types.h>

#include <os.h>
#include <asm/trace/um_backend.h>

#include "kvm_v2_backend.h"

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
			 */
			e->ecx &= ~((1U << 12) | (1U << 26) | (1U << 27) |
				    (1U << 28) | (1U << 29) | (1U << 30));
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
			 */
			e->ebx &= ~((1U << 0)  | (1U << 5)  | (1U << 16) |
				    (1U << 17) | (1U << 18) | (1U << 21) |
				    (1U << 26) | (1U << 27) | (1U << 28) |
				    (1U << 29) | (1U << 30) | (1U << 31));
			/*
			 * Leaf 7.0 ECX:
			 *   bit 1  = AVX512VBMI, 6  = AVX512VBMI2,
			 *   bit 8  = GFNI,       9  = VAES,
			 *   bit 10 = VPCLMULQDQ, 11 = AVX512VNNI,
			 *   bit 12 = AVX512BITALG, 14 = AVX512VPOPCNTDQ.
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
		 * Leaf 0xD is the XSAVE state-component descriptor. With
		 * OSXSAVE off, exposing this leaf would let glibc /
		 * libcrypto derive sizes for state we don't actually
		 * support. Zero the leaf entirely.
		 */
		if (e->function == 0xD)
			e->eax = e->ebx = e->ecx = e->edx = 0;
	}
}

/*
 * Best-effort CPUID install. Failure is non-fatal because:
 *
 *   (a) kvm_v2_init runs from init_backend() during linux_main(), before
 *       mm_init() brings up the buddy allocator — kzalloc with GFP_KERNEL
 *       returns NULL at this point. context.c's file-scope comment + v1
 *       archive's kvm_ensure_cpuid_done() document the same constraint;
 *       v1 deferred CPUID install entirely to first KVM_RUN. v2's A.3
 *       wires the call site here per memo 26 §A.3 ("kzalloc + GET_SUPPORTED
 *       + mask + SET_CPUID2") but treats kzalloc failure as the same
 *       non-fatal pr_warn the v1 archive used (pr_warn_once "using
 *       KVM-default CPUID"). Phase C/D will revisit once the per-CPU
 *       pool's lazy-init pattern lets us defer CPUID to the first
 *       KVM_RUN as v1 did.
 *
 *   (b) Without CPUID install the placeholder vCPU still exists and
 *       sits idle (no KVM_RUN happens until Phase C/D wires the
 *       dispatcher). The vcpu_fd + kvm_run mmap are what Phase B
 *       memslot wiring + Phase C's pool transition need; CPUID is
 *       additive and can land later without rearchitecting.
 *
 * Returns 0 on success, 0 (not negative!) on best-effort failure so
 * vcpu_create still succeeds. The pr_warn announces the degradation.
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
		pr_warn("um: kvm-v2 vcpu_create: cpuid kzalloc(%zu) failed (buddy not up at init_backend time); using KVM-default CPUID — Phase C/D will defer install to first KVM_RUN\n",
			buf_sz);
		return 0;
	}

	cpuid->nent = KVM_V2_CPUID_MAX_ENTRIES;
	rc = os_ioctl_generic(vm->kvm_fd, KVM_GET_SUPPORTED_CPUID,
			      (unsigned long)cpuid);
	if (rc < 0) {
		pr_warn("um: kvm-v2 vcpu_create: KVM_GET_SUPPORTED_CPUID failed (%d); using KVM-default CPUID\n",
			rc);
		kfree(cpuid);
		return 0;
	}

	kvm_v2_curate_cpuid(cpuid);

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_CPUID2, (unsigned long)cpuid);
	if (rc < 0) {
		pr_warn("um: kvm-v2 vcpu_create: KVM_SET_CPUID2 failed (%d); using KVM-default CPUID\n",
			rc);
		kfree(cpuid);
		return 0;
	}

	/*
	 * Hand the buffer to the VM context — kvm_v2_vm_destroy() owns
	 * the kfree (see context.c). Keeping it alive past install lets
	 * later phases (e.g. KVM_GET_CPUID2 introspection, snapshot
	 * paths) re-read what's installed without re-issuing the
	 * GET_SUPPORTED + curate dance.
	 */
	vm->cpuid = cpuid;
	pr_info("um: kvm-v2 vcpu_create: CPUID installed (%u entries; RDRAND/RDSEED/XSAVE/AVX/AVX2/AVX512/FSGSBASE/F16C masked — matches v1)\n",
		cpuid->nent);
	return 0;
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
	 * KVM_SET_CPUID2 is per-vCPU — every pool member needs the
	 * curated mask installed. The first call's GET_SUPPORTED_CPUID
	 * also stashes the buffer on vm->cpuid; subsequent calls re-use
	 * that cached buffer rather than re-querying. Failure remains
	 * non-fatal (see install function header).
	 */
	rc = kvm_v2_install_cpuid(vm, vcpu_fd);
	if (rc)
		goto err_unmap;

	v->vcpu_fd      = vcpu_fd;
	v->kvm_run      = kvm_run;
	v->kvm_run_size = (u32)mmap_size;
	v->cpu          = cpu;

	trace_um_backend_kvm_v2_vcpu_create(vcpu_fd, v->kvm_run_size);
	return 0;

err_unmap:
	os_unmap_memory(kvm_run, mmap_size);
err_close_vcpu:
	os_close_file(vcpu_fd);
	return rc;
}

static void kvm_v2_vcpu_destroy_one(struct kvm_v2_vcpu *v)
{
	if (v->vcpu_fd < 0)
		return;

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
