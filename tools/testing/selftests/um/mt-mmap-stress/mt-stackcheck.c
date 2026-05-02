// SPDX-License-Identifier: GPL-2.0
/*
 * Stack-corruption reproducer. Each pthread:
 *  - sets a local variable on its stack to its tid
 *  - tight-loops writing+reading the variable
 *  - reports if the variable ever reads back wrong
 *
 * NO mmap calls. NO file I/O. Only stack memory + a periodic syscall
 * (sched_yield) to force vCPU dispatch. Any STACK_FAIL hit proves
 * the bug is page-level corruption of pthread stack memory, not in
 * the mmap path.
 *
 * Build: gcc -static -O0 -pthread -o mt-stackcheck mt-stackcheck.c
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ITERS 200000

static volatile long worker_failures;

static void *worker(void *arg)
{
	long tid = (long)arg;
	volatile long marker = tid;
	volatile long counter = 0;

	for (long i = 0; i < ITERS; i++) {
		marker = tid;
		counter++;
		if (marker != tid) {
			__sync_fetch_and_add(&worker_failures, 1);
			fprintf(stderr,
				"STACK_FAIL tid=%ld iter=%ld marker=%ld counter=%ld\n",
				tid, i, marker, counter);
			return (void *)2;
		}
		if ((i & 0x3ff) == 0)
			sched_yield();
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int n = (argc > 1) ? atoi(argv[1]) : 8;
	if (n > 16) n = 16;
	pthread_t t[16];
	long rc = 0;

	for (long i = 0; i < n; i++)
		pthread_create(&t[i], NULL, worker, (void *)i);
	for (long i = 0; i < n; i++) {
		void *r;
		pthread_join(t[i], &r);
		rc |= (long)r;
	}
	if (rc == 0)
		printf("STACK_OK n=%d failures=%ld\n", n, worker_failures);
	else
		printf("STACK_FAIL rc=%ld failures=%ld\n", rc, worker_failures);
	return rc != 0;
}
