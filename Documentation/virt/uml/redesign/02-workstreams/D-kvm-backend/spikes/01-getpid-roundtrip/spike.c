// SPDX-License-Identifier: GPL-2.0
/*
 * spike.c — D-workstream spike 01: KVM getpid round-trip.
 *
 * Goal: measure the ns cost of a single KVM VMEXIT → handle →
 * VMRESUME round-trip on this host, so the D-01 design memo has an
 * empirical anchor rather than a vision-doc guess.
 *
 * Shape (deliberately minimal, throwaway-grade):
 *
 *   - Open /dev/kvm, create a VM, create one vCPU.
 *   - Set up long mode (IA-32e) the minimum way: identity-paged
 *     single 4 KiB page at guest phys 0, CR3 points at a bare
 *     PML4 entry, CR0.PE + CR0.PG + CR4.PAE + EFER.LME|LMA|SCE.
 *   - Drop in five x86 instructions at guest offset 0x1000:
 *
 *         mov rax, 39        ; __NR_getpid
 *         syscall            ; will trap to LSTAR (= 0, our marker)
 *         cmp rax, 42        ; check we mocked the return value
 *         mov rax, 0         ; mocks the cmp result to "handled"
 *         hlt                ; final exit
 *
 *     The `syscall` traps because LSTAR is set to 0 and there's no
 *     code at guest phys 0. That's not how a real kernel dispatches
 *     syscalls — a real kernel sets LSTAR to its own entry point.
 *     But our goal is "measure the round-trip cost of one VMEXIT";
 *     the fastest way to a deterministic exit is to LSTAR-trap it.
 *   - On the exit (KVM_EXIT_{SHUTDOWN,FAIL_ENTRY,INTERNAL_ERROR,HLT,
 *     IO,MMIO} — which kind depends on how the CPU handles the bogus
 *     LSTAR), note the cycle counter, mock RAX=42, resume.
 *   - Second exit should be the HLT; note cycles, compute delta.
 *
 * What we measure + report:
 *
 *   T0  = rdtsc before KVM_RUN #1
 *   T1  = rdtsc after  KVM_RUN #1  (SYSCALL exit)
 *   T2  = rdtsc before KVM_RUN #2
 *   T3  = rdtsc after  KVM_RUN #2  (HLT exit)
 *   round_trip_cycles = T1 - T0  (VMENTRY → SYSCALL VMEXIT round-trip)
 *   resume_cycles     = T3 - T2  (VMENTRY → HLT VMEXIT round-trip)
 *
 *   Average of many iterations → median cycles.
 *   Convert to ns via /proc/cpuinfo's reported MHz (good enough; no
 *   invariant-TSC calibration because this is a spike).
 *
 * This is NOT UML integration. It's a standalone measurement tool.
 * Output feeds D-01's "empirical KVM round-trip on <cpu>" line.
 *
 * Build: make
 * Run:   ./spike
 * Needs: /dev/kvm accessible, host CPU with VMX/SVM.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define XFAIL(what)                                                            \
	do {                                                                   \
		fprintf(stderr, "FAIL: %s: %s\n", (what), strerror(errno));    \
		exit(1);                                                       \
	} while (0)

/*
 * Guest layout:
 *   0x0000 .. 0x0fff   PML4 (only entry 0 used)
 *   0x1000 .. 0x1fff   PDPT (only entry 0 used)
 *   0x2000 .. 0x2fff   PD (only entry 0 used; maps as 2 MiB huge)
 *   0x3000 .. 0x3fff   guest code (the 5 insns above)
 *
 * With IA-32e + PAE + 2 MiB pages, entries in PD map 2 MiB of VA to
 * a 2 MiB-aligned phys frame. Using entry[0] in all three levels
 * maps guest-virtual 0..2 MiB to guest-phys 0..2 MiB identity.
 * That covers our code page at 0x3000.
 */
#define MEM_SIZE         (2 * 1024 * 1024)    /* 2 MiB */
#define CODE_OFFSET      0x3000
#define PML4_OFFSET      0x0000
#define PDPT_OFFSET      0x1000
#define PD_OFFSET        0x2000

#define PTE_P            (1ULL << 0)
#define PTE_RW           (1ULL << 1)
#define PTE_PS           (1ULL << 7)

