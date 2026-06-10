/* SPDX-License-Identifier: GPL-2.0
 *
 * memcheck - minimal memtester-style memory pattern verifier.
 *
 * Allocates an anon MAP_PRIVATE region of <MB> megabytes, writes a
 * pattern, fsync-equivalent (sync via msync MS_SYNC isn't applicable
 * - it's anon), reads back + verifies, then runs a sequence of
 * walking-bit + checkerboard patterns. Each pattern miscompare is
 * counted; final RC = number of corrupt qwords seen across all
 * patterns. Exit 0 = clean.
 *
 * Designed as a drop-in for memtester(1) when apt isn't available.
 * Footprint is intentionally small so the test is fast on UML
 * (which is far slower than bare metal at memory-touching loops).
 *
 * Usage:  memcheck <MB> [iters]
 *   MB    - region size in mebibytes (default 64)
 *   iters - repeat count (default 1)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int verify(uint64_t *p, size_t n, uint64_t want, const char *name)
{
	size_t bad = 0;
	for (size_t i = 0; i < n; i++) {
		if (p[i] != want) {
			if (bad < 8)
				printf("  MISCOMPARE[%s] off=%zu got=%016lx want=%016lx\n",
				       name, i, (unsigned long)p[i],
				       (unsigned long)want);
			bad++;
		}
	}
	if (bad)
		printf("  %s: %zu/%zu mismatches\n", name, bad, n);
	return bad ? 1 : 0;
}

static int fill_and_verify(uint64_t *p, size_t n, uint64_t v, const char *name)
{
	for (size_t i = 0; i < n; i++)
		p[i] = v;
	return verify(p, n, v, name);
}

int main(int argc, char **argv)
{
	size_t mb = (argc >= 2) ? atoi(argv[1]) : 64;
	int iters = (argc >= 3) ? atoi(argv[2]) : 1;
	size_t bytes = mb << 20;
	size_t n = bytes / sizeof(uint64_t);
	int total_bad = 0;

	void *mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 2;
	}

	uint64_t *p = mem;
	uint64_t patterns[] = {
		0x0000000000000000ULL,
		0xFFFFFFFFFFFFFFFFULL,
		0xAAAAAAAAAAAAAAAAULL,
		0x5555555555555555ULL,
		0xDEADBEEFCAFEBABEULL,
		0x0123456789ABCDEFULL,
		0xFEDCBA9876543210ULL,
	};
	const char *names[] = {
		"zero", "ones", "alt-A", "alt-5", "deadbeef",
		"asc", "desc",
	};
	int npat = sizeof(patterns) / sizeof(patterns[0]);

	printf("memcheck mb=%zu iters=%d patterns=%d ...\n", mb, iters, npat);

	for (int it = 0; it < iters; it++) {
		for (int i = 0; i < npat; i++)
			total_bad += fill_and_verify(p, n, patterns[i], names[i]);

		/* Walking-bit (one bit set per qword, rotated) - more
		 * sensitive to addressing/wiring bugs than constant fills. */
		size_t wb_bad = 0;
		for (size_t i = 0; i < n; i++)
			p[i] = 1ULL << (i & 63);
		for (size_t i = 0; i < n; i++) {
			uint64_t want = 1ULL << (i & 63);
			if (p[i] != want) {
				if (wb_bad < 8)
					printf("  MISCOMPARE[walkbit] off=%zu got=%016lx want=%016lx\n",
					       i, (unsigned long)p[i],
					       (unsigned long)want);
				wb_bad++;
			}
		}
		if (wb_bad)
			printf("  walkbit: %zu/%zu mismatches\n", wb_bad, n);
		total_bad += wb_bad ? 1 : 0;

		printf("  iter %d/%d: bad=%d\n", it + 1, iters, total_bad);
	}

	munmap(mem, bytes);

	printf("memcheck: total_bad=%d (rc=%d)\n",
	       total_bad, total_bad ? 1 : 0);
	return total_bad ? 1 : 0;
}
