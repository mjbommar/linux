/* Class A env repro: PID-1 init under hostfs root has no devpts mount
 * and no /dev/ptmx node. Maps to test_openpty failure. */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

int main(void)
{
	int fd = open("/dev/ptmx", O_RDWR);
	int e = errno;
	if (fd >= 0) {
		close(fd);
		printf("REPRO: openpty FAIL ptmx_opened\n");
		return 0;
	}

	int fd2 = posix_openpt(O_RDWR | O_NOCTTY);
	int e2 = errno;
	if (fd2 >= 0) {
		close(fd2);
		printf("REPRO: openpty FAIL posix_openpt_opened\n");
		return 0;
	}

	int ok1 = (e == ENOENT || e == EACCES || e == ENXIO || e == ENODEV);
	int ok2 = (e2 == ENOENT || e2 == EACCES || e2 == ENXIO || e2 == ENODEV);
	if (ok1 && ok2)
		printf("REPRO: openpty PASS\n");
	else
		printf("REPRO: openpty FAIL ptmx_errno=%d openpt_errno=%d\n", e, e2);
	return 0;
}
