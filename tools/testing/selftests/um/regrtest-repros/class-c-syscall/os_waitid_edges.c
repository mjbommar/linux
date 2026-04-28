/* Class C syscall repro: waitid edges. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(void)
{
	siginfo_t si;

	memset(&si, 0, sizeof(si));
	int rc = waitid(P_ALL, 0, &si, WEXITED | WNOHANG);
	int e = errno;
	if (!(rc == -1 && e == ECHILD)) {
		printf("REPRO: os_waitid_edges FAIL no_child rc=%d errno=%d\n", rc, e);
		return 0;
	}

	pid_t child = fork();
	if (child < 0) {
		printf("REPRO: os_waitid_edges FAIL fork_errno=%d\n", errno);
		return 0;
	}
	if (child == 0) {
		pause();
		_exit(0);
	}

	usleep(50000);

	memset(&si, 0, sizeof(si));
	rc = waitid(P_ALL, 0, &si, WEXITED | WNOHANG);
	if (rc != 0 || si.si_pid != 0) {
		printf("REPRO: os_waitid_edges FAIL running_child rc=%d errno=%d si_pid=%d\n",
			rc, errno, si.si_pid);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 0;
	}

	if (kill(child, SIGTERM) < 0) {
		printf("REPRO: os_waitid_edges FAIL kill_errno=%d\n", errno);
		return 0;
	}

	memset(&si, 0, sizeof(si));
	rc = waitid(P_PID, child, &si, WEXITED);
	if (rc != 0) {
		printf("REPRO: os_waitid_edges FAIL waitid_pid rc=%d errno=%d\n", rc, errno);
		return 0;
	}
	if (si.si_pid != child) {
		printf("REPRO: os_waitid_edges FAIL si_pid=%d expected=%d\n",
			si.si_pid, child);
		return 0;
	}

	printf("REPRO: os_waitid_edges PASS\n");
	return 0;
}
