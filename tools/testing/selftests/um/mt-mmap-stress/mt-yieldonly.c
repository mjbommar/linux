// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal yield-only reproducer. Each pthread does:
 *
 *     for i in 0..ITERS: sched_yield()
 *
 * No stack writes (other than function-call frame). No mmap. No user
 * memory reads. Just syscalls in a tight loop.
 *
 * Failure modes to look for:
 *  - SIGSEGV in pthread/libc code (cross-task state contamination)
 *  - Hang (scheduler bug)
 *
 * Build: gcc -static -O0 -pthread -o mt-yieldonly mt-yieldonly.c
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>

static volatile int crash_dumped;
static void sigsegv_handler(int sig, siginfo_t *si, void *ctx_)
{
	if (__sync_lock_test_and_set(&crash_dumped, 1))
		_exit(3);
	ucontext_t *uc = (ucontext_t *)ctx_;
	greg_t *g = uc->uc_mcontext.gregs;
	fprintf(stderr,
		"YIELD_SEGV si_addr=%p RIP=%llx RSP=%llx RAX=%llx RDI=%llx\n",
		si->si_addr,
		(unsigned long long)g[REG_RIP],
		(unsigned long long)g[REG_RSP],
		(unsigned long long)g[REG_RAX],
		(unsigned long long)g[REG_RDI]);
	_exit(3);
}

#define ITERS 200000

static void *worker(void *arg)
{
	long tid = (long)arg;
	for (long i = 0; i < ITERS; i++)
		sched_yield();
	(void)tid;
	return NULL;
}

int main(int argc, char **argv)
{
	struct sigaction sa = {
		.sa_sigaction = sigsegv_handler,
		.sa_flags = SA_SIGINFO | SA_RESETHAND,
	};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);

	int n = (argc > 1) ? atoi(argv[1]) : 8;
	if (n > 16) n = 16;
	pthread_t t[16];

	for (long i = 0; i < n; i++)
		pthread_create(&t[i], NULL, worker, (void *)i);
	for (long i = 0; i < n; i++)
		pthread_join(t[i], NULL);
	printf("YIELD_OK n=%d\n", n);
	return 0;
}
