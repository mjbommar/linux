/* Class D structural repro: ITIMER_VIRTUAL accounting under UML's
 * seccomp stub-child CPU-time split.
 *
 * ITIMER_VIRTUAL fires on user-mode CPU time consumed by the calling
 * process. Under UML's seccomp stub-child model the Python (or any
 * guest userspace) process's user-mode time is split between the
 * spawner (UML kernel-side userspace process) and the stub child
 * (which actually runs guest user code). When setitimer(VIRTUAL, ...)
 * is forwarded to the host it tracks the spawner's CPU time, not the
 * stub child's. The busy loop runs in the stub child, so virtual
 * time accumulates on the wrong process and SIGVTALRM never fires.
 *
 * test_signal::test_itimer_virtual hangs ~180s in CPython regrtest;
 * this reproducer reports the same failure mode in ~1s wall time.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static volatile sig_atomic_t sigvtalrm_count;

static void handler(int sig)
{
	(void)sig;
	sigvtalrm_count++;
}

int main(void)
{
	struct sigaction sa;
	struct itimerval it;
	struct timespec start, now;
	volatile uint64_t spin = 0;
	double elapsed;
	int n;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGVTALRM, &sa, NULL);

	it.it_interval.tv_sec  = 0;
	it.it_interval.tv_usec = 100000; /* 100ms */
	it.it_value.tv_sec     = 0;
	it.it_value.tv_usec    = 100000;
	if (setitimer(ITIMER_VIRTUAL, &it, NULL) != 0) {
		printf("REPRO: itimer_virtual FAIL setitimer_failed\n");
		return 0;
	}

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		for (int i = 0; i < 1000000; i++)
			spin += (uint64_t)i;
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (now.tv_sec - start.tv_sec) +
			  (now.tv_nsec - start.tv_nsec) / 1e9;
	} while (elapsed < 1.0);

	n = sigvtalrm_count;
	if (n >= 3)
		printf("REPRO: itimer_virtual PASS sigvtalrm_count=%d\n", n);
	else
		printf("REPRO: itimer_virtual EXPECTED_FAIL sigvtalrm_count=%d expected_ge_3\n", n);
	return 0;
}
