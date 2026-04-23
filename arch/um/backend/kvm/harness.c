// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — long-mode harness (D-04b.1b).
 *
 * Self-contained diagnostic that verifies the KVM backend's
 * long-mode bring-up plumbing works end-to-end before D-04b.2
 * swaps in UML's own init_mm.pgd. When CONFIG_UM_BACKEND_KVM_
 * HARNESS is set, this runs once on backend init:
 *
 *   1. mmap 2 MiB anonymous as the harness memslot backing.
 *   2. Register it as KVM slot 0 (userspace_addr = mmap'd
 *      host VA, guest_phys_addr = 0, memory_size = 2 MiB).
 *   3. Lay down the GDT (kvm_setup_harness_gdt) + page tables
 *      (kvm_setup_harness_paging) + a small `out %al, $0xf4;
 *      hlt` code sled at the spike-04 offsets.
 *   4. Populate vcpu0 SREGS (kvm_setup_harness_sregs) + regs
 *      (rip = CODE_OFFSET, rflags = 0x2).
 *   5. Invoke KVM_RUN and panic with the exit_reason.
 *
 * Expected exit: KVM_EXIT_IO on port 0xf4 on the first
 * iteration (the `out` instruction triggers a userspace VMEXIT
 * before the HLT is reached). That matches spike 04's measured
 * behaviour across multiple x86_64 silicon generations.
 *
 * The harness intentionally panics — it is not a "continue
 * booting" path. Its whole job is to prove the SREGS setup
 * works; reaching the post-harness kernel code is D-04b.2's
 * scope, with UML kernel code, not the handcrafted sled.
 */
#include <linux/errno.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <os.h>
#include <asm/backend.h>

#include "kvm_backend.h"

/*
 * Must match the offsets sregs.c's kvm_setup_harness_* helpers
 * hard-code. Duplicated here (vs a shared header) because
 * sregs.c uses them internally and harness.c places the exact
 * pages; a shared header would be over-abstraction for ~5
 * #defines that move together.
 */
#define KVM_HARNESS_MEM_SIZE		(2UL * 1024 * 1024)
#define KVM_HARNESS_PML4_OFFSET		0x0000
#define KVM_HARNESS_PDPT_OFFSET		0x1000
#define KVM_HARNESS_PD_OFFSET		0x2000
#define KVM_HARNESS_GDT_OFFSET		0x3000
#define KVM_HARNESS_CODE_OFFSET		0x4000
#define KVM_HARNESS_SLOT		0

/*
 * Guest code at CODE_OFFSET (ring 0 long mode):
 *   out %al, $0xf4             ; triggers KVM_EXIT_IO (2 bytes)
 *   hlt                        ; fallback if we get back here
 *
 * Port 0xf4 is unused on real hardware; QEMU uses it as a
 * debug "shutdown" port, so it's a well-worn VMEXIT trigger
 * for KVM diagnostics. Using it (instead of vmcall) gives a
 * userspace-visible VMEXIT whose cost is representative of
 * the bounce-trampoline model; see spike 04's guest_code
 * comment for the rationale.
 */
static const u8 kvm_harness_code[] = {
	0xe6, 0xf4,	/* out %al, $0xf4 */
	0xf4,		/* hlt */
};

static const char *kvm_harness_exit_name(u32 r)
{
	switch (r) {
	case KVM_EXIT_UNKNOWN:		return "UNKNOWN";
	case KVM_EXIT_EXCEPTION:	return "EXCEPTION";
	case KVM_EXIT_IO:		return "IO";
	case KVM_EXIT_HYPERCALL:	return "HYPERCALL";
	case KVM_EXIT_HLT:		return "HLT";
	case KVM_EXIT_MMIO:		return "MMIO";
	case KVM_EXIT_SHUTDOWN:		return "SHUTDOWN";
	case KVM_EXIT_FAIL_ENTRY:	return "FAIL_ENTRY";
	case KVM_EXIT_INTERNAL_ERROR:	return "INTERNAL_ERROR";
	default:			return "???";
	}
}

