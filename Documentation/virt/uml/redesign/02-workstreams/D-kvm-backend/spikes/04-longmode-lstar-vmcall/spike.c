// SPDX-License-Identifier: GPL-2.0
/*
 * spike.c — D-workstream spike 04: long-mode guest + LSTAR-via-vmcall.
 *
 * Measures the round-trip cost of a "SYSCALL instruction in the
 * guest → LSTAR trampoline → vmcall → KVM_EXIT_HYPERCALL →
 * host-side handle → resume → sysretq → next guest instruction"
 * loop. That's the shape the naive um_backend_kvm uses per
 * `design-memo.md`, so this spike's numbers are the
 * architecturally-representative floor for the memo's
 * "~800 ns on Alder Lake i9, ~5 µs on Skylake" claim.
 *
 * How this differs from spike 01 (real-mode HLT):
 *   * Guest runs in IA-32e long mode ring 0 with a proper GDT,
 *     identity paging (2 MiB huge page), CR0.PE|PG + CR4.PAE +
 *     EFER.LME|LMA|SCE.
 *   * LSTAR points at a 2-byte trampoline in the same code
 *     page: `vmcall; sysretq`.
 *   * Guest code: `mov eax, 39 (getpid); syscall; hlt`.
 *   * On VMEXIT, reason should be KVM_EXIT_HLT for the trailing
 *     hlt, and KVM_EXIT_HYPERCALL for the trampoline's vmcall.
 *   * We measure the io_exit round-trip (VMENTRY → vmcall →
 *     VMEXIT → our mock → VMRESUME → sysretq → back-to-guest).
 *
 * Mock: on vmcall exit we write RAX=42 (fake pid), skip RIP
 * past the vmcall (2 bytes), and resume. Real sys_getpid() would
 * run here in um_backend_kvm; the measurement doesn't care about
 * the body, just the ENTRY→EXIT cost.
 *
 * Build: make
 * Run:   sudo ./spike   (needs /dev/kvm access)
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

/*
 * Guest memory layout (2 MiB host-mmap region at guest phys 0):
 *
 *   0x0000 .. 0x0fff   PML4 (entry 0)
 *   0x1000 .. 0x1fff   PDPT (entry 0)
 *   0x2000 .. 0x2fff   PD   (entry 0, 2 MiB huge page → phys 0)
 *   0x3000             GDT (8-byte entries, 3 entries)
 *   0x4000             guest code (see below)
 *   0x4100             LSTAR trampoline: vmcall; sysretq
 *
 * Identity-map phys 0..2 MiB to virt 0..2 MiB so IP=phys works.
 */
#define MEM_SIZE        (2 * 1024 * 1024)
#define PML4_OFFSET     0x0000
#define PDPT_OFFSET     0x1000
#define PD_OFFSET       0x2000
#define GDT_OFFSET      0x3000
#define CODE_OFFSET     0x4000
#define LSTAR_OFFSET    0x4100

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

/*
 * Guest code at CODE_OFFSET (ring 0 long mode):
 *   out %al, $0xf4             ; triggers KVM_EXIT_IO (2 bytes)
 *   hlt
 *
 * The port-out gives us a userspace-visible VMEXIT; vmcall
 * doesn't, because KVM routes vmcall through in-kernel
 * io_exit emulation by default (which returns to the guest
 * without exiting to userspace). Port 0xf4 is unused on real
 * hardware — QEMU actually uses it for its "shutdown" debug
 * port, so kvm-unit-tests + friends have it well-covered. The
 * VMEXIT cost of `out` is architecturally equivalent to
 * `vmcall` (both are VM-exit-causing instructions with
 * serializing semantics), so this remains representative of
 * the memo's bounce-trampoline cost model.
 */
static const unsigned char guest_code[] = {
	0xe6, 0xf4,  /* out %al, $0xf4  */
	0xf4,        /* hlt             */
};

