// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side signal-mask policy smoke for strict KVM v2 replay.
 *
 * Replay asks KVM_SET_SIGNAL_MASK to block SIGALRM during KVM_RUN so the UML
 * timer cannot create an unrecorded in-guest EINTR point. The tracepoint is
 * the public evidence surface for the per-vCPU mask transition.
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
#define TRACE_EVENT	"um_backend_kvm_v2_sigmask_install"
#define TRACE_ENABLE	"events/um_backend/" TRACE_EVENT "/enable"
#define TRACE_SIZE	65536
#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

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
	(void)mount_if_needed("proc", "/proc");
	(void)mount_if_needed("sysfs", "/sys");
	(void)ensure_dir("/sys/kernel");
	(void)ensure_dir("/sys/kernel/debug");
	(void)ensure_dir("/sys/kernel/tracing");
	(void)mount_if_needed("debugfs", "/sys/kernel/debug");
}

static int make_path(char *buf, size_t size, const char *root,
		     const char *name)
{
	int n = snprintf(buf, size, "%s/%s", root, name);

	return n >= 0 && (size_t)n < size ? 0 : -1;
}

static int write_file(const char *path, const char *value)
{
	size_t len = strlen(value);
	ssize_t written;
	int fd;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	written = write(fd, value, len);
	if (close(fd) < 0)
		return -1;
	return written == (ssize_t)len ? 0 : -1;
}

static int read_file(const char *path, char *buf, size_t size)
{
	ssize_t n;
	int fd;

	if (!size)
		return -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
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

static const char *find_tracefs(void)
{
	static const char * const roots[] = {
		"/sys/kernel/tracing",
		"/sys/kernel/debug/tracing",
	};
	static char enable[256];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(roots); i++) {
		(void)ensure_dir(roots[i]);
		(void)mount_if_needed("tracefs", roots[i]);
		if (!make_path(enable, sizeof(enable), roots[i], TRACE_ENABLE) &&
		    access(enable, W_OK) == 0)
			return roots[i];
	}

	return NULL;
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

static int configure_trace(const char *root, int enabled)
{
	char path[256];

	if (make_path(path, sizeof(path), root, "tracing_on") ||
	    write_file(path, "0\n") < 0)
		return -1;

	if (make_path(path, sizeof(path), root, TRACE_ENABLE) ||
	    write_file(path, enabled ? "1\n" : "0\n") < 0)
		return -1;

	if (!enabled)
		return 0;

	if (make_path(path, sizeof(path), root, "trace") ||
	    write_file(path, "\n") < 0)
		return -1;

	if (make_path(path, sizeof(path), root, "tracing_on") ||
	    write_file(path, "1\n") < 0)
		return -1;

	return 0;
}

int main(void)
{
	char trace_path[256];
	const char *block;
	const char *unblock;
	const char *trace_root;
	char *trace;
	long pid;
	int fd;
	int rc;

	setup_mounts();

	trace_root = find_tracefs();
	if (!trace_root) {
		printf("KVM_RECORD_SIGNAL: SKIP trace_event=%s unavailable\n",
		       TRACE_EVENT);
		return 4;
	}

	if (configure_trace(trace_root, 1) < 0) {
		printf("KVM_RECORD_SIGNAL: FAIL trace setup errno=%d\n", errno);
		return 1;
	}

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_SIGNAL: FAIL open ctl errno=%d\n", errno);
		(void)configure_trace(trace_root, 0);
		return 1;
	}
	if (write_ctl_fd(fd, "start 4096\n") < 0) {
		printf("KVM_RECORD_SIGNAL: FAIL start errno=%d\n", errno);
		(void)close(fd);
		(void)configure_trace(trace_root, 0);
		return 1;
	}

	pid = syscall(SYS_getpid);
	if (pid <= 0) {
		printf("KVM_RECORD_SIGNAL: FAIL record getpid rc=%ld errno=%d\n",
		       pid, errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		(void)configure_trace(trace_root, 0);
		return 1;
	}

	if (write_ctl_fd(fd, "stop\n") < 0) {
		printf("KVM_RECORD_SIGNAL: FAIL stop errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		(void)configure_trace(trace_root, 0);
		return 1;
	}
	if (close(fd) < 0) {
		printf("KVM_RECORD_SIGNAL: FAIL close ctl errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		(void)configure_trace(trace_root, 0);
		return 1;
	}

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_SIGNAL: FAIL open replay ctl errno=%d\n",
		       errno);
		(void)write_ctl("destroy\n");
		(void)configure_trace(trace_root, 0);
		return 1;
	}

	printf("KVM_RECORD_SIGNAL: armed trace_event=%s\n", TRACE_EVENT);
	fflush(stdout);

	if (write_ctl_fd(fd, "replay\n") < 0) {
		printf("KVM_RECORD_SIGNAL: FAIL replay errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		(void)configure_trace(trace_root, 0);
		return 1;
	}

	pid = syscall(SYS_getpid);
	if (pid <= 0) {
		printf("KVM_RECORD_SIGNAL: FAIL replay getpid rc=%ld errno=%d\n",
		       pid, errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		(void)configure_trace(trace_root, 0);
		return 1;
	}
	if (close(fd) < 0) {
		printf("KVM_RECORD_SIGNAL: FAIL close replay ctl errno=%d\n",
		       errno);
		(void)write_ctl("destroy\n");
		(void)configure_trace(trace_root, 0);
		return 1;
	}

	if (make_path(trace_path, sizeof(trace_path), trace_root, "tracing_on") ||
	    write_file(trace_path, "0\n") < 0) {
		printf("KVM_RECORD_SIGNAL: FAIL stop tracing errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	trace = malloc(TRACE_SIZE);
	if (!trace) {
		printf("KVM_RECORD_SIGNAL: FAIL malloc trace\n");
		(void)write_ctl("destroy\n");
		(void)configure_trace(trace_root, 0);
		return 1;
	}
	if (make_path(trace_path, sizeof(trace_path), trace_root, "trace") ||
	    read_file(trace_path, trace, TRACE_SIZE) < 0) {
		printf("KVM_RECORD_SIGNAL: FAIL read trace errno=%d\n", errno);
		free(trace);
		(void)write_ctl("destroy\n");
		(void)configure_trace(trace_root, 0);
		return 1;
	}

	rc = configure_trace(trace_root, 0);
	(void)write_ctl("destroy\n");

	block = strstr(trace, "block_timer=1");
	unblock = block ? strstr(block, "block_timer=0") : NULL;
	if (!block || !unblock) {
		printf("KVM_RECORD_SIGNAL: FAIL block_timer=1 seen=%d restore_seen=%d\n",
		       block ? 1 : 0, unblock ? 1 : 0);
		free(trace);
		return 1;
	}

	printf("KVM_RECORD_SIGNAL: PASS trace_event=%s replayed_pid=%ld restore=1\n",
	       TRACE_EVENT, pid);
	free(trace);
	return rc < 0 ? 1 : 0;
}
