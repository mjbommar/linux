/* Class B process repro: SIGCHLD must wake parent blocked in poll().
 * Uses self-pipe wakeup pattern. Spawn child that exits after 100ms;
 * parent polls a non-data pipe with 5s timeout. Failure mode: poll
 * blocks for full 5s instead of waking on SIGCHLD. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>

static int sp[2];

static void sigchld_handler(int sig)
{
	(void)sig;
	char b = 'c';
	ssize_t w = write(sp[1], &b, 1);
	(void)w;
}

static long ms_since(const struct timespec *t0)
{
	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0->tv_sec) * 1000L +
		(t1.tv_nsec - t0->tv_nsec) / 1000000L;
}

int main(void)
{
	int idle[2];
	if (pipe(idle) < 0 || pipe(sp) < 0) {
		printf("REPRO: sigchld_select FAIL pipe_errno=%d\n", errno);
		return 0;
	}
	fcntl(sp[0], F_SETFL, O_NONBLOCK);
	fcntl(sp[1], F_SETFL, O_NONBLOCK);

	struct sigaction sa = {0};
	sa.sa_handler = sigchld_handler;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGCHLD, &sa, NULL) < 0) {
		printf("REPRO: sigchld_select FAIL sigaction_errno=%d\n", errno);
		return 0;
	}

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	pid_t pid = fork();
	if (pid < 0) {
		printf("REPRO: sigchld_select FAIL fork_errno=%d\n", errno);
		return 0;
	}
	if (pid == 0) {
		usleep(100000);
		_exit(7);
	}

	struct pollfd pf[2];
	pf[0].fd = idle[0];
	pf[0].events = POLLIN;
	pf[0].revents = 0;
	pf[1].fd = sp[0];
	pf[1].events = POLLIN;
	pf[1].revents = 0;

	int pr;
	for (;;) {
		pr = poll(pf, 2, 5000);
		if (pr < 0 && errno == EINTR) continue;
		break;
	}
	long elapsed = ms_since(&t0);

	if (pr < 0) {
		printf("REPRO: sigchld_select FAIL poll_errno=%d\n", errno);
		return 0;
	}
	if (pr == 0) {
		printf("REPRO: sigchld_select FAIL poll_timeout elapsed=%ldms\n", elapsed);
		return 0;
	}
	/* Drain self-pipe. */
	char drain[16];
	ssize_t dr = read(sp[0], drain, sizeof(drain));
	(void)dr;

	int status = 0;
	pid_t r = waitpid(pid, &status, 0);
	if (r != pid) {
		printf("REPRO: sigchld_select FAIL waitpid_errno=%d\n", errno);
		return 0;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 7) {
		printf("REPRO: sigchld_select FAIL status=0x%x\n", status);
		return 0;
	}

	close(idle[0]); close(idle[1]);
	close(sp[0]); close(sp[1]);

	if (elapsed >= 4500) {
		printf("REPRO: sigchld_select FAIL slow_wake elapsed=%ldms\n", elapsed);
		return 0;
	}
	printf("REPRO: sigchld_select PASS elapsed=%ldms\n", elapsed);
	return 0;
}
