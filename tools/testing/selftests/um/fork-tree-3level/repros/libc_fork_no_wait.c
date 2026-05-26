// libc fork; child exits, parent exits IMMEDIATELY without waiting.
// If bug fires in parent: bug is in parent's post-fork stack.
// If bug doesn't fire: bug is in waitpid path or child interaction.
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
int main(void) {
	pid_t p = fork();
	if (p == 0) _exit(0);
	_exit(0);  /* parent: don't wait, exit immediately */
}
