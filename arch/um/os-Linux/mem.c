// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <init.h>
#include <kern_util.h>
#include <os.h>
#include "internal.h"

/*
 * kasan_map_memory - maps memory from @start with a size of @len.
 * The allocated memory is filled with zeroes upon success.
 * @start: the start address of the memory to be mapped
 * @len: the length of the memory to be mapped
 *
 * This function is used to map shadow memory for KASAN in uml
 */
void kasan_map_memory(void *start, size_t len)
{
	if (mmap(start,
		 len,
		 PROT_READ|PROT_WRITE,
		 MAP_FIXED|MAP_ANONYMOUS|MAP_PRIVATE|MAP_NORESERVE,
		 -1,
		 0) == MAP_FAILED) {
		os_info("Couldn't allocate shadow memory: %s\n.",
			strerror(errno));
		exit(1);
	}

	if (madvise(start, len, MADV_DONTDUMP)) {
		os_info("Couldn't set MAD_DONTDUMP on shadow memory: %s\n.",
			strerror(errno));
		exit(1);
	}

	/*
	 * Workstream C-09 / D37 pull-forward #6.
	 *
	 * Historically we set MADV_DONTFORK on the KASAN shadow so it
	 * wasn't inherited into child processes. That is still the
	 * right default for non-fuzz profiles — the shadow is a 16 TB
	 * VA region, and needlessly COW'ing it into every host fork
	 * wastes kernel page-table memory and makes strace/ps output
	 * confusing. But the C-09 snapshot/forkserver (CONFIG_UM_FUZZ_HOOKS)
	 * forks the UML process itself per fuzz iteration, and the
	 * forked worker needs to inherit the shadow via COW — otherwise
	 * the first KASAN-instrumented kernel access in the worker
	 * SEGVs on an unmapped shadow range. See D35's Q2 and D37 #6
	 * for the analysis.
	 *
	 * Under CONFIG_UM_FUZZ_HOOKS, skip the MADV_DONTFORK so fork()
	 * propagates the shadow mapping. The per-iteration COW cost on
	 * sparsely-populated shadow is bounded by how much kernel VA
	 * the testcase touches; empirically well under 1 ms per iter
	 * for syzkaller-shaped workloads.
	 */
	if (!IS_ENABLED(CONFIG_UM_FUZZ_HOOKS)) {
		if (madvise(start, len, MADV_DONTFORK)) {
			os_info("Couldn't set MADV_DONTFORK on shadow memory: %s\n.",
				strerror(errno));
			exit(1);
		}
	}
}

/* Set by make_tempfile() during early boot. */
char *tempdir = NULL;

/* Check if dir is on tmpfs. Return 0 if yes, -1 if no or error. */
static int __init check_tmpfs(const char *dir)
{
	struct statfs st;

	os_info("Checking if %s is on tmpfs...", dir);
	if (statfs(dir, &st) < 0) {
		os_info("%s\n", strerror(errno));
	} else if (st.f_type != TMPFS_MAGIC) {
		os_info("no\n");
	} else {
		os_info("OK\n");
		return 0;
	}
	return -1;
}

/*
 * Choose the tempdir to use. We want something on tmpfs so that our memory is
 * not subject to the host's vm.dirty_ratio. If a tempdir is specified in the
 * environment, we use that even if it's not on tmpfs, but we warn the user.
 * Otherwise, we try common tmpfs locations, and if no tmpfs directory is found
 * then we fall back to /tmp.
 */
static char * __init choose_tempdir(void)
{
	static const char * const vars[] = {
		"TMPDIR",
		"TMP",
		"TEMP",
		NULL
	};
	static const char fallback_dir[] = "/tmp";
	static const char * const tmpfs_dirs[] = {
		"/dev/shm",
		fallback_dir,
		NULL
	};
	int i;
	const char *dir;

	os_info("Checking environment variables for a tempdir...");
	for (i = 0; vars[i]; i++) {
		dir = getenv(vars[i]);
		if ((dir != NULL) && (*dir != '\0')) {
			os_info("%s\n", dir);
			if (check_tmpfs(dir) >= 0)
				goto done;
			else
				goto warn;
		}
	}
	os_info("none found\n");

	for (i = 0; tmpfs_dirs[i]; i++) {
		dir = tmpfs_dirs[i];
		if (check_tmpfs(dir) >= 0)
			goto done;
	}

	dir = fallback_dir;
warn:
	os_warn("Warning: tempdir %s is not on tmpfs\n", dir);
done:
	/* Make a copy since getenv results may not remain valid forever. */
	return strdup(dir);
}

