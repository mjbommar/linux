/* Class C syscall repro: sched_getcpu must succeed (UML is 1-CPU by default,
 * expect cpu==0). */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>

int main(void)
{
	int cpu = sched_getcpu();
	int e = errno;

	if (cpu < 0) {
		printf("REPRO: os_sched_getcpu FAIL rc=%d errno=%d\n", cpu, e);
		return 0;
	}

	printf("REPRO: os_sched_getcpu PASS cpu=%d\n", cpu);
	return 0;
}
