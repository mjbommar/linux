// SPDX-License-Identifier: GPL-2.0
/*
 * Child exits through inline asm before stack writes so the harness can
 * isolate first-write CoW behavior.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	pid_t p = fork();

	if (p == 0) {
		__asm__ volatile(
			"mov $231, %%rax\n"
			"xor %%rdi, %%rdi\n"
			"syscall\n"
			:
			:
			: "rax", "rcx", "r11", "memory"
		);
		__builtin_unreachable();
	}
	_exit(0);
}
