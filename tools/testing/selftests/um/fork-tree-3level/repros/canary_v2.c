// Parent uses libc to log; child writes canary value via raw write
// to a file the parent already opened.
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <string.h>

static unsigned long read_canary(void) {
	unsigned long v;
	__asm__ volatile ("mov %%fs:0x28, %0" : "=r"(v));
	return v;
}

int main(void) {
	unsigned long parent_canary = read_canary();
	int log = open("/tmp/cv.log", O_TRUNC|O_CREAT|O_WRONLY, 0644);
	dprintf(log, "PARENT 0x%lx\n", parent_canary);

	pid_t p = fork();
	if (p == 0) {
		unsigned long child_canary = read_canary();
		/* write child canary via raw asm — no libc */
		char buf[64];
		int n = snprintf(buf, sizeof buf, "CHILD 0x%lx\n", child_canary);
		long r;
		register long r10 asm("r10") = 0;
		asm volatile ("syscall" : "=a"(r) :
			"0"((long)__NR_write),
			"D"((long)log), "S"((long)buf), "d"((long)n), "r"(r10) :
			"rcx", "r11", "memory");
		__asm__ volatile ("mov $231, %%rax\n xor %%rdi, %%rdi\n syscall\n"
				  ::: "rax", "rcx", "r11");
		__builtin_unreachable();
	}
	int st;
	waitpid(p, &st, 0);
	unsigned long parent_post = read_canary();
	dprintf(log, "PARENT_POST 0x%lx wait_st=0x%x\n", parent_post, st);
	close(log);
	return 0;
}
