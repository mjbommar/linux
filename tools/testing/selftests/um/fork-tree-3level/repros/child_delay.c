// SPDX-License-Identifier: GPL-2.0
/* Child sleeps before writing so the harness can distinguish race timing. */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
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
