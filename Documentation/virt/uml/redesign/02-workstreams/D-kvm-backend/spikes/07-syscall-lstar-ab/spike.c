// SPDX-License-Identifier: GPL-2.0
/*
 * spike.c — D-workstream spike 07: SYSCALL-via-LSTAR A/B.
 *
 * Answers: how much of the naive-backend per-syscall cost is
 * the VMEXIT round-trip (measured by spike 04) vs the
 * SYSCALL/SYSRET instruction pair the guest has to execute
 * on each syscall (measured here)?
 *
 * Two guest-code variants, run back-to-back on the same host
 * and same vCPU so the comparison is apples-to-apples:
 *
 *   Variant A — "direct IO exit":
 *       out %al, $0xf4       ; triggers KVM_EXIT_IO
 *       hlt
 *     Matches spike 04. Cost = just the VMEXIT round-trip.
 *
 *   Variant B — "SYSCALL via LSTAR trampoline":
 *       mov $0x27, %eax      ; __NR_getpid
 *       syscall              ; → LSTAR
 *       ...
 *       hlt
 *     LSTAR trampoline at a separate VA does:
 *       out %al, $0xf4       ; triggers KVM_EXIT_IO
 *       sysretq              ; → RCX (== post-syscall RIP)
 *     Cost = SYSCALL + LSTAR-trampoline + IO-exit +
 *            SYSRETQ + return-to-next-guest-insn.
 *
 * The delta B − A on the same run is the SYSCALL+SYSRET
 * overhead the design memo's bounce trampoline pays on top
 * of the bare IO-exit VMEXIT.
 *
 * We run at ring 0 throughout (SYSCALL from ring 0 is
 * technically undefined per the SDM but functions uniformly
 * on all shipping Intel/AMD — the CPU just treats CPL=0
 * SYSCALL as "return via LSTAR, save RIP in RCX"). For an
 * end-to-end UML backend the SYSCALL would originate from
 * ring 3 (guest userspace); the per-instruction cost is the
 * same either way, so this spike's A/B delta is the
 * right bound.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#define XFAIL(what)                                                            \
	do {                                                                   \
		fprintf(stderr, "FAIL: %s: %s\n", (what), strerror(errno));    \
		exit(1);                                                       \
	} while (0)

#define MEM_SIZE        (2 * 1024 * 1024)
#define PML4_OFFSET     0x0000
#define PDPT_OFFSET     0x1000
#define PD_OFFSET       0x2000
#define GDT_OFFSET      0x3000
#define CODE_A_OFFSET   0x4000   /* variant A (IO exit + hlt) */
#define CODE_B_OFFSET   0x4100   /* variant B (syscall + hlt) */
#define LSTAR_OFFSET    0x4200   /* trampoline for variant B */

#define PTE_P           (1ULL << 0)
#define PTE_RW          (1ULL << 1)
#define PTE_US          (1ULL << 2)
#define PTE_PS          (1ULL << 7)

#define CR0_PE          (1UL << 0)
#define CR0_MP          (1UL << 1)
#define CR0_NE          (1UL << 5)
#define CR0_WP          (1UL << 16)
#define CR0_PG          (1UL << 31)
#define CR4_PAE         (1UL << 5)
#define EFER_SCE        (1UL << 0)
#define EFER_LME        (1UL << 8)
#define EFER_LMA        (1UL << 10)

/* Variant A: direct IO exit then hlt. Matches spike 04. */
static const unsigned char code_a[] = {
	0xe6, 0xf4,  /* out %al, $0xf4 */
	0xf4,        /* hlt */
};

/*
 * Variant B: set RAX to a syscall number, execute SYSCALL
 * which traps through LSTAR. The trampoline at LSTAR_OFFSET
 * does the VMEXIT on our behalf via `out`, then SYSRETQ
 * returns to RCX (the saved post-syscall RIP, which is the
 * `hlt` instruction).
 */
static const unsigned char code_b[] = {
	0xb8, 0x27, 0x00, 0x00, 0x00,  /* mov $0x27, %eax */
	0x0f, 0x05,                    /* syscall */
	0xf4,                          /* hlt */
};

