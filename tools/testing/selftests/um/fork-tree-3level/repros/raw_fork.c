// Raw-syscall fork/wait/exit diagnostic. Avoids libc paths so a
// failure can be attributed to kernel fork-state handling rather than
// libc startup or fortify checks. Static, no canary, manual asm.
#define _GNU_SOURCE
#include <sys/syscall.h>

static long raw_syscall0(long n) {
	long r;
	asm volatile ("syscall" : "=a"(r) : "0"(n) : "rcx", "r11", "memory");
	return r;
}
static long raw_syscall1(long n, long a) {
	long r;
	asm volatile ("syscall" : "=a"(r) : "0"(n), "D"(a) : "rcx", "r11", "memory");
	return r;
}
static long raw_syscall4(long n, long a, long b, long c, long d) {
	long r;
	register long r10 asm("r10") = c;
	asm volatile ("syscall" : "=a"(r)
		: "0"(n), "D"(a), "S"(b), "r"(r10), "d"(d)
		: "rcx", "r11", "memory");
	return r;
}

static void raw_write(const char *s, int len) {
	raw_syscall4(__NR_write, 1, (long)s, len, 0);
}

void _start(void)
{
	raw_write("RAW: pre-fork\n", 14);
	long p = raw_syscall0(__NR_fork);
	if (p < 0) {
		raw_write("RAW: fork-fail\n", 15);
		raw_syscall1(__NR_exit_group, 2);
	}
	if (p == 0) {
		raw_write("RAW: child\n", 11);
		raw_syscall1(__NR_exit_group, 42);
	}
	raw_write("RAW: parent-after-fork\n", 23);
	int status = 0;
	long r = raw_syscall4(__NR_wait4, p, (long)&status, 0, 0);
	(void)r;
	raw_write("RAW: parent-after-wait\n", 23);
	raw_syscall1(__NR_exit_group, 99);
}
