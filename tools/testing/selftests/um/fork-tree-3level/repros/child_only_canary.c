// Parent does NOTHING after fork. Child does stack work.
// If canary fires only in child: the bug is in child's post-fork
// state (worker mm, CR3, TLB, etc.).
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
