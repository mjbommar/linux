// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side strict replay mismatch smoke for KVM v2 record/replay.
 *
 * The helper records one supported payload-aware syscall, arms replay, and
 * then calls the same syscall with mismatched replay arguments. The host-side
 * runner expects the kernel to report a strict replay divergence and kill
 * init; returning from the mismatched syscall is a failure.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CTL_PATH	"/sys/kernel/debug/um/kvm_v2_record_ctl"

#ifndef SYS_getcwd
#define SYS_getcwd	79
#endif

#ifndef SYS_clock_gettime
#define SYS_clock_gettime	228
#endif

#ifndef SYS_gettimeofday
#define SYS_gettimeofday	96
#endif

#ifndef SYS_time
#define SYS_time		201
#endif

#if defined(KVM_RECORD_MISMATCH_CLOCK)
#define MISMATCH_NR	SYS_clock_gettime
#define MISMATCH_NAME	"clock_gettime"
#elif defined(KVM_RECORD_MISMATCH_GETTIMEOFDAY)
#define MISMATCH_NR	SYS_gettimeofday
#define MISMATCH_NAME	"gettimeofday"
#elif defined(KVM_RECORD_MISMATCH_TIME)
#define MISMATCH_NR	SYS_time
#define MISMATCH_NAME	"time"
#else
#define MISMATCH_NR	SYS_getcwd
#define MISMATCH_NAME	"getcwd"
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

static int record_supported_syscall(void)
{
#if defined(KVM_RECORD_MISMATCH_CLOCK)
	struct timespec ts;
	long rc = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);

	return rc < 0 ? -1 : 0;
#elif defined(KVM_RECORD_MISMATCH_GETTIMEOFDAY)
	struct timezone tz;
	struct timeval tv;
	long rc = syscall(SYS_gettimeofday, &tv, &tz);

	return rc < 0 ? -1 : 0;
#elif defined(KVM_RECORD_MISMATCH_TIME)
	long rc = syscall(SYS_time, NULL);

	return rc < 0 ? -1 : 0;
#else
	char cwd[256];
	long rc;

	memset(cwd, 0, sizeof(cwd));
	rc = syscall(SYS_getcwd, cwd, sizeof(cwd));
	return rc <= 0 ? -1 : 0;
#endif
}

static long replay_mismatched_syscall(void)
{
#if defined(KVM_RECORD_MISMATCH_CLOCK)
	struct timespec ts;

	memset(&ts, 0, sizeof(ts));
	return syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
#elif defined(KVM_RECORD_MISMATCH_GETTIMEOFDAY)
	struct timeval tv;

	memset(&tv, 0, sizeof(tv));
	return syscall(SYS_gettimeofday, &tv, NULL);
#elif defined(KVM_RECORD_MISMATCH_TIME)
	long value = 0;

	return syscall(SYS_time, &value);
#else
	char cwd[64];

	memset(cwd, 0, sizeof(cwd));
	return syscall(SYS_getcwd, cwd, sizeof(cwd));
#endif
}

int main(void)
{
	long rc;
	int fd;

	setup_mounts();

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_MISMATCH: FAIL open ctl errno=%d\n", errno);
		return 1;
	}
	if (write_ctl_fd(fd, "start 8192\n") < 0) {
		printf("KVM_RECORD_MISMATCH: FAIL start errno=%d\n", errno);
		(void)close(fd);
		return 1;
	}

	if (record_supported_syscall() < 0) {
		printf("KVM_RECORD_MISMATCH: FAIL record %s errno=%d\n",
		       MISMATCH_NAME, errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (write_ctl_fd(fd, "stop\n") < 0) {
		printf("KVM_RECORD_MISMATCH: FAIL stop errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (close(fd) < 0) {
		printf("KVM_RECORD_MISMATCH: FAIL close ctl errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_MISMATCH: FAIL open replay ctl errno=%d\n",
		       errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	printf("KVM_RECORD_MISMATCH: armed syscall=%ld name=%s\n",
	       (long)MISMATCH_NR, MISMATCH_NAME);
	fflush(stdout);

	if (write_ctl_fd(fd, "replay\n") < 0) {
		printf("KVM_RECORD_MISMATCH: FAIL replay errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	rc = replay_mismatched_syscall();
	printf("KVM_RECORD_MISMATCH: FAIL mismatched %s returned rc=%ld errno=%d\n",
	       MISMATCH_NAME, rc, errno);
	return 1;
}
