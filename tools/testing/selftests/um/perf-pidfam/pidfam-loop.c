// SPDX-License-Identifier: GPL-2.0
/*
 * Mixed pid-family gadget perf loop.
 *
 * The perf-getpid microbenchmark only exercises the
 * __NR_getpid handler in the LSTAR gadget. The G4-G6 ladder
 * actually wired seven pid-family handlers + clock_gettime +
 * time + getcpu.  This benchmark covers the non-getpid gadget family.
 *
 * This binary loops through the seven pid-family NRs in
 * round-robin so the measured per-call cost reflects the full
 * gadget dispatch table, not just the head entry. Same
 * freestanding-ELF shape as getpid-loop.c.
 *
 * Output line:
 *   PERF_PIDFAM: backend=<??> n=<N> cycles=<T> ns=<NS>
 *                ns_per_call=<NPC> cyc_per_call=<CPC>
 *                family=getpid,gettid,getppid,getuid,geteuid,getgid,getegid
 *
 * The runner shell-parses this line just like perf-getpid does.
 */

#include <linux/unistd.h>
#include <stdint.h>

#ifndef __NR_getpid
#define __NR_getpid		39
#endif
#ifndef __NR_gettid
#define __NR_gettid		186
#endif
#ifndef __NR_getppid
#define __NR_getppid		110
#endif
#ifndef __NR_getuid
#define __NR_getuid		102
#endif
#ifndef __NR_geteuid
#define __NR_geteuid		107
#endif
#ifndef __NR_getgid
#define __NR_getgid		104
#endif
#ifndef __NR_getegid
#define __NR_getegid		108
#endif
#ifndef __NR_write
#define __NR_write		1
#endif
#ifndef __NR_exit_group
#define __NR_exit_group		231
#endif
#ifndef __NR_clock_gettime
#define __NR_clock_gettime	228
#endif

#define CLOCK_MONOTONIC_RAW	4

#define N_ITERS			(100000 * 7)	/* 100k full family rounds */
#define WARMUP_ITERS		(1000 * 7)
#define STDOUT_FD		1

struct ktimespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

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

static inline long sys_nr(long nr)
{
	long r;
	__asm__ volatile (
		"syscall"
		: "=a"(r)
		: "0"(nr)
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

static const long pidfam_nrs[7] = {
	__NR_getpid, __NR_gettid, __NR_getppid,
	__NR_getuid, __NR_geteuid, __NR_getgid, __NR_getegid,
};

int main(void)
{
	struct ktimespec t0, t1;
	uint64_t cyc0, cyc1, cycles, ns;
	volatile long sink = 0;
	char out[320];
	unsigned n;
	int i;

	/* Warm up caches + branch predictors. */
	for (i = 0; i < WARMUP_ITERS; i++)
		sink += sys_nr(pidfam_nrs[i % 7]);

	(void)sys3(__NR_clock_gettime, CLOCK_MONOTONIC_RAW, (long)&t0, 0);
	cyc0 = rdtsc();
	for (i = 0; i < N_ITERS; i++)
		sink += sys_nr(pidfam_nrs[i % 7]);
	cyc1 = rdtsc();
	(void)sys3(__NR_clock_gettime, CLOCK_MONOTONIC_RAW, (long)&t1, 0);

	cycles = cyc1 - cyc0;
	ns = (t1.tv_sec - t0.tv_sec) * 1000000000ULL +
	     (t1.tv_nsec - t0.tv_nsec);

	n = 0;
	n = append_str(out, n, "PERF_PIDFAM: backend=?? n=");
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
	n = append_str(out, n, " family=getpid,gettid,getppid,getuid,geteuid,getgid,getegid\n");

	sys3(__NR_write, STDOUT_FD, (long)out, n);
	sys3(__NR_exit_group, 0, 0, 0);
	__builtin_unreachable();
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
