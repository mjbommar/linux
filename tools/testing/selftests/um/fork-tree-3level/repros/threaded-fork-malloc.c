// SPDX-License-Identifier: GPL-2.0
//
// Minimal C reproducer for the SMP-T26 glibc-heap-corruption residual.
//
// Mirrors threaded-subprocess-wait.py's fork+exec churn, but the
// child process executes malloc/free cycles to exercise heap
// integrity checks (vs threaded-fork-exec.c which execve's /bin/true
// — too small to hit glibc heap metadata).
//
// If H1 (cross-vCPU TLB stale → recycled-page read) is correct, the
// child process should occasionally trip glibc's malloc/free integrity
// check and abort with SIGABRT (rc=-6 / WTERMSIG=6). The check fires
// when malloc/free metadata bytes (chunk size, prev_size, fd/bk
// pointers) are inconsistent — the same signature we see in the
// Python repro.
//
// Build (multi-binary):
//   cc -O0 -static -pthread -o threaded-fork-malloc \
//      threaded-fork-malloc.c
//   cc -O0 -static -o malloc-stress-child \
//      -DCHILD_MAIN threaded-fork-malloc.c
// Run:
//   ./threaded-fork-malloc [N_WORKERS] [ITERS_PER_WORKER]
// Default: 4 workers × 100 iters = 400 forks (matches Python repro)

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * CHILD path: many varied-size mallocs+frees, then exit(0).
 * Triggers glibc heap integrity checks (which is what fires SIGABRT
 * with "free(): invalid size" / "malloc(): corrupted top size" if
 * heap metadata has been corrupted by stale-TLB reads).
 */
#ifdef CHILD_MAIN
int main(void)
{
	/* ~100 allocations across small/medium/large bins. Includes
	 * tcache, fastbins, smallbin, largebin, mmap'd huge regions.
	 * Frees in mixed order to exercise unlink/unsorted bin logic.
	 */
	enum { N = 100 };
	void *p[N];
	int i;
	static const size_t sizes[] = {
		16, 32, 48, 64, 80, 96, 128, 192, 256, 384,
		512, 768, 1024, 1536, 2048, 3072, 4096, 8192,
		16384, 65536, 131072,
	};
	const int nsizes = sizeof(sizes) / sizeof(sizes[0]);

	for (i = 0; i < N; i++) {
		size_t sz = sizes[i % nsizes];
		p[i] = malloc(sz);
		if (!p[i]) return 2;
		/* Touch first + last byte so the page is faulted in. */
		((char *)p[i])[0] = 0xa5;
		((char *)p[i])[sz - 1] = 0x5a;
	}
	/* Free in reverse order. */
	for (i = N - 1; i >= 0; i--)
		free(p[i]);
	/* Round 2 — alloc + free in interleaved order. */
	for (i = 0; i < N; i++) {
		size_t sz = sizes[(i * 7) % nsizes];
		p[i] = malloc(sz);
		if (!p[i]) return 2;
	}
	for (i = 0; i < N; i++)
		if ((i & 1) == 0) free(p[i]);
	for (i = 0; i < N; i++)
		if ((i & 1) == 1) free(p[i]);
	return 0;
}

#else  /* PARENT path */

static int n_workers = 4;
static int iters_per_worker = 100;
static const char *child_path;

static volatile int total_fails;
static volatile int total_aborts;
static char child_path_buf[4096];

static void *worker(void *arg)
{
	long wid = (long)arg;
	int i;

	for (i = 0; i < iters_per_worker; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			fprintf(stderr, "[w%ld iter=%d] FORK_FAIL: %s\n",
				wid, i, strerror(errno));
			__sync_add_and_fetch(&total_fails, 1);
			return NULL;
		}

		if (pid == 0) {
			char *const argv[] = { (char *)child_path, NULL };
			char *const envp[] = { NULL };
			execve(child_path, argv, envp);
			fprintf(stderr, "[w%ld iter=%d] EXECVE_FAIL: %s\n",
				wid, i, strerror(errno));
			_exit(127);
		}

		int status;
		pid_t r = waitpid(pid, &status, 0);

		if (r < 0) {
			fprintf(stderr, "[w%ld iter=%d] WAIT_FAIL: %s\n",
				wid, i, strerror(errno));
			__sync_add_and_fetch(&total_fails, 1);
			return NULL;
		}

		if (WIFSIGNALED(status) && WTERMSIG(status) == 6) {
			/* SIGABRT — likely glibc integrity check. */
			fprintf(stderr,
				"[w%ld iter=%d] CHILD_ABORT (SIGABRT)\n",
				wid, i);
			__sync_add_and_fetch(&total_aborts, 1);
			__sync_add_and_fetch(&total_fails, 1);
		} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
			int rc  = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
			fprintf(stderr,
				"[w%ld iter=%d] CHILD_FAIL rc=%d sig=%d\n",
				wid, i, rc, sig);
			__sync_add_and_fetch(&total_fails, 1);
		}
	}

	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t *threads;
	long w;
	int total;

	/* Resolve child path: same dir as parent, name 'malloc-stress-child' */
	{
		ssize_t n = readlink("/proc/self/exe", child_path_buf,
				     sizeof(child_path_buf) - 1);
		if (n < 0 || (size_t)n >= sizeof(child_path_buf) - 30) {
			fprintf(stderr, "readlink /proc/self/exe failed\n");
			return 1;
		}
		child_path_buf[n] = 0;
		char *slash = strrchr(child_path_buf, '/');
		if (!slash) {
			fprintf(stderr, "no '/' in /proc/self/exe?\n");
			return 1;
		}
		strcpy(slash + 1, "malloc-stress-child");
		child_path = child_path_buf;
	}

	if (argc >= 2) n_workers = atoi(argv[1]);
	if (argc >= 3) iters_per_worker = atoi(argv[2]);
	if (n_workers < 1 || iters_per_worker < 1) {
		fprintf(stderr, "usage: %s [N_WORKERS] [ITERS_PER_WORKER]\n",
			argv[0]);
		return 2;
	}

	threads = calloc(n_workers, sizeof(pthread_t));
	if (!threads) {
		fprintf(stderr, "OOM\n");
		return 1;
	}

	for (w = 0; w < n_workers; w++) {
		if (pthread_create(&threads[w], NULL, worker, (void *)w)) {
			fprintf(stderr, "pthread_create w=%ld failed\n", w);
			return 1;
		}
	}

	for (w = 0; w < n_workers; w++)
		pthread_join(threads[w], NULL);

	total = n_workers * iters_per_worker;
	printf("DONE iters=%d fails=%d aborts=%d child=%s\n",
	       total, total_fails, total_aborts, child_path);
	return total_fails ? 1 : 0;
}

#endif  /* CHILD_MAIN */
