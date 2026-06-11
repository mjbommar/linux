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
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#define CTL_PATH	"/sys/kernel/debug/um/kvm_v2_record_ctl"
#define STATUS_PATH	"/sys/kernel/debug/um/kvm_v2_record_status"

#ifndef SYS_uname
#define SYS_uname	63
#endif

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

static long scalar_workload(void)
{
	struct utsname uts;
	char cwd[256];
	long sink = 0;
	long cwd_len;
	int i;

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

	return sink;
}

int main(void)
{
	long pid = getpid();
	long entries;
	long syscalls;
	long same_task;
	long other_tasks;
	long first_pid;
	long last_pid;
	long payload_entries;
	long payload_bytes;
	long sink;

	setup_mounts();

	if (access(CTL_PATH, W_OK) < 0 || access(STATUS_PATH, R_OK) < 0) {
		printf("KVM_RECORD_TASK: FAIL missing debugfs controls errno=%d\n",
		       errno);
		return 1;
	}

	if (write_ctl("start 1048576\n") < 0) {
		printf("KVM_RECORD_TASK: FAIL start errno=%d\n", errno);
		return 1;
	}

	if (expect_long("enabled", 1) < 0 ||
	    expect_long("snapshot_task_state", 1) < 0 ||
	    expect_long("snapshot_source_pid", pid) < 0) {
		(void)write_ctl("stop\n");
		(void)write_ctl("destroy\n");
		return 1;
	}

	sink = scalar_workload();

	if (write_ctl("stop\n") < 0) {
		printf("KVM_RECORD_TASK: FAIL stop errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (get_long("entries_recorded", &entries) < 0 ||
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

	if (entries <= 0 || syscalls <= 0 || same_task != syscalls ||
	    other_tasks != 0 || first_pid != pid || last_pid != pid ||
	    payload_entries < 2 || payload_bytes <= 390 || sink < 0) {
		printf("KVM_RECORD_TASK: FAIL pid=%ld entries=%ld syscalls=%ld ",
		       pid, entries, syscalls);
		printf("same=%ld other=%ld first=%ld last=%ld ",
		       same_task, other_tasks, first_pid, last_pid);
		printf("payload_entries=%ld payload_bytes=%ld sink=%ld\n",
		       payload_entries, payload_bytes, sink);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (write_ctl("destroy\n") < 0) {
		printf("KVM_RECORD_TASK: FAIL destroy errno=%d\n", errno);
		return 1;
	}

	printf("KVM_RECORD_TASK: PASS pid=%ld entries=%ld syscalls=%ld ",
	       pid, entries, syscalls);
	printf("same=%ld other=%ld payload_entries=%ld payload_bytes=%ld sink=%ld\n",
	       same_task, other_tasks, payload_entries, payload_bytes, sink);
	return 0;
}
