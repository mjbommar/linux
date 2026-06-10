/*
 * Test: does SIGALRM-driven KVM_RUN -EINTR cause FPU corruption?
 *
 * Set up SIGALRM via setitimer to fire mid-execution. The guest spins
 * doing XMM operations. Host catches -EINTR, then re-enters. Verify
 * XMM state preserved.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/kvm.h>

#define MEM_SIZE (8 * 1024 * 1024)
#define CODE_BASE 0x100000
#define DATA_BASE 0x200000

static int fail(const char *msg) {
	fprintf(stderr, "FAIL: %s (errno=%d)\n", msg, errno);
	return 1;
}

/* Guest: load XMM, spin briefly, IO trap, readback XMM. */
static const unsigned char guest_code[] = {
	/* movdqu xmm0, [DATA_BASE+0] */
	0xf3, 0x0f, 0x6f, 0x04, 0x25,  0x00, 0x00, 0x20, 0x00,
	/* mov $0x10000, %rcx */
	0x48, 0xc7, 0xc1, 0x00, 0x00, 0x01, 0x00,
	/* spin loop: dec rcx; jnz */
	0x48, 0xff, 0xc9,
	0x75, 0xfb,
	/* out al, $0xf5 */
	0xb0, 0x42, 0xe6, 0xf5,
	/* movdqa [DATA_BASE+32], xmm0 */
	0x66, 0x0f, 0x7f, 0x04, 0x25,  0x20, 0x00, 0x20, 0x00,
	/* out al, $0xf6 */
	0xb0, 0x99, 0xe6, 0xf6,
	/* jmp back */
	0xeb, 0xd0,
};

static void build_pt(void *mem) {
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

static volatile int sigalrm_count = 0;
static void sigalrm_handler(int sig) { sigalrm_count++; }

#define ITERS 100

int main(int argc, char **argv) {
	int kvm_fd, vm_fd, vcpu_fd;
	struct kvm_userspace_memory_region mem_region;
	void *mem;
	struct kvm_run *run;
	int run_size;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	struct itimerval itv;
	int rc;

	signal(SIGALRM, sigalrm_handler);

	kvm_fd = open("/dev/kvm", O_RDWR);
	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	mem = mmap(NULL, MEM_SIZE, PROT_READ|PROT_WRITE,
		   MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	memset(mem, 0, MEM_SIZE);
	mem_region.slot = 0;
	mem_region.flags = 0;
	mem_region.guest_phys_addr = 0;
	mem_region.memory_size = MEM_SIZE;
	mem_region.userspace_addr = (uintptr_t)mem;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &mem_region) < 0)
		return fail("memreg");
	build_pt(mem);
	memcpy((char *)mem + CODE_BASE, guest_code, sizeof(guest_code));

	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	run_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	run = mmap(NULL, run_size, PROT_READ|PROT_WRITE, MAP_SHARED, vcpu_fd, 0);

	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0) return fail("get_sregs");
	sregs.cr3 = 0x1000;
	sregs.cr4 = (1ULL << 5) | (1ULL << 9) | (1ULL << 10);
	sregs.cr0 = (1ULL << 0) | (1ULL << 4) | (1ULL << 5) | (1ULL << 31);
	sregs.efer = (1ULL << 8) | (1ULL << 10) | (1ULL << 11);
	struct kvm_segment seg = {
		.base = 0, .limit = 0xffffffff, .selector = 0x8,
		.type = 0xb, .present = 1, .dpl = 0, .s = 1, .l = 1, .g = 1,
	};
	sregs.cs = seg;
	seg.type = 0x3; seg.selector = 0x10;
	sregs.ds = seg; sregs.es = seg; sregs.fs = seg; sregs.gs = seg; sregs.ss = seg;
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0) return fail("set_sregs");

	memset(&regs, 0, sizeof(regs));
	regs.rip = CODE_BASE;
	regs.rsp = DATA_BASE + 0x100000;
	regs.rflags = 2;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0) return fail("set_regs");

	/* Set up SIGALRM 100us interval - should fire within spin loop */
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 100;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 100;
	setitimer(ITIMER_REAL, &itv, NULL);

	int fails = 0;
	int eintr_count = 0;
	for (int iter = 0; iter < ITERS; iter++) {
		uint8_t pattern_byte = (uint8_t)(0x10 + iter);
		uint8_t *pattern_addr = (uint8_t *)mem + DATA_BASE + 0;
		uint8_t *readback_addr = (uint8_t *)mem + DATA_BASE + 32;
		memset(pattern_addr, pattern_byte, 16);
		memset(readback_addr, 0, 16);

		int saw_f5 = 0, saw_f6 = 0;
		while (!(saw_f5 && saw_f6)) {
			rc = ioctl(vcpu_fd, KVM_RUN, 0);
			if (rc < 0) {
				if (errno == EINTR) {
					eintr_count++;
					/* Touch host XMM to maximize chance of clobber */
					__asm__ __volatile__("pxor %%xmm0, %%xmm0" ::: "xmm0");
					continue;
				}
				return fail("KVM_RUN");
			}
			if (run->exit_reason != KVM_EXIT_IO) {
				fprintf(stderr, "iter%d: exit %u\n", iter, run->exit_reason);
				return 1;
			}
			if (run->io.port == 0xf5) saw_f5 = 1;
			if (run->io.port == 0xf6) saw_f6 = 1;
		}

		int matches = 0;
		for (int b = 0; b < 16; b++)
			if (readback_addr[b] == pattern_byte) matches++;
		if (matches != 16) {
			fails++;
			if (fails <= 3) {
				fprintf(stderr,
					"iter%d FAIL pattern=%02x readback=",
					iter, pattern_byte);
				for (int b = 0; b < 16; b++)
					fprintf(stderr, "%02x", readback_addr[b]);
				fprintf(stderr, "\n");
			}
		}
	}

	itv.it_value.tv_usec = 0;
	itv.it_interval.tv_usec = 0;
	setitimer(ITIMER_REAL, &itv, NULL);

	printf("ITERS=%d FAILS=%d EINTR=%d SIGALRM=%d\n",
	       ITERS, fails, eintr_count, sigalrm_count);
	return fails > 0 ? 2 : 0;
}