int kvm_run_harness(void)
{
	struct kvm_um *ctx = kvm_backend_ctx();
	struct kvm_run *run = ctx->run0;
	int vmfd = ctx->vm_fd;
	int vcpu_fd = ctx->vcpu0_fd;
	void *mem;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	struct kvm_userspace_memory_region region;
	int rc;

	if (vmfd < 0 || vcpu_fd < 0 || !run)
		return -EIO;

	/*
	 * Allocate the 2 MiB harness region as anonymous shared
	 * memory. Must be MAP_SHARED so KVM can back the memslot
	 * with it (MAP_PRIVATE would COW on guest writes).
	 */
	mem = os_mmap_rw_anon_shared(KVM_HARNESS_MEM_SIZE);
	if (!mem) {
		pr_err("um: kvm harness: mmap 2 MiB failed\n");
		return -ENOMEM;
	}

	/* GDT + page tables + code at their spike-04 offsets. */
	kvm_setup_harness_gdt((u64 *)(mem + KVM_HARNESS_GDT_OFFSET));
	kvm_setup_harness_paging((u64 *)(mem + KVM_HARNESS_PML4_OFFSET),
				 (u64 *)(mem + KVM_HARNESS_PDPT_OFFSET),
				 (u64 *)(mem + KVM_HARNESS_PD_OFFSET));
	memcpy(mem + KVM_HARNESS_CODE_OFFSET,
	       kvm_harness_code, sizeof(kvm_harness_code));

	region = (struct kvm_userspace_memory_region){
		.slot			= KVM_HARNESS_SLOT,
		.flags			= 0,
		.guest_phys_addr	= 0,
		.memory_size		= KVM_HARNESS_MEM_SIZE,
		.userspace_addr		= (__u64)(uintptr_t)mem,
	};
	rc = os_ioctl_generic(vmfd, KVM_SET_USER_MEMORY_REGION,
			      (unsigned long)&region);
	if (rc < 0) {
		pr_err("um: kvm harness: KVM_SET_USER_MEMORY_REGION failed (%d)\n",
		       rc);
		os_unmap_memory(mem, KVM_HARNESS_MEM_SIZE);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_err("um: kvm harness: KVM_GET_SREGS failed (%d)\n", rc);
		os_unmap_memory(mem, KVM_HARNESS_MEM_SIZE);
		return rc;
	}
	kvm_setup_harness_sregs(&sregs);
	rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_err("um: kvm harness: KVM_SET_SREGS failed (%d)\n", rc);
		os_unmap_memory(mem, KVM_HARNESS_MEM_SIZE);
		return rc;
	}

	regs = (struct kvm_regs){
		.rip	= KVM_HARNESS_CODE_OFFSET,
		.rflags	= 0x2,	/* IF=0, reserved bit set */
	};
	rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS, (unsigned long)&regs);
	if (rc < 0) {
		pr_err("um: kvm harness: KVM_SET_REGS failed (%d)\n", rc);
		os_unmap_memory(mem, KVM_HARNESS_MEM_SIZE);
		return rc;
	}

	pr_info("um: kvm harness: entering KVM_RUN (rip=0x%lx)\n",
		(unsigned long)KVM_HARNESS_CODE_OFFSET);
	rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);

	/*
	 * The harness is one-shot by design. Whatever exit we hit
	 * is what we report; there's no "continue booting" path
	 * after this.
	 *
	 * Emit the result through os_info() BEFORE panic() because
	 * panic() at this early boot stage buffers its printk
	 * output — console registration happens later in
	 * start_kernel() and the buffer never flushes if we
	 * reboot_skas() out of linux_main(). os_info() writes
	 * directly to stderr via the host libc, bypassing the
	 * printk buffer entirely.
	 */
	os_info("um: kvm harness: KVM_RUN rc=%d, exit_reason=%u (%s)\n",
		rc, run->exit_reason,
		kvm_harness_exit_name(run->exit_reason));
	panic("um: kvm harness: KVM_RUN rc=%d, exit_reason=%u (%s) — D-04b.1b sanity complete, D-04b.2 swaps UML CR3 next",
	      rc, run->exit_reason,
	      kvm_harness_exit_name(run->exit_reason));
}
