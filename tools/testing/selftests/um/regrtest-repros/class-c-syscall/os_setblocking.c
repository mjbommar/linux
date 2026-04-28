/* Class C syscall repro: O_NONBLOCK pipe read must return -1 with EAGAIN. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

int main(void)
{
	int p[2];
	if (pipe(p) < 0) {
		printf("REPRO: os_setblocking FAIL pipe_errno=%d\n", errno);
		return 0;
	}

	int fl = fcntl(p[0], F_GETFL);
	if (fl < 0) {
		printf("REPRO: os_setblocking FAIL fcntl_get_errno=%d\n", errno);
		return 0;
	}
	if (fcntl(p[0], F_SETFL, fl | O_NONBLOCK) < 0) {
		printf("REPRO: os_setblocking FAIL fcntl_set_errno=%d\n", errno);
		return 0;
	}

	char b;
	ssize_t r = read(p[0], &b, 1);
	int e = errno;

	if (r != -1) {
		printf("REPRO: os_setblocking FAIL unexpected_read rc=%zd\n", r);
		return 0;
	}
	if (e != EAGAIN && e != EWOULDBLOCK) {
		printf("REPRO: os_setblocking FAIL errno=%d expected=EAGAIN\n", e);
		return 0;
	}

	printf("REPRO: os_setblocking PASS\n");
	return 0;
}
