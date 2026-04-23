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
/*
 * D-04c: SYSCALL-via-LSTAR variant-B guest code + its LSTAR
 * trampoline. Ports spike 07's exact byte layout.
 *   CODE_B: mov $0x27, %eax; syscall; hlt    (8 bytes)
 *   LSTAR:  out %al, $0xf4; sysretq          (5 bytes)
 */
#define KVM_HARNESS_CODE_B_OFFSET	0x5000
#define KVM_HARNESS_LSTAR_OFFSET	0x6000
/*
 * Phase III Lift #1b: ring-3 entry via SYSRETQ.
 *   RING3_CODE_OFFSET — ring-3 page: `out %al, $0xf5; hlt`
 *                       (port 0xf5 is ring-3-only; KVM_EXIT_IO
 *                       on 0xf5 proves SYSRETQ landed in CPL=3).
 *   SYSRETQ_TRAMP_OFFSET — ring-0 trampoline: set RCX, R11, do
 *                          SYSRETQ. Falls back to HLT if SYSRETQ
 *                          fails to transfer control.
 */
#define KVM_HARNESS_RING3_CODE_OFFSET	0x7000
#define KVM_HARNESS_SYSRETQ_TRAMP_OFFSET 0x8000
/*
 * Phase III Lift #1c: syscall-result-flow demonstration. Ring-3
 * code does `mov $0x2a, %eax; syscall; out %al, $0xf6; hlt`; the
 * LSTAR handler computes `rax += 100` and SYSRETQs back. Exit is
 * observed at port 0xf6 with data byte 0x8e (0x2a + 100) — the
 * port-data combination is the signal that (a) SYSCALL trapped
 * into LSTAR, (b) LSTAR ran and mutated RAX, (c) SYSRETQ
 * returned to ring-3, (d) ring-3 saw the mutated value.
 *
 * Scope note: this is deliberately NOT a dispatch through the
 * real sys_call_table. That requires guest-side `current` /
 * percpu / kernel-stack setup which the handcrafted harness
 * doesn't provide; the real dispatch lands with Phase III
 * Lift #1e/1f when the full guest-kernel-entry path is
 * plumbed. Lift #1c's role is to prove the ring-3 ↔ ring-0 ↔
 * ring-3 round-trip *returns a computed result to ring-3*, not
 * that the computation is the real syscall table.
 */
#define KVM_HARNESS_RING3_SYSCALL_OFFSET  0x7100
#define KVM_HARNESS_LSTAR_ADD_OFFSET	  0x9000
#define KVM_HARNESS_SYSRETQ_TRAMP2_OFFSET 0x9100
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

/*
 * D-04c variant-B guest code: set RAX to __NR_getpid (0x27),
 * SYSCALL (traps via LSTAR), HLT on SYSRETQ return.
 */
static const u8 kvm_harness_code_b[] = {
	0xb8, 0x27, 0x00, 0x00, 0x00,	/* mov $0x27, %eax */
	0x0f, 0x05,			/* syscall */
	0xf4,				/* hlt */
};

/*
 * D-04c LSTAR trampoline: on SYSCALL entry the CPU jumps here
 * with RCX = saved post-syscall RIP. The `out` triggers
 * KVM_EXIT_IO; host advances vCPU RIP past the 2-byte `out`
 * (to the SYSRETQ) and KVM_RUNs again. SYSRETQ returns to RCX
 * (the `hlt` following the SYSCALL in code_b). Matches
 * spike 07's lstar_tramp byte layout exactly.
 */
static const u8 kvm_harness_lstar[] = {
	0xe6, 0xf4,		/* out %al, $0xf4 */
	0x48, 0x0f, 0x07,	/* sysretq */
};

/*
 * Phase III Lift #1b: ring-3 page. The `out` touches port 0xf5
 * (distinct from the ring-0 tests' 0xf4) — the VMEXIT's port
 * number is the ring-3-entry proof. IOPL=3 is set in RFLAGS
 * before SYSRETQ so CPL=3 OUT doesn't #GP.
 */
static const u8 kvm_harness_ring3_code[] = {
	0xe6, 0xf5,		/* out %al, $0xf5 */
	0xf4,			/* hlt (fallback) */
};

