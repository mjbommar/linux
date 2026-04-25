/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Fallback-path syscall round-trip bookend (task #241).
 *
 * Sibling to perf-getpid that exercises the non-gadget path
 * deliberately. perf-getpid measures the Class E gadget fast
 * path (~100 cyc / ~28 ns under the gadget kernel). This
 * binary loops a Class A passthrough syscall (`__NR_getsid`)
 * which always VMEXITs — even under the gadget kernel — so
 * the measurement reflects the real cost of the
 * `kvm_enter_guest + KVM_RUN + handle_syscall` round-trip.
 *
 * Used as the regression-and-progress gate for the
 * fallback-lever series (sync_regs / MSR prime / SREGS skip
 * / handle_mm_fault refactor / shadow_fill skip / huge-page
 * shadow PT / per-mm cached shadow PGD). Each lever's commit
 * appends a row to Documentation/virt/uml/redesign/02-
 * workstreams/D-kvm-backend/measurements.md showing the
 * before/after numbers from this binary.
 *
 * No libc dependencies: raw `syscall` instruction, rdtsc
 * directly, write() to emit the report. Same shape as
 * perf-getpid's getpid-loop.c.
 *
 * Output format (one line, newline-terminated):
 *   PERF_FALLBACK: n=<N> cycles=<T> ns=<NS> ns_per_call=<NPC>
 *                  cyc_per_call=<CPC> sink=<SINK>
 *
 * The runner shell-parses this line. backend= is filled in
 * by the runner post-boot based on which kernel it booted;
 * the binary itself doesn't know.
 */

#include <stdint.h>

#define __NR_write		1
#define __NR_exit_group		231
#define __NR_clock_gettime	228
#define __NR_getsid		124	/* x86_64 */
#define CLOCK_MONOTONIC_RAW	4

#define N_ITERS		100000
#define WARMUP_ITERS	1000
#define STDOUT_FD	1

struct ktimespec {
	long tv_sec;
	long tv_nsec;
};

static inline long sys3(long nr, long a, long b, long c)
{
	long r;

	register long r10 __asm__("r10") = 0;
	register long r8  __asm__("r8")  = 0;
	register long r9  __asm__("r9")  = 0;
	__asm__ volatile (
		"syscall"
		: "=a"(r)
		: "0"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory"
	);
	return r;
}

static inline long sys_getsid(void)
{
	long r;

	__asm__ volatile (
		"syscall"
		: "=a"(r)
		: "0"((long)__NR_getsid), "D"(0L)
		: "rcx", "r11", "memory"
	);
	return r;
}

static inline uint64_t rdtsc(void)
{
	uint32_t lo, hi;

	__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

static inline long sys_clock_gettime(int clockid, struct ktimespec *ts)
{
	return sys3(__NR_clock_gettime, clockid, (long)ts, 0);
}

static inline long sys_write(int fd, const void *buf, unsigned long len)
{
	return sys3(__NR_write, fd, (long)buf, (long)len);
}

static inline void sys_exit(int code)
{
	sys3(__NR_exit_group, code, 0, 0);
	__builtin_unreachable();
}

static unsigned int append_u64(char *buf, unsigned int off, uint64_t v)
{
	char tmp[32];
	unsigned int n = 0;

	if (v == 0) {
		buf[off++] = '0';
		return off;
	}
	while (v) {
		tmp[n++] = '0' + (v % 10);
		v /= 10;
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
	struct ktimespec t0, t1;
	uint64_t cyc0, cyc1, cycles, ns;
	volatile long sink = 0;
	char out[256];
	unsigned int n;
	int i;

	/*
	 * Warm up caches + branch predictors so the real
	 * measurement amortizes startup overhead across the
	 * inner loop only. Same pattern as perf-getpid.
	 */
	for (i = 0; i < WARMUP_ITERS; i++)
		sink += sys_getsid();

	sys_clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	cyc0 = rdtsc();
	for (i = 0; i < N_ITERS; i++)
		sink += sys_getsid();
	cyc1 = rdtsc();
	sys_clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

	cycles = cyc1 - cyc0;
	ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
	     (uint64_t)(t1.tv_nsec - t0.tv_nsec);

	n = 0;
	n = append_str(out, n, "PERF_FALLBACK: n=");
	n = append_u64(out, n, N_ITERS);
	n = append_str(out, n, " cycles=");
	n = append_u64(out, n, cycles);
	n = append_str(out, n, " ns=");
	n = append_u64(out, n, ns);
	n = append_str(out, n, " ns_per_call=");
	n = append_u64(out, n, ns / N_ITERS);
	n = append_str(out, n, " cyc_per_call=");
	n = append_u64(out, n, cycles / N_ITERS);
	n = append_str(out, n, " sink=");
	n = append_u64(out, n, (uint64_t)sink);
	out[n++] = '\n';

	sys_write(STDOUT_FD, out, n);
	sys_exit(0);
	return 0;
}

/*
 * Freestanding entry point: no glibc, so we have to stand up
 * our own _start. Same shape as perf-getpid + kvm-bounds.
 */
__asm__ (
	".text\n"
	".globl _start\n"
	"_start:\n"
	"\txor %rbp, %rbp\n"
	"\tcall main\n"
	"\tmov %rax, %rdi\n"
	"\tmov $231, %rax\n"
	"\tsyscall\n"
	"\thlt\n"
);
