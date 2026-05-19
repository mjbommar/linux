/*
 * T69 — bytecode-integrity monitor as an LD_PRELOAD library (v4).
 *
 * v4 changes vs v3:
 *   - "Newly-zeroed byte" tracking, not "page had big zero run".
 *     v3 missed a real cache abort because its had_big_zero flag
 *     was per-page: Python heap pages routinely contain pre-existing
 *     20+ byte zero runs (allocator padding), which marked every
 *     interesting page as already-zero at init and suppressed all
 *     future alerts.
 *   - For each tracked page, snapshot a 4096-bit "was-zero-at-init"
 *     bitmap. On each check, compute "is-zero-now" and look for
 *     bytes that flipped non-zero → zero. If a contiguous run of
 *     >=min_zero such newly-zeroed bytes appears, log it.
 *   - Snapshot is per-byte (4096 bits = 512 bytes per page). Cheap.
 *
 * Build:
 *   gcc -O2 -fPIC -shared -o bc-monitor.so bc-monitor.c -ldl
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <time.h>

#define MAX_REGIONS    128
#define PAGE_SIZE      4096
#define MAX_PAGES_REG  1024              /* 4 MB region cap → 1024 pages */
#define DEFAULT_MIN_ZERO 24
#define BYTES_PER_PAGE_BITMAP (PAGE_SIZE / 8)  /* 512 bytes */

struct region {
	uintptr_t lo;
	uintptr_t hi;
	/* one bit per byte per page: 1 = byte was zero at init */
	uint8_t   *was_zero;     /* MAX_PAGES_REG * BYTES_PER_PAGE_BITMAP bytes */
	/* one bit per page: 1 = page already alerted (suppress repeats) */
	uint8_t   page_alerted[(MAX_PAGES_REG + 7) / 8];
	int       pages_seeded;  /* number of pages whose was_zero is initialised */
};

static struct region R[MAX_REGIONS];
static int n_regions = 0;
static unsigned long call_count = 0;
static int interval = 64;
static int min_zero = DEFAULT_MIN_ZERO;
static FILE *logf = NULL;
static int monitor_inited = 0;

static int longest_zero_run(const uint8_t *p, int *lo_out, int *hi_out)
{
	int best_lo = -1, best_hi = -1, best_len = 0;
	int i = 0;
	while (i < PAGE_SIZE) {
		if (p[i] == 0) {
			int j = i;
			while (j < PAGE_SIZE && p[j] == 0) j++;
			int len = j - i;
			if (len > best_len) { best_len = len; best_lo = i; best_hi = j; }
			i = j;
		} else {
			i++;
		}
	}
	if (lo_out) *lo_out = best_lo;
	if (hi_out) *hi_out = best_hi;
	return best_len;
}

static inline int bit_get(const uint8_t *bs, int idx)
{
	return (bs[idx >> 3] >> (idx & 7)) & 1;
}