/*
 * Phase III Lift #1b: ring-0 SYSRETQ trampoline. Loads RCX with
 * the ring-3 target RIP, R11 with the ring-3 RFLAGS (bit 1
 * reserved + IF + IOPL=3 = 0x3202), then executes SYSRETQ. The
 * CPU loads CS from STAR[63:48]+16 with forced RPL=3, SS from
 * STAR[63:48]+8 with forced RPL=3, RIP from RCX, RFLAGS from
 * R11. The trailing HLT is the fallback path if SYSRETQ somehow
 * fails to transfer control (it shouldn't — misconfiguration
 * would have surfaced as #GP on the SYSRETQ decode).
 *
 * Byte layout (20 bytes total):
 *   48 c7 c1 00 70 00 00   mov    $0x7000, %rcx
 *   49 c7 c3 02 32 00 00   mov    $0x3202, %r11
 *   48 0f 07               sysretq
 *   f4                     hlt
 */
static const u8 kvm_harness_sysretq_tramp[] = {
	0x48, 0xc7, 0xc1, 0x00, 0x70, 0x00, 0x00,	/* mov $0x7000, %rcx */
	0x49, 0xc7, 0xc3, 0x02, 0x32, 0x00, 0x00,	/* mov $0x3202, %r11 */
	0x48, 0x0f, 0x07,				/* sysretq */
	0xf4,						/* hlt */
};

/*
 * Phase III Lift #1c: ring-3 sled that issues a SYSCALL with a
 * known number in RAX, then exports the post-SYSCALL RAX byte
 * via `out %al, $0xf6`. The LSTAR handler (below) adds 100 to
 * RAX before SYSRETQing back, so a PASS is the host observing
 * KVM_EXIT_IO at port 0xf6 with data = input + 100. 10 bytes.
 */
static const u8 kvm_harness_ring3_syscall[] = {
	0xb8, 0x2a, 0x00, 0x00, 0x00,	/* mov $0x2a, %eax */
	0x0f, 0x05,			/* syscall */
	0xe6, 0xf6,			/* out %al, $0xf6 */
	0xf4,				/* hlt */
};

/*
 * Phase III Lift #1c: ring-0 LSTAR handler that does one
 * computation on RAX and returns. The "add $100" body is the
 * smallest operation that produces an observable result delta
 * in ring-3. Future lifts replace this body with a full syscall
 * dispatch (a stack switch + `call *sys_call_table(,%rax,8)` +
 * restore), but Lift #1c only demonstrates the round-trip shape.
 * 7 bytes.
 */
static const u8 kvm_harness_lstar_add[] = {
	0x48, 0x83, 0xc0, 0x64,		/* add $100, %rax */
	0x48, 0x0f, 0x07,		/* sysretq */
};

/*
 * Phase III Lift #1c: second SYSRETQ trampoline targeting the
 * ring-3 syscall sled (0x7100 instead of 0x7000). Otherwise
 * identical to the Lift #1b trampoline; we use a fresh copy
 * rather than patching the 0x8000 tramp in-place so the two
 * tests remain independent and diagnosable. 20 bytes.
 */
