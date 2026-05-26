/* Class D structural repro: ITIMER_PROF accounting under UML's seccomp
 * stub-child CPU-time split.
 *
 * ITIMER_PROF tracks user+kernel CPU time. Like ITIMER_VIRTUAL it can
 * be mis-accounted under UML seccomp because the stub child actually
 * burns the cycles. Same shape as itimer_virtual: 100ms interval,
 * ~1s busy loop, expect >=3 SIGPROF.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static volatile sig_atomic_t sigprof_count;

static void handler(int sig)
{
	(void)sig;
	sigprof_count++;
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
	sigaction(SIGPROF, &sa, NULL);

	it.it_interval.tv_sec  = 0;
	it.it_interval.tv_usec = 100000;
	it.it_value.tv_sec     = 0;
	it.it_value.tv_usec    = 100000;
	if (setitimer(ITIMER_PROF, &it, NULL) != 0) {
		printf("REPRO: itimer_prof FAIL setitimer_failed\n");
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

	n = sigprof_count;
	if (n >= 3)
		printf("REPRO: itimer_prof PASS sigprof_count=%d\n", n);
	else
		printf("REPRO: itimer_prof EXPECTED_FAIL sigprof_count=%d expected_ge_3\n", n);
	return 0;
}
