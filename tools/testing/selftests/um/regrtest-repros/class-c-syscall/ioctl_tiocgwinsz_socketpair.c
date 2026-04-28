/* Class C syscall repro: TIOCGWINSZ on a non-tty fd must fail with ENOTTY. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>

int main(void)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		printf("REPRO: ioctl_tiocgwinsz_socketpair FAIL socketpair_errno=%d\n", errno);
		return 0;
	}

	struct winsize ws;
	int rc = ioctl(sv[0], TIOCGWINSZ, &ws);
	int e = errno;

	if (rc == 0) {
		printf("REPRO: ioctl_tiocgwinsz_socketpair FAIL unexpected_success\n");
		return 0;
	}
	if (e != ENOTTY) {
		printf("REPRO: ioctl_tiocgwinsz_socketpair FAIL errno=%d expected=ENOTTY\n", e);
		return 0;
	}

	printf("REPRO: ioctl_tiocgwinsz_socketpair PASS\n");
	return 0;
}
