/*
 * Minimal multi-thread shared-mm TLB-shootdown repro.
 *
 * N threads share the process address space (pthreads). Each thread loops:
 *   1. mmap a private anonymous page at a per-thread VA range
 *   2. write a thread-id pattern into it
 *   3. read it back; check pattern matches
 *   4. munmap
 *
 * The mmap/munmap activity from each thread modifies the shared mm. If
 * cross-vCPU TLB shootdown is broken, a thread may read stale bytes
 * from an unmapped page (bare metal would always see the right data
 * because the kernel's IPI-based TLB shootdown handles SMP coherence).
 *
 * Build for the GUEST (under UML):
 *   /usr/bin/gcc -static -pthread -O2 -o /tmp/mt_mmap_repro /tmp/mt_mmap_repro.c
 * Run under UML:
 *   /tmp/uml-kvmint/linux backend=force=kvm mem=512M ... init=/tmp/mt_mmap_repro
 *
 * Exits 0 on N iterations clean per thread, non-zero with a diagnostic
 * line on the first mismatch or signal.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>
#include <errno.h>

#define NTHREADS    5
#define ITERATIONS  20000
#define PAGE_SIZE   4096

static atomic_int errors = 0;
static atomic_int done = 0;

struct ctx {
	int tid;
};

static void *worker(void *arg)
{
	struct ctx *c = arg;
	unsigned char want = (unsigned char)(0xa0 + c->tid);

	for (int i = 0; i < ITERATIONS; i++) {
		void *p = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			fprintf(stderr, "FAIL tid=%d iter=%d mmap: errno=%d\n",
				c->tid, i, (int)errno);
			atomic_fetch_add(&errors, 1);
			break;
		}
		memset(p, want, PAGE_SIZE);
		unsigned char got = ((unsigned char *)p)[PAGE_SIZE / 2];
		if (got != want) {
			fprintf(stderr, "FAIL tid=%d iter=%d got=0x%x want=0x%x p=%p\n",
				c->tid, i, got, want, p);
			atomic_fetch_add(&errors, 1);
			munmap(p, PAGE_SIZE);
			break;
		}
		if (munmap(p, PAGE_SIZE) < 0) {
			fprintf(stderr, "FAIL tid=%d iter=%d munmap: errno=%d\n",
				c->tid, i, (int)errno);
			atomic_fetch_add(&errors, 1);
			break;
		}
	}
	atomic_fetch_add(&done, 1);
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t threads[NTHREADS];
	struct ctx ctxs[NTHREADS];

	for (int i = 0; i < NTHREADS; i++) {
		ctxs[i].tid = i;
		if (pthread_create(&threads[i], NULL, worker, &ctxs[i]) != 0) {
			fprintf(stderr, "MAIN: pthread_create %d failed\n", i);
			return 2;
		}
	}
	for (int i = 0; i < NTHREADS; i++)
		pthread_join(threads[i], NULL);

	int e = atomic_load(&errors);
	if (e) {
		printf("REPRO: ERRORS=%d done=%d nthreads=%d iters=%d\n",
		       e, atomic_load(&done), NTHREADS, ITERATIONS);
		return 1;
	}
	printf("REPRO: OK done=%d nthreads=%d iters=%d\n",
	       atomic_load(&done), NTHREADS, ITERATIONS);
	return 0;
}
