/*
 * Mimic UML's task switching: multiple "tasks" share one vcpu.
 * Between dispatches, the host (us) saves task A's FPU and loads task B's,
 * then runs B. After B exits, save B's FPU, load A's, run A. Verify each
 * task's FPU is preserved across the trip.
 *
 * If this test fails, KVM has a bug in FPU isolation across SET_FPU / RUN
 * cycles.
 *
 * Also test the WITHOUT explicit SET_FPU case to see if KVM's guest_fpu
 * gets clobbered between exits.
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
 *     movdqu xmm0, [DATA_BASE+0]   ; load pattern
 *     out al, $0xf5                 ; KVM_EXIT_IO
 *     movdqa [DATA_BASE+32], xmm0  ; readback xmm0 (post-exit)
 *     out al, $0xf6                 ; signal "iter done"
 *     jmp loop
 */
static const unsigned char guest_code[] = {
	0xf3, 0x0f, 0x6f, 0x04, 0x25,  0x00, 0x00, 0x20, 0x00,
	0xb0, 0x42,
	0xe6, 0xf5,
	0x66, 0x0f, 0x7f, 0x04, 0x25,  0x20, 0x00, 0x20, 0x00,
	0xb0, 0x99,
	0xe6, 0xf6,
	0xeb, 0xe4,
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

#define ITERS 200
#define NTASKS 3

int main(int argc, char **argv) {
	int kvm_fd, vm_fd, vcpu_fd;
	struct kvm_userspace_memory_region mem_region;
	void *mem;
	struct kvm_run *run;
	int run_size;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	int rc;

	int explicit_set_fpu = (argc > 1 && !strcmp(argv[1], "set_fpu"));
	int touch_host_fpu = (argc > 2 && !strcmp(argv[2], "touch_xmm"));

	kvm_fd = open("/dev/kvm", O_RDWR);
	if (kvm_fd < 0) return fail("open /dev/kvm");
	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0) return fail("KVM_CREATE_VM");

	mem = mmap(NULL, MEM_SIZE, PROT_READ|PROT_WRITE,
		   MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	memset(mem, 0, MEM_SIZE);
	mem_region.slot = 0;
	mem_region.flags = 0;
	mem_region.guest_phys_addr = 0;
	mem_region.memory_size = MEM_SIZE;
	mem_region.userspace_addr = (uintptr_t)mem;
	if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &mem_region) < 0)
		return fail("SET_USER_MEMORY_REGION");

	build_pt(mem);
	memcpy((char *)mem + CODE_BASE, guest_code, sizeof(guest_code));

	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	if (vcpu_fd < 0) return fail("KVM_CREATE_VCPU");
	run_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	run = mmap(NULL, run_size, PROT_READ|PROT_WRITE, MAP_SHARED, vcpu_fd, 0);

	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0) return fail("GET_SREGS");
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
	if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0) return fail("SET_SREGS");

	memset(&regs, 0, sizeof(regs));
	regs.rip = CODE_BASE;
	regs.rsp = DATA_BASE + 0x100000;
	regs.rflags = 2;
	if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0) return fail("SET_REGS");

	/* Per-task FPU snapshots (just like UML's arch_thread.kvm_v2.fpu) */
	struct kvm_fpu task_fpus[NTASKS];
	memset(task_fpus, 0, sizeof(task_fpus));
	int task_fpu_valid[NTASKS] = {0};

	int fails = 0;
	for (int iter = 0; iter < ITERS; iter++) {
		int tid = iter % NTASKS;
		uint8_t pattern_byte = (uint8_t)(0x10 + tid * 0x10 + (iter / NTASKS) % 0x10);
		uint8_t *pattern_addr = (uint8_t *)mem + DATA_BASE + 0;
		uint8_t *readback_addr = (uint8_t *)mem + DATA_BASE + 32;
		memset(pattern_addr, pattern_byte, 16);
		memset(readback_addr, 0, 16);

		/* Restore this task's FPU snapshot */
		if (explicit_set_fpu && task_fpu_valid[tid]) {
			if (ioctl(vcpu_fd, KVM_SET_FPU, &task_fpus[tid]) < 0)
				return fail("SET_FPU");
		}

		/* Optionally touch host XMM to simulate libc memset use */
		if (touch_host_fpu) {
			__asm__ __volatile__("pxor %%xmm0, %%xmm0\n" ::: "xmm0", "memory");
		}

		int saw_f5 = 0, saw_f6 = 0;
		while (!(saw_f5 && saw_f6)) {
			rc = ioctl(vcpu_fd, KVM_RUN, 0);
			if (rc < 0) {
				fprintf(stderr, "iter%d: KVM_RUN failed errno=%d\n", iter, errno);
				return 1;
			}
			if (run->exit_reason != KVM_EXIT_IO) {
				fprintf(stderr, "iter%d: unexpected exit %u\n",
					iter, run->exit_reason);
				return 1;
			}
			if (run->io.port == 0xf5) saw_f5 = 1;
			if (run->io.port == 0xf6) saw_f6 = 1;
		}

		/* Save FPU snapshot for this task */
		if (explicit_set_fpu) {
			if (ioctl(vcpu_fd, KVM_GET_FPU, &task_fpus[tid]) < 0)
				return fail("GET_FPU");
			task_fpu_valid[tid] = 1;
		}

		int matches = 0;
		for (int b = 0; b < 16; b++)
			if (readback_addr[b] == pattern_byte) matches++;
		if (matches != 16) {
			fails++;
			if (fails <= 3) {
				fprintf(stderr,
					"iter%d tid%d FAIL pattern=%02x readback=",
					iter, tid, pattern_byte);
				for (int b = 0; b < 16; b++)
					fprintf(stderr, "%02x", readback_addr[b]);
				fprintf(stderr, "\n");
			}
		}
	}

	printf("ITERS=%d NTASKS=%d FAILS=%d (%s%s)\n",
	       ITERS, NTASKS, fails,
	       explicit_set_fpu ? "explicit_fpu_save" : "no_fpu_save",
	       touch_host_fpu ? "+touch_host_xmm" : "");
	return fails > 0 ? 2 : 0;
}
