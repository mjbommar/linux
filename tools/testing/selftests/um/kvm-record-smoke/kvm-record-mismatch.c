// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side strict replay mismatch smoke for KVM v2 record/replay.
 *
 * The helper records one payload-aware getcwd(2), arms replay, and then calls
 * getcwd(2) with a different size argument. The host-side runner expects the
 * kernel to report a strict replay divergence and kill init; returning from
 * the mismatched syscall is a failure.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CTL_PATH	"/sys/kernel/debug/um/kvm_v2_record_ctl"

#ifndef SYS_getcwd
#define SYS_getcwd	79
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
	char recorded_cwd[256];
	char replay_cwd[64];
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

	memset(recorded_cwd, 0, sizeof(recorded_cwd));
	rc = syscall(SYS_getcwd, recorded_cwd, sizeof(recorded_cwd));
	if (rc <= 0) {
		printf("KVM_RECORD_MISMATCH: FAIL record getcwd rc=%ld errno=%d\n",
		       rc, errno);
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

	printf("KVM_RECORD_MISMATCH: armed syscall=%ld name=getcwd\n",
	       (long)SYS_getcwd);
	fflush(stdout);

	if (write_ctl_fd(fd, "replay\n") < 0) {
		printf("KVM_RECORD_MISMATCH: FAIL replay errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	memset(replay_cwd, 0, sizeof(replay_cwd));
	rc = syscall(SYS_getcwd, replay_cwd, sizeof(replay_cwd));
	printf("KVM_RECORD_MISMATCH: FAIL mismatched getcwd returned rc=%ld errno=%d\n",
	       rc, errno);
	return 1;
}
