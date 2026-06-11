// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side raw-time payload smoke for strict KVM v2 replay.
 *
 * The helper records one clock_gettime(2) result, arms replay, and verifies
 * that replay returns the recorded timestamp bytes instead of consulting host
 * time again.
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
	struct timespec recorded;
	struct timespec replayed;
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

	memset(&recorded, 0, sizeof(recorded));
	rc = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &recorded);
	if (rc < 0) {
		printf("KVM_RECORD_TIME: FAIL record clock_gettime rc=%ld errno=%d\n",
		       rc, errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
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

	if (write_ctl_fd(fd, "replay\n") < 0) {
		printf("KVM_RECORD_TIME: FAIL replay errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	memset(&replayed, 0, sizeof(replayed));
	rc = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &replayed);
	if (rc < 0) {
		printf("KVM_RECORD_TIME: FAIL replay clock_gettime rc=%ld errno=%d\n",
		       rc, errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (close(fd) < 0) {
		printf("KVM_RECORD_TIME: FAIL close replay ctl errno=%d\n",
		       errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (recorded.tv_sec != replayed.tv_sec ||
	    recorded.tv_nsec != replayed.tv_nsec) {
		printf("KVM_RECORD_TIME: FAIL mismatch recorded=%ld.%09ld replayed=%ld.%09ld\n",
		       (long)recorded.tv_sec, recorded.tv_nsec,
		       (long)replayed.tv_sec, replayed.tv_nsec);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (write_ctl("destroy\n") < 0) {
		printf("KVM_RECORD_TIME: FAIL destroy errno=%d\n", errno);
		return 1;
	}

	printf("KVM_RECORD_TIME: PASS syscall=%ld name=clock_gettime sec=%ld nsec=%ld\n",
	       (long)SYS_clock_gettime, (long)replayed.tv_sec,
	       replayed.tv_nsec);
	return 0;
}