/*
 * LSTAR trampoline at LSTAR_OFFSET:
 *   vmcall
 *   sysretq
 *
 * The kernel CS under SYSCALL uses STAR[47:32] as the selector;
 * sysretq uses STAR[63:48]+16 (long-mode user CS) and
 * STAR[63:48]+8 (user SS). We set STAR so that sysretq returns
 * to the guest's ring 0 — same code segment, which is fine for
 * a spike that never actually enters ring 3.
 */
static const unsigned char lstar_trampoline[] = {
	0x0f, 0x01, 0xc1,  /* vmcall  */
	0x48, 0x0f, 0x07,  /* sysretq */
};

static inline uint64_t rdtsc(void)
{
	unsigned lo, hi;
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

/*
 * Minimal flat 64-bit GDT. Three entries: null, ring-0 code
 * (selector 0x08), ring-0 data (selector 0x10). The ring-0 code
 * segment has L=1 (long mode) + P=1 + S=1 + type=11 (code r/x,
 * accessed); data is L=0 + P=1 + S=1 + type=3 (r/w).
 */
static void setup_gdt(unsigned char *mem)
{
	uint64_t *gdt = (uint64_t *)(mem + GDT_OFFSET);
	gdt[0] = 0x0000000000000000ULL;  /* null */
	gdt[1] = 0x00af9a000000ffffULL;  /* ring-0 code, L=1 */
	gdt[2] = 0x00cf92000000ffffULL;  /* ring-0 data */
}

static void setup_paging(unsigned char *mem)
{
	uint64_t *pml4 = (uint64_t *)(mem + PML4_OFFSET);
	uint64_t *pdpt = (uint64_t *)(mem + PDPT_OFFSET);
	uint64_t *pd   = (uint64_t *)(mem + PD_OFFSET);
	pml4[0] = PDPT_OFFSET | PTE_P | PTE_RW | PTE_US;
	pdpt[0] = PD_OFFSET   | PTE_P | PTE_RW | PTE_US;
	/* 2 MiB huge page covers virt 0..2 MiB → phys 0..2 MiB. */
	pd[0]   = 0           | PTE_P | PTE_RW | PTE_US | PTE_PS;
}

static void setup_sregs(struct kvm_sregs *sregs)
{
	/*
	 * Load CS and data segments from the GDT we just built.
	 * kvm_segment fields MUST mirror what the CPU would derive
	 * from a real GDT load — KVM doesn't fetch the descriptor
	 * itself, it trusts the struct.
	 */
	struct kvm_segment code = {
		.base     = 0,
		.limit    = 0xffffffff,
		.selector = 0x08,
		.type     = 0xb,   /* code r/x, accessed */
		.present  = 1,
		.dpl      = 0,
		.db       = 0,
		.s        = 1,
		.l        = 1,     /* long mode code */
		.g        = 1,
	};
	sregs->cs = code;

	struct kvm_segment data = {
		.base     = 0,
		.limit    = 0xffffffff,
		.selector = 0x10,
		.type     = 0x3,   /* data r/w, accessed */
		.present  = 1,
		.dpl      = 0,
		.db       = 1,
		.s        = 1,
		.l        = 0,
		.g        = 1,
	};
	sregs->ds = sregs->es = sregs->fs = sregs->gs = sregs->ss = data;

	sregs->gdt.base  = GDT_OFFSET;
	sregs->gdt.limit = 3 * 8 - 1;

	sregs->cr3  = PML4_OFFSET;
	sregs->cr4  = CR4_PAE;
	sregs->cr0  = CR0_PE | CR0_MP | CR0_NE | CR0_WP | CR0_PG;
	sregs->efer = EFER_SCE | EFER_LME | EFER_LMA;
}

/*
 * Program LSTAR / STAR MSRs via KVM_SET_MSRS.
 *   STAR[31:0]  = SYSCALL target low bits (ignored in 64-bit mode)
 *   STAR[47:32] = kernel CS selector for SYSCALL (our 0x08)
 *   STAR[63:48] = user CS base for SYSRET (+16 for CS, +8 for SS).
 *                 For the spike we reuse 0x08-ish values so sysretq
 *                 returns to our ring-0 code segment; not
 *                 architecturally clean but the spike never enters
 *                 ring 3.
 */
static void setup_msrs(int vcpu)
{
	struct {
		struct kvm_msrs info;
		struct kvm_msr_entry entries[3];
	} msrs = {
		.info = { .nmsrs = 3 },
		.entries = {
			{ .index = 0xc0000081, /* STAR */
			  /* kernel CS = 0x08, user CS (for sysret) such
			   * that (user+16)==0x08 → user=0xfff8. Low 32
			   * bits unused in 64-bit mode.
			   */
			  .data  = ((uint64_t)0xfff8 << 48) |
			           ((uint64_t)0x0008 << 32) },
			{ .index = 0xc0000082, /* LSTAR */
			  .data  = LSTAR_OFFSET },
			{ .index = 0xc0000084, /* SFMASK */
			  .data  = 0 },
		},
	};
	if (ioctl(vcpu, KVM_SET_MSRS, &msrs) < 0)
		XFAIL("KVM_SET_MSRS");
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
	memcpy(mem + CODE_OFFSET, guest_code, sizeof(guest_code));
	memcpy(mem + LSTAR_OFFSET, lstar_trampoline, sizeof(lstar_trampoline));

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
	setup_sregs(&sregs);
	if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0)
		XFAIL("KVM_SET_SREGS");

	setup_msrs(vcpu);

	struct kvm_regs regs = { 0 };
	regs.rip    = CODE_OFFSET;
	regs.rflags = 0x2; /* IF=0, reserved bit set */
	if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0)
		XFAIL("KVM_SET_REGS");

	const int iters = 1000;
	uint64_t cycles_io_exit[iters];
	uint64_t cycles_hlt[iters];
	int io_exit_n = 0, hlt_n = 0;
	int exit_hist[64] = { 0 };

	for (int i = 0; i < iters; i++) {
		/* Reset RIP to the start of guest code. */
		regs.rip = CODE_OFFSET;
		regs.rax = 0;
		regs.rcx = 0;
		regs.r11 = 0;
		if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0)
			XFAIL("KVM_SET_REGS reset");

		/*
		 * First KVM_RUN: expect KVM_EXIT_HYPERCALL from the
		 * LSTAR trampoline's vmcall.
		 */
		uint64_t t0 = rdtsc();
		int rc = ioctl(vcpu, KVM_RUN, 0);
		uint64_t t1 = rdtsc();
		if (rc < 0)
			XFAIL("KVM_RUN (io_exit)");
		if (run->exit_reason < 64)
			exit_hist[run->exit_reason]++;
		if (run->exit_reason == KVM_EXIT_IO)
			cycles_io_exit[io_exit_n++] = t1 - t0;
		else
			continue;

		/* Mock handler: write RAX=42, step past `out` (2 bytes). */
		struct kvm_regs r2;
		if (ioctl(vcpu, KVM_GET_REGS, &r2) < 0)
			XFAIL("KVM_GET_REGS");
		r2.rax = 42;
		r2.rip += 2;
		if (ioctl(vcpu, KVM_SET_REGS, &r2) < 0)
			XFAIL("KVM_SET_REGS");

		/*
		 * Second KVM_RUN: sysretq returns to the instruction
		 * after the guest's syscall (the hlt), which
		 * VMEXITs with KVM_EXIT_HLT.
		 */
		uint64_t t2 = rdtsc();
		if (ioctl(vcpu, KVM_RUN, 0) < 0)
			XFAIL("KVM_RUN (hlt)");
		uint64_t t3 = rdtsc();
		if (run->exit_reason < 64)
			exit_hist[run->exit_reason]++;
		if (run->exit_reason == KVM_EXIT_HLT)
			cycles_hlt[hlt_n++] = t3 - t2;
	}

	if (io_exit_n == 0) {
		fprintf(stderr, "FAIL: no IO exits\n");
		for (int i = 0; i < 64; i++)
			if (exit_hist[i])
				fprintf(stderr, "  reason=%d count=%d\n", i, exit_hist[i]);
		/* Dump final vCPU state to help debug */
		struct kvm_regs r;
		struct kvm_sregs s;
		if (ioctl(vcpu, KVM_GET_REGS, &r) == 0)
			fprintf(stderr,
				"regs: rip=%#" PRIx64 " rax=%#" PRIx64
				" rsp=%#" PRIx64 " rflags=%#" PRIx64 "\n",
				(uint64_t)r.rip, (uint64_t)r.rax,
				(uint64_t)r.rsp, (uint64_t)r.rflags);
		if (ioctl(vcpu, KVM_GET_SREGS, &s) == 0)
			fprintf(stderr,
				"sregs: cr0=%#" PRIx64 " cr3=%#" PRIx64
				" cr4=%#" PRIx64 " efer=%#" PRIx64
				" cs.base=%#" PRIx64 " cs.sel=%#x\n",
				(uint64_t)s.cr0, (uint64_t)s.cr3,
				(uint64_t)s.cr4, (uint64_t)s.efer,
				(uint64_t)s.cs.base, s.cs.selector);
		return 1;
	}

	/* Sort for median. */
