// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side task-owned KVM v2 record smoke.
 *
 * The debugfs record start path snapshots the task that writes "start".
 * This helper runs as one process, writes the control commands itself, and
 * verifies that the recorded syscall stream belongs to that same task.
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
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define CTL_PATH	"/sys/kernel/debug/um/kvm_v2_record_ctl"
#define STATUS_PATH	"/sys/kernel/debug/um/kvm_v2_record_status"

#ifndef SYS_uname
#define SYS_uname	63
#endif

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

struct workload_sample {
	struct timespec ts;
	struct timeval tv;
	struct timezone tz;
	long time_value;
	long time_ret;
};

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

static int write_ctl(const char *cmd)
{
	size_t len = strlen(cmd);
	ssize_t written;
	int fd;

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	written = write(fd, cmd, len);
	if (close(fd) < 0)
		return -1;
	return written == (ssize_t)len ? 0 : -1;
}

static int write_ctl_fd(int fd, const char *cmd)
{
	size_t len = strlen(cmd);
	ssize_t written = write(fd, cmd, len);

	return written == (ssize_t)len ? 0 : -1;
}

static int read_status(char *buf, size_t size)
{
	ssize_t n;
	int fd;

	if (!size)
		return -1;

	fd = open(STATUS_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	if (close(fd) < 0)
		return -1;
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return 0;
}

static long status_long(const char *key, int *found)
{
	char buf[4096];
	char *line;
	size_t key_len = strlen(key);

	*found = 0;
	if (read_status(buf, sizeof(buf)) < 0)
		return 0;

	for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
		char *value;

		if (strncmp(line, key, key_len) || line[key_len] != ':')
			continue;
		value = line + key_len + 1;
		while (*value == ' ')
			value++;
		*found = 1;
		return strtol(value, NULL, 0);
	}

	return 0;
}

static int expect_long(const char *key, long expected)
{
	int found;
	long got = status_long(key, &found);

	if (!found) {
		printf("KVM_RECORD_TASK: FAIL missing %s\n", key);
		return -1;
	}
	if (got != expected) {
		printf("KVM_RECORD_TASK: FAIL %s=%ld expected=%ld\n",
		       key, got, expected);
		return -1;
	}
	return 0;
}

static int get_long(const char *key, long *out)
{
	int found;

	*out = status_long(key, &found);
	if (!found) {
		printf("KVM_RECORD_TASK: FAIL missing %s\n", key);
		return -1;
	}
	return 0;
}

static long deterministic_workload(struct workload_sample *sample)
{
	struct utsname uts;
	char cwd[256];
	long sink = 0;
	long cwd_len;
	long rc;
	int i;

	memset(sample, 0, sizeof(*sample));

	for (i = 0; i < 128; i++) {
		sink += getpid();
		sink += getppid();
		sink += syscall(SYS_gettid);
	}

	memset(&uts, 0, sizeof(uts));
	if (syscall(SYS_uname, &uts) == 0)
		sink += uts.sysname[0] + uts.machine[0];
	else
		return -1;

	memset(cwd, 0, sizeof(cwd));
	cwd_len = syscall(SYS_getcwd, cwd, sizeof(cwd));
	if (cwd_len > 0)
		sink += cwd[0] + cwd[cwd_len - 1];
	else
		return -1;

	rc = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &sample->ts);
	if (rc < 0)
		return -1;

	rc = syscall(SYS_gettimeofday, &sample->tv, &sample->tz);
	if (rc < 0)
		return -1;

	sample->time_ret = syscall(SYS_time, &sample->time_value);
	if (sample->time_ret < 0 || sample->time_ret != sample->time_value)
		return -1;

	sink += sample->ts.tv_nsec & 0xff;
	sink += sample->tv.tv_usec & 0xff;
	sink += sample->time_value & 0xff;

	return sink;
}

static int samples_match(const struct workload_sample *recorded,
			 const struct workload_sample *replayed)
{
	return recorded->ts.tv_sec == replayed->ts.tv_sec &&
	       recorded->ts.tv_nsec == replayed->ts.tv_nsec &&
	       recorded->tv.tv_sec == replayed->tv.tv_sec &&
	       recorded->tv.tv_usec == replayed->tv.tv_usec &&
	       recorded->tz.tz_minuteswest == replayed->tz.tz_minuteswest &&
	       recorded->tz.tz_dsttime == replayed->tz.tz_dsttime &&
	       recorded->time_ret == replayed->time_ret &&
	       recorded->time_value == replayed->time_value;
}