/*
 * LSTAR trampoline for variant B:
 *   out %al, $0xf4        (2 bytes)
 *   sysretq               (2 bytes: 0x48 0x0f 0x07 — 3 bytes)
 *
 * Wait — sysretq is 3 bytes (0x48 0x0f 0x07 — REX.W + 0F 07).
 * We only advance RIP past the `out` (2 bytes) before VMRESUMing,
 * so the CPU reaches sysretq on its own.
 */
static const unsigned char lstar_tramp[] = {
	0xe6, 0xf4,        /* out %al, $0xf4 */
	0x48, 0x0f, 0x07,  /* sysretq */
};

static inline uint64_t rdtsc(void)
{
	unsigned int lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

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

static void setup_gdt(unsigned char *mem)
{
	uint64_t *gdt = (uint64_t *)(mem + GDT_OFFSET);
	gdt[0] = 0;
	gdt[1] = 0x00af9a000000ffffULL; /* ring-0 code, L=1 */
	gdt[2] = 0x00cf92000000ffffULL; /* ring-0 data */
}

static void setup_paging(unsigned char *mem)
{
	uint64_t *pml4 = (uint64_t *)(mem + PML4_OFFSET);
	uint64_t *pdpt = (uint64_t *)(mem + PDPT_OFFSET);
	uint64_t *pd   = (uint64_t *)(mem + PD_OFFSET);
	pml4[0] = PDPT_OFFSET | PTE_P | PTE_RW | PTE_US;
	pdpt[0] = PD_OFFSET   | PTE_P | PTE_RW | PTE_US;
	pd[0]   = 0           | PTE_P | PTE_RW | PTE_US | PTE_PS;
}

static void setup_sregs(struct kvm_sregs *sregs)
{
	struct kvm_segment code = {
		.base = 0, .limit = 0xffffffff, .selector = 0x08,
		.type = 0xb, .present = 1, .dpl = 0,
		.db = 0, .s = 1, .l = 1, .g = 1,
	};
	sregs->cs = code;
	struct kvm_segment data = {
		.base = 0, .limit = 0xffffffff, .selector = 0x10,
		.type = 0x3, .present = 1, .dpl = 0,
		.db = 1, .s = 1, .l = 0, .g = 1,
	};
	sregs->ds = sregs->es = sregs->fs = sregs->gs = sregs->ss = data;
	sregs->gdt.base = GDT_OFFSET;
	sregs->gdt.limit = 3 * 8 - 1;
	sregs->cr3 = PML4_OFFSET;
	sregs->cr4 = CR4_PAE;
	sregs->cr0 = CR0_PE | CR0_MP | CR0_NE | CR0_WP | CR0_PG;
	sregs->efer = EFER_SCE | EFER_LME | EFER_LMA;
}

static void setup_msrs(int vcpu)
{
	struct {
		struct kvm_msrs info;
		struct kvm_msr_entry entries[3];
	} msrs = {
		.info = { .nmsrs = 3 },
		.entries = {
			{
				.index = 0xc0000081, /* STAR: kernel CS=0x08 */
				.data = ((uint64_t)0xfff8 << 48) |
					((uint64_t)0x0008 << 32)
			},
			{
				.index = 0xc0000082, /* LSTAR */
				.data = LSTAR_OFFSET
			},
			{
				.index = 0xc0000084, /* SFMASK */
				.data = 0
			},
		},
	};
	if (ioctl(vcpu, KVM_SET_MSRS, &msrs) < 0)
		XFAIL("KVM_SET_MSRS");
}

/* Sort ascending for median. */
static void sort_u64(uint64_t *a, int n)
{
	for (int i = 0; i < n - 1; i++)
		for (int j = i + 1; j < n; j++)
			if (a[j] < a[i]) {
				uint64_t t = a[i];
				a[i] = a[j];
				a[j] = t;
			}
}

static void report(const char *label, uint64_t *cyc, int n, double mhz)
{
	if (n == 0) {
		printf("  %-22s: no samples\n", label);
		return;
	}
	uint64_t sum = 0;
	for (int i = 0; i < n; i++)
		sum += cyc[i];
	uint64_t mean = sum / n;
	uint64_t p10 = cyc[n / 10];
	uint64_t p50 = cyc[n / 2];
	uint64_t p90 = cyc[(n * 9) / 10];
	printf("  %-22s: p10=%-6" PRIu64 " p50=%-6" PRIu64
	       " p90=%-6" PRIu64 " mean=%-6" PRIu64,
	       label, p10, p50, p90, mean);
	if (mhz > 0)
		printf(" (~%.0f ns @ p50)", p50 * 1000.0 / mhz);
	printf("\n");
}

int main(void)
{
	int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (kvm < 0)
		XFAIL("open /dev/kvm");

	int vm = ioctl(kvm, KVM_CREATE_VM, 0);
	if (vm < 0)
		XFAIL("KVM_CREATE_VM");
	if (ioctl(vm, KVM_SET_TSS_ADDR, 0xfffbd000) < 0)
		XFAIL("KVM_SET_TSS_ADDR");

	unsigned char *mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
				  MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
				  -1, 0);
	if (mem == MAP_FAILED)
		XFAIL("mmap guest mem");

	setup_gdt(mem);
	setup_paging(mem);
	memcpy(mem + CODE_A_OFFSET, code_a, sizeof(code_a));
	memcpy(mem + CODE_B_OFFSET, code_b, sizeof(code_b));
	memcpy(mem + LSTAR_OFFSET, lstar_tramp, sizeof(lstar_tramp));

	struct kvm_userspace_memory_region region = {
		.slot = 0, .guest_phys_addr = 0,
		.memory_size = MEM_SIZE, .userspace_addr = (uintptr_t)mem,
	};
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0)
		XFAIL("KVM_SET_USER_MEMORY_REGION");

	int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	if (vcpu < 0)
		XFAIL("KVM_CREATE_VCPU");
	int mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	struct kvm_run *run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
				   MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED)
		XFAIL("mmap kvm_run");

	struct kvm_sregs sregs;
	if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0)
		XFAIL("KVM_GET_SREGS");
	setup_sregs(&sregs);
	if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0)
		XFAIL("KVM_SET_SREGS");
	setup_msrs(vcpu);

	const int iters = 1000;
	uint64_t cyc_a[iters];
	uint64_t cyc_b[iters];
	int a_n = 0, b_n = 0;

	struct kvm_regs regs;

	/* Variant A: iters iterations. Start at CODE_A_OFFSET each time. */
	for (int i = 0; i < iters; i++) {
		memset(&regs, 0, sizeof(regs));
		regs.rip    = CODE_A_OFFSET;
		regs.rflags = 0x2;
		if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0)
			XFAIL("KVM_SET_REGS A");

		uint64_t t0 = rdtsc();
		ioctl(vcpu, KVM_RUN, 0);
		uint64_t t1 = rdtsc();

		if (run->exit_reason != KVM_EXIT_IO)
			continue;
		cyc_a[a_n++] = t1 - t0;
	}

	/* Variant B: SYSCALL variant. */
	for (int i = 0; i < iters; i++) {
		memset(&regs, 0, sizeof(regs));
		regs.rip    = CODE_B_OFFSET;
		regs.rflags = 0x2;
		if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0)
			XFAIL("KVM_SET_REGS B");

		uint64_t t0 = rdtsc();
		ioctl(vcpu, KVM_RUN, 0);
		uint64_t t1 = rdtsc();

		if (run->exit_reason != KVM_EXIT_IO)
			continue;
		cyc_b[b_n++] = t1 - t0;
	}

	sort_u64(cyc_a, a_n);
	sort_u64(cyc_b, b_n);

	double mhz = cpu_mhz();
	printf("iterations per variant  : %d\n", iters);
	printf("variant A measurable    : %d\n", a_n);
	printf("variant B measurable    : %d\n", b_n);
	printf("cpu MHz (approx)        : %.0f\n", mhz);
	report("A: direct IO exit", cyc_a, a_n, mhz);
	report("B: SYSCALL->LSTAR->IO", cyc_b, b_n, mhz);

	if (a_n > 0 && b_n > 0) {
		int64_t delta = (int64_t)cyc_b[b_n / 2] - (int64_t)cyc_a[a_n / 2];
		printf("\n  SYSCALL+SYSRET overhead : %" PRId64 " cycles",
		       delta);
		if (mhz > 0)
			printf(" (~%.0f ns)", delta * 1000.0 / mhz);
		printf("\n");
	}

	munmap(run, mmap_size);
	close(vcpu);
	close(vm);
	munmap(mem, MEM_SIZE);
	close(kvm);
	return 0;
}
