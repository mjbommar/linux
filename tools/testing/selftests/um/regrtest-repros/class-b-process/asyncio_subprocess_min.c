/* Class B process repro: minimal asyncio.subprocess analog. Pipe +
 * fork; child writes "hello\n" then exits; parent uses poll()+read()
 * to drain stdout, then waitpid. Validates close-on-exit + pipe drain
 * + SIGCHLD reap interaction. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>

#define EXPECT "hello\n"

int main(void)
{
	int fds[2];
	if (pipe(fds) < 0) {
		printf("REPRO: asyncio_subprocess_min FAIL pipe_errno=%d\n", errno);
		return 0;
	}

	pid_t pid = fork();
	if (pid < 0) {
		printf("REPRO: asyncio_subprocess_min FAIL fork_errno=%d\n", errno);
		return 0;
	}
	if (pid == 0) {
		close(fds[0]);
		const char *m = EXPECT;
		size_t n = strlen(m), put = 0;
		while (put < n) {
			ssize_t w = write(fds[1], m + put, n - put);
			if (w < 0) { if (errno == EINTR) continue; _exit(2); }
			put += w;
		}
		close(fds[1]);
		_exit(0);
	}

	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);

	char buf[128];
	size_t got = 0;
	int saw_eof = 0;
	int loops = 0;
	while (!saw_eof && loops < 200) {
		struct pollfd pf = { .fd = fds[0], .events = POLLIN };
		int pr = poll(&pf, 1, 2000);
		if (pr < 0) {
			if (errno == EINTR) { loops++; continue; }
			printf("REPRO: asyncio_subprocess_min FAIL poll_errno=%d\n", errno);
			return 0;
		}
		if (pr == 0) {
			printf("REPRO: asyncio_subprocess_min FAIL poll_timeout got=%zu\n", got);
			return 0;
		}
		if (pf.revents & (POLLIN | POLLHUP)) {
			ssize_t r = read(fds[0], buf + got, sizeof(buf) - got);
			if (r == 0) { saw_eof = 1; break; }
			if (r < 0) {
				if (errno == EAGAIN || errno == EINTR) { loops++; continue; }
				printf("REPRO: asyncio_subprocess_min FAIL read_errno=%d\n", errno);
				return 0;
			}
			got += r;
			if (got >= sizeof(buf)) break;
		}
		loops++;
	}
	close(fds[0]);

	int status = 0;
	pid_t r = waitpid(pid, &status, 0);
	if (r != pid) {
		printf("REPRO: asyncio_subprocess_min FAIL waitpid_errno=%d\n", errno);
		return 0;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("REPRO: asyncio_subprocess_min FAIL status=0x%x\n", status);
		return 0;
	}

	if (got != strlen(EXPECT) || memcmp(buf, EXPECT, strlen(EXPECT)) != 0) {
		printf("REPRO: asyncio_subprocess_min FAIL payload got=%zu\n", got);
		return 0;
	}
	printf("REPRO: asyncio_subprocess_min PASS bytes=%zu\n", got);
	return 0;
}
