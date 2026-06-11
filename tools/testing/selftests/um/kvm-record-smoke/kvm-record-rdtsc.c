// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side RDTSC negative smoke for strict KVM v2 replay.
 *
 * Replay sets CR4.TSD so user RDTSC cannot observe host time outside the log.
 * The helper records one getpid(2) entry, arms replay, and executes RDTSC
 * before consuming that entry. A correct replay run kills init on the RDTSC
 * fault. If RDTSC returns, the helper consumes the getpid(2) entry to leave
 * replay mode and prints a failure.
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

static unsigned long long read_tsc(void)
{
	unsigned int lo;
	unsigned int hi;

	asm volatile("rdtsc" : "=a" (lo), "=d" (hi));
	return ((unsigned long long)hi << 32) | lo;
}

int main(void)
{
	unsigned long long tsc;
	long pid;
	int fd;

	setup_mounts();

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_RDTSC: FAIL open ctl errno=%d\n", errno);
		return 1;
	}
	if (write_ctl_fd(fd, "start 4096\n") < 0) {
		printf("KVM_RECORD_RDTSC: FAIL start errno=%d\n", errno);
		(void)close(fd);
		return 1;
	}

	pid = syscall(SYS_getpid);
	if (pid <= 0) {
		printf("KVM_RECORD_RDTSC: FAIL record getpid rc=%ld errno=%d\n",
		       pid, errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (write_ctl_fd(fd, "stop\n") < 0) {
		printf("KVM_RECORD_RDTSC: FAIL stop errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (close(fd) < 0) {
		printf("KVM_RECORD_RDTSC: FAIL close ctl errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_RDTSC: FAIL open replay ctl errno=%d\n",
		       errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	printf("KVM_RECORD_RDTSC: armed instruction=rdtsc\n");
	fflush(stdout);

	if (write_ctl_fd(fd, "replay\n") < 0) {
		printf("KVM_RECORD_RDTSC: FAIL replay errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	tsc = read_tsc();
	pid = syscall(SYS_getpid);
	printf("KVM_RECORD_RDTSC: FAIL rdtsc returned tsc=%llu replayed_pid=%ld\n",
	       tsc, pid);
	return 1;
}
