// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side timestamp-instruction negative smoke for strict KVM v2 replay.
 *
 * Replay sets CR4.TSD so direct user timestamp reads cannot observe host time
 * outside the log. The helper records one getpid(2) entry, arms replay, and
 * executes the selected instruction before consuming that entry. A correct
 * replay run kills init on the timestamp fault. If the instruction returns,
 * the helper consumes the getpid(2) entry to leave replay mode and prints a
 * failure.
 */

#define _GNU_SOURCE

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CTL_PATH	"/sys/kernel/debug/um/kvm_v2_record_ctl"

#ifdef KVM_RECORD_TIMESTAMP_RDTSCP
#define RESULT_PREFIX	"KVM_RECORD_RDTSCP"
#define INSN_NAME	"rdtscp"
#else
#define RESULT_PREFIX	"KVM_RECORD_RDTSC"
#define INSN_NAME	"rdtsc"
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

static bool timestamp_instruction_supported(void)
{
#ifdef KVM_RECORD_TIMESTAMP_RDTSCP
	unsigned int a;
	unsigned int b;
	unsigned int c;
	unsigned int d;

	asm volatile("cpuid"
		     : "=a" (a), "=b" (b), "=c" (c), "=d" (d)
		     : "0" (0x80000000U), "2" (0U));
	if (a < 0x80000001)
		return false;

	asm volatile("cpuid"
		     : "=a" (a), "=b" (b), "=c" (c), "=d" (d)
		     : "0" (0x80000001U), "2" (0U));
	return d & (1U << 27);
#else
	return true;
#endif
}

static unsigned long long read_timestamp(void)
{
	unsigned int lo;
	unsigned int hi;

#ifdef KVM_RECORD_TIMESTAMP_RDTSCP
	unsigned int aux;

	asm volatile("rdtscp" : "=a" (lo), "=d" (hi), "=c" (aux) :: "memory");
#else
	asm volatile("rdtsc" : "=a" (lo), "=d" (hi));
#endif
	return ((unsigned long long)hi << 32) | lo;
}

int main(void)
{
	unsigned long long tsc;
	long pid;
	int fd;

	setup_mounts();

	if (!timestamp_instruction_supported()) {
		printf("%s: SKIP instruction=%s unsupported\n",
		       RESULT_PREFIX, INSN_NAME);
		return 4;
	}

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("%s: FAIL open ctl errno=%d\n", RESULT_PREFIX, errno);
		return 1;
	}
	if (write_ctl_fd(fd, "start 4096\n") < 0) {
		printf("%s: FAIL start errno=%d\n", RESULT_PREFIX, errno);
		(void)close(fd);
		return 1;
	}

	pid = syscall(SYS_getpid);
	if (pid <= 0) {
		printf("%s: FAIL record getpid rc=%ld errno=%d\n",
		       RESULT_PREFIX, pid, errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (write_ctl_fd(fd, "stop\n") < 0) {
		printf("%s: FAIL stop errno=%d\n", RESULT_PREFIX, errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (close(fd) < 0) {
		printf("%s: FAIL close ctl errno=%d\n", RESULT_PREFIX, errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("%s: FAIL open replay ctl errno=%d\n",
		       RESULT_PREFIX, errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	printf("%s: armed instruction=%s\n", RESULT_PREFIX, INSN_NAME);
	fflush(stdout);

	if (write_ctl_fd(fd, "replay\n") < 0) {
		printf("%s: FAIL replay errno=%d\n", RESULT_PREFIX, errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	tsc = read_timestamp();
	pid = syscall(SYS_getpid);
	printf("%s: FAIL %s returned tsc=%llu replayed_pid=%ld\n",
	       RESULT_PREFIX, INSN_NAME, tsc, pid);
	return 1;
}