static inline void bit_set(uint8_t *bs, int idx)
{
	bs[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

static int find_region(uintptr_t lo)
{
	for (int i = 0; i < n_regions; i++)
		if (R[i].lo == lo) return i;
	return -1;
}

/* Compute the "is-zero-now" bitmap of a page (1 bit per byte). */
static void compute_zero_bitmap(const uint8_t *p, uint8_t *bm)
{
	memset(bm, 0, BYTES_PER_PAGE_BITMAP);
	for (int i = 0; i < PAGE_SIZE; i++)
		if (p[i] == 0)
			bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/* Find the longest contiguous run of bytes that are zero NOW and
 * were NOT zero at init (i.e., newly-zeroed). diff_bm = now_bm AND
 * NOT was_zero. Returns run length and (lo,hi) byte indices. */
static int longest_newly_zero_run(const uint8_t *now_bm,
				  const uint8_t *was_zero,
				  int *lo_out, int *hi_out)
{
	int best_lo = -1, best_hi = -1, best_len = 0;
	int i = 0;
	while (i < PAGE_SIZE) {
		int now_zero = (now_bm[i >> 3] >> (i & 7)) & 1;
		int orig_zero = (was_zero[i >> 3] >> (i & 7)) & 1;
		if (now_zero && !orig_zero) {
			int j = i;
			while (j < PAGE_SIZE) {
				int nz = (now_bm[j >> 3] >> (j & 7)) & 1;
				int oz = (was_zero[j >> 3] >> (j & 7)) & 1;
				if (!nz || oz) break;
				j++;
			}
			int len = j - i;
			if (len > best_len) { best_len = len; best_lo = i; best_hi = j; }
			i = j;
		} else {
			i++;
		}
	}
	if (lo_out) *lo_out = best_lo;
	if (hi_out) *hi_out = best_hi;
	return best_len;
}

/* Initialise per-page was_zero bitmap for a freshly-added region. */
static void seed_region(int idx)
{
	uintptr_t lo = R[idx].lo;
	uintptr_t hi = R[idx].hi;
	int npages = (int)((hi - lo) / PAGE_SIZE);
	if (npages > MAX_PAGES_REG) npages = MAX_PAGES_REG;
	for (int pg = R[idx].pages_seeded; pg < npages; pg++) {
		const uint8_t *p = (const uint8_t *)(lo + (uintptr_t)pg * PAGE_SIZE);
		uint8_t *bm = R[idx].was_zero + (size_t)pg * BYTES_PER_PAGE_BITMAP;
		compute_zero_bitmap(p, bm);
	}
	R[idx].pages_seeded = npages;
}

static void scan_maps(void)
{
	FILE *m = fopen("/proc/self/maps", "r");
	char line[1024];
	if (!m) return;
	while (fgets(line, sizeof(line), m) && n_regions < MAX_REGIONS) {
		uintptr_t lo, hi;
		char perms[8] = {0};
		char path[256] = {0};
		int n = sscanf(line, "%lx-%lx %7s %*x %*s %*d %255[^\n]",
			       &lo, &hi, perms, path);
		if (n < 3) continue;
		if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p') continue;
		if (path[0] != '\0' && path[0] != '[') continue;
		if (strstr(path, "[stack")) continue;
		if (strstr(path, "[vdso") || strstr(path, "[vvar") ||
		    strstr(path, "[vsyscall")) continue;
		if (hi - lo > 4ull * 1024 * 1024) continue;
		/*
		 * UML kvm-v2 carves the guest user VA out of 0x550000000000+.
		 * The cache-aborting bytecode pages have always been observed
		 * in [heap] at 0x550000xxx_xxx (R9 + R13 captures). Skip
		 * unrelated mmap arenas (e.g. 0x7xxx... glibc malloc pools)
		 * to suppress noise. If host-side smoke testing is needed,
		 * set BC_MONITOR_NO_VA_FILTER=1.
		 */
		if (!getenv("BC_MONITOR_NO_VA_FILTER") &&
		    (lo < 0x550000000000ull || lo >= 0x550001000000ull))
			continue;

		int idx = find_region(lo);
		if (idx >= 0) {
			if (hi > R[idx].hi) {
				R[idx].hi = hi;
				seed_region(idx);
			}
			continue;
		}
		/* New region. */
		R[n_regions].lo = lo;
		R[n_regions].hi = hi;
		R[n_regions].was_zero = calloc(MAX_PAGES_REG, BYTES_PER_PAGE_BITMAP);
		if (!R[n_regions].was_zero) continue;
		memset(R[n_regions].page_alerted, 0,
		       sizeof(R[n_regions].page_alerted));
		R[n_regions].pages_seeded = 0;
		seed_region(n_regions);
		n_regions++;
	}
	fclose(m);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Scan every page of every tracked region. */
static void check_all(unsigned int hint_nr)
{
	if (!logf) return;
	uint8_t now_bm[BYTES_PER_PAGE_BITMAP];
	for (int i = 0; i < n_regions; i++) {
		uintptr_t lo = R[i].lo;
		uintptr_t hi = R[i].hi;
		int npages = (int)((hi - lo) / PAGE_SIZE);
		if (npages > MAX_PAGES_REG) npages = MAX_PAGES_REG;
		if (npages > R[i].pages_seeded) npages = R[i].pages_seeded;
		for (int pg = 0; pg < npages; pg++) {
			if (bit_get(R[i].page_alerted, pg)) continue;
			const uint8_t *p = (const uint8_t *)(lo + (uintptr_t)pg * PAGE_SIZE);
			compute_zero_bitmap(p, now_bm);
			uint8_t *was_bm = R[i].was_zero + (size_t)pg * BYTES_PER_PAGE_BITMAP;
			int zlo, zhi;
			int len = longest_newly_zero_run(now_bm, was_bm, &zlo, &zhi);
			if (len < min_zero) continue;
			uintptr_t page_va = lo + (uintptr_t)pg * PAGE_SIZE;
			fprintf(logf,
				"[%llu] BC_NEW_ZERO pid=%d region=%lx-%lx page=%lx newly_zero=[%d,%d) len=%d call=%lu hint_nr=%u\n",
				(unsigned long long)now_ns(),
				getpid(), lo, hi, page_va,
				zlo, zhi, len, call_count, hint_nr);
			int dlo = zlo > 16 ? zlo - 16 : 0;
			int dhi = zhi + 16 < PAGE_SIZE ? zhi + 16 : PAGE_SIZE;
			fprintf(logf, "  NEW[%d..%d]:", dlo, dhi);
			for (int k = dlo; k < dhi; k++)
				fprintf(logf, " %02x", p[k]);
			fprintf(logf, "\n");
			fflush(logf);
			bit_set(R[i].page_alerted, pg);
		}
	}
}

static void init_monitor(void)
{
	if (monitor_inited) return;
	monitor_inited = 1;
	const char *log_path = getenv("BC_MONITOR_LOG");
	if (!log_path) log_path = "/home/mjbommar/bc-monitor.log";
	logf = fopen(log_path, "a");
	if (!logf) return;
	setvbuf(logf, NULL, _IOLBF, 0);
	const char *iv = getenv("BC_MONITOR_INTERVAL");
	if (iv) interval = atoi(iv);
	if (interval < 1) interval = 1;
	const char *mz = getenv("BC_MIN_ZERO_RUN");
	if (mz) min_zero = atoi(mz);
	if (min_zero < 8) min_zero = 8;
	fprintf(logf,
		"[%llu] BC_MONITOR_INIT pid=%d interval=%d min_zero=%d (v4)\n",
		(unsigned long long)now_ns(), getpid(), interval, min_zero);
	fflush(logf);
	scan_maps();
}

typedef pid_t (*getpid_fn)(void);
typedef pid_t (*gettid_fn)(void);
typedef int   (*clock_gettime_fn)(clockid_t, struct timespec *);

static getpid_fn        real_getpid;
static gettid_fn        real_gettid;
static clock_gettime_fn real_clock_gettime;

pid_t getpid(void)
{
	if (!real_getpid) {
		real_getpid = dlsym(RTLD_NEXT, "getpid");
		init_monitor();
	}
	pid_t r = real_getpid();
	call_count++;
	if (call_count % interval == 0) {
		scan_maps();
		check_all(39);
	}
	return r;
}

pid_t gettid(void)
{
	if (!real_gettid) real_gettid = dlsym(RTLD_NEXT, "gettid");
	if (!monitor_inited) init_monitor();
	pid_t r = real_gettid();
	call_count++;
	if (call_count % interval == 0) check_all(186);
	return r;
}

int clock_gettime(clockid_t clk, struct timespec *ts)
{
	if (!real_clock_gettime)
		real_clock_gettime = dlsym(RTLD_NEXT, "clock_gettime");
	if (!monitor_inited) init_monitor();
	int r = real_clock_gettime(clk, ts);
	call_count++;
	if (call_count % interval == 0) check_all(228);
	return r;
}