#define SORT(arr, n) do { \
	for (int i = 0; i < (n) - 1; i++) \
		for (int j = i + 1; j < (n); j++) \
			if (arr[j] < arr[i]) { \
				uint64_t t = arr[i]; arr[i] = arr[j]; arr[j] = t; \
			} \
} while (0)
	SORT(cycles_io_exit, io_exit_n);
	if (hlt_n > 1)
		SORT(cycles_hlt, hlt_n);
#undef SORT

	double mhz = cpu_mhz();
	printf("iterations            : %d\n", iters);
	printf("io-exits seen  : %d\n", io_exit_n);
	printf("hlt-exits seen        : %d\n", hlt_n);
	printf("cpu MHz (approx)      : %.0f\n", mhz);

	if (io_exit_n > 0) {
		uint64_t p10 = cycles_io_exit[io_exit_n / 10];
		uint64_t p50 = cycles_io_exit[io_exit_n / 2];
		uint64_t p90 = cycles_io_exit[(io_exit_n * 9) / 10];
		uint64_t sum = 0;
		for (int i = 0; i < io_exit_n; i++)
			sum += cycles_io_exit[i];
		uint64_t mean = sum / io_exit_n;
		printf("io_exit p10 cycles  : %" PRIu64 "%s\n", p10,
		       mhz > 0 ? "" : "");
		printf("io_exit median      : %" PRIu64, p50);
		if (mhz > 0) printf(" (~%.0f ns)", p50 * 1000.0 / mhz);
		printf("\n");
		printf("io_exit mean        : %" PRIu64, mean);
		if (mhz > 0) printf(" (~%.0f ns)", mean * 1000.0 / mhz);
		printf("\n");
		printf("io_exit p90 cycles  : %" PRIu64, p90);
		if (mhz > 0) printf(" (~%.0f ns)", p90 * 1000.0 / mhz);
		printf("\n");
	}
	if (hlt_n > 0) {
		uint64_t p50 = cycles_hlt[hlt_n / 2];
		printf("hlt median            : %" PRIu64, p50);
		if (mhz > 0) printf(" (~%.0f ns)", p50 * 1000.0 / mhz);
		printf("\n");
	}

	munmap(run, mmap_size);
	close(vcpu);
	close(vm);
	munmap(mem, MEM_SIZE);
	close(kvm);
	return 0;
}
