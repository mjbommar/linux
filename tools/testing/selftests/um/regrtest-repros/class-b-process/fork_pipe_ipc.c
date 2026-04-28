/* Class B process repro: fork + pipe IPC. Child writes 100 messages
 * with rolling checksum, parent reads and validates. Stresses
 * stub-child fd inheritance + pipe drain on child exit. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#define N_MSG 100
#define MSG_LEN 32

static unsigned long fnv1a(const void *buf, size_t n)
{
	unsigned long h = 1469598103934665603UL;
	const unsigned char *p = buf;
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211UL;
	}
	return h;
}

int main(void)
{
	int fds[2];
	if (pipe(fds) < 0) {
		printf("REPRO: fork_pipe_ipc FAIL pipe_errno=%d\n", errno);
		return 0;
	}

	pid_t pid = fork();
	if (pid < 0) {
		printf("REPRO: fork_pipe_ipc FAIL fork_errno=%d\n", errno);
		return 0;
	}

	if (pid == 0) {
		close(fds[0]);
		char buf[MSG_LEN];
		for (int i = 0; i < N_MSG; i++) {
			memset(buf, 0, sizeof(buf));
			snprintf(buf, sizeof(buf), "msg-%05d-payload-XYZ", i);
			ssize_t w = write(fds[1], buf, MSG_LEN);
			if (w != MSG_LEN)
				_exit(2);
		}
		close(fds[1]);
		_exit(0);
	}

	close(fds[1]);
	char rbuf[N_MSG * MSG_LEN];
	size_t got = 0;
	while (got < sizeof(rbuf)) {
		ssize_t r = read(fds[0], rbuf + got, sizeof(rbuf) - got);
		if (r == 0)
			break;
		if (r < 0) {
			if (errno == EINTR) continue;
			printf("REPRO: fork_pipe_ipc FAIL read_errno=%d got=%zu\n", errno, got);
			return 0;
		}
		got += r;
	}
	close(fds[0]);

	int status = 0;
	pid_t r = waitpid(pid, &status, 0);
	if (r != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("REPRO: fork_pipe_ipc FAIL waitpid r=%d status=0x%x\n", (int)r, status);
		return 0;
	}

	if (got != sizeof(rbuf)) {
		printf("REPRO: fork_pipe_ipc FAIL short_read got=%zu want=%zu\n", got, sizeof(rbuf));
		return 0;
	}

	int seq_ok = 1;
	for (int i = 0; i < N_MSG; i++) {
		char want[MSG_LEN];
		memset(want, 0, sizeof(want));
		snprintf(want, sizeof(want), "msg-%05d-payload-XYZ", i);
		if (memcmp(rbuf + i * MSG_LEN, want, MSG_LEN) != 0) {
			seq_ok = 0;
			break;
		}
	}

	unsigned long h = fnv1a(rbuf, sizeof(rbuf));
	if (seq_ok)
		printf("REPRO: fork_pipe_ipc PASS checksum=0x%lx\n", h);
	else
		printf("REPRO: fork_pipe_ipc FAIL sequence_mismatch\n");
	return 0;
}
