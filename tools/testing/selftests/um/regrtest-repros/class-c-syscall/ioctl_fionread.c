/* Class C syscall repro: pipe + FIONREAD ioctl. UML must report bytes
 * available on the read end. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

int main(void)
{
	int p[2];
	if (pipe(p) < 0) {
		printf("REPRO: ioctl_fionread FAIL pipe_errno=%d\n", errno);
		return 0;
	}

	const char buf[100] = { 0 };
	ssize_t w = write(p[1], buf, sizeof(buf));
	if (w != (ssize_t)sizeof(buf)) {
		printf("REPRO: ioctl_fionread FAIL write_n=%zd errno=%d\n", w, errno);
		return 0;
	}

	int n = -1;
	if (ioctl(p[0], FIONREAD, &n) < 0) {
		printf("REPRO: ioctl_fionread FAIL ioctl_errno=%d\n", errno);
		return 0;
	}

	if (n != 100) {
		printf("REPRO: ioctl_fionread FAIL n=%d expected=100\n", n);
		return 0;
	}

	printf("REPRO: ioctl_fionread PASS\n");
	return 0;
}
