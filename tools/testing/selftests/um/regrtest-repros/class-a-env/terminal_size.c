/* Class A env repro: PID-1 init has no controlling tty, so TIOCGWINSZ
 * on stdin/stdout should fail (ENOTTY) or report (0,0). Maps to
 * test_shutil's os.get_terminal_size() failure. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <termios.h>

int main(void)
{
	struct winsize ws0 = {0}, ws1 = {0};
	int r0 = ioctl(0, TIOCGWINSZ, &ws0);
	int e0 = errno;
	int r1 = ioctl(1, TIOCGWINSZ, &ws1);
	int e1 = errno;

	int ok0 = (r0 < 0 && (e0 == ENOTTY || e0 == EINVAL || e0 == EBADF))
		  || (r0 == 0 && ws0.ws_col == 0 && ws0.ws_row == 0);
	int ok1 = (r1 < 0 && (e1 == ENOTTY || e1 == EINVAL || e1 == EBADF))
		  || (r1 == 0 && ws1.ws_col == 0 && ws1.ws_row == 0);

	if (ok0 && ok1)
		printf("REPRO: terminal_size PASS\n");
	else
		printf("REPRO: terminal_size FAIL stdin=(%d,%dx%d) stdout=(%d,%dx%d)\n",
		       r0, ws0.ws_col, ws0.ws_row, r1, ws1.ws_col, ws1.ws_row);
	return 0;
}
