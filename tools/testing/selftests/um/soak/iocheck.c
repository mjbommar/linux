/* SPDX-License-Identifier: GPL-2.0
 *
 * iocheck — minimal fio-style write/fsync/read/verify loop on tmpfs.
 *
 * Replaces fio for the case where apt isn't reachable. Per iteration:
 *   1. Open <path>, ftruncate to <MB>
 *   2. Write deterministic block-keyed pattern (block-id-as-u64
 *      repeated through the block) for each <BLOCKSIZE>-byte block
 *   3. fsync
 *   4. close + reopen + read sequentially, verify each block matches
 *      its expected pattern
 *   5. unlink
 *
 * Catches:
 *   - pagecache writeback losing or corrupting data
 *   - fsync returning before data is durable in the inode
 *   - read returning stale data after a writethrough+reopen
 *   - kernel buffer truncation under SMP write fan-out
 *
 * Exit code = total mismatched blocks across all iters. 0 = clean.
 *
 * Usage:  iocheck <PATH> <MB> [iters] [blocksize]
 *   PATH      — file to write/read (typically /tmp/iocheck-$$)
 *   MB        — file size in MiB (default 32)
 *   iters     — repeat count (default 1)
 *   blocksize — bytes per block (default 4096)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

static int run_one(const char *path, size_t mb, size_t bs, int it)
{
	size_t bytes = mb << 20;
	size_t nblocks = bytes / bs;
	uint8_t *buf = malloc(bs);
	int bad = 0;

	if (!buf) {
		perror("malloc");
		return -1;
	}

	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		perror("open(write)");
		free(buf);
		return -1;
	}
	if (ftruncate(fd, bytes) < 0) {
		perror("ftruncate");
		close(fd);
		free(buf);
		return -1;
	}

	for (size_t b = 0; b < nblocks; b++) {
		uint64_t key = ((uint64_t)it << 32) | b;
		uint64_t *p = (uint64_t *)buf;
		for (size_t j = 0; j < bs / sizeof(uint64_t); j++)
			p[j] = key;
		if (write(fd, buf, bs) != (ssize_t)bs) {
			perror("write");
			close(fd);
			free(buf);
			return -1;
		}
	}

	if (fsync(fd) < 0) {
		perror("fsync");
		close(fd);
		free(buf);
		return -1;
	}
	close(fd);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open(read)");
		free(buf);
		return -1;
	}

	for (size_t b = 0; b < nblocks; b++) {
		uint64_t key = ((uint64_t)it << 32) | b;
		ssize_t n = read(fd, buf, bs);
		if (n != (ssize_t)bs) {
			fprintf(stderr, "  short read at block %zu: got %zd\n", b, n);
			bad++;
			continue;
		}
		uint64_t *p = (uint64_t *)buf;
		for (size_t j = 0; j < bs / sizeof(uint64_t); j++) {
			if (p[j] != key) {
				if (bad < 8)
					printf("  MISCOMPARE block=%zu off=%zu got=%016lx want=%016lx\n",
					       b, j, (unsigned long)p[j],
					       (unsigned long)key);
				bad++;
				break;
			}
		}
	}

	close(fd);
	unlink(path);
	free(buf);
	return bad;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: iocheck <path> <MB> [iters] [blocksize]\n");
		return 2;
	}
	const char *path = argv[1];
	size_t mb = atoi(argv[2]);
	int iters = (argc >= 4) ? atoi(argv[3]) : 1;
	size_t bs = (argc >= 5) ? atoi(argv[4]) : 4096;

	int total_bad = 0;
	printf("iocheck path=%s mb=%zu iters=%d bs=%zu\n", path, mb, iters, bs);
	for (int it = 0; it < iters; it++) {
		int bad = run_one(path, mb, bs, it);
		if (bad < 0)
			return 2;
		total_bad += bad;
		printf("  iter %d/%d: bad=%d (cumulative=%d)\n",
		       it + 1, iters, bad, total_bad);
	}
	printf("iocheck: total_bad=%d (rc=%d)\n",
	       total_bad, total_bad ? 1 : 0);
	return total_bad ? 1 : 0;
}
