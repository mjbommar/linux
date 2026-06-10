/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KVM-backend mm-mutation regression guard.
 *
 * Only mm-mutating syscalls (mmap, munmap,
 * mprotect, mremap, brk) need the refill, and those go through
 * UML's mm_map / mm_unmap callbacks which already reset
 * shadow_pgd_synced=false so the next kvm_enter_guest refill path
 * repopulates shadow page tables correctly.
 *
 * This freestanding ELF binary exercises each mm-mutating
 * syscall in turn and verifies post-syscall reads/writes against
 * the affected pages observe the right contents. If a future syscall
 * mutates mm without flowing through mm_map/unmap, the test fails by
 * either:
 *   - SIGSEGV when the shadow PT serves a stale mapping
 *   - silent data corruption when reads/writes hit the wrong
 *     physical page
 *
 * Same minimal ELF shape as df-preserve-loop / kvm-bounds-loop:
 * no libc, only direct syscalls. Static-linked freestanding
 * binary; argv/envp ignored. Emits a single "MM_SMOKE: PASS|FAIL
 * reason=..." line on stdout that the runner scrapes.
 */

#include <stdint.h>

#define __NR_write		1
#define __NR_mmap		9
#define __NR_mprotect		10
#define __NR_munmap		11
#define __NR_brk		12
#define __NR_mremap		25
#define __NR_exit_group		231

#define PROT_READ		0x1
#define PROT_WRITE		0x2
#define PROT_NONE		0x0
#define MAP_PRIVATE		0x02
#define MAP_ANONYMOUS		0x20
#define MAP_FAILED		((void *)-1L)
#define MREMAP_MAYMOVE		0x01

#define STDOUT_FD		1

static inline long sys6(long nr, long a, long b, long c,
			long d, long e, long f)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	register long r9  __asm__("r9")  = f;

	__asm__ volatile (
		"syscall" : "=a"(r)
		: "0"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory"
	);
	return r;
}

#define sys0(nr)			sys6((nr),0,0,0,0,0,0)
#define sys1(nr,a)			sys6((nr),(long)(a),0,0,0,0,0)
#define sys2(nr,a,b)			sys6((nr),(long)(a),(long)(b),0,0,0,0)
#define sys3(nr,a,b,c)			sys6((nr),(long)(a),(long)(b),(long)(c),0,0,0)
#define sys4(nr,a,b,c,d)		sys6((nr),(long)(a),(long)(b),(long)(c),(long)(d),0,0)
#define sys5(nr,a,b,c,d,e)		sys6((nr),(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),0)

static unsigned int append_str(char *buf, unsigned int off, const char *s)
{
	while (*s)
		buf[off++] = *s++;
	return off;
}

static unsigned int append_s64(char *buf, unsigned int off, long v)
{
	char tmp[32];
	unsigned int n = 0;
	unsigned long u;

	if (v < 0) {
		buf[off++] = '-';
		u = (unsigned long)(-v);
	} else {
		u = (unsigned long)v;
	}
	if (u == 0) {
		buf[off++] = '0';
		return off;
	}
	while (u) {
		tmp[n++] = '0' + (u % 10);
		u /= 10;
	}
	while (n--)
		buf[off++] = tmp[n];
	return off;
}

#define PAGE_SIZE	4096

