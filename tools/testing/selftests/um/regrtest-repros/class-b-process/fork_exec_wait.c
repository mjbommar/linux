/* Class B process repro: fork+execve+waitpid loop. UML stub-child path
 * must reap N children with correct exit status. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>

#define N_CHILDREN 50

int main(void)
{
	pid_t pids[N_CHILDREN];
	char *const argv[] = { (char *)"/bin/true", NULL };
	char *const envp[] = { NULL };

	for (int i = 0; i < N_CHILDREN; i++) {
		pid_t p = fork();
		if (p < 0) {
			printf("REPRO: fork_exec_wait FAIL fork_errno=%d i=%d\n", errno, i);
			return 0;
		}
		if (p == 0) {
			execve("/bin/true", argv, envp);
			_exit(127);
		}
		pids[i] = p;
	}

	int reaped = 0, bad_status = 0, bad_wait = 0, last_errno = 0;
	for (int i = 0; i < N_CHILDREN; i++) {
		int status = 0;
		pid_t r = waitpid(pids[i], &status, 0);
		if (r < 0) {
			last_errno = errno;
			bad_wait++;
			continue;
		}
		if (r != pids[i]) {
			bad_wait++;
			continue;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			bad_status++;
		reaped++;
	}

	if (reaped == N_CHILDREN && bad_status == 0 && bad_wait == 0) {
		printf("REPRO: fork_exec_wait PASS\n");
	} else {
		printf("REPRO: fork_exec_wait FAIL reaped=%d bad_status=%d bad_wait=%d errno=%d\n",
			reaped, bad_status, bad_wait, last_errno);
	}
	return 0;
}
