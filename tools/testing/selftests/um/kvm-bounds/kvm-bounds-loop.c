/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KVM gadget user-pointer bounds-check selftest.
 *
 * The clock_gettime / time / getcpu gadget handlers store
 * through user-supplied output pointers at CPL=0. Without a
 * bounds check, the gadget could (a) write to a canonical
 * kernel VA mapped in the shadow PT (corrupt ring-0 data) or
 * (b) trigger #GP on a non-canonical address (no IDT[13]
 * handler; that would triple-fault).  The gadget path has an inline cap check
 * (cmp ptr, %gs:TASK_SIZE_CAP; jbe fallback) before each store
 * so bad pointers route to handle_syscall and return -EFAULT
 * per POSIX.
 *
 * This selftest passes 0xffff800000001000 (canonical kernel
 * VA) and 0x800000000000 (non-canonical) to each gadget and
 * asserts each call returns -EFAULT (errno 14). On a regression
 * of G1 the gadget would either silently corrupt ring-0 data
 * (bad map case) or triple-fault the guest (non-canonical case),
 * either of which surfaces here as a non-EFAULT return or a
 * boot crash.
 *
 * Freestanding 64-bit ELF, same shape as getpid-loop /
 * df-preserve-loop.  Routed through both kvmint (no gadget) and
 * kvmbench (gadget) by the runner so we get a baseline + a
 * gadget-side check.
 */

#include <stdint.h>

#define __NR_write		1
#define __NR_exit_group		231
#define __NR_clock_gettime	228
#define __NR_time		201
#define __NR_getcpu		309
#define CLOCK_MONOTONIC		1
#define STDOUT_FD		1
#define EXPECT_EFAULT		(-14L)

static inline long sys3(long nr, long a, long b, long c)
{
	long r;

	register long r10 __asm__("r10") = 0;
	register long r8  __asm__("r8")  = 0;
	register long r9  __asm__("r9")  = 0;

	__asm__ volatile (
		"syscall" : "=a"(r)
		: "0"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory"
	);
	return r;
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

static unsigned int append_str(char *buf, unsigned int off, const char *s)
{
	while (*s)
		buf[off++] = *s++;
	return off;
}

int main(void)
{
	long rc_clock_kvm, rc_time_kvm, rc_getcpu_kvm;
	long rc_clock_noncan, rc_time_noncan, rc_getcpu_noncan;
	long rc_clock_edge, rc_time_edge, rc_getcpu_edge;
	int passed = 0, total = 9;
	char out[512];
	unsigned int n = 0;

	/*
	 * Canonical kernel VA - bit 47 set, sign-extended high.
	 * Well above task_size_cap (~128 TB on 64-bit UML).
	 */
	void *kva = (void *)0xffff800000001000UL;

	/*
	 * Non-canonical address - bit 47 = 0, bit 48 = 1. CPU
	 * raises #GP if accessed at CPL=0 without G1's bounds
	 * check; with the check, gadget falls back before the
	 * faulting access.
	 */
	void *noncan = (void *)0x800000000000UL;

	/*
	 * Edge case (audit P3 #9): pointer in
	 * [task_size - 16, task_size - 1]. With G1's range-aware
	 * cap = task_size - 16, the gadget's `cmp ptr, %gs:cap;
	 * jbe fallback` fires here and routes to the SYSCALL
	 * fallback. Fallback's access_ok then checks
	 * `ptr + size <= task_size` and rejects (the 16-byte
	 * struct __kernel_timespec for clock_gettime, the 8-byte
	 * time_t for time, the 4-byte u32 for getcpu would all
	 * write past task_size). Expect -EFAULT for all three.
	 *
	 * UML task_size on x86_64 is 0x7f8000000000 by default
	 * (see arch/um/include/asm/processor-generic.h /
	 * arch/x86/um/asm/processor_64.h). Pick task_size - 8
	 * (one valid u64 *might* fit for size=8 on a hypothetical
	 * narrower cap, but with cap = task_size - 16 it's
	 * guaranteed rejected).
	 */
	void *edge = (void *)0x7f7ffffffff8UL;

	/* Test 1-3: kernel-VA pointer for each output gadget. */
	rc_clock_kvm  = sys3(__NR_clock_gettime, CLOCK_MONOTONIC,
			     (long)kva, 0);
	rc_time_kvm   = sys3(__NR_time, (long)kva, 0, 0);
	rc_getcpu_kvm = sys3(__NR_getcpu, (long)kva, 0, 0);

	/* Test 4-6: non-canonical pointer for each output gadget. */
	rc_clock_noncan  = sys3(__NR_clock_gettime, CLOCK_MONOTONIC,
				(long)noncan, 0);
	rc_time_noncan   = sys3(__NR_time, (long)noncan, 0, 0);
	rc_getcpu_noncan = sys3(__NR_getcpu, (long)noncan, 0, 0);

	/* Test 7-9: edge-of-task_size pointer (G1 range cap). */
	rc_clock_edge  = sys3(__NR_clock_gettime, CLOCK_MONOTONIC,
			      (long)edge, 0);
	rc_time_edge   = sys3(__NR_time, (long)edge, 0, 0);
	rc_getcpu_edge = sys3(__NR_getcpu, (long)edge, 0, 0);

	if (rc_clock_kvm == EXPECT_EFAULT)
		passed++;
	if (rc_time_kvm == EXPECT_EFAULT)
		passed++;
	if (rc_getcpu_kvm == EXPECT_EFAULT)
		passed++;
	if (rc_clock_noncan == EXPECT_EFAULT)
		passed++;
	if (rc_time_noncan == EXPECT_EFAULT)
		passed++;
	if (rc_getcpu_noncan == EXPECT_EFAULT)
		passed++;
	if (rc_clock_edge == EXPECT_EFAULT)
		passed++;
	if (rc_time_edge == EXPECT_EFAULT)
		passed++;
	if (rc_getcpu_edge == EXPECT_EFAULT)
		passed++;

	n = append_str(out, n, "KVM_BOUNDS: clock_kva=");
	n = append_s64(out, n, rc_clock_kvm);
	n = append_str(out, n, " time_kva=");
	n = append_s64(out, n, rc_time_kvm);
	n = append_str(out, n, " getcpu_kva=");
	n = append_s64(out, n, rc_getcpu_kvm);
	n = append_str(out, n, " clock_noncan=");
	n = append_s64(out, n, rc_clock_noncan);
	n = append_str(out, n, " time_noncan=");
	n = append_s64(out, n, rc_time_noncan);
	n = append_str(out, n, " getcpu_noncan=");
	n = append_s64(out, n, rc_getcpu_noncan);
	n = append_str(out, n, " clock_edge=");
	n = append_s64(out, n, rc_clock_edge);
	n = append_str(out, n, " time_edge=");
	n = append_s64(out, n, rc_time_edge);
	n = append_str(out, n, " getcpu_edge=");
	n = append_s64(out, n, rc_getcpu_edge);
	n = append_str(out, n, " passed=");
	n = append_s64(out, n, passed);
	n = append_str(out, n, "/");
	n = append_s64(out, n, total);
	n = append_str(out, n, " expected_each=-14\n");

	sys3(__NR_write, STDOUT_FD, (long)out, n);
	sys3(__NR_exit_group, passed == total ? 0 : 1, 0, 0);
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