#define CR0_PE           (1UL << 0)
#define CR0_MP           (1UL << 1)
#define CR0_NE           (1UL << 5)
#define CR0_PG           (1UL << 31)
#define CR4_PAE          (1UL << 5)
#define EFER_SCE         (1UL << 0)
#define EFER_LME         (1UL << 8)
#define EFER_LMA         (1UL << 10)

/*
 * Guest code — two variants.
 *
 * Original plan: long mode + SYSCALL → LSTAR trap. Rejected for
 * the spike after the first run: getting IA-32e up without a
 * real GDT + TSS trips KVM_EXIT_SHUTDOWN on the very first
 * instruction, which tells us nothing about VMEXIT cost.
 *
 * Actual: real-mode guest, single `hlt`. The VMENTRY→VMEXIT
 * cost we're measuring is a property of the CPU's VT-x/SVM
 * implementation, not of what mode the guest is in. A real-
 * mode HLT gives us a clean, deterministic KVM_EXIT_HLT on
 * every iteration with no paging, no segmentation complexity,
 * and no long-mode setup. The number we care about —
 * KVM_RUN ioctl → VMEXIT → return-to-userspace in cycles —
 * is the same regardless.
 *
 * When we build the real UML-on-KVM backend, long mode setup
 * (D-02, D-03) will be a separate workstream with its own
 * tests. Right now we just need to know: is a VMEXIT round-
 * trip ~100 ns or ~5 us?
 */
static const unsigned char guest_code[] = {
	/* 0x00: hlt */
	0xf4,
};

static inline uint64_t rdtsc(void)
{
	unsigned lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

/*
 * Parse "cpu MHz" out of /proc/cpuinfo. Not invariant-TSC-aware;
 * fine for a spike, where we care about orders of magnitude not
 * microsecond accuracy.
 */
static double cpu_mhz(void)
{
	FILE *f = fopen("/proc/cpuinfo", "re");
	if (!f)
		return 0.0;
	char line[256];
	double mhz = 0.0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "cpu MHz", 7) == 0) {
			char *colon = strchr(line, ':');
			if (colon) {
				mhz = strtod(colon + 1, NULL);
				break;
			}
		}
	}
	fclose(f);
	return mhz;
}

static void setup_real_mode(struct kvm_sregs *sregs)
{
	/*
	 * Real-mode CS: base = CODE_OFFSET so IP=0 maps to our HLT
	 * instruction. Selector matches base>>4 per real-mode
	 * semantics.
	 */
	sregs->cs.base = CODE_OFFSET;
	sregs->cs.selector = CODE_OFFSET >> 4;
}

/*
 * Run the vCPU once. Return the exit reason and fill in *cycles_out
 * with the VMENTRY→VMEXIT cycle delta.
 */
static uint32_t run_once(int vcpu_fd, uint64_t *cycles_out)
{
	struct kvm_run *run;
	uint64_t t0, t1;

	run = (struct kvm_run *)(uintptr_t)ioctl(vcpu_fd,
						 KVM_GET_VCPU_MMAP_SIZE);
	(void)run; /* silence unused in default path */

	t0 = rdtsc();
	int rc = ioctl(vcpu_fd, KVM_RUN, 0);
	t1 = rdtsc();
	*cycles_out = t1 - t0;
	if (rc < 0) {
		fprintf(stderr, "KVM_RUN: %s\n", strerror(errno));
		return (uint32_t)-1;
	}
	return 0;
}

