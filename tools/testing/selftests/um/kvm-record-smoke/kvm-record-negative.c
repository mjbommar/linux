// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side negative smoke for strict KVM v2 replay.
 *
 * The helper arms replay with an empty log and then executes one unsupported
 * syscall. The host-side runner expects the kernel to log the strict replay
 * rejection and kill init; returning from the syscall is a failure.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CTL_PATH	"/sys/kernel/debug/um/kvm_v2_record_ctl"

#ifdef KVM_RECORD_NEGATIVE_OPENAT
#ifndef SYS_openat
#define SYS_openat		257
#endif
#define KVM_RECORD_NEGATIVE_SYSCALL	SYS_openat
#define KVM_RECORD_NEGATIVE_NAME	"openat"
#elif defined(KVM_RECORD_NEGATIVE_READ)
#ifndef SYS_read
#define SYS_read		0
#endif
#define KVM_RECORD_NEGATIVE_SYSCALL	SYS_read
#define KVM_RECORD_NEGATIVE_NAME	"read"
#elif defined(KVM_RECORD_NEGATIVE_WRITE)
#ifndef SYS_write
#define SYS_write		1
#endif
#define KVM_RECORD_NEGATIVE_SYSCALL	SYS_write
#define KVM_RECORD_NEGATIVE_NAME	"write"
#elif defined(KVM_RECORD_NEGATIVE_IOCTL)
#ifndef SYS_ioctl
#define SYS_ioctl		16
#endif
#define KVM_RECORD_NEGATIVE_SYSCALL	SYS_ioctl
#define KVM_RECORD_NEGATIVE_NAME	"ioctl"
#elif !defined(SYS_getrandom)
#define KVM_RECORD_NEGATIVE_SYSCALL	SYS_getuid
#define KVM_RECORD_NEGATIVE_NAME	"getuid"
#else
#define KVM_RECORD_NEGATIVE_SYSCALL	SYS_getrandom
#define KVM_RECORD_NEGATIVE_NAME	"getrandom"
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

static long trigger_unsupported_syscall(void)
{
#ifdef KVM_RECORD_NEGATIVE_OPENAT
	return syscall(KVM_RECORD_NEGATIVE_SYSCALL, AT_FDCWD, "/dev/null",
		       O_RDONLY | O_CLOEXEC);
#elif defined(KVM_RECORD_NEGATIVE_READ)
	char byte;

	return syscall(KVM_RECORD_NEGATIVE_SYSCALL, STDIN_FILENO, &byte,
		       sizeof(byte));
#elif defined(KVM_RECORD_NEGATIVE_WRITE)
	char byte = 0;

	return syscall(KVM_RECORD_NEGATIVE_SYSCALL, STDOUT_FILENO, &byte,
		       sizeof(byte));
#elif defined(KVM_RECORD_NEGATIVE_IOCTL)
	char byte;

	return syscall(KVM_RECORD_NEGATIVE_SYSCALL, STDOUT_FILENO, 0, &byte);
#elif defined(SYS_getrandom)
	char byte;

	return syscall(KVM_RECORD_NEGATIVE_SYSCALL, &byte, sizeof(byte), 0);
#else
	return syscall(KVM_RECORD_NEGATIVE_SYSCALL);
#endif
}

int main(void)
{
	long rc;
	int fd;

	setup_mounts();

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_NEGATIVE: FAIL open ctl errno=%d\n", errno);
		return 1;
	}
	if (write_ctl_fd(fd, "start 4096\n") < 0) {
		printf("KVM_RECORD_NEGATIVE: FAIL start errno=%d\n", errno);
		(void)close(fd);
		return 1;
	}
	if (write_ctl_fd(fd, "stop\n") < 0) {
		printf("KVM_RECORD_NEGATIVE: FAIL stop errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (close(fd) < 0) {
		printf("KVM_RECORD_NEGATIVE: FAIL close ctl errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_NEGATIVE: FAIL open replay ctl errno=%d\n",
		       errno);
		(void)write_ctl("destroy\n");
		return 1;
	}
	printf("KVM_RECORD_NEGATIVE: armed syscall=%ld name=%s\n",
	       (long)KVM_RECORD_NEGATIVE_SYSCALL, KVM_RECORD_NEGATIVE_NAME);
	fflush(stdout);

	if (write_ctl_fd(fd, "replay\n") < 0) {
		printf("KVM_RECORD_NEGATIVE: FAIL replay errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	rc = trigger_unsupported_syscall();
	printf("KVM_RECORD_NEGATIVE: FAIL unsupported syscall returned rc=%ld errno=%d\n",
	       rc, errno);
	return 1;
}
