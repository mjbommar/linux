/* Class D diagnostic: getrusage(RUSAGE_SELF) after a 1s busy loop.
 *
 * On host Linux: ru_utime should be ~1s (the busy loop's user CPU
 * time is attributed to RUSAGE_SELF).
 *
 * On UML seccomp: ru_utime is expected to be ~0 because the user-mode
 * cycles were burned in the stub child rather than the spawner that
 * getrusage sees. This is the structural root cause of the Class D
 * itimer bugs (same accounting split).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>

int main(void)
{
	struct rusage ru;
	struct timespec start, now;
	volatile uint64_t spin = 0;
	double elapsed, ru_utime_ms, ru_stime_ms;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		for (int i = 0; i < 1000000; i++)
			spin += (uint64_t)i;
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (now.tv_sec - start.tv_sec) +
			  (now.tv_nsec - start.tv_nsec) / 1e9;
	} while (elapsed < 1.0);

	memset(&ru, 0, sizeof(ru));
	if (getrusage(RUSAGE_SELF, &ru) != 0) {
		printf("REPRO: getrusage_split FAIL getrusage_failed\n");
		return 0;
	}

	ru_utime_ms = ru.ru_utime.tv_sec * 1000.0 + ru.ru_utime.tv_usec / 1000.0;
	ru_stime_ms = ru.ru_stime.tv_sec * 1000.0 + ru.ru_stime.tv_usec / 1000.0;

	if (ru_utime_ms >= 800.0)
		printf("REPRO: getrusage_split PASS ru_utime_ms=%.1f ru_stime_ms=%.1f wall_ms=%.1f\n",
		       ru_utime_ms, ru_stime_ms, elapsed * 1000.0);
	else if (ru_utime_ms < 100.0)
		printf("REPRO: getrusage_split EXPECTED_FAIL ru_utime_ms=%.1f ru_stime_ms=%.1f wall_ms=%.1f stub_child_split\n",
		       ru_utime_ms, ru_stime_ms, elapsed * 1000.0);
	else
		printf("REPRO: getrusage_split FAIL ru_utime_ms=%.1f ru_stime_ms=%.1f wall_ms=%.1f partial\n",
		       ru_utime_ms, ru_stime_ms, elapsed * 1000.0);
	return 0;
}