int main(void)
{
	int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (kvm < 0)
		XFAIL("open /dev/kvm");

	int api = ioctl(kvm, KVM_GET_API_VERSION, 0);
	if (api != KVM_API_VERSION)
		XFAIL("KVM_GET_API_VERSION");

	int vm = ioctl(kvm, KVM_CREATE_VM, 0);
	if (vm < 0)
		XFAIL("KVM_CREATE_VM");

	/* KVM requires this on x86 for TSS + identity map placement. */
	if (ioctl(vm, KVM_SET_TSS_ADDR, 0xfffbd000) < 0)
		XFAIL("KVM_SET_TSS_ADDR");

	unsigned char *mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
				  MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
				  -1, 0);
	if (mem == MAP_FAILED)
		XFAIL("mmap guest mem");

	memcpy(mem + CODE_OFFSET, guest_code, sizeof(guest_code));

	struct kvm_userspace_memory_region region = {
		.slot            = 0,
		.guest_phys_addr = 0,
		.memory_size     = MEM_SIZE,
		.userspace_addr  = (uintptr_t)mem,
	};
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0)
		XFAIL("KVM_SET_USER_MEMORY_REGION");

	int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	if (vcpu < 0)
		XFAIL("KVM_CREATE_VCPU");

	int mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size <= 0)
		XFAIL("KVM_GET_VCPU_MMAP_SIZE");

	struct kvm_run *run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
				   MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED)
		XFAIL("mmap kvm_run");

	struct kvm_sregs sregs;
	if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0)
		XFAIL("KVM_GET_SREGS");
	setup_real_mode(&sregs);
	if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0)
		XFAIL("KVM_SET_SREGS");

	struct kvm_regs regs = { 0 };
	regs.rip = 0;        /* real-mode IP — CS.base covers CODE_OFFSET */
	regs.rflags = 0x2;   /* reserved bit set; IF=0 (no interrupts) */
	if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0)
		XFAIL("KVM_SET_REGS");

	/*
	 * Measurement loop: each iteration re-sets the guest vCPU
	 * state and runs KVM_RUN once. The guest executes a single
	 * HLT and VMEXITs with reason == KVM_EXIT_HLT. We record
	 * the cycle delta; that's the "KVM round-trip" number the
	 * D-01 design memo needs.
	 */
	int exit_hist[64] = { 0 };
	const int iters = 1000;
	uint64_t cycles_hlt[iters];
	int measured = 0;

	for (int i = 0; i < iters; i++) {
		if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0)
			XFAIL("KVM_SET_REGS reset");

		uint64_t c = 0;
		(void)run_once(vcpu, &c);

		uint32_t reason = run->exit_reason;
		if (reason < 64)
			exit_hist[reason]++;
		if (reason == KVM_EXIT_HLT) {
			cycles_hlt[measured++] = c;
		}
	}

	if (measured == 0) {
		fprintf(stderr, "FAIL: zero measurable iterations\n");
		fprintf(stderr, "exit-reason histogram (over all attempts):\n");
		for (int i = 0; i < 64; i++)
			if (exit_hist[i])
				fprintf(stderr, "  reason=%d count=%d\n",
					i, exit_hist[i]);
		return 1;
	}

	/* Simple median via selection sort of a ≤iters-element array. */
	for (int i = 0; i < measured - 1; i++)
		for (int j = i + 1; j < measured; j++) {
			if (cycles_hlt[j] < cycles_hlt[i]) {
				uint64_t t = cycles_hlt[i];
				cycles_hlt[i] = cycles_hlt[j];
				cycles_hlt[j] = t;
			}
		}

	uint64_t median_hlt = cycles_hlt[measured / 2];
	uint64_t p10 = cycles_hlt[measured / 10];
	uint64_t p90 = cycles_hlt[(measured * 9) / 10];
	uint64_t sum = 0;
	for (int i = 0; i < measured; i++)
		sum += cycles_hlt[i];
	uint64_t mean = sum / measured;
	double mhz = cpu_mhz();

	printf("iterations              : %d\n", measured);
	printf("cpu MHz (approx)        : %.0f\n", mhz);
	printf("p10 cycles              : %" PRIu64, p10);
	if (mhz > 0)
		printf(" (~%.0f ns)", p10 * 1000.0 / mhz);
	printf("\n");
	printf("median cycles           : %" PRIu64, median_hlt);
	if (mhz > 0)
		printf(" (~%.0f ns)", median_hlt * 1000.0 / mhz);
	printf("\n");
	printf("mean cycles             : %" PRIu64, mean);
	if (mhz > 0)
		printf(" (~%.0f ns)", mean * 1000.0 / mhz);
	printf("\n");
	printf("p90 cycles              : %" PRIu64, p90);
	if (mhz > 0)
		printf(" (~%.0f ns)", p90 * 1000.0 / mhz);
	printf("\n");

	munmap(run, mmap_size);
	close(vcpu);
	close(vm);
	munmap(mem, MEM_SIZE);
	close(kvm);
	return 0;
}
