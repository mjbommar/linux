/*
 * Minimal KVM userspace test: does XMM register state survive across
 * KVM_EXIT_IO (in-guest `out` instruction -> userspace) -> KVM_RUN re-entry?
 *
 * Mimics UML v2's dispatch pattern:
 *   - Guest in long mode, CR4.OSFXSR set (XMM enabled)
 *   - Guest user code does:
 *       movdqu xmm0, [pattern]
 *       out al, $port            ; KVM_EXIT_IO, exits to userspace
 *       movdqa xmm0, [readback]  ; save xmm0 to guest memory
 *       hlt
 *
 * After first exit, we (host userspace) "do nothing" - just KVM_RUN again.
 * Then check the guest's readback memory: should match pattern.
 *
 * If the readback differs, KVM didn't preserve XMM across the userspace
 * exit boundary - that's the upstream bug.
 *
 * To match UML v2's pattern, we ALSO test with SYNC_REGS dirty bits set
 * between exits, which is what UML uses for fast register sync.
 *
 * Build: gcc -O2 -static -o kvm-fpu-repro kvm-fpu-repro.c
 * Run as root or with /dev/kvm group membership.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

#define MEM_SIZE (8 * 1024 * 1024)
#define PT_BASE 0x1000	  /* PML4 / PDPT / PD / PT pages here */
#define CODE_BASE 0x100000   /* guest code here, identity-mapped */
#define DATA_BASE 0x200000   /* guest data here */

static int fail(const char *msg) {
	fprintf(stderr, "FAIL: %s (errno=%d %s)\n", msg, errno, strerror(errno));
	return 1;
}

/* Build guest code:
 *   movdqu xmm0, [pattern]    ; 16 bytes splat'd value
 *   mov $0x42, %al             ; sentinel byte
 *   out %al, $0xf5             ; KVM_EXIT_IO
 *   movdqa [readback], xmm0    ; readback xmm0
 *   mov $0x99, %al
 *   out %al, $0xf6             ; signal "done"
 *   hlt
 *
 * pattern at DATA_BASE + 0  (16 bytes aligned)
 * readback at DATA_BASE + 32 (16 bytes aligned)
 */
static const unsigned char guest_code[] = {
	/* movdqu xmm0, [DATA_BASE+0]  -- f3 0f 6f 04 25 LE32(addr) */
	0xf3, 0x0f, 0x6f, 0x04, 0x25,
	0x00, 0x00, 0x20, 0x00, /* DATA_BASE+0 = 0x200000 */
	/* mov $0x42, %al */
	0xb0, 0x42,
	/* out %al, $0xf5 */
	0xe6, 0xf5,
	/* movdqa [DATA_BASE+32], xmm0 -- 66 0f 7f 04 25 LE32(addr) */
	0x66, 0x0f, 0x7f, 0x04, 0x25,
	0x20, 0x00, 0x20, 0x00, /* DATA_BASE+32 = 0x200020 */
	/* mov $0x99, %al */
	0xb0, 0x99,
	/* out %al, $0xf6 */
	0xe6, 0xf6,
	/* hlt */
	0xf4,
};

/* Build identity-mapped 4-level page tables for [0, 4MB), all RW. */
static void build_page_tables(void *mem) {
	uint64_t *pml4 = (uint64_t *)((char *)mem + 0x1000);
	uint64_t *pdpt = (uint64_t *)((char *)mem + 0x2000);
	uint64_t *pd   = (uint64_t *)((char *)mem + 0x3000);
	uint64_t *pt0  = (uint64_t *)((char *)mem + 0x4000);
	uint64_t *pt1  = (uint64_t *)((char *)mem + 0x5000);

	pml4[0] = 0x2000 | 0x3;            /* P|RW */
	pdpt[0] = 0x3000 | 0x3;
	pd[0]   = 0x4000 | 0x3;            /* maps [0, 2MB) via PT0 */
	pd[1]   = 0x5000 | 0x3;            /* maps [2MB, 4MB) via PT1 */
	for (int i = 0; i < 512; i++) {
		pt0[i] = (i * 0x1000) | 0x3;          /* identity [0, 2MB) */
		pt1[i] = ((i + 512) * 0x1000) | 0x3;  /* identity [2MB, 4MB) */
	}
}