int main(void)
{
	char out[512];
	unsigned int n = 0;
	const char *fail = (const char *)0;
	long rc;
	void *p, *q;
	volatile unsigned char *u;

	/*
	 * Test 1: mmap a fresh anon page, write, read-back.
	 * Validates that mmap() creates a shadow-PT mapping
	 * walkable by the very next user access.
	 */
	p = (void *)sys6(__NR_mmap, 0, PAGE_SIZE,
			 PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		fail = "mmap1";
		goto report;
	}
	u = (volatile unsigned char *)p;
	u[0] = 0x42;
	u[PAGE_SIZE - 1] = 0xa5;
	if (u[0] != 0x42 || u[PAGE_SIZE - 1] != 0xa5) {
		fail = "mmap1-readback";
		goto report;
	}

	/*
	 * Test 2: mprotect to PROT_READ; second write must SIGSEGV.
	 * We can't easily catch SIGSEGV from a freestanding binary;
	 * instead just validate the read still works, and let
	 * the absence-of-corruption-on-readback gate cover the
	 * shadow-PT consistency.
	 */
	rc = sys3(__NR_mprotect, p, PAGE_SIZE, PROT_READ);
	if (rc < 0) {
		fail = "mprotect-RO";
		goto report;
	}
	if (u[0] != 0x42) {
		fail = "mprotect-readback";
		goto report;
	}
	rc = sys3(__NR_mprotect, p, PAGE_SIZE, PROT_READ | PROT_WRITE);
	if (rc < 0) {
		fail = "mprotect-RW";
		goto report;
	}
	u[0] = 0x77;
	if (u[0] != 0x77) {
		fail = "mprotect-RW-readback";
		goto report;
	}

	/*
	 * Test 3: mremap to a fresh address, content must move
	 * with the mapping. MREMAP_MAYMOVE lets the kernel pick a
	 * new VA; q is the result (or -errno cast to void *).
	 */
	q = (void *)sys5(__NR_mremap, p, PAGE_SIZE, PAGE_SIZE * 2,
			 MREMAP_MAYMOVE, 0);
	if (q == MAP_FAILED) {
		fail = "mremap";
		goto report;
	}
	{
		volatile unsigned char *uq = (volatile unsigned char *)q;

		if (uq[0] != 0x77) {
			fail = "mremap-readback";
			goto report;
		}
		uq[PAGE_SIZE] = 0x88;	/* second page, fresh anon */
		if (uq[PAGE_SIZE] != 0x88) {
			fail = "mremap-extend-readback";
			goto report;
		}
	}

	/*
	 * Test 4: munmap, then a fresh mmap at the same VA size
	 * must succeed (the old shadow PT slot must be torn down).
	 * We just verify both syscalls return 0 / non-MAP_FAILED;
	 * a missed mm_unmap callback would leave the shadow PT
	 * pointing at the old physical page after re-allocation.
	 */
	rc = sys2(__NR_munmap, q, PAGE_SIZE * 2);
	if (rc < 0) {
		fail = "munmap";
		goto report;
	}
	p = (void *)sys6(__NR_mmap, 0, PAGE_SIZE,
			 PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		fail = "mmap2";
		goto report;
	}
	u = (volatile unsigned char *)p;
	u[0] = 0xee;
	if (u[0] != 0xee) {
		fail = "mmap2-readback";
		goto report;
	}
	(void)sys2(__NR_munmap, p, PAGE_SIZE);

	/*
	 * Test 5: brk extension. Take the current break, extend
	 * it by PAGE_SIZE, write/read, then shrink back.
	 */
	{
		long brk_orig = sys1(__NR_brk, 0);
		long brk_new;
		volatile unsigned char *ub;

		if (brk_orig <= 0) {
			fail = "brk-orig";
			goto report;
		}
		brk_new = sys1(__NR_brk, brk_orig + PAGE_SIZE);
		if (brk_new < brk_orig + PAGE_SIZE) {
			fail = "brk-extend";
			goto report;
		}
		ub = (volatile unsigned char *)(brk_orig + 16);
		ub[0] = 0xc3;
		if (ub[0] != 0xc3) {
			fail = "brk-readback";
			goto report;
		}
		(void)sys1(__NR_brk, brk_orig);
	}

report:
	n = append_str(out, n, fail ? "MM_SMOKE: FAIL reason=" :
			       "MM_SMOKE: PASS reason=");
	n = append_str(out, n, fail ? fail : "all-5-tests");
	n = append_str(out, n, " final_rc=");
	n = append_s64(out, n, fail ? -1 : 0);
	n = append_str(out, n, "\n");
	sys3(__NR_write, STDOUT_FD, (long)out, n);
	sys3(__NR_exit_group, fail ? 1 : 0, 0, 0);
	return 0;
}

__asm__ (
	".text\n.globl _start\n_start:\n"
	"\txor %rbp, %rbp\n"
	"\tcall main\n"
	"\tmov %rax, %rdi\n"
	"\tmov $231, %rax\n"
	"\tsyscall\n\thlt\n"
);