/*
 * Create an unlinked tempfile in a suitable tempdir. template must be the
 * basename part of the template with a leading '/'.
 */
static int __init make_tempfile(const char *template)
{
	char *tempname;
	int fd;

	if (tempdir == NULL) {
		tempdir = choose_tempdir();
		if (tempdir == NULL) {
			os_warn("Failed to choose tempdir: %s\n",
				strerror(errno));
			return -1;
		}
	}

	/*
	 * FD disposition (C-09 commit 4): inherit (physmem fd).
	 * This fd backs the UML kernel's physical-memory file and
	 * is the target of all kernel-page mmap()s. Across a
	 * forkserver fork() the worker inherits this fd and the
	 * associated mappings (MAP_SHARED on the same inode via
	 * MADV_DOFORK; see arch/um/kernel/physmem.c). Closing or
	 * re-creating it in the worker would unmap the kernel's
	 * .data and heap. Must remain CLOEXEC to avoid leaks into
	 * the userspace stub's exec().
	 */
#ifdef O_TMPFILE
	fd = open(tempdir, O_CLOEXEC | O_RDWR | O_EXCL | O_TMPFILE, 0700);
	/*
	 * If the running system does not support O_TMPFILE flag then retry
	 * without it.
	 */
	if (fd != -1 || (errno != EINVAL && errno != EISDIR &&
			errno != EOPNOTSUPP))
		return fd;
#endif

	tempname = malloc(strlen(tempdir) + strlen(template) + 1);
	if (tempname == NULL)
		return -1;

	strcpy(tempname, tempdir);
	strcat(tempname, template);
	fd = mkstemp(tempname);
	if (fd < 0) {
		os_warn("open - cannot create %s: %s\n", tempname,
			strerror(errno));
		goto out;
	}
	/*
	 * mkstemp() has no SOCK_CLOEXEC-style atomic flag; set
	 * FD_CLOEXEC immediately after create to match the
	 * O_TMPFILE path's disposition.
	 */
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		perror("fcntl FD_CLOEXEC");
		goto close;
	}
	if (unlink(tempname) < 0) {
		perror("unlink");
		goto close;
	}
	free(tempname);
	return fd;
close:
	close(fd);
out:
	free(tempname);
	return -1;
}

#define TEMPNAME_TEMPLATE "/vm_file-XXXXXX"

static int __init create_tmp_file(unsigned long long len)
{
	int fd, err;
	char zero;

	fd = make_tempfile(TEMPNAME_TEMPLATE);
	if (fd < 0)
		exit(1);

	/*
	 * Seek to len - 1 because writing a character there will
	 * increase the file size by one byte, to the desired length.
	 */
	if (lseek64(fd, len - 1, SEEK_SET) < 0) {
		perror("lseek64");
		exit(1);
	}

	zero = 0;

	err = write(fd, &zero, 1);
	if (err != 1) {
		perror("write");
		exit(1);
	}

	return fd;
}

int __init create_mem_file(unsigned long long len)
{
	int err, fd;

	fd = create_tmp_file(len);

	err = os_set_exec_close(fd);
	if (err < 0) {
		errno = -err;
		perror("exec_close");
	}
	return fd;
}

void __init check_tmpexec(void)
{
	void *addr;
	int err, fd = create_tmp_file(UM_KERN_PAGE_SIZE);

	addr = mmap(NULL, UM_KERN_PAGE_SIZE,
		    PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE, fd, 0);
	os_info("Checking PROT_EXEC mmap in %s...", tempdir);
	if (addr == MAP_FAILED) {
		err = errno;
		os_warn("%s\n", strerror(err));
		close(fd);
		if (err == EPERM)
			os_warn("%s must be not mounted noexec\n", tempdir);
		exit(1);
	}
	os_info("OK\n");
	munmap(addr, UM_KERN_PAGE_SIZE);

	close(fd);
}
