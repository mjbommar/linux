// SPDX-License-Identifier: GPL-2.0
//
// Minimal C reproducer for fork/exec page-table state regressions.
//
// Mirrors threaded-subprocess-wait.py's fork+exec churn but in pure
// C, without Python interpreter overhead. N pthreads each loop fork()
// + execve(/bin/true) + waitpid().
//
// A failure here is independent of Python's subprocess/threading
// machinery and points at fork+exec page-table state under concurrent
// worker_thread activity in the same mm.
//
// Build:  cc -O0 -static -pthread -o threaded-fork-exec threaded-fork-exec.c
// Run:    ./threaded-fork-exec [N_WORKERS] [ITERS_PER_WORKER]
// Default: 2 workers x 200 iters = 400 forks (matches Python repro)

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int n_workers = 2;
static int iters_per_worker = 200;

static volatile int total_fails;

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
			/* Child: exec /bin/true (smallest-possible exec'd
			 * binary that returns 0). */
			char *const argv[] = { "/bin/true", NULL };
			char *const envp[] = { NULL };
			execve("/bin/true", argv, envp);
			/* execve only returns on failure. */
			fprintf(stderr, "[w%ld iter=%d] EXECVE_FAIL: %s\n",
				wid, i, strerror(errno));
			_exit(127);
		}

		/* Parent: wait for child. */
		int status;
		pid_t r = waitpid(pid, &status, 0);

		if (r < 0) {
			fprintf(stderr, "[w%ld iter=%d] WAIT_FAIL: %s\n",
				wid, i, strerror(errno));
			__sync_add_and_fetch(&total_fails, 1);
			return NULL;
		}

		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
			int rc  = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
			fprintf(stderr, "[w%ld iter=%d] CHILD_FAIL rc=%d sig=%d\n",
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

	if (argc >= 2)
		n_workers = atoi(argv[1]);
	if (argc >= 3)
		iters_per_worker = atoi(argv[2]);
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
	printf("DONE iters=%d fails=%d\n", total, total_fails);
	return total_fails ? 1 : 0;
}
