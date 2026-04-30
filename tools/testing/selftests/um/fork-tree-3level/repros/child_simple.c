// Child only does printf and _exit. Parent exits immediately.
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
int main(void) {
	pid_t p = fork();
	if (p == 0) {
		printf("CHILD_OK\n");
		fflush(stdout);
		_exit(0);
	}
	_exit(0);
}
