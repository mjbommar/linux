// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase A.3: vCPU placeholder + CPUID install.
 *
 * Per memo 26 §A.3. One placeholder vCPU is created at init time so
 * later phases (B/C/D) have a stable target for KVM_SET_USER_MEMORY_REGION
 * + KVM_RUN wiring as they land. Phase C replaces this single vCPU with
 * a per-host-CPU pool; for now A.3 only needs the fd, the mmap'd kvm_run,
 * and a CPUID2 buffer that mirrors v1's curated mask so the eventual
 * guest sees the same feature surface v1 exposed.
 *
 * Why CPUID is installed here (not in context.c's vm_create):
 *   KVM_SET_CPUID2 is a vCPU ioctl — it requires the fd from
 *   KVM_CREATE_VCPU. Even KVM_GET_SUPPORTED_CPUID, while a KVM-fd
 *   ioctl, needs a kzalloc buffer; kvm_v2_init runs from init_backend()
 *   during linux_main(), BEFORE mm_init() brings the buddy allocator
 *   up. v1 archive's lifecycle.c documented the same constraint and
 *   deferred CPUID install entirely; v2 piggybacks on A.3 because the
 *   placeholder vCPU is the first kzalloc-safe site that already needs
 *   to touch the vCPU fd.
 *
 * Why the curated mask is verbatim from v1:
 *   v1 spent multiple iterations narrowing the feature surface
 *   (RDRAND/RDSEED for record-replay determinism; XSAVE/AVX/AVX2/AVX512
 *   family because v1's sregs setup never enabled CR4.OSXSAVE / set
 *   XCR0; FSGSBASE because CR4.FSGSBASE was off). v2's sregs setup is
 *   not yet written — it lives in Phase C. Until then matching v1's
 *   mask exactly avoids re-discovering the same #UD/#GP cliffs glibc
 *   and libcrypto fall off when those bits are advertised but not
 *   actually backed by host-side CR4/XCR0 setup. Phase C may revisit
 *   when proper XSAVE/XCR0 plumbing lands.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <os.h>
#include <asm/trace/um_backend.h>

#include "kvm_v2_backend.h"

/*
 * Single placeholder vCPU. Phase C replaces this with a per-host-CPU
 * pool keyed by `nr_cpu_ids`; the file-scope static keeps A.3's
 * lifecycle simple (init/shutdown only) while still letting later
 * phases reach the fd via kvm_v2_vcpu_get() if they need to before
 * the pool lands.
 */
static struct kvm_v2_vcpu vcpu = {
	.vcpu_fd = -1,
};

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

int kvm_v2_vcpu_create(struct kvm_v2_vm *vm)
{
	int vcpu_fd, mmap_size, rc;
	void *kvm_run;

	if (!vm)
		return -EINVAL;
	if (vcpu.vcpu_fd >= 0) {
		pr_warn("um: kvm-v2 vcpu_create: already created (vcpu_fd=%d)\n",
			vcpu.vcpu_fd);
		return -EBUSY;
	}

	/*
	 * KVM_GET_VCPU_MMAP_SIZE is a /dev/kvm-fd ioctl — query it
	 * once here. Phase C's per-CPU pool will reuse this size for
	 * every vCPU it creates (KVM guarantees the size is constant
	 * for the kernel's lifetime).
	 */
	mmap_size = os_ioctl_generic(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size <= 0) {
		pr_err("um: kvm-v2 vcpu_create: KVM_GET_VCPU_MMAP_SIZE failed (%d)\n",
		       mmap_size);
		return mmap_size ? mmap_size : -EIO;
	}

	vcpu_fd = os_ioctl_generic(vm->vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0) {
		pr_err("um: kvm-v2 vcpu_create: KVM_CREATE_VCPU(id=0) failed (%d)\n",
		       vcpu_fd);
		return vcpu_fd;
	}

	kvm_run = os_mmap_rw_shared(vcpu_fd, mmap_size);
	if (!kvm_run) {
		pr_err("um: kvm-v2 vcpu_create: mmap of kvm_run (size %d) failed\n",
		       mmap_size);
		rc = -ENOMEM;
		goto err_close_vcpu;
	}

	rc = kvm_v2_install_cpuid(vm, vcpu_fd);
	if (rc)
		goto err_unmap;

	vcpu.vcpu_fd      = vcpu_fd;
	vcpu.kvm_run      = kvm_run;
	vcpu.kvm_run_size = (u32)mmap_size;

	trace_um_backend_kvm_v2_vcpu_create(vcpu_fd, vcpu.kvm_run_size);

	pr_info("um: kvm-v2 vcpu_create: vcpu_fd=%d kvm_run_size=%u (placeholder; Phase C replaces with per-CPU pool)\n",
		vcpu_fd, vcpu.kvm_run_size);
	return 0;

err_unmap:
	os_unmap_memory(kvm_run, mmap_size);
err_close_vcpu:
	os_close_file(vcpu_fd);
	return rc;
}

void kvm_v2_vcpu_destroy(void)
{
	if (vcpu.vcpu_fd < 0)
		return;

	/*
	 * mmap of kvm_run survives the vcpu_fd close (the mapping is
	 * refcounted in the kernel), so munmap first; the v1 archive's
	 * shutdown path documents the same ordering.
	 */
	if (vcpu.kvm_run) {
		os_unmap_memory(vcpu.kvm_run, (int)vcpu.kvm_run_size);
		vcpu.kvm_run = NULL;
	}

	pr_info("um: kvm-v2 vcpu_destroy: closing vcpu_fd=%d\n", vcpu.vcpu_fd);
	os_close_file(vcpu.vcpu_fd);
	vcpu.vcpu_fd      = -1;
	vcpu.kvm_run_size = 0;
}
