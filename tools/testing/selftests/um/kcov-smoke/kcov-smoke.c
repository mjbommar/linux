// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side UML KCOV smoke test.
 *
 * Opens /sys/kernel/debug/kcov, initializes and maps the trace area,
 * enables per-task PC tracing, runs a syscall, and requires at least
 * one collected PC. Intended for the UML fuzz and fuzz-deep profiles.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define KCOV_INIT_TRACE		_IOR('c', 1, unsigned long)
#define KCOV_ENABLE		_IO('c', 100)
#define KCOV_DISABLE		_IO('c', 101)
#define KCOV_TRACE_PC		0
#define COVER_SIZE		(64 << 10)

static int fail_errno_value(const char *op, int err)
{
	printf("KCOV_SMOKE: FAIL %s errno=%d (%s)\n",
	       op, err, strerror(err));
	return 1;
}

static int fail_errno(const char *op)
{
	return fail_errno_value(op, errno);
}

static void run_covered_syscalls(void)
{
	char buf[32];
	ssize_t ret;
	int fd;

	fd = open("/proc/self/stat", O_RDONLY);
	if (fd >= 0) {
		ret = read(fd, buf, sizeof(buf));
		(void)ret;
		close(fd);
	} else {
		ret = read(-1, NULL, 0);
		(void)ret;
	}

	(void)getpid();
}

int main(void)
{
	unsigned long *cover;
	unsigned long entries;
	size_t map_size = COVER_SIZE * sizeof(*cover);
	int fd;

	fd = open("/sys/kernel/debug/kcov", O_RDWR);
	if (fd < 0) {
		if (errno == ENOENT || errno == ENODEV || errno == ENXIO) {
			printf("KCOV_SMOKE: SKIP kcov debugfs node unavailable errno=%d\n",
			       errno);
			return 4;
		}
		return fail_errno("open");
	}

	if (ioctl(fd, KCOV_INIT_TRACE, COVER_SIZE) < 0) {
		int err = errno;

		close(fd);
		return fail_errno_value("KCOV_INIT_TRACE", err);
	}

	cover = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (cover == MAP_FAILED) {
		int err = errno;

		close(fd);
		return fail_errno_value("mmap", err);
	}

	if (ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC) < 0) {
		int err = errno;

		munmap(cover, map_size);
		close(fd);
		return fail_errno_value("KCOV_ENABLE", err);
	}

	__atomic_store_n(&cover[0], 0, __ATOMIC_RELAXED);

	run_covered_syscalls();

	entries = __atomic_load_n(&cover[0], __ATOMIC_RELAXED);

	if (ioctl(fd, KCOV_DISABLE, 0) < 0) {
		int err = errno;

		munmap(cover, map_size);
		close(fd);
		return fail_errno_value("KCOV_DISABLE", err);
	}

	if (entries == 0) {
		printf("KCOV_SMOKE: FAIL entries=0\n");
		munmap(cover, map_size);
		close(fd);
		return 1;
	}

	printf("KCOV_SMOKE: PASS mode=pc entries=%lu first_pc=0x%lx\n",
	       entries, cover[1]);

	munmap(cover, map_size);
	close(fd);
	return 0;
}