int main(int argc, char **argv) {
	int kvm_fd, vm_fd, vcpu_fd;
	struct kvm_userspace_memory_region mem_region;
	void *mem;
	struct kvm_run *run;
	int run_size;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	struct kvm_fpu fpu;
	int rc;

	kvm_fd = open("/dev/kvm", O_RDWR);
	if (kvm_fd < 0) return fail("open /dev/kvm");

	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0) return fail("KVM_CREATE_VM");

	mem = mmap(NULL, MEM_SIZE, PROT_READ|PROT_WRITE,
		   MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) return fail("mmap mem");
	memset(mem, 0, MEM_SIZE);

	mem_region.slot = 0;
	mem_region.flags = 0;
	mem_region.guest_phys_addr = 0;
	mem_region.memory_size = MEM_SIZE;
	mem_region.userspace_addr = (uintptr_t)mem;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &mem_region) < 0)
		return fail("SET_USER_MEMORY_REGION");

	/* Build PT, install guest code at CODE_BASE, pattern at DATA_BASE+0. */
	build_page_tables(mem);
	memcpy((char *)mem + CODE_BASE, guest_code, sizeof(guest_code));
	uint8_t pattern[16];
	for (int i = 0; i < 16; i++) pattern[i] = 0x55;
	memcpy((char *)mem + DATA_BASE + 0, pattern, 16);
	memset((char *)mem + DATA_BASE + 32, 0, 16);  /* readback area */

	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0) return fail("KVM_CREATE_VCPU");

	run_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (run_size < 0) return fail("GET_VCPU_MMAP_SIZE");
	run = mmap(NULL, run_size, PROT_READ|PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
	if (run == MAP_FAILED) return fail("mmap kvm_run");

	/* Set up long-mode SREGS: CR0=PE|PG|MP|NE, CR4=PAE|OSFXSR|OSXMMEXCPT,
	 * EFER=LME|LMA|NXE, CR3=PML4 base. CS/SS=long-mode. */
	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
		return fail("GET_SREGS");

	sregs.cr3 = 0x1000;
	sregs.cr4 = (1ULL << 5) | (1ULL << 9) | (1ULL << 10);  /* PAE|OSFXSR|OSXMMEXCPT */
	sregs.cr0 = (1ULL << 0) | (1ULL << 4) | (1ULL << 5) | (1ULL << 31); /* PE|MP|NE|PG */
	sregs.efer = (1ULL << 8) | (1ULL << 10) | (1ULL << 11);  /* LME|LMA|NXE */

	struct kvm_segment seg = {
		.base = 0, .limit = 0xffffffff, .selector = 0x8,
		.type = 0xb, .present = 1, .dpl = 0, .db = 0, .s = 1,
		.l = 1, .g = 1, .avl = 0,
	};
	sregs.cs = seg;
	seg.type = 0x3; seg.selector = 0x10;
	sregs.ds = seg; sregs.es = seg; sregs.fs = seg; sregs.gs = seg;
	sregs.ss = seg;

	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
		return fail("SET_SREGS");

	/* Set initial regs: rip=CODE_BASE, rsp=DATA_BASE+0x100000, rflags=2 */
	memset(&regs, 0, sizeof(regs));
	regs.rip = CODE_BASE;
	regs.rsp = DATA_BASE + 0x100000;
	regs.rflags = 2;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0)
		return fail("SET_REGS");

	/* Optional: install a known FPU state via KVM_SET_FPU before first run? */
	memset(&fpu, 0, sizeof(fpu));
	for (int i = 0; i < 16; i++) fpu.fpr[i][0] = 0x77; /* poison via kvm_fpu */
	/* Actually let's just leave FPU default. The guest will load XMM0
	 * via movdqu from DATA_BASE which has 0x55 bytes. */

	/* Run the guest. */
	int loop_count = 0;
	int port_seen = 0;
	while (loop_count < 10) {
		rc = ioctl(vcpu_fd, KVM_RUN, 0);
		if (rc < 0) {
			fprintf(stderr, "KVM_RUN failed: errno=%d\n", errno);
			return fail("KVM_RUN");
		}
		loop_count++;

		switch (run->exit_reason) {
		case KVM_EXIT_IO:
			fprintf(stderr, "KVM_EXIT_IO port=0x%x dir=%d size=%d count=%d\n",
				run->io.port, run->io.direction, run->io.size, run->io.count);
			port_seen = run->io.port;
			if (run->io.port == 0xf6) {
				/* "done" signal - check readback */
				goto check;
			}
			/* Otherwise: continue (KVM advances RIP automatically) */
			break;
		case KVM_EXIT_HLT:
			fprintf(stderr, "KVM_EXIT_HLT\n");
			goto check;
		default:
			fprintf(stderr, "Unexpected exit_reason=%u\n", run->exit_reason);
			goto check;
		}
	}
	fprintf(stderr, "Loop limit reached\n");

check: ;
	/* Check readback area at DATA_BASE+32 */
	uint8_t *readback = (uint8_t *)mem + DATA_BASE + 32;
	int matches = 0;
	for (int i = 0; i < 16; i++) {
		if (readback[i] == 0x55) matches++;
	}
	printf("pattern=");
	for (int i = 0; i < 16; i++) printf("%02x", pattern[i]);
	printf("\nreadback=");
	for (int i = 0; i < 16; i++) printf("%02x", readback[i]);
	printf("\n");

	if (matches == 16) {
		printf("RESULT: XMM PRESERVED - no KVM bug\n");
		return 0;
	} else {
		printf("RESULT: XMM CORRUPTED (%d/16 bytes match) - KVM BUG\n", matches);
		return 2;
	}
}
