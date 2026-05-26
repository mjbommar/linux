// Block ALL signals before fork. If bug persists: signal delivery
// is NOT the cause. If bug disappears: bug is in signal/sigframe path.
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

int main(void) {
	sigset_t all;
	sigfillset(&all);
	sigprocmask(SIG_BLOCK, &all, NULL);

	pid_t p = fork();
	if (p == 0) {
		/* Child: signals blocked, can't be killed by SIGCHLD-style */
		printf("CHILD_OK\n");
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(p, &st, 0);
	printf("PARENT_OK st=0x%x\n", st);
	return 0;
}
