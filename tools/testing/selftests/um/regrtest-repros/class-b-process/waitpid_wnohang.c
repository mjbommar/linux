/* Class B process repro: waitpid(WNOHANG) state machine. Spawn a
 * child that exits after a short sleep; parent loops waitpid(WNOHANG)
 * and verifies (0)*+, then PID-with-status, then ECHILD. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define POLL_DELAY_US 5000
#define MAX_ITERS 2000

int main(void)
{
	pid_t pid = fork();
	if (pid < 0) {
		printf("REPRO: waitpid_wnohang FAIL fork_errno=%d\n", errno);
		return 0;
	}
	if (pid == 0) {
		usleep(100000);
		_exit(42);
	}

	int zero_count = 0;
	int reaped_pid = 0;
	int reaped_status = 0;
	int saw_reap = 0;
	int saw_echild = 0;
	int echild_errno = 0;

	for (int i = 0; i < MAX_ITERS; i++) {
		int status = 0;
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == 0) {
			if (saw_reap) {
				printf("REPRO: waitpid_wnohang FAIL zero_after_reap iter=%d\n", i);
				return 0;
			}
			zero_count++;
			usleep(POLL_DELAY_US);
			continue;
		}
		if (r == pid) {
			if (saw_reap) {
				printf("REPRO: waitpid_wnohang FAIL double_reap\n");
				return 0;
			}
			saw_reap = 1;
			reaped_pid = pid;
			reaped_status = status;
			continue;
		}
		if (r < 0) {
			if (errno == ECHILD && saw_reap) {
				saw_echild = 1;
				echild_errno = errno;
				break;
			}
			printf("REPRO: waitpid_wnohang FAIL wait_errno=%d saw_reap=%d\n",
				errno, saw_reap);
			return 0;
		}
		printf("REPRO: waitpid_wnohang FAIL unexpected_pid=%d\n", (int)r);
		return 0;
	}

	if (!saw_reap) {
		printf("REPRO: waitpid_wnohang FAIL no_reap zero_count=%d\n", zero_count);
		return 0;
	}
	if (!WIFEXITED(reaped_status) || WEXITSTATUS(reaped_status) != 42) {
		printf("REPRO: waitpid_wnohang FAIL bad_status=0x%x\n", reaped_status);
		return 0;
	}
	if (!saw_echild) {
		printf("REPRO: waitpid_wnohang FAIL no_echild\n");
		return 0;
	}
	(void)reaped_pid; (void)echild_errno;
	printf("REPRO: waitpid_wnohang PASS zero_polls=%d\n", zero_count);
	return 0;
}
