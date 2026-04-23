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
#include <linux/init.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <os.h>
#include <mem.h>		/* uml_physmem */
#include <as-layout.h>		/* physmem_size */
#include <asm/backend.h>

#include "kvm_backend.h"

/*
 * Must match the offsets sregs.c's kvm_setup_harness_* helpers
 * hard-code. Duplicated here (vs a shared header) because
 * sregs.c uses them internally and harness.c places the exact
 * pages; a shared header would be over-abstraction for ~5
 * #defines that move together.
 */
#define KVM_HARNESS_MEM_SIZE		(4UL * 1024 * 1024)
#define KVM_HARNESS_PML4_OFFSET		0x0000
#define KVM_HARNESS_PDPT_OFFSET		0x1000
#define KVM_HARNESS_PD_OFFSET		0x2000
#define KVM_HARNESS_GDT_OFFSET		0x3000
#define KVM_HARNESS_CODE_OFFSET		0x4000
/*
 * D-04b.2b.alt: a second HLT byte placed at offset 0x210000
 * (2 MiB + 64 KiB into the 4 MiB slot). Validates the kvm-owned
 * pgd's arbitrary-RIP execution: guest accesses beyond the
 * first 2 MiB hugepage still resolve through the PD's subsequent
 * entries, and the memslot expansion covers the guest_phys.
 */
#define KVM_HARNESS_HLT2_OFFSET		0x210000
#define KVM_HARNESS_SLOT		0

/*
 * D-04b.2b.2: a second KVM memslot covering UML's own memory
 * at a non-overlapping guest_phys base. Slot 0 is the harness's
 * 4 MiB self-owned region at guest_phys 0; slot 1 covers
 * guest_phys [UML_GPA_BASE, UML_GPA_BASE + physmem_size)
 * mapped to host-VA [uml_physmem, uml_physmem + physmem_size).
 * 256 MiB is chosen because it's past the 4 MiB harness slot
 * with room to spare and still falls within the 1 GiB pd[0..512)
 * identity mapping the harness installs.
 */
#define KVM_HARNESS_UML_SLOT		1
#define KVM_HARNESS_UML_GPA_BASE	0x10000000UL	/* 256 MiB */

/*
 * D-04b.2b.2: a naked function whose body is exactly `hlt`. KVM
 * resumes after VMEXIT back into the same RIP, so we never
 * execute the next instruction — no ret / push needed, so
 * __attribute__((naked)) is safe (no frame setup either). The
 * function's address is UML-kernel-text; the harness subtracts
 * uml_physmem to translate into guest-VA, and the guest sees
 * the same bytes via slot 1's EPT mapping.
 */
__attribute__((naked)) static void kvm_harness_hlt_target(void)
{
	asm volatile("hlt");
}

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

/*
 * Local rdtsc — x86_64 only (UML x86_64 runs on host x86_64 CPUs,
 * so the instruction is always available in harness-mode). Not
 * serializing; the loop's KVM_RUN ioctls provide enough implicit
 * ordering for the ~4000+-cycle VMEXIT round-trips we're
 * measuring.
 */
