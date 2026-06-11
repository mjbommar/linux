// libc binary that uses raw syscall(SYS_clone) directly to bypass
// glibc's CLONE_CHILD_SETTID/CLEARTID handling. Static + libc
// functions for printf etc, but custom fork.
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sched.h>
#include <signal.h>

int main(void)
{
	printf("LIBC_NOCLONE: pre-fork pid=%d\n", getpid());
	fflush(stdout);
	/* clone with just SIGCHLD - no CLONE_CHILD_SETTID/CLEARTID */
	long p = syscall(SYS_clone, SIGCHLD, 0L, NULL, NULL, 0L);
	if (p < 0) { perror("clone"); return 2; }
	if (p == 0) {
		printf("LIBC_NOCLONE: child pid=%d\n", getpid());
		fflush(stdout);
		_exit(42);
	}
	int st = 0;
	pid_t r = waitpid(p, &st, 0);
	printf("LIBC_NOCLONE: parent r=%d st=0x%x WIFE=%d WEX=%d\n",
	       (int)r, st, WIFEXITED(st), WEXITSTATUS(st));
	fflush(stdout);
	return 0;
}
