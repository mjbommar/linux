// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side raw-time negative smoke for strict KVM v2 replay.
 *
 * The helper arms replay with an empty log and then executes clock_gettime(2)
 * through the syscall path. The host-side runner expects the kernel to log the
 * strict replay rejection and kill init; returning from the syscall is a
 * failure because replay must not observe host time outside the log.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CTL_PATH	"/sys/kernel/debug/um/kvm_v2_record_ctl"

#ifndef SYS_clock_gettime
#define SYS_clock_gettime	228
#endif

static int ensure_dir(const char *path)
{
	if (!mkdir(path, 0755) || errno == EEXIST)
		return 0;
	return -1;
}

static int mount_if_needed(const char *type, const char *target)
{
	if (!mount("none", target, type, 0, "") || errno == EBUSY)
		return 0;
	return -1;
}

static void setup_mounts(void)
{
	(void)ensure_dir("/proc");
	(void)ensure_dir("/sys");
	(void)ensure_dir("/sys/kernel");
	(void)ensure_dir("/sys/kernel/debug");
	(void)mount_if_needed("proc", "/proc");
	(void)mount_if_needed("debugfs", "/sys/kernel/debug");
}

static int write_ctl_fd(int fd, const char *cmd)
{
	size_t len = strlen(cmd);
	ssize_t written = write(fd, cmd, len);

	return written == (ssize_t)len ? 0 : -1;
}

static int write_ctl(const char *cmd)
{
	int fd;
	int rc;

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	rc = write_ctl_fd(fd, cmd);
	if (close(fd) < 0)
		return -1;
	return rc;
}

int main(void)
{
	struct timespec ts;
	long rc;
	int fd;

	setup_mounts();

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_TIME: FAIL open ctl errno=%d\n", errno);
		return 1;
	}
	if (write_ctl_fd(fd, "start 4096\n") < 0) {
		printf("KVM_RECORD_TIME: FAIL start errno=%d\n", errno);
		(void)close(fd);
		return 1;
	}
	if (write_ctl_fd(fd, "stop\n") < 0) {
		printf("KVM_RECORD_TIME: FAIL stop errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (close(fd) < 0) {
		printf("KVM_RECORD_TIME: FAIL close ctl errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_TIME: FAIL open replay ctl errno=%d\n",
		       errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	printf("KVM_RECORD_TIME: armed syscall=%ld name=clock_gettime\n",
	       (long)SYS_clock_gettime);
	fflush(stdout);

	if (write_ctl_fd(fd, "replay\n") < 0) {
		printf("KVM_RECORD_TIME: FAIL replay errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	memset(&ts, 0, sizeof(ts));
	rc = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
	printf("KVM_RECORD_TIME: FAIL clock_gettime returned rc=%ld errno=%d sec=%ld nsec=%ld\n",
	       rc, errno, (long)ts.tv_sec, ts.tv_nsec);
	return 1;
}
