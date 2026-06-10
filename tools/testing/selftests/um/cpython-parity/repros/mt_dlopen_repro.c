/*
 * Multi-thread file-mmap+PROT_EXEC repro - mimics cpython's subinterpreter
 * import workload: each thread repeatedly dlopen/dlclose a small .so,
 * exercising file-backed PROT_EXEC mmap from sibling threads sharing the mm.
 *
 * Build inside guest userspace:
 *   /usr/bin/gcc -O2 -pthread -ldl -o /tmp/mt_dlopen_repro /tmp/mt_dlopen_repro.c
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <errno.h>

#define NTHREADS    5
#define ITERATIONS  2000

static atomic_int errors = 0;
static atomic_int done = 0;

/* libm.so.6 is universally present and small. Resolves cos() to verify
 * the dlopen actually loaded code; cos(0.0) == 1.0 deterministically. */
static const char *LIBPATH = "libm.so.6";
static const char *FUNCNAME = "cos";

struct ctx {
	int tid;
};

static void *worker(void *arg)
{
	struct ctx *c = arg;

	for (int i = 0; i < ITERATIONS; i++) {
		void *h = dlopen(LIBPATH, RTLD_NOW | RTLD_LOCAL);
		if (!h) {
			fprintf(stderr, "FAIL tid=%d iter=%d dlopen: %s\n",
				c->tid, i, dlerror());
			atomic_fetch_add(&errors, 1);
			break;
		}
		/* Resolve and call cos to ensure code page actually maps. */
		double (*fn)(double) = (double (*)(double))dlsym(h, FUNCNAME);
		if (!fn) {
			fprintf(stderr, "FAIL tid=%d iter=%d dlsym: %s\n",
				c->tid, i, dlerror());
			atomic_fetch_add(&errors, 1);
			dlclose(h);
			break;
		}
		double r = fn(0.0);
		if (r != 1.0) {
			fprintf(stderr, "FAIL tid=%d iter=%d cos(0.0)=%g!=1.0\n",
				c->tid, i, r);
			atomic_fetch_add(&errors, 1);
			dlclose(h);
			break;
		}
		if (dlclose(h) != 0) {
			fprintf(stderr, "FAIL tid=%d iter=%d dlclose: %s\n",
				c->tid, i, dlerror());
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