static const u8 kvm_harness_sysretq_tramp2[] = {
	0x48, 0xc7, 0xc1, 0x00, 0x71, 0x00, 0x00,	/* mov $0x7100, %rcx */
	0x49, 0xc7, 0xc3, 0x02, 0x32, 0x00, 0x00,	/* mov $0x3202, %r11 */
	0x48, 0x0f, 0x07,				/* sysretq */
	0xf4,						/* hlt */
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

	/*
	 * D-04c: variant-B guest code + LSTAR trampoline at
	 * spike-07-matching offsets. Unreferenced until the
	 * SYSCALL+LSTAR test after the main IO-exit loop.
	 */
	memcpy(mem + KVM_HARNESS_CODE_B_OFFSET,
	       kvm_harness_code_b, sizeof(kvm_harness_code_b));
	memcpy(mem + KVM_HARNESS_LSTAR_OFFSET,
	       kvm_harness_lstar, sizeof(kvm_harness_lstar));

	/*
	 * Phase III Lift #1b: ring-3 sled + ring-0 SYSRETQ tramp.
	 * Unreferenced until the ring-3 test routine after the D-04c
	 * LSTAR loop.
	 */
	memcpy(mem + KVM_HARNESS_RING3_CODE_OFFSET,
	       kvm_harness_ring3_code, sizeof(kvm_harness_ring3_code));
	memcpy(mem + KVM_HARNESS_SYSRETQ_TRAMP_OFFSET,
	       kvm_harness_sysretq_tramp, sizeof(kvm_harness_sysretq_tramp));

	/*
	 * Phase III Lift #1c: alternate ring-3 syscall sled, LSTAR
	 * handler that computes a result, and a second SYSRETQ
	 * trampoline targeting the 0x7100 sled. Exercised after the
	 * Lift #1b test.
	 */
	memcpy(mem + KVM_HARNESS_RING3_SYSCALL_OFFSET,
	       kvm_harness_ring3_syscall,
	       sizeof(kvm_harness_ring3_syscall));
	memcpy(mem + KVM_HARNESS_LSTAR_ADD_OFFSET,
	       kvm_harness_lstar_add, sizeof(kvm_harness_lstar_add));
	memcpy(mem + KVM_HARNESS_SYSRETQ_TRAMP2_OFFSET,
	       kvm_harness_sysretq_tramp2,
	       sizeof(kvm_harness_sysretq_tramp2));

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
	 * D-04c: SYSCALL-via-LSTAR round-trip measurement. Ports
	 * spike 07's variant-B methodology: program MSR_STAR /
	 * MSR_LSTAR / MSR_FMASK, loop KVM_SET_REGS → KVM_RUN 1000
	 * times, record cycles per iteration, report min/median/
	 * p95/max. Each iteration:
	 *
	 *   1. KVM_SET_REGS resets RIP=CODE_B_OFFSET, all other
	 *      regs 0, rflags=0x2.
	 *   2. KVM_RUN → guest executes mov+syscall → traps
	 *      through LSTAR → LSTAR-tramp's `out %al, $0xf4`
	 *      → KVM_EXIT_IO.
	 *
	 * Reset at step 1 clears KVM's pending-I/O state from the
	 * previous iteration's out, so no second KVM_RUN is needed
	 * (matches spike 07 exactly). The SYSRETQ after the `out`
	 * never executes in the measured path — its cost shows up
	 * in real-backend syscall dispatch (D-04c.2+) but isn't
	 * part of the spike-comparable measurement.
	 *
	 * MSR_STAR encoding per AMD64 SDM vol 3 §6.1.1:
	 *   bits 47:32 — kernel CS selector (SYSCALL loads this;
	 *                0x0008 = harness ring-0 CS).
	 *   bits 63:48 — "STAR.SYSRET_CS_SEL"; we set 0xfff8 so
	 *                (0xfff8 + 16) = 0x0008. Unused in the
	 *                measured path since SYSRETQ doesn't
	 *                execute.
	 */
	{
		struct {
			struct kvm_msrs info;
			struct kvm_msr_entry entries[3];
		} msrs = {
			.info = { .nmsrs = 3 },
			.entries = {
				{
					.index = 0xc0000081,	/* MSR_STAR */
					.data  = ((u64)0xfff8 << 48) |
						 ((u64)0x0008 << 32),
				},
				{
					.index = 0xc0000082,	/* MSR_LSTAR */
					.data  = KVM_HARNESS_LSTAR_OFFSET,
				},
				{
					.index = 0xc0000084,	/* MSR_FMASK */
					.data  = 0,
				},
			},
		};

		rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS,
				      (unsigned long)&msrs);
		if (rc < 0) {
			os_info("um: kvm harness: KVM_SET_MSRS (LSTAR/STAR) failed (%d)\n",
				rc);
			goto d04c_done;
		}
	}

	{
		enum { ITERS_B = 1000 };
		static u64 cyc_b[ITERS_B];
		unsigned int n = 0, i;
		u32 last_exit = 0;
		int last_rc = 0;

		for (i = 0; i < ITERS_B; i++) {
			u64 t0, t1;

			regs = (struct kvm_regs){
				.rip	= KVM_HARNESS_CODE_B_OFFSET,
				.rflags	= 0x2,
			};
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
			cyc_b[n++] = t1 - t0;
		}

		if (n >= 2) {
			kvm_harness_insertion_sort(cyc_b, n);
			os_info("um: kvm harness: LSTAR %u/%u variant-B IO exits; min=%llu median=%llu p95=%llu max=%llu cyc\n",
				n, ITERS_B,
				(unsigned long long)cyc_b[0],
				(unsigned long long)cyc_b[n / 2],
				(unsigned long long)cyc_b[(n * 95) / 100],
				(unsigned long long)cyc_b[n - 1]);
		} else {
			os_info("um: kvm harness: LSTAR only %u iters; last rc=%d exit=%u (%s)\n",
				n, last_rc, last_exit,
				kvm_harness_exit_name(last_exit));
		}
	}

