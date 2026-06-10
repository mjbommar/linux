// SPDX-License-Identifier: GPL-2.0
/*
 * Host-only test for the atomic stack-and-control swap primitive.
 * From inside a deep C call chain, the primitive switches to a fresh
 * stack and jumps to a clean entry function. It leaves the inherited
 * stack behind without executing `ret` against any saved-RIP slots.
 *
 * The test uses pure inline asm rather than a hand-built rt_sigframe:
 * `movq new_rsp, %rsp; jmpq *entry`.
 *
 *   1. main() recurses ~20 frames deep to mimic the
 *      "syscall_handler -> vfs_write -> ... -> fork_on_resume_loop"
 *      shape UML hits. Saved RIPs on the stack should be
 *      irrelevant once we pivot.
 *   2. At the bottom of the recursion, call build_frame_and_jump
 *      which does inline `movq new_rsp, %rsp; jmpq *entry`.
 *      No syscall, no sigframe, no glibc.
 *   3. clean_entry runs on the new stack and _exit(0)s.
 *   4. If we ever return from build_frame_and_jump, print FAIL.
 *
 * x86_64 only.
 *
 * Build:
 *     cc -O0 -static -o rt_sigreturn_test rt_sigreturn_test.c
 *
 * Acceptance:
 *     $ ./rt_sigreturn_test
 *     main entered
 *     recursion depth=20
 *     about to pivot stack
 *     STACK_PIVOT: arrived
 *     $ echo $?
 *     0
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static char alt_stack[64 * 1024] __attribute__((aligned(16)));

/*
 * Forward decl: clean_entry is the post-pivot landing.
 * Marked noreturn because it _exit()s; the compiler is happy not
 * to allocate a stack frame at the entry point as long as we
 * make sure we never call it the C way.
 */
static void clean_entry(void) __attribute__((noreturn));

static const char arrived_msg[] = "STACK_PIVOT: arrived\n";
static const char trampoline_msg[] = "about to pivot stack\n";

/*
 * The primitive: atomic stack swap + indirect jump.  No
 * syscalls, no sigframes, no glibc state.  Once we execute the
 * asm block, the caller's stack frame is unreachable and
 * clean_entry runs on the fresh alt_stack.
 *
 * %rsp alignment: x86_64 ABI says %rsp at function entry is
 * (16N + 8): the 8 is the CALL-pushed return address. Since
 * we jmpq (no CALL), we need %rsp to be 16-byte aligned at
 * function entry instead.  Adjust accordingly.
 *
 * Register clobbers: we leave segment registers, FS_BASE,
 * GS_BASE alone (TLS continues to work).  We zero RBP so
 * stack-unwinders see a clean root.  RAX/RCX/RDX are caller-
 * saved per the ABI; clean_entry restores them as needed.
 */
static void __attribute__((noinline, noreturn))
build_frame_and_jump(void)
{
	uintptr_t new_rsp;

	new_rsp = (uintptr_t)(alt_stack + sizeof(alt_stack) - 128);
	new_rsp &= ~(uintptr_t)15;

	write(1, trampoline_msg, sizeof(trampoline_msg) - 1);

	asm volatile (
		"movq %0, %%rsp\n\t"
		"xorq %%rbp, %%rbp\n\t"
		"jmpq *%1\n\t"
		: : "r"(new_rsp), "r"(&clean_entry) : "memory"
	);
	__builtin_unreachable();
}

static void clean_entry(void)
{
	write(1, arrived_msg, sizeof(arrived_msg) - 1);
	_exit(0);
}

/*
 * Mimic a deep call chain. Recurse 20 levels. Marked
 * noinline + volatile sentinel so the compiler can't fold the
 * recursion away.
 */
static volatile int sentinel;

static void __attribute__((noinline)) deep_recurse(int depth)
{
	char local[64];
	char buf[32];
	int n;

	memset(local, 0xa5, sizeof(local));
	sentinel = depth;

	if (depth >= 20) {
		n = snprintf(buf, sizeof(buf), "recursion depth=%d\n",
			     depth);
		write(1, buf, (size_t)n);
		build_frame_and_jump();
		/* Unreachable; if we get here the syscall returned
		 * without jumping, which is the failure mode we want
		 * to surface to the operator.
		 */
		write(1, "BUG: rt_sigreturn returned\n", 27);
		_exit(1);
	}

	deep_recurse(depth + 1);

	/* Defeat tail-call optimization. */
	sentinel = depth + 1;
	(void)local;
}

int main(void)
{
	write(1, "main entered\n", 13);
	deep_recurse(0);
	write(1, "BUG: deep_recurse returned\n", 27);
	return 1;
}
