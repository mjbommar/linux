// SPDX-License-Identifier: GPL-2.0
/*
 * D-06 getpid() round-trip bookend (memo 08 sub-commit #6).
 *
 * Measures the cost of one SYS_getpid round-trip from guest
 * userspace under whatever UML backend is selected at host
 * boot. Run as init= from the host selftest, which compares
 * the number on backend=kvm against the backend=seccomp
 * baseline.
 *
 * No libc dependencies: raw `syscall` instruction, rdtsc
 * directly, write() syscall to emit the report. A libc-
 * linked version would pull in glibc's TLS + cancellation
 * checks, which are irrelevant overhead for the bookend.
 * Compile static with `-static -nostdlib -ffreestanding`;
 * _start calls main and issues exit_group.
 *
 * Output format (one line, newline-terminated):
 *   PERF_GETPID: backend=<??> n=<N> cycles=<T> ns=<NS> ns_per_call=<NPC>
 *
 * The runner shell-parses this line. `backend=??` is filled
 * in by the runner post-boot based on its own knowledge of
 * which kernel it booted; the binary itself doesn't know.
 *
 * Clock source: `rdtsc`. On UML-under-KVM this reads the
 * host TSC via KVM's passthrough (modern x86 host TSCs are
 * invariant, so a single-core pin makes the measurement
 * stable). No TSC rate calibration — the runner is fed
 * `tsc_hz` from /proc/cpuinfo if it wants to convert
 * cycles→ns; otherwise the cycle count alone is the
 * cross-backend comparison surface.
 */

#include <linux/unistd.h>
#include <stdint.h>

#ifndef __NR_getpid
#define __NR_getpid	39
#endif
#ifndef __NR_write
#define __NR_write	1
#endif
#ifndef __NR_exit_group
#define __NR_exit_group	231
#endif
#ifndef __NR_clock_gettime
#define __NR_clock_gettime	228
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW	4
#endif

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

static inline long sys_getpid(void)
{
	long r;
	__asm__ volatile (
		"syscall"
		: "=a"(r)
		: "0"((long)__NR_getpid)
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

/* Append unsigned decimal to buf; returns new length. */
static unsigned append_u64(char *buf, unsigned off, uint64_t v)
{
	char tmp[32];
	unsigned n = 0;

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

static unsigned append_str(char *buf, unsigned off, const char *s)
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
	unsigned n;
	int i;

	/* Warm up caches + branch predictors so the real
	 * measurement amortizes startup overhead across the
	 * inner loop only.
	 */
	for (i = 0; i < WARMUP_ITERS; i++)
		sink += sys_getpid();

	sys_clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	cyc0 = rdtsc();
	for (i = 0; i < N_ITERS; i++)
		sink += sys_getpid();
	cyc1 = rdtsc();
	sys_clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

	cycles = cyc1 - cyc0;
	ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
	     (uint64_t)(t1.tv_nsec - t0.tv_nsec);

	n = 0;
	n = append_str(out, n, "PERF_GETPID: n=");
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
 * our own _start. Just calls main() and exit()s with its
 * return code. The stack pointer is already valid on entry
 * from the kernel; no need to set up argc/argv either.
 */
__asm__ (
	".text\n"
	".globl _start\n"
	"_start:\n"
	"\txor %rbp, %rbp\n"
	"\tcall main\n"
	"\tmov %rax, %rdi\n"
	"\tmov $231, %rax\n"		/* __NR_exit_group */
	"\tsyscall\n"
	"\thlt\n"
);