d04c_done:
	/*
	 * Phase III Lift #1b: SYSRETQ into ring-3, execute `out %al,
	 * $0xf5; hlt`, observe KVM_EXIT_IO on port 0xf5. Port 0xf5
	 * is deliberately distinct from the 0xf4 the ring-0 tests
	 * use — a 0xf5 exit is the unambiguous signal that SYSRETQ
	 * landed CPL=3 and the ring-3 code executed. Anything else
	 * (exit on 0xf4, KVM_EXIT_SHUTDOWN, KVM_EXIT_FAIL_ENTRY)
	 * indicates the SYSRETQ transition failed or never happened.
	 *
	 * Sequence:
	 *   1. KVM_SET_MSRS overrides STAR[63:48] to 0x18, so
	 *      SYSRETQ loads CS=0x28|3=0x2b (ring-3 code; GDT idx 5)
	 *      and SS=0x20|3=0x23 (ring-3 data; GDT idx 4). The
	 *      kernel half STAR[47:32]=0x0008 is preserved from the
	 *      D-04c program (unused here since we never SYSCALL
	 *      out of ring 3).
	 *   2. KVM_SET_REGS sets RIP = SYSRETQ_TRAMP_OFFSET so the
	 *      ring-0 vCPU starts at the tramp. The tramp sets
	 *      RCX/R11 and issues SYSRETQ itself — we don't hand-
	 *      fill RCX/R11 from the host because SYSRETQ's "load
	 *      RFLAGS from R11" semantics require R11's bit-1
	 *      reserved to be 1, and letting the guest tramp set
	 *      it is closer to how a real kernel-to-userspace
	 *      return is coded.
	 *   3. KVM_RUN → ring-0 tramp → SYSRETQ → ring-3 code →
	 *      out %al, $0xf5 → KVM_EXIT_IO port=0xf5.
	 */
	{
		struct kvm_userspace_memory_region region;
		struct {
			struct kvm_msrs info;
			struct kvm_msr_entry entries[1];
		} star_override = {
			.info = { .nmsrs = 1 },
			.entries = {
				{
					.index = 0xc0000081,	/* MSR_STAR */
					/*
					 * Ring-3 SYSRET base = 0x18 (selector
					 * anchor); kernel CS stays 0x0008.
					 */
					.data  = ((u64)0x0018 << 48) |
						 ((u64)0x0008 << 32),
				},
			},
		};

		/*
		 * Re-register slot 0 to force KVM to re-read the
		 * harness-area bytes. The GDT was rewritten in the
		 * 6-entry layout above (kvm_setup_harness_gdt) but the
		 * vCPU cached the 3-entry descriptor via KVM_SET_SREGS
		 * earlier. Re-loading SREGS with the updated GDT limit
		 * is enough; no slot re-register needed (shared host
		 * mapping).
		 */
		(void)region;

		rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
				      (unsigned long)&sregs);
		if (rc < 0) {
			os_info("um: kvm harness: 1b KVM_GET_SREGS failed (%d)\n",
				rc);
			goto phase3_1b_done;
		}
		/*
		 * GDT limit now covers all 6 entries (48 bytes - 1 = 47).
		 * Must match sregs.c::KVM_HARNESS_GDT_LIMIT; duplicated
		 * here rather than via a shared header for the same
		 * reason other GDT/paging constants are duplicated — the
		 * harness's layout + offsets move together with sregs.c's.
		 */
		sregs.gdt.limit = 47;
		rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS,
				      (unsigned long)&sregs);
		if (rc < 0) {
			os_info("um: kvm harness: 1b KVM_SET_SREGS failed (%d)\n",
				rc);
			goto phase3_1b_done;
		}

		rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS,
				      (unsigned long)&star_override);
		if (rc < 0) {
			os_info("um: kvm harness: 1b KVM_SET_MSRS (STAR override) failed (%d)\n",
				rc);
			goto phase3_1b_done;
		}

		regs = (struct kvm_regs){
			.rip	= KVM_HARNESS_SYSRETQ_TRAMP_OFFSET,
			.rflags	= 0x2,
		};
		rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
				      (unsigned long)&regs);
		if (rc < 0) {
			os_info("um: kvm harness: 1b KVM_SET_REGS failed (%d)\n",
				rc);
			goto phase3_1b_done;
		}

		rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
		os_info("um: kvm harness: 1b KVM_RUN rc=%d exit_reason=%u (%s)\n",
			rc, run->exit_reason,
			kvm_harness_exit_name(run->exit_reason));

		if (rc >= 0 && run->exit_reason == KVM_EXIT_IO) {
			unsigned int port = run->io.port;

			if (port == 0xf5) {
				os_info("um: kvm harness: 1b PASS — ring-3 entry verified (port=0xf5 IO exit)\n");
			} else {
				os_info("um: kvm harness: 1b UNEXPECTED port=0x%x (expected 0xf5; 0xf4 would mean we stayed in ring-0)\n",
					port);
			}
		}
	}

	/*
	 * Phase III Lift #1c: ring-3 → LSTAR → ring-3 round-trip
	 * returning a computed result. Reuses the STAR[63:48]=0x18
	 * / ring-3 selector setup from Lift #1b (MSRs still
	 * programmed; GDT still loaded). Overrides MSR_LSTAR to
	 * point at the "add $100, %rax; sysretq" handler, sets RIP
	 * to the second SYSRETQ tramp (targeting the syscall sled
	 * at 0x7100), runs.
	 *
	 * Expected: KVM_EXIT_IO port=0xf6, io-data byte=0x8e
	 * (0x2a input + 100 = 0x8e). Any other port or any other
	 * data value means the round-trip is broken somewhere —
	 * the port proves ring-3 executed after SYSRETQ, and the
	 * data byte proves LSTAR ran and mutated RAX.
	 */
	{
		struct {
			struct kvm_msrs info;
			struct kvm_msr_entry entries[1];
		} lstar_override = {
			.info = { .nmsrs = 1 },
			.entries = {
				{
					.index = 0xc0000082,	/* MSR_LSTAR */
					.data  = KVM_HARNESS_LSTAR_ADD_OFFSET,
				},
			},
		};

		rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS,
				      (unsigned long)&lstar_override);
		if (rc < 0) {
			os_info("um: kvm harness: 1c KVM_SET_MSRS (LSTAR override) failed (%d)\n",
				rc);
			goto phase3_1b_done;
		}

		/*
		 * Reset vCPU to ring-0 SREGS. After Lift #1b's IO
		 * exit the vCPU is parked in CPL=3 (CS=0x2b). Lift
		 * #1c's trampoline at 0x9100 is ring-0 code — without
		 * a CS reset the first instruction (mov imm → rcx)
		 * runs fine but `sysretq` from CPL=3 is #UD, which
		 * triple-faults without an IDT handler and exits as
		 * KVM_EXIT_SHUTDOWN. kvm_setup_harness_sregs re-
		 * applies the ring-0 CS/SS/flat-data layout; the
		 * GDT limit stays at 47 (6 entries) since we
		 * still need the ring-3 descriptors for the SYSRETQ
		 * inside this test's LSTAR handler.
		 */
		rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
				      (unsigned long)&sregs);
		if (rc < 0) {
			os_info("um: kvm harness: 1c KVM_GET_SREGS failed (%d)\n",
				rc);
			goto phase3_1b_done;
		}
		kvm_setup_harness_sregs(&sregs);
		sregs.gdt.limit = 47;
		rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS,
				      (unsigned long)&sregs);
		if (rc < 0) {
			os_info("um: kvm harness: 1c KVM_SET_SREGS failed (%d)\n",
				rc);
			goto phase3_1b_done;
		}

		regs = (struct kvm_regs){
			.rip	= KVM_HARNESS_SYSRETQ_TRAMP2_OFFSET,
			.rflags	= 0x2,
		};
		rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
				      (unsigned long)&regs);
		if (rc < 0) {
			os_info("um: kvm harness: 1c KVM_SET_REGS failed (%d)\n",
				rc);
			goto phase3_1b_done;
		}

		rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
		os_info("um: kvm harness: 1c KVM_RUN rc=%d exit_reason=%u (%s)\n",
			rc, run->exit_reason,
			kvm_harness_exit_name(run->exit_reason));

		if (rc >= 0 && run->exit_reason == KVM_EXIT_IO) {
			unsigned int port = run->io.port;
			u8 data = *((u8 *)run + run->io.data_offset);
			u8 expected = 0x2a + 100;

			if (port == 0xf6 && data == expected) {
				os_info("um: kvm harness: 1c PASS — ring-3 → LSTAR → ring-3 round-trip verified (port=0xf6 data=0x%x)\n",
					data);
			} else if (port == 0xf6) {
				os_info("um: kvm harness: 1c UNEXPECTED data=0x%x on port 0xf6 (expected 0x%x = 0x2a + 100)\n",
					data, expected);
			} else {
				os_info("um: kvm harness: 1c UNEXPECTED port=0x%x data=0x%x (expected port=0xf6 data=0x%x)\n",
					port, data, expected);
			}
		}
	}
phase3_1b_done:
	/*
	 * The harness is one-shot by design. Report the final exit
	 * state (via os_info so it bypasses the unregistered printk
	 * buffer) and panic to stop the UML process — this is a
	 * diagnostic, not a continue-booting path.
	 */
	os_info("um: kvm harness: final KVM_RUN rc=%d, exit_reason=%u (%s)\n",
		rc, run->exit_reason,
		kvm_harness_exit_name(run->exit_reason));
	panic("um: kvm harness: done — D-04b + D-04c + Phase III Lift #1b/1c architectural validation complete");
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
