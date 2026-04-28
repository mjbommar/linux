/* Dump the termios mode UML hands PID-1 init under con0=fd:0,fd:1.
 *
 * The substrate gate's class-a-env/termios_get says "tcgetattr
 * succeeds on stdin" — surprising under PID-1, since we expected no
 * tty. The actual UML boot wires guest fd 0/1/2 through con0 to the
 * host's tty, so it IS a real tty — just maybe not the one CPython's
 * test_termios expects. This probe dumps the iflag/oflag/cflag/lflag
 * bits so memo 29 §2.5.2 can decide whether the skip-list entry for
 * test_termios is right or whether UML should be giving CPython a
 * different mode.
 *
 * Always emits PASS — this reproducer is diagnostic, not pass/fail.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

int main(void)
{
	struct termios t;
	if (tcgetattr(0, &t) < 0) {
		printf("REPRO: termios_mode_probe PASS no_tty errno=%d\n", errno);
		return 0;
	}
	printf("REPRO: termios_mode_probe PASS iflag=0x%x oflag=0x%x cflag=0x%x lflag=0x%x icanon=%d echo=%d\n",
		(unsigned)t.c_iflag,
		(unsigned)t.c_oflag,
		(unsigned)t.c_cflag,
		(unsigned)t.c_lflag,
		(t.c_lflag & ICANON) ? 1 : 0,
		(t.c_lflag & ECHO) ? 1 : 0);
	return 0;
}