int main(void)
{
	struct workload_sample recorded_sample;
	struct workload_sample replayed_sample;
	long pid = getpid();
	long entries;
	long syscalls;
	long same_task;
	long other_tasks;
	long first_pid;
	long last_pid;
	long payload_entries;
	long payload_bytes;
	long replayed_entries;
	long replayed_payload_entries;
	long replayed_payload_bytes;
	long replay_failures;
	long sink;
	int ctl_fd;

	setup_mounts();

	if (access(CTL_PATH, W_OK) < 0 || access(STATUS_PATH, R_OK) < 0) {
		printf("KVM_RECORD_TASK: FAIL missing debugfs controls errno=%d\n",
		       errno);
		return 1;
	}

	ctl_fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (ctl_fd < 0) {
		printf("KVM_RECORD_TASK: FAIL open ctl errno=%d\n", errno);
		return 1;
	}
	if (write_ctl_fd(ctl_fd, "start 1048576\n") < 0) {
		printf("KVM_RECORD_TASK: FAIL start errno=%d\n", errno);
		(void)close(ctl_fd);
		return 1;
	}

	sink = deterministic_workload(&recorded_sample);

	if (write_ctl_fd(ctl_fd, "stop\n") < 0) {
		printf("KVM_RECORD_TASK: FAIL stop errno=%d\n", errno);
		(void)close(ctl_fd);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (close(ctl_fd) < 0) {
		printf("KVM_RECORD_TASK: FAIL close ctl errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (expect_long("enabled", 0) < 0 ||
	    expect_long("snapshot_task_state", 1) < 0 ||
	    expect_long("snapshot_source_pid", pid) < 0 ||
	    get_long("entries_recorded", &entries) < 0 ||
	    get_long("syscall_count", &syscalls) < 0 ||
	    get_long("syscalls_from_snapshot_task", &same_task) < 0 ||
	    get_long("syscalls_from_other_tasks", &other_tasks) < 0 ||
	    get_long("first_syscall_pid", &first_pid) < 0 ||
	    get_long("last_syscall_pid", &last_pid) < 0 ||
	    get_long("payload_entries_recorded", &payload_entries) < 0 ||
	    get_long("payload_bytes_recorded", &payload_bytes) < 0) {
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (entries <= 0 || syscalls <= 0 || entries != syscalls ||
	    same_task != syscalls ||
	    other_tasks != 0 || first_pid != pid || last_pid != pid ||
	    payload_entries < 5 || payload_bytes <= 440 || sink < 0) {
		printf("KVM_RECORD_TASK: FAIL pid=%ld entries=%ld syscalls=%ld ",
		       pid, entries, syscalls);
		printf("same=%ld other=%ld first=%ld last=%ld ",
		       same_task, other_tasks, first_pid, last_pid);
		printf("payload_entries=%ld payload_bytes=%ld sink=%ld\n",
		       payload_entries, payload_bytes, sink);
		(void)write_ctl("destroy\n");
		return 1;
	}

	ctl_fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (ctl_fd < 0) {
		printf("KVM_RECORD_TASK: FAIL open replay ctl errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (write_ctl_fd(ctl_fd, "replay\n") < 0) {
		printf("KVM_RECORD_TASK: FAIL replay errno=%d\n", errno);
		(void)close(ctl_fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	sink = deterministic_workload(&replayed_sample);
	if (close(ctl_fd) < 0) {
		printf("KVM_RECORD_TASK: FAIL close replay ctl errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (expect_long("enabled", 0) < 0 ||
	    get_long("entries_recorded", &entries) < 0 ||
	    get_long("syscall_count", &syscalls) < 0 ||
	    get_long("syscalls_from_snapshot_task", &same_task) < 0 ||
	    get_long("syscalls_from_other_tasks", &other_tasks) < 0 ||
	    get_long("entries_replayed", &replayed_entries) < 0 ||
	    get_long("payload_entries_recorded", &payload_entries) < 0 ||
	    get_long("payload_entries_replayed", &replayed_payload_entries) < 0 ||
	    get_long("payload_bytes_recorded", &payload_bytes) < 0 ||
	    get_long("payload_bytes_replayed", &replayed_payload_bytes) < 0 ||
	    get_long("strict_replay_failures", &replay_failures) < 0) {
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (sink < 0 || replay_failures != 0 || replayed_entries != entries ||
	    replayed_payload_entries != payload_entries ||
	    replayed_payload_bytes != payload_bytes) {
		printf("KVM_RECORD_TASK: FAIL replay entries=%ld/%ld ",
		       replayed_entries, entries);
		printf("payload_entries=%ld/%ld payload_bytes=%ld/%ld ",
		       replayed_payload_entries, payload_entries,
		       replayed_payload_bytes, payload_bytes);
		printf("failures=%ld sink=%ld\n", replay_failures, sink);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (!samples_match(&recorded_sample, &replayed_sample)) {
		printf("KVM_RECORD_TASK: FAIL time payload mismatch ");
		printf("recorded_clock=%ld.%09ld replayed_clock=%ld.%09ld ",
		       (long)recorded_sample.ts.tv_sec,
		       recorded_sample.ts.tv_nsec,
		       (long)replayed_sample.ts.tv_sec,
		       replayed_sample.ts.tv_nsec);
		printf("recorded_time=%ld replayed_time=%ld\n",
		       recorded_sample.time_value, replayed_sample.time_value);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (write_ctl("destroy\n") < 0) {
		printf("KVM_RECORD_TASK: FAIL destroy errno=%d\n", errno);
		return 1;
	}

	printf("KVM_RECORD_TASK: PASS pid=%ld entries=%ld syscalls=%ld ",
	       pid, entries, syscalls);
	printf("same=%ld other=%ld payload_entries=%ld payload_bytes=%ld ",
	       same_task, other_tasks, payload_entries, payload_bytes);
	printf("replayed=%ld time=%ld sink=%ld\n", replayed_entries,
	       replayed_sample.time_value, sink);
	return 0;
}
