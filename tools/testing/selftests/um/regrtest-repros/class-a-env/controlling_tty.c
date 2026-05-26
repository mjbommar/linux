/* Class A env repro: PID-1 init has no controlling tty, so opening
 * /dev/tty must fail with ENXIO. Maps to test_termios / test_tty
 * fallbacks that try /dev/tty when stdin isn't a tty. */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(void)
{
	int fd = open("/dev/tty", O_RDWR);
	int e = errno;
	if (fd >= 0) {
		close(fd);
		printf("REPRO: controlling_tty FAIL opened\n");
		return 0;
	}
	if (e == ENXIO || e == ENOENT || e == EACCES || e == ENODEV)
		printf("REPRO: controlling_tty PASS\n");
	else
		printf("REPRO: controlling_tty FAIL errno=%d\n", e);
	return 0;
}