static inline u64 kvm_harness_rdtsc(void)
{
	u32 lo, hi;

	asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

/* Ascending integer-quickselect-free median via simple sort. */
static void kvm_harness_insertion_sort(u64 *a, unsigned int n)
{
	unsigned int i, j;

	for (i = 1; i < n; i++) {
		u64 key = a[i];

		j = i;
		while (j > 0 && a[j - 1] > key) {
			a[j] = a[j - 1];
			j--;
		}
		a[j] = key;
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

	/*
	 * GDT + page tables + code at their spike-04 offsets. Use
	 * the range-covering paging helper (D-04b.2a) so the guest
	 * sees all 1024 × 2 MiB = 1 GiB of identity-mapped address
	 * space, not just the initial 2 MiB that fits inside the
	 * harness slot. Over-mapping is fine — accesses past the
	 * harness slot end hit the Policy A memslot (via
	 * kvm_ensure_memslot's deferred registration) so EPT
	 * redirects them to UML's own VA. Sets up the infrastructure
	 * for D-04b.2b where RIP points at UML kernel text.
	 */
	kvm_setup_harness_gdt((u64 *)(mem + KVM_HARNESS_GDT_OFFSET));
	kvm_setup_harness_paging_range((u64 *)(mem + KVM_HARNESS_PML4_OFFSET),
				       (u64 *)(mem + KVM_HARNESS_PDPT_OFFSET),
				       (u64 *)(mem + KVM_HARNESS_PD_OFFSET),
				       KVM_HARNESS_PD_OFFSET, 512);
	memcpy(mem + KVM_HARNESS_CODE_OFFSET,
	       kvm_harness_code, sizeof(kvm_harness_code));

	/*
	 * D-04b.2b.alt: a lone `hlt` byte at an offset beyond the
	 * first 2 MiB hugepage boundary. Executing from here
	 * exercises the PD's second entry (pd[1]) and proves the
	 * kvm-owned pgd can route arbitrary RIP.
	 */
	*(u8 *)(mem + KVM_HARNESS_HLT2_OFFSET) = 0xf4;	/* hlt */

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

	pr_info("um: kvm harness: entering KVM_RUN loop (rip=0x%lx)\n",
		(unsigned long)KVM_HARNESS_CODE_OFFSET);

	/*
	 * D-04b.1c: N iterations, per-iteration rdtsc bracketing.
	 * Each iteration resets RIP to CODE_OFFSET so the same
	 * two-byte `out %al, $0xf4` executes. Record cycles; at
	 * the end report min / median / max via os_info() (direct
	 * stderr; the printk buffer isn't flushed until console
	 * registration, which never happens on this one-shot).
	 *
	 * ITERS=1000 matches spike 04's methodology; fits comfortably
	 * in the harness's 2 MiB slot + a small bss array.
	 */
	{
		enum { ITERS = 1000 };
		static u64 cycles[ITERS];
		unsigned int n = 0, i;
		u32 last_exit = 0;
		int last_rc = 0;

		for (i = 0; i < ITERS; i++) {
			u64 t0, t1;

			/*
			 * KVM leaves the vCPU in a "pending I/O
			 * emulation" state after KVM_EXIT_IO; a bare
			 * second KVM_RUN completes it (advancing RIP
			 * past `out`) regardless of any KVM_SET_REGS
			 * we do in between. Matches spike 04's
			 * pattern: after the IO exit, explicitly advance
			 * RIP to the hlt, run once to clear the pending
			 * state (exits with HLT), then reset RIP to
			 * CODE_OFFSET for the next iteration.
			 */
			regs.rip = KVM_HARNESS_CODE_OFFSET;
			regs.rflags = 0x2;
			regs.rax = 0;
			rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
					      (unsigned long)&regs);
			if (rc < 0) {
				last_rc = rc;
				break;
			}

			t0 = kvm_harness_rdtsc();
			rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
			t1 = kvm_harness_rdtsc();
			last_rc = rc;
			last_exit = run->exit_reason;
			if (rc < 0 || run->exit_reason != KVM_EXIT_IO)
				break;
			cycles[n++] = t1 - t0;

			/*
			 * Advance RIP past `out` (2 bytes) and KVM_RUN
			 * once more to let KVM complete the pending I/O
			 * emulation — the vCPU runs `hlt` next and exits
			 * cleanly, leaving it ready for the next
			 * iteration's RIP reset. The HLT exit itself
			 * isn't measured; we only care about the IO exit
			 * cost here.
			 */
			regs.rip = KVM_HARNESS_CODE_OFFSET + 2;
			rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
					      (unsigned long)&regs);
			if (rc < 0) {
				last_rc = rc;
				break;
			}
			rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
			if (rc < 0 || run->exit_reason != KVM_EXIT_HLT) {
				last_rc = rc;
				last_exit = run->exit_reason;
				break;
			}
		}

		if (n >= 2) {
			kvm_harness_insertion_sort(cycles, n);
			os_info("um: kvm harness: %u/%u IO exits measured; "
				"min=%llu median=%llu p95=%llu max=%llu cyc\n",
				n, ITERS,
				(unsigned long long)cycles[0],
				(unsigned long long)cycles[n / 2],
				(unsigned long long)cycles[(n * 95) / 100],
				(unsigned long long)cycles[n - 1]);
		} else {
			os_info("um: kvm harness: only %u iterations measurable; last rc=%d exit=%u (%s)\n",
				n, last_rc, last_exit,
				kvm_harness_exit_name(last_exit));
		}
	}

	/*
	 * D-04b.2b.alt arbitrary-RIP validation: program RIP at
	 * the HLT byte placed beyond the first 2 MiB hugepage
	 * boundary, KVM_RUN once, assert KVM_EXIT_HLT. Proves the
	 * kvm-owned pgd routes RIP correctly through pd[1]
	 * (covering 2..4 MiB) rather than just pd[0].
	 */
	regs = (struct kvm_regs){
		.rip	= KVM_HARNESS_HLT2_OFFSET,
		.rflags	= 0x2,
	};
	rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS, (unsigned long)&regs);
	if (rc < 0) {
		os_info("um: kvm harness: arbitrary-RIP KVM_SET_REGS failed (%d)\n",
			rc);
	} else {
		rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
		os_info("um: kvm harness: arbitrary-RIP KVM_RUN rc=%d exit_reason=%u (%s) rip=0x%lx\n",
			rc, run->exit_reason,
			kvm_harness_exit_name(run->exit_reason),
			(unsigned long)KVM_HARNESS_HLT2_OFFSET);
	}

	/*
	 * D-04b.2b.2: register a second memslot covering UML's own
	 * memory, then set RIP to the guest-VA of
	 * kvm_harness_hlt_target (a naked `hlt`-only function in
	 * UML kernel text). This proves the vCPU executes real UML
	 * kernel binary bytes, not just our handcrafted sled.
	 *
	 * Layout:
	 *   slot 0 (registered above): guest_phys [0, 4 MiB)
	 *          → harness's self-owned 4 MiB region
	 *   slot 1 (registered here):  guest_phys [256 MiB,
	 *          256 MiB + physmem_size) → UML's uml_physmem
	 *          onwards
	 *
	 * Guest page tables identity-map guest_VA = guest_phys for
	 * both ranges (they're both inside the pd[0..512) 2 MiB
	 * hugepage coverage kvm_setup_harness_paging_range
	 * installed). So:
	 *   guest_VA of target = 256 MiB + (&target - uml_physmem)
	 */
	if (uml_physmem && physmem_size) {
		struct kvm_userspace_memory_region uml_region = {
			.slot			= KVM_HARNESS_UML_SLOT,
			.flags			= 0,
			.guest_phys_addr	= KVM_HARNESS_UML_GPA_BASE,
			.memory_size		= physmem_size,
			.userspace_addr		= uml_physmem,
		};
		int rc2 = os_ioctl_generic(vmfd, KVM_SET_USER_MEMORY_REGION,
					   (unsigned long)&uml_region);
		if (rc2 < 0) {
			os_info("um: kvm harness: UML slot register failed (%d)\n",
				rc2);
		} else {
			unsigned long target = (unsigned long)&kvm_harness_hlt_target;
			unsigned long guest_va =
				KVM_HARNESS_UML_GPA_BASE + (target - uml_physmem);

			os_info("um: kvm harness: UML slot registered (host_va=%lx size=%llx gpa_base=%lx); target &hlt=%lx → guest_va=%lx\n",
				uml_physmem, physmem_size,
				KVM_HARNESS_UML_GPA_BASE, target, guest_va);

			regs = (struct kvm_regs){
				.rip	= guest_va,
				.rflags	= 0x2,
			};
			rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
					      (unsigned long)&regs);
			if (rc < 0) {
				os_info("um: kvm harness: UML-text KVM_SET_REGS failed (%d)\n",
					rc);
			} else {
				rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
				os_info("um: kvm harness: UML-text KVM_RUN rc=%d exit_reason=%u (%s) rip=0x%lx\n",
					rc, run->exit_reason,
					kvm_harness_exit_name(run->exit_reason),
					guest_va);
			}
		}
	} else {
		os_info("um: kvm harness: uml_physmem or physmem_size zero; skipping UML-text test\n");
	}

	/*
	 * The harness is one-shot by design. Report the final exit
	 * state (via os_info so it bypasses the unregistered printk
	 * buffer) and panic to stop the UML process — this is a
	 * diagnostic, not a continue-booting path.
	 */
	os_info("um: kvm harness: final KVM_RUN rc=%d, exit_reason=%u (%s)\n",
		rc, run->exit_reason,
		kvm_harness_exit_name(run->exit_reason));
	panic("um: kvm harness: done — D-04b full architectural validation complete");
}

/*
 * D-04b.2b.1: harness fires from a late_initcall rather than
 * kvm_init(). By late_initcall firing, uml_physmem /
 * physmem_size are populated (arch_setup ran; D-05a lets
 * time_init progress so jiffies advance through do_initcalls).
 *
 * Still uses the self-registered 2/4 MiB harness slot, not the
 * Policy A memslot — that's a follow-on refactor. All this sub-
 * step does is relocate the timing.
 */
static int __init kvm_harness_late_start(void)
{
	if (!kvm_backend_ctx() || kvm_backend_vcpu0_fd() < 0) {
		pr_info("um: kvm harness: backend not initialized (not backend=kvm?); skipping\n");
		return 0;
	}
	os_info("um: kvm harness: late_initcall firing\n");
	pr_warn("um: kvm harness: late_initcall firing — replacing normal boot with diagnostic\n");
	kvm_run_harness();
	panic("um: kvm harness returned — should not happen");
}
late_initcall(kvm_harness_late_start);
