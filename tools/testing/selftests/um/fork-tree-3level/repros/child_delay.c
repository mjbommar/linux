// Child sleeps before doing anything — tests if the bug is a race.
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
	pid_t p = fork();
	if (p == 0) {
		usleep(100000); /* 100ms */
		printf("CHILD_OK\n");
		fflush(stdout);
		_exit(0);
	}
	int st;
	waitpid(p, &st, 0);
	return 0;
}
