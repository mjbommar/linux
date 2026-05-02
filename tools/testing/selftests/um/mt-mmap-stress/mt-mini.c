// SPDX-License-Identifier: GPL-2.0
/*
 * SMP-T10 minimal reproducer for the mt-byteset T>=N flake.
 *
 * Two pthreads each run a tight mmap+memset+munmap loop while a
 * SIGSEGV handler captures full register state + per-thread TLS
 * markers + caller stack content. Goal: ground-truth on whether
 * the corruption is from another thread (cross-task aliasing),
 * cross-vCPU stale TLB, or kernel-side state-swap.
 *
 * Each worker advertises:
 *   - last_mmap_p TLS    : the pointer we just got from mmap()
 *   - last_iter   TLS    : iteration index inside the worker loop
 *   - last_tid    TLS    : the worker's thread index (0/1)
 *   - last_cpu    TLS    : sched_getcpu() at start of slow_memset
 *
 * On SIGSEGV the handler dumps:
 *   - all GP regs from ucontext_t
 *   - the four TLS markers
 *   - 8 qwords near rbp (the slow_memset stack frame)
 *   - the worker's getcpu() AT FAULT (not at start of memset)
 *
 * Exit codes: 0 = ALL_OK, 2 = verify-mismatch, 3 = SIGSEGV.
 *
 * Build: gcc -static -O0 -pthread -o mt-mini mt-mini.c
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

/*
 * SMP-T11 state-trace dump trigger. Disables tracing first (so the
 * dispatches we do to issue the dump don't overwrite the failing
 * context in the per-CPU ring), then dumps. No-op (silent) when the
 * kernel wasn't built with CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE=y or
 * when tracing isn't enabled at runtime.
 *
 * Called from the FAIL paths so the per-CPU ring captures up to the
 * moment of detection — kernel dumps to dmesg (KVMV2T lines).
 */
static void kvmv2_state_trace_dump(void)
{
	int fd;

	fd = open("/sys/kernel/debug/um_kvm_v2_trace/enabled", O_WRONLY);
	if (fd >= 0) {
		(void)!write(fd, "0\n", 2);
		close(fd);
	}
	fd = open("/sys/kernel/debug/um_kvm_v2_trace/dump", O_WRONLY);
	if (fd < 0)
		return;
	(void)!write(fd, "1\n", 2);
	close(fd);
}

#define ITERS    50
#define ALLOC_SZ 0x10000

static __thread void *last_mmap_p;
static __thread int   last_iter;
static __thread long  last_tid;
static __thread int   last_cpu;

static volatile int crash_dumped;

static void sigsegv_handler(int sig, siginfo_t *si, void *ctx_)
{
	if (__sync_lock_test_and_set(&crash_dumped, 1))
		_exit(3);

	/* Trigger kvm-v2 state-trace dump as early as possible — before
	 * fprintf/_exit run additional syscalls on this vCPU and possibly
	 * push older entries out of the ring. */
	kvmv2_state_trace_dump();
	ucontext_t *uc = (ucontext_t *)ctx_;
	greg_t *g = uc->uc_mcontext.gregs;
	int now_cpu = sched_getcpu();
	fprintf(stderr,
		"DIAG: tid=%ld iter=%d last_mmap_p=%p si_addr=%p start_cpu=%d fault_cpu=%d\n"
		"      RIP=%llx RSP=%llx RBP=%llx\n"
		"      RAX=%llx RBX=%llx RCX=%llx RDX=%llx\n"
		"      RDI=%llx RSI=%llx R8=%llx R9=%llx\n",
		last_tid, last_iter, last_mmap_p, si->si_addr,
		last_cpu, now_cpu,
		(unsigned long long)g[REG_RIP],
		(unsigned long long)g[REG_RSP],
		(unsigned long long)g[REG_RBP],
		(unsigned long long)g[REG_RAX],
		(unsigned long long)g[REG_RBX],
		(unsigned long long)g[REG_RCX],
		(unsigned long long)g[REG_RDX],
		(unsigned long long)g[REG_RDI],
		(unsigned long long)g[REG_RSI],
		(unsigned long long)g[REG_R8],
		(unsigned long long)g[REG_R9]);
	uint64_t rbp = (uint64_t)g[REG_RBP];
	if (rbp) {
		unsigned char *sp = (unsigned char *)rbp;
		fprintf(stderr, "      stack@rbp:");
		for (int i = -0x30; i <= 0x10; i += 8) {
			fprintf(stderr, " [rbp%+d]=0x%016llx", i,
				*(unsigned long long *)(sp + i));
		}
		fprintf(stderr, "\n");
	}
	fflush(stderr);
	_exit(3);
}

static void slow_memset(void *p, unsigned char val, size_t n)
{
	volatile unsigned char *vp = (volatile unsigned char *)p;
	for (size_t i = 0; i < n; i++)
		vp[i] = val;
}

static void *worker(void *arg)
{
	long tid = (long)arg;
	last_tid = tid;
	for (int i = 0; i < ITERS; i++) {
		last_iter = i;
		void *p = mmap(NULL, ALLOC_SZ, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		last_mmap_p = p;
		if (p == MAP_FAILED) {
			kvmv2_state_trace_dump();
			fprintf(stderr, "MMAP_FAILED tid=%ld iter=%d\n",
				tid, i);
			return (void *)1;
		}
		if (p == NULL) {
			kvmv2_state_trace_dump();
			fprintf(stderr, "MMAP_NULL tid=%ld iter=%d\n",
				tid, i);
			return (void *)4;
		}
		last_cpu = sched_getcpu();
		slow_memset(p, (unsigned char)tid, ALLOC_SZ);
		for (size_t j = 0; j < ALLOC_SZ; j += 4096) {
			if (((unsigned char *)p)[j] != (unsigned char)tid) {
				kvmv2_state_trace_dump();
				fprintf(stderr,
					"VERIFY_FAIL tid=%ld iter=%d off=%#zx got=%u expect=%u p=%p\n",
					tid, i, j, ((unsigned char *)p)[j],
					(unsigned char)tid, p);
				munmap(p, ALLOC_SZ);
				return (void *)2;
			}
		}
		munmap(p, ALLOC_SZ);
	}
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

	int n = (argc > 1) ? atoi(argv[1]) : 2;
	if (n > 16) n = 16;
	pthread_t t[16];
	for (long i = 0; i < n; i++)
		pthread_create(&t[i], NULL, worker, (void *)i);
	int rc = 0;
	for (int i = 0; i < n; i++) {
		void *r;
		pthread_join(t[i], &r);
		if (r) rc = 1;
	}
	if (rc == 0) printf("ALL_OK n=%d\n", n);
	return rc;
}
