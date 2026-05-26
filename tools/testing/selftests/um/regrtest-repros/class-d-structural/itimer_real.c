/* Class D sanity check: ITIMER_REAL fires on wall time, so it should
 * NOT be affected by the UML stub-child CPU-time split. If this also
 * fails, something is more deeply broken than the structural CPU-time
 * mis-accounting issue.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static volatile sig_atomic_t sigalrm_count;

static void handler(int sig)
{
	(void)sig;
	sigalrm_count++;
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
	sigaction(SIGALRM, &sa, NULL);

	it.it_interval.tv_sec  = 0;
	it.it_interval.tv_usec = 100000;
	it.it_value.tv_sec     = 0;
	it.it_value.tv_usec    = 100000;
	if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
		printf("REPRO: itimer_real FAIL setitimer_failed\n");
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

	n = sigalrm_count;
	if (n >= 3)
		printf("REPRO: itimer_real PASS sigalrm_count=%d\n", n);
	else
		printf("REPRO: itimer_real FAIL sigalrm_count=%d expected_ge_3\n", n);
	return 0;
}
