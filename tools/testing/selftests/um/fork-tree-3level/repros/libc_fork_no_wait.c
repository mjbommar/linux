// libc fork; child exits, parent exits immediately without waiting.
// This isolates parent post-fork exit from waitpid and child
// interaction.
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
int main(void) {
	pid_t p = fork();
	if (p == 0) _exit(0);
	_exit(0);  /* parent: don't wait, exit immediately */
}
