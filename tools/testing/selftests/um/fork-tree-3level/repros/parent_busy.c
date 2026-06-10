// Lots of syscalls in PARENT before fork. If bug rate INCREASES,
// parent state accumulates. If unchanged, child path is the issue.
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
	/* 100 getpid calls before fork - accumulate parent state. */
	for (int i = 0; i < 100; i++) (void)getpid();
	pid_t p = fork();
	if (p == 0) {
		printf("CHILD_OK\n");
		fflush(stdout);
		_exit(0);
	}
	int st;
	waitpid(p, &st, 0);
	return 0;
}
