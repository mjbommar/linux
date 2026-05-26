// SPDX-License-Identifier: GPL-2.0
/*
 * Raw-syscall mmap stress test. Bypasses glibc's mmap wrapper to
 * eliminate any glibc-side translation of syscall return values.
 * Each pthread does mmap+memset+verify+munmap directly via inline
 * `syscall` instructions and inspects RAX bit-for-bit.
 *
 * Goal: prove whether the user-visible RAX after mmap is ever
 *  - 0 (NULL)
 *  - in errno-range [-4096, -1]
 *  - or a valid user-space address
 *
 * If the kernel says it's returning a valid address (per
 * UM_MMAP_DIAG/UM_MARSHAL_DIAG) but the user sees 0, this isolates
 * the corruption to the kernel→user RAX transport (sysretq + KVM
 * mmap) rather than glibc.
 *
 * Build: gcc -static -O0 -pthread -o mt-rawmmap mt-rawmmap.c
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static void dump_trace(void)
{
	int fd = open("/sys/kernel/debug/um_kvm_v2_trace/enabled", O_WRONLY);
	if (fd >= 0) { (void)!write(fd, "0\n", 2); close(fd); }
	fd = open("/sys/kernel/debug/um_kvm_v2_trace/dump", O_WRONLY);
	if (fd < 0) return;
	(void)!write(fd, "1\n", 2);
	close(fd);
}

#define ITERS    50
#define ALLOC_SZ 0x10000

static __thread long last_iter;

/*
 * Raw mmap via inline syscall. Mimics glibc __mmap but DOES NOT
 * translate errno-range to MAP_FAILED — returns rax verbatim. Caller
 * must distinguish 0 / errno-range / valid by inspection.
 */
static unsigned long raw_mmap(unsigned long addr, unsigned long len,
			      unsigned long prot, unsigned long flags,
			      long fd, unsigned long off)
{
	unsigned long ret;

	register unsigned long r10 asm("r10") = flags;
	register long r8 asm("r8") = fd;
	register unsigned long r9 asm("r9") = off;

	asm volatile (
		"syscall"
		: "=a"(ret)
		: "0"(__NR_mmap),
		  "D"(addr), "S"(len), "d"(prot),
		  "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory"
	);
	return ret;
}

static unsigned long raw_munmap(unsigned long addr, unsigned long len)
{
	unsigned long ret;
	asm volatile (
		"syscall"
		: "=a"(ret)
		: "0"(__NR_munmap), "D"(addr), "S"(len)
		: "rcx", "r11", "memory"
	);
	return ret;
}

static void *worker(void *arg)
{
	long tid = (long)arg;
	for (int i = 0; i < ITERS; i++) {
		last_iter = i;
		unsigned long ret = raw_mmap(0, ALLOC_SZ,
					     PROT_READ | PROT_WRITE,
					     MAP_PRIVATE | MAP_ANONYMOUS,
					     -1, 0);

		/* errno-range: rax >= 0xfffffffffffff001 (= -4095UL) */
		if ((ret & 0xfffffffffffff000UL) == 0xfffffffffffff000UL) {
			dump_trace();
			fprintf(stderr,
				"RAW_ERR tid=%ld iter=%d rax=%#lx errno=%ld\n",
				tid, i, ret, -(long)ret);
			return (void *)1;
		}
		if (ret == 0) {
			dump_trace();
			fprintf(stderr,
				"RAW_NULL tid=%ld iter=%d (kernel returned 0)\n",
				tid, i);
			return (void *)4;
		}
		if (ret < 0x40000000UL || ret >= 0x800000000000UL) {
			dump_trace();
			fprintf(stderr,
				"RAW_WEIRD tid=%ld iter=%d rax=%#lx\n",
				tid, i, ret);
			return (void *)5;
		}
		void *p = (void *)ret;
		volatile unsigned char *vp = p;
		for (size_t j = 0; j < ALLOC_SZ; j++)
			vp[j] = (unsigned char)tid;
		for (size_t j = 0; j < ALLOC_SZ; j += 4096) {
			unsigned char got = vp[j];
			if (got != (unsigned char)tid) {
				/* Re-read to confirm. */
				unsigned char re_got = vp[j];
				dump_trace();
				fprintf(stderr,
					"RAW_VERIFY tid=%ld iter=%d off=%#zx "
					"got=%u re_got=%u expect=%u rax=%#lx\n",
					tid, i, j, got, re_got,
					(unsigned char)tid, ret);
				raw_munmap(ret, ALLOC_SZ);
				return (void *)2;
			}
		}
		raw_munmap(ret, ALLOC_SZ);
	}
	(void)last_iter;
	return NULL;
}

int main(int argc, char **argv)
{
	int n = (argc > 1) ? atoi(argv[1]) : 8;
	if (n > 16)
		n = 16;
	pthread_t t[16];
	long rc = 0;

	for (long i = 0; i < n; i++)
		pthread_create(&t[i], NULL, worker, (void *)i);
	for (long i = 0; i < n; i++) {
		void *r;
		pthread_join(t[i], &r);
		rc |= (long)r;
	}
	if (rc == 0)
		printf("RAW_OK n=%d\n", n);
	else
		printf("RAW_FAIL rc=%ld\n", rc);
	return rc != 0;
}
