// Child does NOTHING (no stack writes) — straight to exit_group via
// inline asm. If this PASSes but child_simple FAILs: bug is in CoW
// page handling for child's first stack write.
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

int main(void) {
	pid_t p = fork();
	if (p == 0) {
		/* No stack writes whatsoever — straight asm to exit_group(0). */
		__asm__ volatile (
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
