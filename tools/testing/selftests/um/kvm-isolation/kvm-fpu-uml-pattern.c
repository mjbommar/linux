/*
 * Extended KVM-only test mimicking UML v2's dispatch pattern more closely.
 *
 * Key UML-specific things we test:
 *  1. Use SYNC_REGS dirty bits (kvm_dirty_regs |= KVM_SYNC_X86_REGS) to update
 *     regs between KVM_RUNs — UML uses this to avoid KVM_SET_REGS ioctls.
 *  2. Many KVM_RUN cycles (multiple IO traps).
 *  3. Modify CR4.PGE between dispatches via SYNC_X86_SREGS dirty bit
 *     (UML's TLB flush trick).
 *  4. Loop: load XMM with iter-specific pattern, IO trap, check readback.
 *
 * If ANY iteration's readback differs from pattern, KVM didn't preserve XMM
 * across that specific cycle's exit/re-entry boundary.
 *
 * Build: gcc -O2 -static -o kvm-fpu-uml-pattern kvm-fpu-uml-pattern.c
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
#define CODE_BASE 0x100000
#define DATA_BASE 0x200000

static int fail(const char *msg) {
	fprintf(stderr, "FAIL: %s (errno=%d %s)\n", msg, errno, strerror(errno));
	return 1;
}

/*
 * Guest code (loops):
 *   loop:
 *     movdqu xmm0, [DATA_BASE+0]   ; load pattern (host updates this each iter)
 *     mov $0x42, %al
 *     out %al, $0xf5               ; KVM_EXIT_IO (UML's #PF stub-out pattern)
 *     movdqa [DATA_BASE+32], xmm0  ; readback xmm0 (host checks)
 *     mov $0x99, %al
 *     out %al, $0xf6               ; signal "iter done"
 *     jmp loop
 *
 * Host updates pattern at DATA_BASE+0 between iterations and verifies
 * readback at DATA_BASE+32.
 */
static const unsigned char guest_code[] = {
	/* 0:  movdqu xmm0, [0x200000] */
	0xf3, 0x0f, 0x6f, 0x04, 0x25,
	0x00, 0x00, 0x20, 0x00,
	/* 9:  mov $0x42, %al */
	0xb0, 0x42,
	/* 11: out %al, $0xf5 */
	0xe6, 0xf5,
	/* 13: movdqa [0x200020], xmm0 */
	0x66, 0x0f, 0x7f, 0x04, 0x25,
	0x20, 0x00, 0x20, 0x00,
	/* 22: mov $0x99, %al */
	0xb0, 0x99,
	/* 24: out %al, $0xf6 */
	0xe6, 0xf6,
	/* 26: jmp $-26 (back to start) */
	0xeb, 0xe4,
};

static void build_page_tables(void *mem) {
	uint64_t *pml4 = (uint64_t *)((char *)mem + 0x1000);
	uint64_t *pdpt = (uint64_t *)((char *)mem + 0x2000);
	uint64_t *pd   = (uint64_t *)((char *)mem + 0x3000);
	uint64_t *pt0  = (uint64_t *)((char *)mem + 0x4000);
	uint64_t *pt1  = (uint64_t *)((char *)mem + 0x5000);

	pml4[0] = 0x2000 | 0x3;
	pdpt[0] = 0x3000 | 0x3;
	pd[0]   = 0x4000 | 0x3;
	pd[1]   = 0x5000 | 0x3;
	for (int i = 0; i < 512; i++) {
		pt0[i] = (i * 0x1000) | 0x3;
		pt1[i] = ((i + 512) * 0x1000) | 0x3;
	}
}

#define ITERS 200

