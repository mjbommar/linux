/* Cross-process futex on a memfd-shared page.
 *
 * Verify that a futex on a memfd-shared page works when the parent
 * and child live in different VA spaces. The seccomp stub-data path
 * depends on this shape when worker and stub state are split.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int futex_wait(uint32_t *uaddr, uint32_t expect)
{
	struct timespec to = { .tv_sec = 2, .tv_nsec = 0 };
	return syscall(SYS_futex, uaddr, FUTEX_WAIT, expect, &to, NULL, 0);
}

static int futex_wake(uint32_t *uaddr, int n)
{
	return syscall(SYS_futex, uaddr, FUTEX_WAKE, n, NULL, NULL, 0);
}

int main(void)
{
	int mfd = memfd_create("xprocfutex", 0);
	if (mfd < 0) {
		printf("REPRO: cross_process_futex FAIL memfd_create errno=%d\n", errno);
		return 0;
	}
	if (ftruncate(mfd, 4096) < 0) {
		printf("REPRO: cross_process_futex FAIL ftruncate errno=%d\n", errno);
		return 0;
	}
	uint32_t *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	if (p == MAP_FAILED) {
		printf("REPRO: cross_process_futex FAIL mmap errno=%d\n", errno);
		return 0;
	}
	*p = 0x1234;

	pid_t pid = fork();
	if (pid < 0) {
		printf("REPRO: cross_process_futex FAIL fork errno=%d\n", errno);
		return 0;
	}
	if (pid == 0) {
		struct timespec d = { .tv_sec = 0, .tv_nsec = 50000000 };
		nanosleep(&d, NULL);
		*p = 0x4321;
		futex_wake(p, 1);
		_exit(0);
	}

	int rc = futex_wait(p, 0x1234);
	int e = errno;
	int status;
	waitpid(pid, &status, 0);

	if (rc == 0 && *p == 0x4321) {
		printf("REPRO: cross_process_futex PASS\n");
	} else if (rc < 0 && e == EAGAIN && *p == 0x4321) {
		printf("REPRO: cross_process_futex PASS race_eagain\n");
	} else {
		printf("REPRO: cross_process_futex FAIL rc=%d errno=%d val=0x%x\n",
			rc, e, *p);
	}
	return 0;
}
