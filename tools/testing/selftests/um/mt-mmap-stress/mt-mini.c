// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal two-thread mmap/memset/munmap stress reproducer.
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
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define ITERS    50
#define ALLOC_SZ 0x10000

/*
 * Optional jitter sweep. MT_JITTER_NS inserts a nanosleep between
 * slow_memset() and verify in each worker iteration. Default 0 means
 * no extra delay.
 */
static long jitter_ns;

static __thread void *last_mmap_p;
static __thread int   last_iter;
static __thread long  last_tid;
static __thread int   last_cpu;

static volatile int crash_dumped;

static void sigsegv_handler(int sig, siginfo_t *si, void *ctx_)
{
	if (__sync_lock_test_and_set(&crash_dumped, 1))
		_exit(3);

	ucontext_t *uc = (ucontext_t *)ctx_;
	greg_t *g = uc->uc_mcontext.gregs;
	int now_cpu = sched_getcpu();
	fprintf(stderr,
		"FAULT_CONTEXT tid=%ld iter=%d last_mmap_p=%p si_addr=%p start_cpu=%d fault_cpu=%d\n"
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

/*
 * Strict slow_memset reads each byte back immediately after writing.
 * This separates write-time corruption from later verify-time changes.
 *
 * Returns offset of first mismatch, or n on success.
 * Only used when MT_STRICT_MEMSET=1.
 */
static size_t strict_memset(void *p, unsigned char val, size_t n)
{
	volatile unsigned char *vp = (volatile unsigned char *)p;
	for (size_t i = 0; i < n; i++) {
		vp[i] = val;
		if (vp[i] != val)
			return i;
	}
	return n;
}

static int strict_memset_enabled;

/*
 * Read /proc/self/pagemap to get the PFN for a virtual address. Used
 * at VERIFY_FAIL/STRICT_MEMSET_FAIL time to log the failing address's
 * PFN. Returns 0 on any error.
 */
static unsigned long pagemap_pfn(const void *vaddr)
{
	int fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0)
		return 0;
	off_t off = ((uintptr_t)vaddr / 4096) * 8;
	if (lseek(fd, off, SEEK_SET) < 0) {
		close(fd);
		return 0;
	}
	uint64_t entry = 0;
	if (read(fd, &entry, sizeof(entry)) != sizeof(entry)) {
		close(fd);
		return 0;
	}
	close(fd);
	if (!(entry & (1ULL << 63)))   /* page present? */
		return 0;
	return (unsigned long)(entry & ((1ULL << 55) - 1));
}

/*
 * Optional CPU pinning via MT_PIN_GUEST_CPUS=1. Worker tid N pins to
 * CPU (N % online).
 */
static int pin_guest_cpus_enabled;

/*
 * Optional prefault via MT_PREFAULT=1. If enabled, slow_memset() does
 * not need to service first-touch page faults in the hot write loop.
 *
 * If MT_PREFAULT=1 makes failures vanish: fault-replay confirmed,
 * fix is in arch/um/backend/kvm-v2/vcpu.c PF EINTR handler.
 * If failures persist: hypothesis dead, look elsewhere.
 */
static int prefault_enabled;

static void *worker(void *arg)
{
	long tid = (long)arg;
	last_tid = tid;
	if (pin_guest_cpus_enabled) {
		int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
		int target = (int)(tid % ncpus);
		cpu_set_t cs;
		CPU_ZERO(&cs);
		CPU_SET(target, &cs);
		int r = pthread_setaffinity_np(pthread_self(),
					       sizeof(cs), &cs);
		if (r != 0)
			fprintf(stderr,
				"PIN_FAIL tid=%ld target=%d r=%d (pthread_setaffinity_np)\n",
				tid, target, r);
		else if (tid == 0)
			fprintf(stderr, "PIN_OK tid=%ld target=%d\n",
				tid, target);
	}
	for (int i = 0; i < ITERS; i++) {
		last_iter = i;
		void *p = mmap(NULL, ALLOC_SZ, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		last_mmap_p = p;
		if (p == MAP_FAILED) {
			fprintf(stderr, "MMAP_FAILED tid=%ld iter=%d\n",
				tid, i);
			return (void *)1;
		}
		if (p == NULL) {
			fprintf(stderr, "MMAP_NULL tid=%ld iter=%d\n",
				tid, i);
			return (void *)4;
		}
		last_cpu = sched_getcpu();
		if (prefault_enabled) {
			/*
			 * Use MADV_POPULATE_WRITE instead of read-touch.
			 * Anonymous mmap reads map the shared
			 * zero page read-only; the COW fault still fires
			 * on first WRITE. POPULATE_WRITE forces writable
			 * fault-in for the whole range, so strict_memset's
			 * first stores per page won't trigger #PF.
			 */
			if (madvise(p, ALLOC_SZ, MADV_POPULATE_WRITE) != 0) {
				static int once = 0;
				if (!once) {
					once = 1;
					fprintf(stderr,
						"WARN: MADV_POPULATE_WRITE failed errno=%d (continuing)\n",
						errno);
				}
			}
		}
		if (strict_memset_enabled) {
			size_t first_mismatch =
				strict_memset(p, (unsigned char)tid, ALLOC_SZ);
			if (first_mismatch < ALLOC_SZ) {
				unsigned long pfn = pagemap_pfn(
					(unsigned char *)p + first_mismatch);
				fprintf(stderr,
					"STRICT_MEMSET_FAIL tid=%ld iter=%d off=%#zx p=%p pfn=%lx (write-time corruption)\n",
					tid, i, first_mismatch, p, pfn);
				munmap(p, ALLOC_SZ);
				return (void *)5;
			}
		} else {
			slow_memset(p, (unsigned char)tid, ALLOC_SZ);
		}
		if (jitter_ns > 0) {
			struct timespec ts = {
				.tv_sec  = jitter_ns / 1000000000L,
				.tv_nsec = jitter_ns % 1000000000L,
			};
			nanosleep(&ts, NULL);
		}
		for (size_t j = 0; j < ALLOC_SZ; j += 4096) {
			volatile unsigned char *vp = (volatile unsigned char *)p;
			unsigned char got = vp[j];
			if (got != (unsigned char)tid) {
				/*
				 * Capture at the moment of failure:
				 *
				 *   read1   = the byte we just read (already in `got`)
				 *   probe1  = read 16 surrounding bytes (does the
				 *             expected value exist nearby? if yes,
				 *             stale TLB / off-by-one at write time)
				 *   read2   = re-read the SAME offset right after
				 *             a brief pause (does it still fail?
				 *             yes => permanent; no => transient)
				 *   write_retry = write 'V' to the SAME offset, then
				 *             re-read. PASS => bug was at write time
				 *             (write went to wrong place originally)
				 *             but retry now hits correct mapping.
				 *             Still FAIL => page itself is broken.
				 *   page_scan = scan whole page for any non-zero
				 *             non-V bytes (cross-task content?)
				 */
				unsigned char read2;
				unsigned char probe[8] = { 0 };
				int probe_off;
				size_t k;
				int has_v = 0, has_nonzero = 0, has_other = 0;
				unsigned char other_val = 0;

				/* probe1: 8 bytes around offset j */
				for (probe_off = 0; probe_off < 8; probe_off++) {
					if (j + probe_off < ALLOC_SZ)
						probe[probe_off] = vp[j + probe_off];
				}

				/* read2: short wait then re-read same offset */
				{
					struct timespec ts = {0, 1000};
					nanosleep(&ts, NULL);
				}
				read2 = vp[j];

				/* write_retry: re-write same byte then re-read */
				vp[j] = (unsigned char)tid;
				unsigned char write_retry = vp[j];

				/* page_scan: any tid-bytes? any other non-zero? */
				for (k = 0; k < ALLOC_SZ; k++) {
					unsigned char b = ((unsigned char *)p)[k];
					if (b == (unsigned char)tid) has_v = 1;
					else if (b != 0) {
						has_nonzero = 1;
						if (!has_other) {
							other_val = b;
							has_other = 1;
						}
					}
				}

				unsigned long pfn =
					pagemap_pfn((unsigned char *)p + j);
				fprintf(stderr,
					"VERIFY_FAIL tid=%ld iter=%d off=%#zx got=%u expect=%u p=%p\n"
					"VERIFY_PROBE read2=%u write_retry=%u probe=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n"
					"VERIFY_PAGE_SCAN has_v=%d has_nonzero=%d other_val=%u fault_cpu=%d pfn=%lx\n",
					tid, i, j, got, (unsigned char)tid, p,
					read2, write_retry,
					probe[0], probe[1], probe[2], probe[3],
					probe[4], probe[5], probe[6], probe[7],
					has_v, has_nonzero, other_val,
					sched_getcpu(), pfn);
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

	const char *jitter_env = getenv("MT_JITTER_NS");
	if (jitter_env && *jitter_env)
		jitter_ns = strtol(jitter_env, NULL, 10);
	if (jitter_ns > 0)
		fprintf(stderr, "MT_JITTER_NS=%ld\n", jitter_ns);

	const char *strict_env = getenv("MT_STRICT_MEMSET");
	if (strict_env && *strict_env)
		strict_memset_enabled = atoi(strict_env);
	if (strict_memset_enabled)
		fprintf(stderr, "MT_STRICT_MEMSET=1\n");

	const char *pin_env = getenv("MT_PIN_GUEST_CPUS");
	if (pin_env && *pin_env)
		pin_guest_cpus_enabled = atoi(pin_env);
	if (pin_guest_cpus_enabled)
		fprintf(stderr, "MT_PIN_GUEST_CPUS=1\n");

	const char *prefault_env = getenv("MT_PREFAULT");
	if (prefault_env && *prefault_env)
		prefault_enabled = atoi(prefault_env);
	if (prefault_enabled)
		fprintf(stderr, "MT_PREFAULT=1 (read each page before write loop)\n");

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
