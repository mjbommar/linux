/* Class A env repro: tcgetattr on a non-tty fd must fail with
 * ENOTTY. Maps to test_termios under PID-1 init. */
#define _GNU_SOURCE
#include <stdio.h>
#include <termios.h>
#include <errno.h>

int main(void)
{
	struct termios t;
	int r = tcgetattr(0, &t);
	int e = errno;
	if (r < 0 && (e == ENOTTY || e == EINVAL || e == EBADF))
		printf("REPRO: termios_get PASS\n");
	else if (r == 0)
		printf("REPRO: termios_get FAIL succeeded\n");
	else
		printf("REPRO: termios_get FAIL errno=%d\n", e);
	return 0;
}
