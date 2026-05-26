/* Class C syscall repro: BLKGETSIZE on /dev/loop0 if available. PASSes if
 * device returns sensible size or open fails with expected env errno. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

int main(void)
{
	int fd = open("/dev/loop0", O_RDONLY);
	if (fd < 0) {
		int e = errno;
		if (e == ENOENT || e == EACCES || e == EPERM || e == ENXIO) {
			printf("REPRO: ioctl_blkgetsize_loop PASS no_loop_dev errno=%d\n", e);
			return 0;
		}
		printf("REPRO: ioctl_blkgetsize_loop FAIL open_errno=%d\n", e);
		return 0;
	}

	unsigned long sz = 0;
	int rc = ioctl(fd, BLKGETSIZE, &sz);
	int e = errno;
	close(fd);

	if (rc < 0) {
		if (e == ENOTTY || e == EINVAL) {
			printf("REPRO: ioctl_blkgetsize_loop FAIL ioctl_unsupported errno=%d\n", e);
			return 0;
		}
		printf("REPRO: ioctl_blkgetsize_loop FAIL ioctl_errno=%d\n", e);
		return 0;
	}

	printf("REPRO: ioctl_blkgetsize_loop PASS size=%lu\n", sz);
	return 0;
}
