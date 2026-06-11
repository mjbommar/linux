// SPDX-License-Identifier: GPL-2.0
//
// Minimal regression test for fork-state exit-code propagation.
//
// Shape: a 3-level process tree where the middle process forks a
// child, waits for it, and exits cleanly. The failure signature is
// that do_exit reports the parent's group_exit_code as 0xff (255)
// instead of 0. Two-level trees and 3-level trees without the middle
// wait do not exercise this path.
//
// Run from a shell init script under UML so init (PID 1) is the
// shell and this binary is PID 2; child is PID 3.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

int main(void)
{
	pid_t p = fork();
	if (p < 0) {
		printf("FORK_TREE_3LEVEL: FAIL fork errno=%d\n", errno);
		return 2;
	}
	if (p == 0) {
		_exit(42);  /* known child exit code */
	}
	int st = 0;
	pid_t r = waitpid(p, &st, 0);
	if (r != p || !WIFEXITED(st) || WEXITSTATUS(st) != 42) {
		printf("FORK_TREE_3LEVEL: FAIL wait r=%d st=0x%x errno=%d\n",
		       (int)r, st, errno);
		return 3;
	}
	printf("FORK_TREE_3LEVEL: PASS child=%d status=0x%x\n", (int)p, st);
	return 0;
}
