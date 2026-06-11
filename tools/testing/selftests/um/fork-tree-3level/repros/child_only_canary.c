// Parent exits immediately after fork while the child does stack work.
// A child-only canary failure points at post-fork child state such as
// worker mm, CR3, or TLB state.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* Simple function with stack canary (libc emits one for any
 * function with a local buffer. Our -fno-stack-protector affects
 * THIS binary but glibc's internal functions still have their own
 * canaries from glibc's own build). */
int worker(int n) {
	char buf[64];
	memset(buf, 0xaa, sizeof buf);
	return strlen(buf);
}

int main(void) {
	pid_t p = fork();
	if (p == 0) {
		/* Child: call libc's stack-canary-checked memset/strlen. */
		int x = worker(42);
		printf("CHILD_OK x=%d\n", x);
		fflush(stdout);
		_exit(0);
	}
	/* Parent: exit immediately without doing any work. */
	_exit(0);
}
