// SPDX-License-Identifier: GPL-2.0
/*
 * Stack-corruption reproducer. Each pthread tight-loops writing+reading
 * a local stack variable; periodic sched_yield. Verifies whether the
 * stack-marker value ever changes (i.e., another thread/kernel wrote
 * to OUR stack memory).
 *
 * Companion to mt-yieldonly.c: this checks WHETHER user-stack memory
 * gets corrupted (as opposed to RIP/saved-return-addr corruption).
 *
 * Build: gcc -static -O0 -pthread -o mt-stackcheck mt-stackcheck.c
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
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