int main(int argc, char **argv) {
	int kvm_fd, vm_fd, vcpu_fd;
	struct kvm_userspace_memory_region mem_region;
	void *mem;
	struct kvm_run *run;
	int run_size;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	int rc;
	int sync_caps;

	int use_sync_regs = (argc > 1 && !strcmp(argv[1], "sync"));
	int use_pge_toggle = (argc > 2 && !strcmp(argv[2], "pge"));

	kvm_fd = open("/dev/kvm", O_RDWR);
	if (kvm_fd < 0) return fail("open /dev/kvm");

	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0) return fail("KVM_CREATE_VM");

	mem = mmap(NULL, MEM_SIZE, PROT_READ|PROT_WRITE,
		   MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) return fail("mmap");
	memset(mem, 0, MEM_SIZE);

	mem_region.slot = 0;
	mem_region.flags = 0;
	mem_region.guest_phys_addr = 0;
	mem_region.memory_size = MEM_SIZE;
	mem_region.userspace_addr = (uintptr_t)mem;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &mem_region) < 0)
		return fail("SET_USER_MEMORY_REGION");

	build_page_tables(mem);
	memcpy((char *)mem + CODE_BASE, guest_code, sizeof(guest_code));

	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0) return fail("KVM_CREATE_VCPU");

	run_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	run = mmap(NULL, run_size, PROT_READ|PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
	if (run == MAP_FAILED) return fail("mmap kvm_run");

	/* Enable SYNC_REGS if requested. */
	sync_caps = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_SYNC_REGS);
	if (use_sync_regs) {
		run->kvm_valid_regs = KVM_SYNC_X86_REGS | KVM_SYNC_X86_SREGS;
		printf("Using SYNC_REGS (caps=0x%x)\n", sync_caps);
	}

	/* Long-mode SREGS. */
	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0) return fail("GET_SREGS");
	sregs.cr3 = 0x1000;
	sregs.cr4 = (1ULL << 5) | (1ULL << 9) | (1ULL << 10);  /* PAE|OSFXSR|OSXMMEXCPT */
	sregs.cr0 = (1ULL << 0) | (1ULL << 4) | (1ULL << 5) | (1ULL << 31);
	sregs.efer = (1ULL << 8) | (1ULL << 10) | (1ULL << 11);
	struct kvm_segment seg = {
		.base = 0, .limit = 0xffffffff, .selector = 0x8,
		.type = 0xb, .present = 1, .dpl = 0, .db = 0, .s = 1,
		.l = 1, .g = 1, .avl = 0,
	};
	sregs.cs = seg;
	seg.type = 0x3; seg.selector = 0x10;
	sregs.ds = seg; sregs.es = seg; sregs.fs = seg; sregs.gs = seg; sregs.ss = seg;
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0) return fail("SET_SREGS");

	memset(&regs, 0, sizeof(regs));
	regs.rip = CODE_BASE;
	regs.rsp = DATA_BASE + 0x100000;
	regs.rflags = 2;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0) return fail("SET_REGS");

	/* Run loop. Each iteration: update pattern, run, check readback. */
	int fails = 0;
	for (int iter = 0; iter < ITERS; iter++) {
		uint8_t pattern_byte = (uint8_t)(iter + 1);  /* unique per iter */
		uint8_t *pattern_addr = (uint8_t *)mem + DATA_BASE + 0;
		uint8_t *readback_addr = (uint8_t *)mem + DATA_BASE + 32;
		memset(pattern_addr, pattern_byte, 16);
		memset(readback_addr, 0, 16);

		/* Optionally toggle CR4.PGE between iters (UML's TLB flush trick) */
		if (use_pge_toggle && use_sync_regs) {
			run->s.regs.sregs.cr4 ^= (1ULL << 7);  /* CR4.PGE */
			run->kvm_dirty_regs |= KVM_SYNC_X86_SREGS;
		}

		/* Run guest until both 0xf5 and 0xf6 IO traps fire */
		int saw_f5 = 0, saw_f6 = 0;
		while (!(saw_f5 && saw_f6)) {
			rc = ioctl(vcpu_fd, KVM_RUN, 0);
			if (rc < 0) {
				fprintf(stderr, "iter%d: KVM_RUN failed errno=%d\n", iter, errno);
				return 1;
			}
			if (run->exit_reason != KVM_EXIT_IO) {
				fprintf(stderr, "iter%d: unexpected exit %u\n", iter, run->exit_reason);
				return 1;
			}
			if (run->io.port == 0xf5) saw_f5 = 1;
			if (run->io.port == 0xf6) saw_f6 = 1;
		}

		/* Check readback */
		int matches = 0;
		for (int b = 0; b < 16; b++)
			if (readback_addr[b] == pattern_byte) matches++;
		if (matches != 16) {
			fails++;
			if (fails <= 3) {
				fprintf(stderr, "iter%d FAIL pattern=%02x matches=%d readback=",
					iter, pattern_byte, matches);
				for (int b = 0; b < 16; b++)
					fprintf(stderr, "%02x", readback_addr[b]);
				fprintf(stderr, "\n");
			}
		}
	}

	printf("ITERS=%d FAILS=%d (%s%s)\n",
	       ITERS, fails,
	       use_sync_regs ? "sync_regs" : "set_regs",
	       use_pge_toggle ? "+pge_toggle" : "");
	return fails > 0 ? 2 : 0;
}
