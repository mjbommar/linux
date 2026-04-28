/* Class A env repro: PID-1 init has stdin/stdout/stderr wired to
 * UML's serial console (fd:0,fd:1) which is not a tty from the
 * kernel's perspective. Maps to test_tty / sys.stdin.isatty(). */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	int a = isatty(0);
	int b = isatty(1);
	int c = isatty(2);
	if (!a && !b && !c)
		printf("REPRO: tty_isatty PASS\n");
	else
		printf("REPRO: tty_isatty FAIL stdin=%d stdout=%d stderr=%d\n",
		       a, b, c);
	return 0;
}
